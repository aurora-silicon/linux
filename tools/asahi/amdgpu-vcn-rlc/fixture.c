// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef int64_t loff_t;

#define __iomem
#define __user
#define GFP_KERNEL 0
#define AMDGPU_VCNFW_LOG_SIZE 256U
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define dev_err(dev, fmt, ...) ((void)(dev))

struct amdgpu_vcn_fwlog {
	u32 header_size;
	u32 rptr;
	u32 wptr;
	u32 reserved;
};

struct amdgpu_bo_res {
	struct {
		bool is_nonposted;
	} bus;
};

struct amdgpu_bo {
	struct {
		struct amdgpu_bo_res *resource;
	} tbo;
};

struct amdgpu_vcn_inst {
	struct {
		u8 *cpu_addr;
		size_t mem_size;
	} fw_shared;
	struct amdgpu_bo *vcpu_bo;
};

struct inode {
	void *i_private;
};

struct file {
	struct inode *f_inode;
};

static struct inode *file_inode(struct file *f)
{
	return f->f_inode;
}

struct amdgpu_device;

struct amdgpu_rlc_funcs {
	void (*get_csb_buffer)(struct amdgpu_device *adev, volatile u32 *buffer);
};

struct amdgpu_device {
	void *dev;
	struct {
		struct {
			struct amdgpu_bo *clear_state_obj;
			u32 clear_state_size;
			u32 *cs_ptr;
			const struct amdgpu_rlc_funcs *funcs;
		} rlc;
	} gfx;
};

static int amdgpu_vcnfw_log = 1;
static u8 vram_log[AMDGPU_VCNFW_LOG_SIZE];
static u32 vram_csb[16];
static bool fail_alloc;
static unsigned int io_readl_calls;
static unsigned int io_writel_calls;
static unsigned int io_memcpy_from_calls;
static unsigned int io_memcpy_to_calls;
static unsigned int direct_csb_calls;

static u32 readl(const void __iomem *addr)
{
	u32 v;

	io_readl_calls++;
	memcpy(&v, addr, sizeof(v));
	return v;
}

static void writel(u32 v, void __iomem *addr)
{
	io_writel_calls++;
	memcpy(addr, &v, sizeof(v));
}

static void *kvmalloc(size_t sz, int gfp)
{
	(void)gfp;
	assert(sz <= AMDGPU_VCNFW_LOG_SIZE);
	if (fail_alloc)
		return NULL;
	return malloc(sz);
}

static void *kvzalloc(size_t sz, int gfp)
{
	(void)gfp;
	if (fail_alloc)
		return NULL;
	return calloc(1, sz);
}

static void kvfree(const void *p)
{
	free((void *)p);
}

static unsigned long copy_to_user(void *dst, const void *src, size_t len)
{
	memcpy(dst, src, len);
	return 0;
}

static void amdgpu_bo_memcpy_from(struct amdgpu_bo *bo, void *dst,
				  const void *src, size_t len)
{
	assert(bo->tbo.resource->bus.is_nonposted);
	assert((const u8 *)src >= vram_log &&
	       (const u8 *)src + len <= vram_log + sizeof(vram_log));
	io_memcpy_from_calls++;
	memcpy(dst, src, len);
}

static void amdgpu_bo_memcpy_to(struct amdgpu_bo *bo, void *dst,
				const void *src, size_t len)
{
	assert(bo->tbo.resource->bus.is_nonposted);
	assert(dst == vram_csb && len <= sizeof(vram_csb));
	io_memcpy_to_calls++;
	memcpy(dst, src, len);
}

#include "extracted.h"

static void mock_get_csb(struct amdgpu_device *adev, volatile u32 *buf)
{
	u32 i;

	if ((u32 *)buf == vram_csb) {
		assert(!adev->gfx.rlc.clear_state_obj->tbo.resource->bus.is_nonposted);
		direct_csb_calls++;
	}
	for (i = 0; i < adev->gfx.rlc.clear_state_size; i++)
		buf[i] = 0xa5000000U + i;
}

