/* SPDX-License-Identifier: Apache-2.0 */
#include "hisi_comp.h"

#define BLOCK_SIZE	512000
#define CACHE_NUM	1	//4

#define ZLIB_HEADER	"\x78\x9c"
#define ZLIB_HEADER_SZ	2

/*
 * We use a extra field for gzip block length. So the fourth byte is \x04.
 * This is necessary because our software don't know the size of block when
 * using an hardware decompresser (It is known by hardware). This help our
 * decompresser to work and helpfully, compatible with gzip.
 */
#define GZIP_HEADER	"\x1f\x8b\x08\x04\x00\x00\x00\x00\x00\x03"
#define GZIP_HEADER_SZ	10
#define GZIP_EXTRA_SZ	10
#define GZIP_TAIL_SZ	8

#define STREAM_CHUNK		1024
#define STREAM_CHUNK_OUT	(64*1024)

#define LOAD_SRC_TO_MSG(msg, src, idx, len)				\
	do {								\
		memcpy(msg, src + idx, len);				\
		idx += len;						\
	} while (0)

#define STORE_MSG_TO_DST(dst, idx, msg, len)				\
	do {								\
		memcpy(dst + idx, msg, len);				\
		idx += len;						\
	} while (0)

#define Z_OK            0
#define Z_STREAM_END    1
#define Z_ERRNO		(-1)
#define Z_STREAM_ERROR	(-EIO)

#define swab32(x) \
	((((x) & 0x000000ff) << 24) | \
	(((x) & 0x0000ff00) <<  8) | \
	(((x) & 0x00ff0000) >>  8) | \
	(((x) & 0xff000000) >> 24))

#define cpu_to_be32(x) swab32(x)

struct hisi_strm_info {
	struct wd_comp_arg	*arg;
	void	*next_in;
	void	*next_out;
	size_t	total_in;
	size_t	total_out;
	size_t	avail_in;
	size_t	avail_out;
	void	*ctx_buf;
	void	*ss_region;
	size_t	ss_region_size;
	int	stream_pos;
	int	alg_type;
	int	op_type;
	int	dw9;
	int	ctx_dw0;
	int	ctx_dw1;
	int	ctx_dw2;
	int	isize;
	int	checksum;
	uint64_t	si;
	uint64_t	di;
};

struct hisi_comp_sess {
	struct wd_ctx		ctx;
	struct wd_scheduler	sched;
	struct hisi_qm_capa	capa;
	struct hisi_strm_info	strm;
	int	inited;
};

struct hisi_sched {
	int	alg_type;
	int	op_type;
	int	dw9;
	struct hisi_zip_sqe	*msgs;
	struct wd_comp_arg	*arg;
	uint64_t	si;
	uint64_t	di;
	size_t		total_out;
	size_t	store_len;
	int	load_head;
	int	real_len;	/* gzip without header & extra */
};

static void hizip_sched_init(struct wd_scheduler *sched, int i)
{
	struct wd_msg	*wd_msg = &sched->msgs[i];
	struct hisi_zip_sqe	*msg;
	struct hisi_sched	*hpriv = sched->priv;

	msg = wd_msg->msg = &hpriv->msgs[i];
	msg->dw9 = hpriv->dw9;
	msg->dest_avail_out = sched->msg_data_size;
	msg->source_addr_l = (__u64)wd_msg->data_in & 0xffffffff;
	msg->source_addr_h = (__u64)wd_msg->data_in >> 32;
	msg->dest_addr_l = (__u64)wd_msg->data_out & 0xffffffff;
	msg->dest_addr_h = (__u64)wd_msg->data_out >> 32;
}

/*
 * Return 0 if gzip header is full loaded.
 * Return -1 if gzip header is only partial loaded.
 */
static int hizip_parse_gzip_header(struct wd_msg *msg,
				   struct hisi_sched *hpriv,
				   struct wd_comp_arg *arg)
{
	size_t templen;

