#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "wd_comp.h"

#define PATHLEN		256

#define FLAG_ZLIB	(1 << 0)
#define FLAG_GZIP	(1 << 1)
#define FLAG_DECMPS	(1 << 2)
#define FLAG_STREAM	(1 << 3)

static int test_compress(char *src, char *dst, int flag)
{
	handler_t		handle;
	struct wd_comp_arg	arg;
	struct stat		st;
	int	sfd, dfd, ret;
	char	algs[60];

	if (!src || !dst)
		return -EINVAL;
	if (flag & FLAG_ZLIB)
		sprintf(algs, "zlib");
	else if (flag & FLAG_GZIP)
		sprintf(algs, "gzip");
	sfd = open(src, O_RDONLY);
	if (sfd < 0)
		return -EINVAL;
	dfd = open(dst, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
	if (dfd < 0) {
		ret = -EINVAL;
		goto out;
	}
	memset(&arg, 0, sizeof(struct wd_comp_arg));
	fstat(sfd, &st);
	arg.src_len = st.st_size;
	arg.src = malloc(sizeof(char) * arg.src_len);
	if (!arg.src) {
		ret = -ENOMEM;
		goto out_src;
	}
	arg.dst_len = arg.src_len << 2;	// for decompress
	arg.dst = malloc(sizeof(char) * arg.dst_len);
	if (!arg.dst) {
		ret = -ENOMEM;
		goto out_dst;
	}
	ret = read(sfd, arg.src, arg.src_len);
	if ((ret < 0) || (ret != arg.src_len)) {
		fprintf(stderr, "fail to load data (%d)\n", ret);
		goto out_read;
	}
	arg.flag |= FLAG_INPUT_FINISH;
	handle = wd_alg_comp_alloc_sess(algs, 0, NULL);
	if (flag & FLAG_DECMPS) {
		ret = wd_alg_decompress(handle, &arg);
		if (ret) {
			fprintf(stderr, "fail to decompress (%d)\n", ret);
		}
	} else {
		ret = wd_alg_compress(handle, &arg);
		if (ret) {
			fprintf(stderr, "fail to compress (%d)\n", ret);
		}
	}
	wd_alg_comp_free_sess(handle);
	ret = write(dfd, arg.dst, arg.dst_len);
	free(arg.dst);
	free(arg.src);
	close(dfd);
	close(sfd);
	return 0;
out_read:
	free(arg.dst);
out_dst:
	free(arg.src);
out_src:
	close(dfd);
out:
	close(sfd);
	return ret;
}

int main(int argc, char *argv[])
{
	int	fd, opt;
	int	flag = 0, flag_mask;
	char	src[PATHLEN+1], dst[PATHLEN+1];

	while ((opt = getopt(argc, argv, "i:o:hdgsz")) != -1) {
		switch (opt) {
		case 'i':
			snprintf(src, PATHLEN, "%s", optarg);
			break;
		case 'o':
			snprintf(dst, PATHLEN, "%s", optarg);
			break;
		case 'd':
			flag |= FLAG_DECMPS;
			break;
		case 'g':
			flag |= FLAG_GZIP;
			break;
		case 's':
			flag |= FLAG_STREAM;
			break;
		case 'z':
			flag |= FLAG_ZLIB;
			break;
		case 'h':
			printf("./test_comp -i <src> -o <dst> [-d] [-z] [-g]\n");
			break;
		default:
			fprintf(stderr, "Unrecognized option!\n");
			break;
		}
	}
	fd = open("/dev/hisi_zip-0", O_RDWR);
	if (fd < 0) {
		printf("failed to open dev node:%d\n", errno);
		return fd;
	}
	flag_mask = FLAG_GZIP | FLAG_ZLIB;
	if (!flag || ((flag & flag_mask) == flag_mask)) {
		printf("wrong flag setting:0x%x\n", flag);
		return -EINVAL;
	}

	test_compress(src, dst, flag);
	close(fd);
	return 0;
}