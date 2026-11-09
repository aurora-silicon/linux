// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Aurora Silicon */

#include <linux/apple-sep-fv.h>
#include <linux/bio.h>
#include <linux/blk-crypto.h>
#include <linux/blkdev.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>

#define FVREAD_BLOCK_SIZE	4096
#define FVREAD_MAX_BLOCKS	16

struct fvread_block_file {
	struct file *file;
	struct block_device *bdev;
};

int fvread_block_open(int fd, void **handle, u64 *size)
{
	struct fvread_block_file *block;
	struct file *file;

	if (!handle || !size)
		return -EINVAL;
	file = fget(fd);
	if (!file)
		return -EBADF;
	if (!S_ISBLK(file_inode(file)->i_mode) ||
	    !(file->f_mode & FMODE_READ) || (file->f_mode & FMODE_WRITE)) {
		fput(file);
		return -EPERM;
	}
	block = kzalloc(sizeof(*block), GFP_KERNEL);
	if (!block) {
		fput(file);
		return -ENOMEM;
	}
	block->file = file;
	block->bdev = file_bdev(file);
	*size = bdev_nr_bytes(block->bdev);
	*handle = block;
	return 0;
}

void fvread_block_close(void *handle)
{
	struct fvread_block_file *block = handle;

	if (!block)
		return;
	fput(block->file);
	kfree(block);
}

ssize_t fvread_block_read_plain(void *handle, u64 offset, void *data, size_t len)
{
	struct fvread_block_file *block = handle;
	loff_t pos;
	ssize_t done;

	if (!block || !data || !len || offset > LLONG_MAX ||
	    len > LLONG_MAX - offset || offset + len > bdev_nr_bytes(block->bdev))
		return -EINVAL;
	pos = offset;
	done = kernel_read(block->file, data, len, &pos);
	if (done < 0)
		return done;
	return done == len ? done : -EIO;
}

static int fvread_read_one(struct fvread_block_file *block, u64 offset,
			   u64 dun, const struct blk_crypto_key *key, void *out)
{
	struct page *page;
	struct bio *bio;
	u64 duns[BLK_CRYPTO_DUN_ARRAY_SIZE] = { dun };
	void *mapped;
	int ret;

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		return -ENOMEM;
	bio = bio_alloc(block->bdev, 1, REQ_OP_READ, GFP_KERNEL);
	if (!bio) {
		__free_page(page);
		return -ENOMEM;
	}
	bio->bi_iter.bi_sector = offset >> SECTOR_SHIFT;
	if (bio_add_page(bio, page, FVREAD_BLOCK_SIZE, 0) != FVREAD_BLOCK_SIZE) {
		ret = -EIO;
		goto out_bio;
	}
	bio_crypt_set_ctx(bio, key, duns, GFP_KERNEL);
	ret = submit_bio_wait(bio);
	if (ret)
		goto out_bio;
	mapped = kmap_local_page(page);
	memcpy(out, mapped, FVREAD_BLOCK_SIZE);
	kunmap_local(mapped);
out_bio:
	bio_put(bio);
	__free_page(page);
	return ret;
}

ssize_t fvread_block_read_crypto(void *handle, u64 offset, u64 dun,
				 const struct apple_sep_fv_key *fvkey,
				 void *data, size_t len)
{
	struct fvread_block_file *block = handle;
	struct blk_crypto_key key;
	size_t done = 0;
	int ret;

	if (!block || !fvkey || !data || !len ||
	    !IS_ALIGNED(offset, FVREAD_BLOCK_SIZE) ||
	    !IS_ALIGNED(len, FVREAD_BLOCK_SIZE) ||
	    len / FVREAD_BLOCK_SIZE > FVREAD_MAX_BLOCKS ||
	    offset > bdev_nr_bytes(block->bdev) ||
	    len > bdev_nr_bytes(block->bdev) - offset)
		return -EINVAL;

	ret = blk_crypto_init_key(&key, fvkey->opaque,
				  APPLE_SEP_FV_OPAQUE_KEY_SIZE,
				  BLK_CRYPTO_KEY_TYPE_HW_WRAPPED,
				  BLK_ENCRYPTION_MODE_AES_256_XTS, 8,
				  FVREAD_BLOCK_SIZE, BLK_CRYPTO_CFG_ALLOW_HW);
	if (ret)
		return ret;
	ret = blk_crypto_start_using_key(block->bdev, &key);
	if (ret)
		goto out_wipe;
	while (done < len) {
		ret = fvread_read_one(block, offset + done,
				      dun + done / FVREAD_BLOCK_SIZE, &key,
				      (u8 *)data + done);
		if (ret)
			break;
		done += FVREAD_BLOCK_SIZE;
	}
	blk_crypto_evict_key(block->bdev, &key);
out_wipe:
	memzero_explicit(&key, sizeof(key));
	return ret ? ret : done;
}