	if (hpriv->store_len < GZIP_HEADER_SZ) {
		if (arg->src_len + hpriv->store_len < GZIP_HEADER_SZ)
			templen = arg->src_len;
		else	/* hpriv->store_len < GZIP_HEADER_SZ */
			templen = GZIP_HEADER_SZ - hpriv->store_len;
		LOAD_SRC_TO_MSG(msg->data_in + hpriv->store_len,
				arg->src, hpriv->si, templen);
		hpriv->store_len += templen;
		arg->src_len -= templen;
		if ((hpriv->store_len >= GZIP_HEADER_SZ) &&
		    (*((char *)msg->data_in + 3) != 0x04)) {
			/* remove gzip header from src buffer */
			hpriv->store_len -= GZIP_HEADER_SZ;
			hpriv->load_head = 1;
			return 0;
		}
		if (!arg->src_len)
			return -1;
	}
	if (hpriv->store_len < GZIP_HEADER_SZ + GZIP_EXTRA_SZ) {
		if (arg->src_len + hpriv->store_len <=
		    GZIP_HEADER_SZ + GZIP_EXTRA_SZ) {
			templen = arg->src_len;
		} else {
			/* hpriv->store_len < GZIP_HEADER_SZ + GZIP_EXTRA_SZ */
			templen = GZIP_HEADER_SZ + GZIP_EXTRA_SZ -
				  hpriv->store_len;
		}
		LOAD_SRC_TO_MSG(msg->data_in + hpriv->store_len,
				arg->src, hpriv->si, templen);
		hpriv->store_len += templen;
		arg->src_len -= templen;
		if (hpriv->store_len >= GZIP_HEADER_SZ + GZIP_EXTRA_SZ){
			/* real_len should compare to src_len */
			memcpy(&hpriv->real_len, msg->data_in + 6, 4);
			/* remove gzip and extra header from src buffer */
			hpriv->store_len -= GZIP_HEADER_SZ + GZIP_EXTRA_SZ;
			hpriv->load_head = 1;
			return 0;
		}
		if (!arg->src_len)
			return -1;
	}
	return 0;
}

static int hizip_sched_input(struct wd_msg *msg, void *priv)
{
	size_t templen;
	struct hisi_zip_sqe	*m = msg->msg;
	struct hisi_sched	*hpriv = (struct hisi_sched *)priv;
	struct wd_comp_arg	*arg = hpriv->arg;
	int ret;

	if (!hpriv->load_head && (hpriv->op_type == INFLATE)) {
		/* check whether compressed head is passed */
		if (hpriv->alg_type == ZLIB) {
			if (arg->src_len + hpriv->store_len <= ZLIB_HEADER_SZ)
				templen = arg->src_len;
			else	/* hpriv->store_len < ZLIB_HEADER_SZ */
				templen = ZLIB_HEADER_SZ - hpriv->store_len;
			LOAD_SRC_TO_MSG(msg->data_in + hpriv->store_len,
					arg->src, hpriv->si, templen);
			hpriv->store_len += templen;
			arg->src_len -= templen;
			if (hpriv->store_len >= ZLIB_HEADER_SZ) {
				/* remove zlib header from src buffer */
				hpriv->store_len -= ZLIB_HEADER_SZ;
				hpriv->load_head = 1;
			}
		} else {
			ret = hizip_parse_gzip_header(msg, hpriv, arg);
			if (ret < 0)
				return 0;
		}
		if (!arg->src_len)
			return 0;
	}
	/* update the real size for gzip */
	if ((hpriv->op_type == INFLATE) && (hpriv->alg_type == GZIP) &&
	    (arg->src_len > hpriv->real_len))
		arg->src_len = hpriv->real_len;
	if (hpriv->store_len + arg->src_len > BLOCK_SIZE) {
		/* load partial data from arg->src */
		templen = BLOCK_SIZE - hpriv->store_len;
		LOAD_SRC_TO_MSG(msg->data_in + hpriv->store_len,
				arg->src, hpriv->si, templen);
		hpriv->store_len += templen;
		if (hpriv->alg_type == GZIP)
			hpriv->real_len -= templen;
		arg->src_len -= templen;
	} else {
		LOAD_SRC_TO_MSG(msg->data_in + hpriv->store_len,
				arg->src, hpriv->si, arg->src_len);
		hpriv->store_len += arg->src_len;
		if (hpriv->alg_type == GZIP)
			hpriv->real_len -= arg->src_len;
		arg->src_len = 0;
	}
	if (arg->flag & FLAG_INPUT_FINISH) {
		m->input_data_length = hpriv->store_len;
		hpriv->store_len = 0;
	} else if (hpriv->store_len == BLOCK_SIZE) {
		m->input_data_length = BLOCK_SIZE;
		hpriv->store_len = 0;
	}

	return 0;
}