static void test_vcn_fwlog(void)
{
	struct amdgpu_bo_res res = { .bus = { .is_nonposted = true } };
	struct amdgpu_bo bo = { .tbo = { .resource = &res } };
	struct amdgpu_vcn_inst vcn = {
		.fw_shared = { .cpu_addr = vram_log, .mem_size = 0 },
		.vcpu_bo = &bo,
	};
	struct inode ino = { .i_private = &vcn };
	struct file f = { .f_inode = &ino };
	struct amdgpu_vcn_fwlog *hdr = (struct amdgpu_vcn_fwlog *)vram_log;
	u8 out[AMDGPU_VCNFW_LOG_SIZE];
	loff_t pos = 0;
	ssize_t n;

	/* Malformed: header_size > write_pos across wraparound must fail. */
	memset(vram_log, 0, sizeof(vram_log));
	hdr->header_size = 64;
	hdr->rptr = 200;
	hdr->wptr = 32;
	assert(amdgpu_debugfs_vcn_fwlog_read(&f, (char *)out, sizeof(out), &pos) == -EFAULT);

	/* Malformed: header_size smaller than header struct. */
	hdr->header_size = 4;
	hdr->rptr = 16;
	hdr->wptr = 32;
	assert(amdgpu_debugfs_vcn_fwlog_read(&f, (char *)out, sizeof(out), &pos) == -EFAULT);

	/* Malformed: header_size > AMDGPU_VCNFW_LOG_SIZE. */
	hdr->header_size = AMDGPU_VCNFW_LOG_SIZE + 16;
	hdr->rptr = AMDGPU_VCNFW_LOG_SIZE;
	hdr->wptr = AMDGPU_VCNFW_LOG_SIZE;
	assert(amdgpu_debugfs_vcn_fwlog_read(&f, (char *)out, sizeof(out), &pos) == -EFAULT);

	/* Malformed: rptr < header_size. */
	hdr->header_size = 64;
	hdr->rptr = 32;
	hdr->wptr = 96;
	assert(amdgpu_debugfs_vcn_fwlog_read(&f, (char *)out, sizeof(out), &pos) == -EFAULT);

	/* Valid wrapped read on non-posted BAR. */
	memset(vram_log, 0, sizeof(vram_log));
	hdr->header_size = 32;
	hdr->rptr = 240;
	hdr->wptr = 48;
	for (u32 i = 240; i < AMDGPU_VCNFW_LOG_SIZE; i++)
		vram_log[i] = (u8)(i - 240 + 1);
	for (u32 i = 32; i < 48; i++)
		vram_log[i] = (u8)(16 + (i - 32) + 1);

	io_memcpy_from_calls = 0;
	io_writel_calls = 0;
	n = amdgpu_debugfs_vcn_fwlog_read(&f, (char *)out, sizeof(out), &pos);
	assert(n == 32);
	assert(io_memcpy_from_calls == 2);
	assert(io_writel_calls == 1);
	assert(hdr->rptr == 48);
	for (u32 i = 0; i < 32; i++)
		assert(out[i] == (u8)(i + 1));
}

static void test_rlc_csb(void)
{
	struct amdgpu_bo_res res = { .bus = { .is_nonposted = true } };
	struct amdgpu_bo bo = { .tbo = { .resource = &res } };
	static const struct amdgpu_rlc_funcs funcs = {
		.get_csb_buffer = mock_get_csb,
	};
	struct amdgpu_device adev = {
		.dev = NULL,
		.gfx = {
			.rlc = {
				.clear_state_obj = &bo,
				.clear_state_size = 8,
				.cs_ptr = vram_csb,
				.funcs = &funcs,
			},
		},
	};

	/* Non-posted OOM must propagate -ENOMEM and leave VRAM untouched. */
	memset(vram_csb, 0x55, sizeof(vram_csb));
	fail_alloc = true;
	io_memcpy_to_calls = 0;
	assert(amdgpu_gfx_rlc_setup_csb(&adev) == -ENOMEM);
	assert(io_memcpy_to_calls == 0);
	assert(vram_csb[0] == 0x55555555U);

	/* Non-posted success must stage and copy via amdgpu_bo_memcpy_to. */
	fail_alloc = false;
	assert(amdgpu_gfx_rlc_setup_csb(&adev) == 0);
	assert(io_memcpy_to_calls == 1);
	for (u32 i = 0; i < 8; i++)
		assert(vram_csb[i] == 0xa5000000U + i);

	/* Posted path writes directly. */
	res.bus.is_nonposted = false;
	memset(vram_csb, 0, sizeof(vram_csb));
	direct_csb_calls = 0;
	assert(amdgpu_gfx_rlc_setup_csb(&adev) == 0);
	assert(direct_csb_calls == 1);
	assert(vram_csb[7] == 0xa5000007U);
}

int main(void)
{
	test_vcn_fwlog();
	test_rlc_csb();
	puts("amdgpu-vcn-rlc: pass");
	return 0;
}
