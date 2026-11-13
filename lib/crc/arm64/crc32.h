// SPDX-License-Identifier: GPL-2.0-only

#include <asm/alternative.h>
#include <asm/cpufeature.h>
#include <asm/simd.h>

// The minimum input length to consider the 4-way interleaved code path
static const size_t min_len = 1024;

// The block size of the scalar 3-way interleaved code path (3 lanes of 1360 bytes)
#define CRC32_3WAY_BLOCK	(3 * 1360)

asmlinkage u32 crc32_le_arm64(u32 crc, unsigned char const *p, size_t len);
asmlinkage u32 crc32c_le_arm64(u32 crc, unsigned char const *p, size_t len);
asmlinkage u32 crc32_be_arm64(u32 crc, unsigned char const *p, size_t len);

asmlinkage u32 crc32_le_arm64_4way(u32 crc, unsigned char const *p, size_t len);
asmlinkage u32 crc32c_le_arm64_4way(u32 crc, unsigned char const *p, size_t len);
asmlinkage u32 crc32_be_arm64_4way(u32 crc, unsigned char const *p, size_t len);

asmlinkage u32 crc32_le_arm64_3way(u32 crc, unsigned char const *p, size_t nblocks);
asmlinkage u32 crc32c_le_arm64_3way(u32 crc, unsigned char const *p, size_t nblocks);
asmlinkage u32 crc32_be_arm64_3way(u32 crc, unsigned char const *p, size_t nblocks);

/*
 * Process whole CRC32_3WAY_BLOCK blocks with the scalar 3-way code. It keeps
 * the CRC unit saturated without the PMULL fold, needs no SIMD registers and
 * so works in any context without the kernel_neon_begin()/end() cost, which
 * dominates for page-sized inputs such as filesystem block checksums.
 */
/* TEST ONLY (BTRFSCSUM A/B): 0 = old 4-way PMULL path */
static bool crc32_arm64_3way = true;
module_param(crc32_arm64_3way, bool, 0644);

#define crc32_3way(fn, crc, p, len)					\
do {									\
	if (len >= CRC32_3WAY_BLOCK && READ_ONCE(crc32_arm64_3way)) {					\
		size_t __n = len / CRC32_3WAY_BLOCK;			\
									\
		crc = fn(crc, p, __n);					\
		p += __n * CRC32_3WAY_BLOCK;				\
		len -= __n * CRC32_3WAY_BLOCK;				\
	}								\
} while (0)

static inline u32 crc32_le_arch(u32 crc, const u8 *p, size_t len)
{
	if (!alternative_has_cap_likely(ARM64_HAS_CRC32))
		return crc32_le_base(crc, p, len);

	crc32_3way(crc32_le_arm64_3way, crc, p, len);

	if (len >= min_len && cpu_have_named_feature(PMULL) &&
	    likely(may_use_simd())) {
		scoped_ksimd()
			crc = crc32_le_arm64_4way(crc, p, len);

		p += round_down(len, 64);
		len %= 64;

		if (!len)
			return crc;
	}

	return crc32_le_arm64(crc, p, len);
}

static inline u32 crc32c_arch(u32 crc, const u8 *p, size_t len)
{
	if (!alternative_has_cap_likely(ARM64_HAS_CRC32))
		return crc32c_base(crc, p, len);

	crc32_3way(crc32c_le_arm64_3way, crc, p, len);

	if (len >= min_len && cpu_have_named_feature(PMULL) &&
	    likely(may_use_simd())) {
		scoped_ksimd()
			crc = crc32c_le_arm64_4way(crc, p, len);

		p += round_down(len, 64);
		len %= 64;

		if (!len)
			return crc;
	}

	return crc32c_le_arm64(crc, p, len);
}

static inline u32 crc32_be_arch(u32 crc, const u8 *p, size_t len)
{
	if (!alternative_has_cap_likely(ARM64_HAS_CRC32))
		return crc32_be_base(crc, p, len);

	crc32_3way(crc32_be_arm64_3way, crc, p, len);

	if (len >= min_len && cpu_have_named_feature(PMULL) &&
	    likely(may_use_simd())) {
		scoped_ksimd()
			crc = crc32_be_arm64_4way(crc, p, len);

		p += round_down(len, 64);
		len %= 64;

		if (!len)
			return crc;
	}

	return crc32_be_arm64(crc, p, len);
}

static inline u32 crc32_optimizations_arch(void)
{
	if (alternative_has_cap_likely(ARM64_HAS_CRC32))
		return CRC32_LE_OPTIMIZATION |
		       CRC32_BE_OPTIMIZATION |
		       CRC32C_OPTIMIZATION;
	return 0;
}
