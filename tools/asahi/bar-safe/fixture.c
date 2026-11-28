// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define __iomem
#define CHIP_BONAIRE 2
#define dev_err(...) do {} while (0)

struct amdgpu_bo {
	struct {
		struct {
			struct {
				bool is_nonposted;
			} bus;
		} *resource;
	} tbo;
};

struct radeon_bo {
	bool reserved;
	bool mapped;
	int map_error;
};

struct radeon_device {
	struct { struct radeon_bo *vcpu_bo; } vce;
	const struct firmware { const void *data; size_t size; } *vce_fw;
	int family;
};

static unsigned char aperture[16];
static bool needs_io;
static const char *operations;

static void expect_op(char op)
{
	assert(*operations == op);
	operations++;
}

static void *normal_set(void *dst, int value, size_t count)
{
	assert(!needs_io || dst != aperture);
	expect_op('s');
	return memset(dst, value, count);
}

static void *normal_copy(void *dst, const void *src, size_t count)
{
	assert(!needs_io || (dst != aperture && src != aperture));
	expect_op('c');
	return memcpy(dst, src, count);
}

static void memset_io(void *dst, int value, size_t count)
{
	assert(dst == aperture && count == sizeof(aperture));
	expect_op('S');
	memset(dst, value, count);
}

static void memcpy_toio(void *dst, const void *src, size_t count)
{
	assert(dst == aperture && src != aperture && count <= sizeof(aperture));
	expect_op('W');
	memcpy(dst, src, count);
}

static void memcpy_fromio(void *dst, const void *src, size_t count)
{
	assert(src == aperture && dst != aperture && count <= sizeof(aperture));
	expect_op('R');
	memcpy(dst, src, count);
}

static int radeon_bo_reserve(struct radeon_bo *bo, bool no_wait)
{
	assert(!no_wait && !bo->reserved);
	bo->reserved = true;
	return 0;
}

static int radeon_bo_kmap(struct radeon_bo *bo, void **addr)
{
	assert(bo->reserved && !bo->mapped);
	if (bo->map_error)
		return bo->map_error;
	bo->mapped = true;
	*addr = aperture;
	return 0;
}

static void radeon_bo_kunmap(struct radeon_bo *bo)
{
	assert(bo->reserved && bo->mapped);
	bo->mapped = false;
}

static void radeon_bo_unreserve(struct radeon_bo *bo)
{
	assert(bo->reserved && !bo->mapped);
	bo->reserved = false;
}

static size_t radeon_bo_size(struct radeon_bo *bo)
{
	assert(bo->mapped);
	return sizeof(aperture);
}

static int vce_v1_0_load_fw(struct radeon_device *dev, void __iomem *addr)
{
	const unsigned char zero[sizeof(aperture)] = {};

	assert(dev->vce.vcpu_bo->mapped && addr == aperture);
	assert(!memcmp(aperture, zero, sizeof(zero)));
	expect_op('L');
	return -EIO;
}

#define memset normal_set
#define memcpy normal_copy
#include "bar-access.h"
#undef memcpy
#undef memset

static void amdgpu_test(bool nonposted, bool bar)
{
	struct amdgpu_bo bo;
	typeof(*bo.tbo.resource) resource = { .bus.is_nonposted = nonposted };
	const unsigned char data[] = { 1, 2, 3, 4, 5 };
	unsigned char out[sizeof(data)] = {};
	unsigned char ram[sizeof(aperture)] = {};
	unsigned char *mapping = bar ? aperture : ram;

	bo.tbo.resource = &resource;
	needs_io = nonposted;
	operations = nonposted ? "SWR" : "scc";
	amdgpu_bo_memset(&bo, mapping, 0xa5, sizeof(aperture));
	for (size_t i = 0; i < sizeof(aperture); i++)
		assert(mapping[i] == 0xa5);
	amdgpu_bo_memcpy_to(&bo, mapping, data, sizeof(data));
	amdgpu_bo_memcpy_from(&bo, out, mapping, sizeof(out));
	assert(!memcmp(out, data, sizeof(out)));
	for (size_t i = sizeof(data); i < sizeof(aperture); i++)
		assert(mapping[i] == 0xa5);
	assert(!*operations);
}

static void radeon_test(bool legacy, int map_error)
{
	const unsigned char data[] = { 9, 8, 7, 6 };
	const struct firmware fw = { .data = data, .size = sizeof(data) };
	struct radeon_bo bo = { .map_error = map_error };
	struct radeon_device dev = {
		.family = legacy ? CHIP_BONAIRE - 1 : CHIP_BONAIRE,
		.vce.vcpu_bo = &bo,
		.vce_fw = &fw,
	};

	memset(aperture, 0xa5, sizeof(aperture));
	needs_io = true;
	operations = map_error ? "" : legacy ? "SL" : "SW";
	assert(radeon_vce_resume(&dev) == (map_error ? map_error : (legacy ? -EIO : 0)));
	assert(!bo.reserved && !bo.mapped && !*operations);
	if (map_error)
		return;
	if (!legacy)
		assert(!memcmp(aperture, data, sizeof(data)));
	for (size_t i = legacy ? 0 : sizeof(data); i < sizeof(aperture); i++)
		assert(!aperture[i]);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "amdgpu-ram"))
		amdgpu_test(false, false);
	else if (!strcmp(argv[1], "amdgpu-posted"))
		amdgpu_test(false, true);
	else if (!strcmp(argv[1], "amdgpu-nonposted"))
		amdgpu_test(true, true);
	else if (!strcmp(argv[1], "radeon-vce"))
		radeon_test(false, 0);
	else if (!strcmp(argv[1], "radeon-vce-legacy"))
		radeon_test(true, 0);
	else if (!strcmp(argv[1], "radeon-map-error"))
		radeon_test(false, -ENOMEM);
	else
		return 2;

	return 0;
}
