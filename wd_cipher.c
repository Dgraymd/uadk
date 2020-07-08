/* SPDX-License-Identifier: Apache-2.0 */
#include "hisi_sec.h"
#include "wd_cipher.h"

#define XTS_MODE_KEY_DIVISOR 2
#define SM4_KEY_SIZE         16
#define DES_KEY_SIZE	     8
#define DES3_3KEY_SIZE	     (3 * DES_KEY_SIZE)
#define MAX_CIPHER_KEY_SIZE  64

struct wd_alg_cipher {
	char	*drv_name;
	char	*alg_name;
	int	(*init)(struct wd_cipher_sess *sess);
	void	(*exit)(struct wd_cipher_sess *sess);
	int	(*prep)(struct wd_cipher_sess *sess,
			struct wd_cipher_arg *arg);
	void	(*fini)(struct wd_cipher_sess *sess);
	int	(*set_key)(struct wd_cipher_sess *sess, const __u8 *key,
			   __u32 key_len);
	int	(*encrypt)(struct wd_cipher_sess *sess,
			   struct wd_cipher_arg *arg);
	int	(*decrypt)(struct wd_cipher_sess *sess,
			   struct wd_cipher_arg *arg);
	int	(*async_poll)(struct wd_cipher_sess *sess, __u32 count);
}

wd_alg_cipher_list[] = {
	{
		.drv_name	= "hisi_sec2",
		.alg_name	= "cipher",
		.init		= hisi_cipher_init,
		.exit		= hisi_cipher_exit,
		.prep		= hisi_cipher_prep,
		.fini		= hisi_cipher_fini,
		.set_key	= hisi_cipher_set_key,
		.encrypt	= hisi_cipher_encrypt,
		.decrypt	= hisi_cipher_decrypt,
		.async_poll	= hisi_cipher_poll,
	},
};

handle_t wd_alg_cipher_alloc_sess(struct wd_cipher_sess_setup *setup,
				  wd_dev_mask_t *dev_mask)
{
	struct uacce_dev_list	*head = NULL, *p, *prev;
	wd_dev_mask_t		*mask = NULL;
	struct wd_cipher_sess	*sess = NULL;
	int	i, found, max = 0, ret;
	char	*dev_name;

	if (!setup->alg_name)
		return 0;
	if (setup->alg < WD_CIPHER_ALG_TYPE_NONE ||
	    setup->alg >= WD_CIPHER_ALG_TYPE_MAX) {
		WD_ERR("setup cipher alg err.\n");
		return -EINVAL;
	}
	if (setup->mode < WD_CIPHER_MODE_TYPE_NONE ||
	    setup->mode >= WD_CIPHER_MODE_TYPE_MAX) {
		WD_ERR("setup cipher mode err.\n");
		return -EINVAL;
	}

	mask = calloc(1, sizeof(wd_dev_mask_t));
	if (!mask)
		return (handle_t)sess;
	head = wd_list_accels(mask);
	if (!head) {
		WD_ERR("Failed to get any accelerators in system!\n");
		return (handle_t)sess;
	}
	/* merge two masks */
	if (dev_mask && (dev_mask->magic == WD_DEV_MASK_MAGIC) &&
	    dev_mask->len && (dev_mask->len <= mask->len)) {
		for (i = 0; i < mask->len; i++)
			mask->mask[i] &= dev_mask->mask[i];
	}
	for (p = head, prev = NULL; p; ) {
		if (!is_accel_avail(mask, p->info->node_id)) {
			RM_NODE(head, prev, p);
			continue;
		}
		found = match_alg_name(p->info->algs, setup->alg_name);
		if (found) {
			if (p->info->avail_instn <= max) {
				prev = p;
				p = p->next;
				continue;
			}
			/* move to head */
			max = p->info->avail_instn;
			if (p == head) {
				prev = p;
				p = p->next;
			} else {
				prev->next = p->next;
				p->next = head;
				head = p;
				p = prev->next;
			}
		} else {
			wd_clear_mask(mask, p->info->node_id);
			RM_NODE(head, prev, p);
		}
	}
	for (p = head, i = 0; p; p = p->next) {
		/* mount driver */
		dev_name = wd_get_accel_name(p->info->dev_root, 1);
		found = 0;
		for (i = 0; i < ARRAY_SIZE(wd_alg_cipher_list); i++) {
			if (!strncmp(dev_name, wd_alg_cipher_list[i].drv_name,
				     strlen(dev_name))) {
				found = 1;
				break;
			}
		}
		free(dev_name);
		if (found)
			break;
	}
	if (!found)
		goto out;
	sess = calloc(1, (sizeof(struct wd_cipher_sess)));
	if (!sess)
		goto out;

	sess->alg = setup->alg;
	sess->mode = setup->mode;
	sess->key = malloc(MAX_CIPHER_KEY_SIZE);
	if (sess->key) {
		WD_ERR("alloc cipher sess key fail!\n");
		free(sess);
		goto out;
	}
	sess->alg_name = strdup(setup->alg_name);
	dev_name = wd_get_accel_name(p->info->dev_root, 0);
	snprintf(sess->node_path, MAX_DEV_NAME_LEN, "/dev/%s", dev_name);
	free(dev_name);
	sess->dev_mask = mask;
	sess->drv = &wd_alg_cipher_list[i];
	if (sess->drv->init) {
		ret = sess->drv->init(sess);
		if (ret)
			WD_ERR("fail to init session (%d)\n", ret);
	}
out:
	while (head) {
		p = head;
		head = head->next;
		free(p->info);
		free(p);
	}
	return (handle_t)sess;
}