static int hizip_sched_output(struct wd_msg *msg, void *priv)
{
	struct hisi_zip_sqe *m = msg->msg;
	__u32 status = m->dw3 & 0xff;
	__u32 type = m->dw9 & 0xff;
	char gzip_extra[GZIP_EXTRA_SZ] = {0x08, 0x00, 0x48, 0x69, 0x04, 0x00,
					  0x00, 0x00, 0x00, 0x00};
	struct hisi_sched	*hpriv = (struct hisi_sched *)priv;

	if (status && (status != 0x0d)) {
		WD_ERR("bad status (s=%d, t=%d)\n", status, type);
		return -EFAULT;
	}
	if (hpriv->op_type == DEFLATE) {
		if (hpriv->alg_type == ZLIB) {
			STORE_MSG_TO_DST(hpriv->arg->dst, hpriv->di,
					 ZLIB_HEADER, ZLIB_HEADER_SZ);
		} else {
			STORE_MSG_TO_DST(hpriv->arg->dst, hpriv->di,
					 GZIP_HEADER, GZIP_HEADER_SZ);
			memcpy(gzip_extra + 6, &m->produced, 4);
			STORE_MSG_TO_DST(hpriv->arg->dst, hpriv->di,
					 gzip_extra, GZIP_EXTRA_SZ);
		}
	}
	if (hpriv->total_out + m->produced > hpriv->arg->dst_len) {
		WD_ERR("output data will overflow\n");
		return -ENOMEM;
	}
	STORE_MSG_TO_DST(hpriv->arg->dst, hpriv->di,
			 msg->data_out, m->produced);
	hpriv->total_out = hpriv->di;
	return 0;
}

static int hisi_comp_block_init(struct wd_comp_sess *sess)
{
	struct hisi_comp_sess	*priv;
	struct wd_scheduler	*sched;
	struct hisi_sched	*sched_priv;
	struct hisi_qm_capa	*capa;
	int	ret;

	priv = (struct hisi_comp_sess *)sess->priv;
	sched = &priv->sched;
	capa = &priv->capa;
	sched->data = capa;
	sched->q_num = 1;
	sched->ss_region_size = 0; /* let system make decision */
	sched->msg_cache_num = CACHE_NUM;
	/* use double size of the input data */
	sched->msg_data_size = BLOCK_SIZE << 1;
	sched->init_cache = hizip_sched_init;
	sched->input = hizip_sched_input;
	sched->output = hizip_sched_output;
	sched->hw_alloc = hisi_qm_alloc_ctx;
	sched->hw_free = hisi_qm_free_ctx;
	sched->hw_send = hisi_qm_send;
	sched->hw_recv = hisi_qm_recv;

	sched->qs = malloc(sizeof(*sched->qs) * sched->q_num);
	if (!sched->qs)
		return -ENOMEM;

	sched_priv = malloc(sizeof(struct hisi_sched));
	if (!sched_priv)
		goto out_priv;
	sched_priv->msgs = malloc(sizeof(struct hisi_zip_sqe) * CACHE_NUM);
	if (!sched_priv->msgs)
		goto out_msg;
	if (!strncmp(sess->alg_name, "zlib", strlen("zlib"))) {
		sched_priv->alg_type = ZLIB;
		sched_priv->dw9 = 2;
		capa->alg = "zlib";
	} else {	// gzip
		sched_priv->alg_type = GZIP;
		sched_priv->dw9 = 3;
		capa->alg = "gzip";
	}
	sched_priv->si = 0;
	sched_priv->di = 0;
	sched_priv->total_out = 0;
	sched_priv->store_len = 0;
	sched_priv->load_head = 0;
	sched_priv->real_len = 0;
	sched->priv = sched_priv;
	ret = wd_sched_init(sched, sess->node_path);
	if (ret < 0)
		goto out_sched;
	return 0;
out_sched:
	free(sched_priv->msgs);
out_msg:
	free(sched_priv);
out_priv:
	free(sched->qs);
	return ret;
}

