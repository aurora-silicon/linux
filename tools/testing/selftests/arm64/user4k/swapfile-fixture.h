/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef USER4K_SWAPFILE_FIXTURE_H
#define USER4K_SWAPFILE_FIXTURE_H

static const char *swap_path(void)
{
	return access("/swapdir/swapfile", F_OK) ? "/dev/vda" :
						   "/swapdir/swapfile";
}

/* Remove every fourth native page: every 64K group crosses file extents. */
static int prepare_swapfile(void)
{
	const unsigned long page = getauxval(AT_PAGESZ), initial = 32UL << 20;
	unsigned char *buf = calloc(1, page);
	unsigned long offset;
	int fd = -1, ret = -1;

	if (!buf || (mkdir("/swapdir", 0755) && errno != EEXIST) ||
	    mount("/dev/vda", "/swapdir", "ext4", 0, NULL))
		goto out;
	fd = open("/swapdir/swapfile", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC,
		  0600);
	if (fd < 0)
		goto out;
	memset(buf, 0xa5, page);
	for (offset = 0; offset < initial; offset += page)
		if (write(fd, buf, page) != (ssize_t)page)
			goto out;
	if (fsync(fd))
		goto out;
	for (offset = initial; offset; offset -= 4 * page)
		if (fallocate(fd, FALLOC_FL_COLLAPSE_RANGE, offset - 2 * page,
			      page))
			goto out;
	memset(buf, 0, page);
	((unsigned int *)(buf + 1024))[0] = 1;
	((unsigned int *)(buf + 1024))[1] = (initial * 3 / 4) / page - 1;
	memcpy(buf + page - 10, "SWAPSPACE2", 10);
	if (pwrite(fd, buf, page, 0) != (ssize_t)page || fsync(fd))
		goto out;
	ret = 0;
out:
	if (fd >= 0)
		close(fd);
	free(buf);
	return ret;
}

#endif