void wd_alg_cipher_free_sess(handle_t handle)
{
	struct wd_cipher_sess *sess = (struct wd_cipher_sess *)handle;

	if (!sess)
		return;

	if (sess->drv->exit)
		sess->drv->exit(sess);

	if (sess->dev_mask->mask)
		free(sess->dev_mask->mask);

	if (sess->dev_mask)
		free(sess->dev_mask);

	free(sess->alg_name);
	free(sess);
}

int wd_alg_do_cipher(handle_t handle, struct wd_cipher_arg *arg)
{
	struct wd_cipher_sess *sess = (struct wd_cipher_sess *)handle;

	return 0;
}

int wd_alg_encrypt(handle_t handle, struct wd_cipher_arg *arg)
{
	struct wd_cipher_sess *sess = (struct wd_cipher_sess *)handle;

	if (!arg || !sess->drv->encrypt)
		return -EINVAL;

	return sess->drv->encrypt(sess, arg);
}

int wd_alg_decrypt(handle_t handle, struct wd_cipher_arg *arg)
{
	struct wd_cipher_sess *sess = (struct wd_cipher_sess *)handle;

	if (!arg || !sess->drv->decrypt)
		return -EINVAL;

	return sess->drv->decrypt(sess, arg);
}

static int is_des_weak_key(const __u64 *key, __u16 keylen)
{
	return 0;
}

static int aes_key_len_check(__u16 length)
{
	switch (length) {
		case AES_KEYSIZE_128:
		case AES_KEYSIZE_192:
		case AES_KEYSIZE_256:
			return 0;
		default:
			return -EINVAL;
	}
}

static int cipher_key_len_check(enum wd_cipher_alg alg, __u16 length)
{
	int ret = 0;

	switch (alg) {
	case WD_CIPHER_SM4:
		if (length != SM4_KEY_SIZE)
			ret = -EINVAL;
		break;
	case WD_CIPHER_AES:
		ret = aes_key_len_check(length);
		break;
	case WD_CIPHER_DES:
		if (length != DES_KEY_SIZE)
			ret = -EINVAL;
		break;
	case WD_CIPHER_3DES:
		if (length != DES3_3KEY_SIZE)
			ret = -EINVAL;
		break;
	default:
		WD_ERR("%s: input alg err!\n", __func__);
		return -EINVAL;
	}

	return ret;
}

int wd_alg_set_key(handle_t handle, __u8 *key, __u32 key_len)
{
	struct wd_cipher_sess *sess = (struct wd_cipher_sess *)handle;
	__u16 length = key_len;
	int ret;

	if (!key || !sess) {
		WD_ERR("%s inpupt param err!\n", __func__);
		return -EINVAL;
	}

	/* fix me: need check key_len */
	if (sess->mode == WD_CIPHER_XTS)
		length = key_len / XTS_MODE_KEY_DIVISOR;

	ret = cipher_key_len_check(sess->alg, length);
	if (ret) {
		WD_ERR("%s inpupt key length err!\n", __func__);
		return -EINVAL;
	}
	if (sess->mode == WD_CIPHER_DES && is_des_weak_key(key, length)) {
		WD_ERR("%s: input des key is weak key!\n", __func__);
		return -EINVAL;
	}

	return sess->drv->set_key(sess, key, length);
}

int wd_alg_cipher_poll(handle_t handle, __u32 count)
{
	struct wd_cipher_sess *sess = (struct wd_cipher_sess *)handle;

	return sess->drv->async_poll(sess, count);;
}