static int hisi_comp_block_prep(struct wd_comp_sess *sess,
				 struct wd_comp_arg *arg)
{
	struct hisi_comp_sess	*priv;
	struct wd_scheduler	*sched;
	struct hisi_sched	*sched_priv;
	struct hisi_qm_capa	*capa;
	struct hisi_qm_priv	*qm_priv;
	int	i, j, ret = -EINVAL;

	priv = (struct hisi_comp_sess *)sess->priv;
	sched = &priv->sched;
	sched_priv = sched->priv;
	capa = &priv->capa;
	sched->data = capa;

	sched_priv->op_type = (arg->flag & FLAG_DEFLATE) ? DEFLATE: INFLATE;
	sched_priv->arg = arg;

	qm_priv = (struct hisi_qm_priv *)&priv->capa.priv;
	qm_priv->sqe_size = sizeof(struct hisi_zip_sqe);
	qm_priv->op_type = sched_priv->op_type;
	for (i = 0; i < sched->q_num; i++) {
		ret = sched->hw_alloc(&sched->qs[i], sched->data);
		if (ret)
			goto out_hw;
		ret = wd_start_ctx(&sched->qs[i]);
		if (ret)
			goto out_start;
	}
	return ret;
out_start:
	sched->hw_free(&sched->qs[i]);
out_hw:
	for (j = i - 1; j >= 0; j--) {
		wd_stop_ctx(&sched->qs[j]);
		sched->hw_free(&sched->qs[j]);
	}
	return ret;
}

static void hisi_comp_block_fini(struct wd_comp_sess *sess)
{
}

static void hisi_comp_block_exit(struct wd_comp_sess *sess)
{
	struct hisi_comp_sess	*priv;
	struct wd_scheduler	*sched;
	struct hisi_sched	*sched_priv;

	priv = (struct hisi_comp_sess *)sess->priv;
	sched = &priv->sched;
	wd_sched_fini(sched);
	sched_priv = sched->priv;
	free(sched_priv->msgs);
	free(sched_priv);
	free(sched->qs);
}

static int hisi_comp_block_deflate(struct wd_comp_sess *sess,
				    struct wd_comp_arg *arg)
{
	struct hisi_comp_sess	*priv;
	struct wd_scheduler	*sched;
	struct hisi_sched	*sched_priv;
	int	ret;

	priv = (struct hisi_comp_sess *)sess->priv;
	sched = &priv->sched;
	sched_priv = sched->priv;
	/* ZLIB engine can do only one time with buffer less than 16M */
	if (sched_priv->alg_type == ZLIB) {
		if (arg->src_len > BLOCK_SIZE) {
			WD_ERR("zlib total_len(%ld) > BLOCK_SIZE(%d)\n",
				arg->src_len, BLOCK_SIZE);
			return -EINVAL;
		}
		if (BLOCK_SIZE > 16 << 20) {
			WD_ERR("BLOCK_SIZE(%ld) > HW limitation (16MB)\n",
				arg->src_len);
			return -EINVAL;
		}
	}

	while (arg->src_len || !wd_sched_empty(sched)) {
		ret = wd_sched_work(sched, arg->src_len);
		if (ret < 0) {
			WD_ERR("fail to deflate by wd_sched (%d)\n", ret);
			return ret;
		}
	}
	arg->dst_len = sched_priv->total_out;
	return 0;
}

static int hisi_comp_block_inflate(struct wd_comp_sess *sess,
				    struct wd_comp_arg *arg)
{
	struct hisi_comp_sess	*priv;
	struct wd_scheduler	*sched;
	struct hisi_sched	*sched_priv;
	int	ret;

	priv = (struct hisi_comp_sess *)sess->priv;
	sched = &priv->sched;
	sched_priv = sched->priv;
	/* ZLIB engine can do only one time with buffer less than 16M */
	if (sched_priv->alg_type == ZLIB) {
		if (arg->src_len > BLOCK_SIZE) {
			WD_ERR("zlib total_len(%ld) > BLOCK_SIZE(%d)\n",
				arg->src_len, BLOCK_SIZE);
			return -EINVAL;
		}
		if (BLOCK_SIZE > 16 << 20) {
			WD_ERR("BLOCK_SIZE(%ld) > HW limitation (16MB)\n",
				arg->src_len);
			return -EINVAL;
		}
	}

	while (arg->src_len || !wd_sched_empty(sched)) {
		ret = wd_sched_work(sched, arg->src_len);
		if (ret < 0) {
			WD_ERR("fail to inflate by wd_sched (%d)\n", ret);
			return ret;
		}
	}
	arg->dst_len = sched_priv->total_out;
	return 0;
}

static int hisi_comp_strm_init(struct wd_comp_sess *sess)
{
	struct hisi_comp_sess	*priv = (struct hisi_comp_sess *)sess->priv;
	struct hisi_strm_info	*strm = &priv->strm;
	int	ret;

	if (strm->alg_type == ZLIB) {
		priv->capa.alg = "zlib";
		strm->alg_type = ZLIB;
		strm->dw9 = 2;
	} else {
		priv->capa.alg = "gzip";
		strm->alg_type = GZIP;
		strm->dw9 = 3;
	}
	ret = wd_request_ctx(&priv->ctx, sess->node_path);
	return ret;
}

static int hisi_comp_strm_prep(struct wd_comp_sess *sess,
			       struct wd_comp_arg *arg)
{
	struct hisi_comp_sess	*priv = (struct hisi_comp_sess *)sess->priv;
	struct hisi_strm_info	*strm = &priv->strm;
	struct hisi_qm_priv	*qm_priv;
	int	ret;

	qm_priv = (struct hisi_qm_priv *)&priv->capa.priv;
	qm_priv->sqe_size = sizeof(struct hisi_zip_sqe);
	qm_priv->op_type = (arg->flag & FLAG_DEFLATE) ? DEFLATE: INFLATE;
	ret = hisi_qm_alloc_ctx(&priv->ctx, &priv->capa);
	if (ret)
		goto out;
	ret = wd_start_ctx(&priv->ctx);
	if (ret)
		goto out_ctx;
	strm->si = 0;
	strm->di = 0;
	strm->op_type = qm_priv->op_type;
	if (wd_is_nosva(&priv->ctx)) {
		strm->ss_region_size = 4096 + ASIZE * 2 + HW_CTX_SIZE;
		strm->ss_region = wd_reserve_mem(&priv->ctx,
					sizeof(char) * strm->ss_region_size);
		if (!strm->ss_region) {
			WD_ERR("fail to allocate memory for SS region\n");
			ret = -ENOMEM;
			goto out_ss;
		}
		ret = smm_init(strm->ss_region, strm->ss_region_size, 0xF);
		if (ret)
			goto out_smm;
		ret = -ENOMEM;
		strm->next_in = smm_alloc(strm->ss_region, ASIZE);
		if (!strm->next_in)
			goto out_smm;
		strm->next_out = smm_alloc(strm->ss_region, ASIZE);
		if (!strm->next_out)
			goto out_smm_out;
		strm->ctx_buf = smm_alloc(strm->ss_region, HW_CTX_SIZE);
		if (!strm->ctx_buf)
			goto out_smm_ctx;
	} else {
		strm->next_in = malloc(ASIZE);
		if (!strm->next_in)
			goto out_in;
		strm->next_out = malloc(ASIZE);
		if (!strm->next_out)
			goto out_out;
		strm->ctx_buf = malloc(HW_CTX_SIZE);
		if (!strm->ctx_buf)
			goto out_buf;
	}
	return 0;
out_smm_ctx:
	smm_free(strm->ss_region, strm->next_out);
out_smm_out:
	smm_free(strm->ss_region, strm->next_in);
out_smm:
	free(strm->ss_region);
out_ss:
	wd_stop_ctx(&priv->ctx);
out_ctx:
	hisi_qm_free_ctx(&priv->ctx);
out:
	return ret;
out_buf:
	free(strm->next_out);
out_out:
	free(strm->next_in);
out_in:
	wd_stop_ctx(&priv->ctx);
	hisi_qm_free_ctx(&priv->ctx);
	return -ENOMEM;
}

static void hisi_comp_strm_fini(struct wd_comp_sess *sess)
{
	struct hisi_comp_sess	*priv;
	struct hisi_strm_info	*strm;

	priv = (struct hisi_comp_sess *)sess->priv;
	strm = &priv->strm;
	if (wd_is_nosva(&priv->ctx)) {
		smm_free(strm->ss_region, strm->next_in);
		smm_free(strm->ss_region, strm->next_out);
		smm_free(strm->ss_region, strm->ctx_buf);
		free(strm->ss_region);
	} else {
		free(strm->next_in);
		free(strm->next_out);
		free(strm->ctx_buf);
	}
	wd_stop_ctx(&priv->ctx);
	hisi_qm_free_ctx(&priv->ctx);
}

static unsigned int bit_reverse(register unsigned int x)
{
	x = (((x & 0xaaaaaaaa) >> 1) | ((x & 0x55555555) << 1));
	x = (((x & 0xcccccccc) >> 2) | ((x & 0x33333333) << 2));
	x = (((x & 0xf0f0f0f0) >> 4) | ((x & 0x0f0f0f0f) << 4));
	x = (((x & 0xff00ff00) >> 8) | ((x & 0x00ff00ff) << 8));

	return((x >> 16) | (x << 16));
}

static int append_store_block(struct hisi_strm_info *strm, int flush)
{
	char	store_block[5] = {0x1, 0x00, 0x00, 0xff, 0xff};
	uint32_t	 checksum = strm->checksum;
	uint32_t	 isize = strm->isize;

	memcpy(strm->next_out, store_block, 5);
	strm->total_out += 5;
	strm->avail_out -= 5;
	if (flush != WD_FINISH)
		return Z_STREAM_END;

	if (strm->alg_type == HW_ZLIB) { /*if zlib, ADLER32*/
		checksum = (uint32_t) cpu_to_be32(checksum);
		memcpy(strm->next_out + 5, &checksum, 4);
		strm->total_out += 4;
		strm->avail_out -= 4;
	} else if (strm->alg_type == HW_GZIP) {  /*if gzip, CRC32 and ISIZE*/
		checksum = ~checksum;
		checksum = bit_reverse(checksum);
		memcpy(strm->next_out + 5, &checksum, 4);
		memcpy(strm->next_out + 9, &isize, 4);
		strm->total_out += 8;
		strm->avail_out -= 8;
	} else
		WD_ERR("in append store block, wrong alg type %d.\n",
			strm->alg_type);

	return Z_STREAM_END;
}

static int hisi_strm_comm(struct wd_comp_sess *sess, int flush)
{
	struct hisi_comp_sess	*priv;
	struct hisi_zip_sqe	*msg, *recv_msg;
	struct hisi_strm_info	*strm;
	uint32_t	status, type;
	uint64_t	stream_mode, stream_new, flush_type;
	int	ret = 0;

	priv = (struct hisi_comp_sess *)sess->priv;
	strm = &priv->strm;
	if (strm->avail_in == 0)
		return append_store_block(strm, flush);
	msg = malloc(sizeof(*msg));
	if (!msg) {
		WD_ERR("alloc msg fail!\n");
		return -EINVAL;
	}

	stream_mode = STATEFUL;
	stream_new = strm->stream_pos;
	flush_type = (flush == WD_FINISH) ? HZ_FINISH : HZ_SYNC_FLUSH;

	memset((void *)msg, 0, sizeof(*msg));
	msg->dw9 = strm->dw9;
	msg->dw7 |= ((stream_new << 2 | stream_mode << 1 | flush_type)) <<
		    STREAM_FLUSH_SHIFT;
	msg->source_addr_l = (uint64_t)strm->next_in & 0xffffffff;
	msg->source_addr_h = (uint64_t)strm->next_in >> 32;
	msg->dest_addr_l = (uint64_t)strm->next_out & 0xffffffff;
	msg->dest_addr_h = (uint64_t)strm->next_out >> 32;
	msg->input_data_length = strm->avail_in;
	msg->dest_avail_out = strm->avail_out;

	if (strm->op_type == INFLATE) {
		msg->stream_ctx_addr_l = (uint64_t)strm->ctx_buf & 0xffffffff;
		msg->stream_ctx_addr_h = (uint64_t)strm->ctx_buf >> 32;
	}
	msg->ctx_dw0 = strm->ctx_dw0;
	msg->ctx_dw1 = strm->ctx_dw1;
	msg->ctx_dw2 = strm->ctx_dw2;
	msg->isize = strm->isize;
	msg->checksum = strm->checksum;
	if (strm->stream_pos == STREAM_NEW) {
		strm->stream_pos = STREAM_OLD;
		strm->total_out = 0;
	}

	ret = hisi_qm_send(&priv->ctx, msg);
	if (ret == -EBUSY) {
		usleep(1);
		goto recv_again;
	}
	if (ret) {
		WD_ERR("send failure (%d)\n", ret);
		goto out;
	}

recv_again:
	ret = hisi_qm_recv(&priv->ctx, (void **)&recv_msg);
	if (ret == -EIO) {
		fputs(" wd_recv fail!\n", stderr);
		goto out_recv;
	/* synchronous mode, if get none, then get again */
	} else if (ret == -EAGAIN)
		goto recv_again;
	status = recv_msg->dw3 & 0xff;
	type = recv_msg->dw9 & 0xff;
	if (status && (status != 0x0d) && (status != 0x13)) {
		WD_ERR("bad status (s=%d, t=%d)\n", status, type);
		goto out_recv;
	}
	strm->avail_out -= recv_msg->produced;
	strm->total_out += recv_msg->produced;
	strm->avail_in -= recv_msg->consumed;
	strm->ctx_dw0 = recv_msg->ctx_dw0;
	strm->ctx_dw1 = recv_msg->ctx_dw1;
	strm->ctx_dw2 = recv_msg->ctx_dw2;
	strm->isize = recv_msg->isize;
	strm->checksum = recv_msg->checksum;
	if (strm->avail_out == 0)
		strm->next_in +=  recv_msg->consumed;
	if (strm->avail_out > 0) {
		strm->avail_in = 0;
	}

	if (ret == 0 && flush == WD_FINISH)
		ret = Z_STREAM_END;
	else if (ret == 0 &&  (recv_msg->dw3 & 0x1ff) == 0x113)
		ret = Z_STREAM_END;    /* decomp_is_end  region */
out_recv:
	free(msg);
out:
	return ret;
}

static int hisi_comp_strm_deflate(struct wd_comp_sess *sess,
				  struct wd_comp_arg *arg)
{
	struct hisi_comp_sess	*priv;
	struct hisi_strm_info	*strm;
	size_t	len, have;
	int	ret, flush;
	const char	zip_head[2] = {0x78, 0x9c};
	const char	gzip_head[10] = {0x1f, 0x8b, 0x08, 0x0,
					 0x0, 0x0, 0x0, 0x0, 0x0, 0x03};

	priv = (struct hisi_comp_sess *)sess->priv;
	strm = &priv->strm;
	strm->arg = arg;
	strm->op_type = DEFLATE;
	if (strm->alg_type == ZLIB)
		STORE_MSG_TO_DST(arg->dst, strm->di, &zip_head, 2);
	else
		STORE_MSG_TO_DST(arg->dst, strm->di, &gzip_head, 10);

	strm->stream_pos = STREAM_NEW;
	do {
		if (STREAM_CHUNK > arg->src_len)
			len = arg->src_len;
		else
			len = STREAM_CHUNK;
		LOAD_SRC_TO_MSG(strm->next_in, arg->src, strm->si, len);
		arg->src_len -= len;
		strm->avail_in = len;
		if (arg->src_len)
			flush = WD_SYNC_FLUSH;
		else
			flush = WD_FINISH;

		do {
			strm->avail_out = STREAM_CHUNK_OUT;
			ret = hisi_strm_comm(sess, flush);
			if (ret < 0)
				goto out;
			have = STREAM_CHUNK_OUT - strm->avail_out;
			STORE_MSG_TO_DST(arg->dst, strm->di,
					 strm->next_out, have);
			arg->dst_len = strm->di;
		} while (strm->avail_out == 0);

		/* done when last data in file processed */
	} while (flush != WD_FINISH);
	return 0;
out:
	return ret;
}

static int hisi_comp_strm_inflate(struct wd_comp_sess *sess,
				  struct wd_comp_arg *arg)
{
	struct hisi_comp_sess	*priv;
	struct hisi_strm_info	*strm;
	char	zip_head[2] = {0};
	char	gzip_head[10] = {0};
	size_t	have, len;
	int	ret;

	priv = (struct hisi_comp_sess *)sess->priv;
	strm = &priv->strm;
	strm->arg = arg;
	strm->op_type = INFLATE;
	if (strm->alg_type == ZLIB)
		LOAD_SRC_TO_MSG(&zip_head, arg->src, strm->si, 2);
	else
		LOAD_SRC_TO_MSG(&gzip_head, arg->src, strm->si, 10);
	strm->stream_pos = STREAM_NEW;
	do {
		if (STREAM_CHUNK > arg->src_len)
			len = arg->src_len;
		else
			len = STREAM_CHUNK;
		LOAD_SRC_TO_MSG(strm->next_in, arg->src, strm->si, len);
		arg->src_len -= len;
		strm->avail_in = len;
		if (strm->avail_in == 0)
			break;
		/* finish compression if all of source has been read in */
		do {
			strm->avail_out = STREAM_CHUNK_OUT;
			ret = hisi_strm_comm(sess, WD_SYNC_FLUSH);
			if (ret < 0)
				goto out;
			have = STREAM_CHUNK_OUT - strm->avail_out;
			STORE_MSG_TO_DST(arg->dst, strm->di,
					 strm->next_out, have);
			arg->dst_len = strm->di;
		} while (strm->avail_out == 0);

		/* done when last data in file processed */
	} while (ret != Z_STREAM_END);

	return 0;
out:
	return ret;
}

int hisi_comp_init(struct wd_comp_sess *sess)
{
	struct hisi_comp_sess	*priv;

	priv = calloc(1, sizeof(struct hisi_comp_sess));
	if (!priv)
		return -ENOMEM;
	priv->inited = 0;
	sess->priv = priv;
	if (sess->mode & MODE_STREAM)
		hisi_comp_strm_init(sess);
	else
		hisi_comp_block_init(sess);
	return 0;
}

void hisi_comp_exit(struct wd_comp_sess *sess)
{
	if (sess->drv->fini)
		sess->drv->fini(sess);
	free(sess->priv);
	sess->priv = NULL;
}

int hisi_comp_prep(struct wd_comp_sess *sess, struct wd_comp_arg *arg)
{
	struct hisi_comp_sess	*priv = sess->priv;
	int	ret;

	if (priv->inited)
		return 0;
	if (sess->mode & MODE_STREAM)
		ret = hisi_comp_strm_prep(sess, arg);
	else
		ret = hisi_comp_block_prep(sess, arg);
	if (!ret)
		priv->inited = 1;
	return ret;
}

void hisi_comp_fini(struct wd_comp_sess *sess)
{
	struct hisi_comp_sess	*priv = sess->priv;

	if (priv->inited)
		return;
	if (sess->mode & MODE_STREAM)
		hisi_comp_strm_fini(sess);
	else
		hisi_comp_block_fini(sess);
	priv->inited = 0;
}

int hisi_comp_deflate(struct wd_comp_sess *sess, struct wd_comp_arg *arg)
{
	int	ret;

	if (sess->mode & MODE_STREAM)
		ret = hisi_comp_strm_deflate(sess, arg);
	else
		ret = hisi_comp_block_deflate(sess, arg);
	return ret;
}

int hisi_comp_inflate(struct wd_comp_sess *sess, struct wd_comp_arg *arg)
{
	int	ret;

	if (sess->mode & MODE_STREAM)
		ret = hisi_comp_strm_inflate(sess, arg);
	else
		ret = hisi_comp_block_inflate(sess, arg);
	return ret;
}

int hisi_comp_poll(struct wd_comp_sess *sess, struct wd_comp_arg *arg)
{
	return 0;
}