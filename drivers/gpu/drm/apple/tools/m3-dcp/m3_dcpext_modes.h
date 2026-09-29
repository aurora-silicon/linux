/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M3_DCPEXT_MODES_H
#define M3_DCPEXT_MODES_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <stdbool.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;
#endif

/* Linear SDR scanout allocation and four-lane HBR transport ceiling. */
#define M3_DCPEXT_MAX_WIDTH 3840
#define M3_DCPEXT_MAX_HEIGHT 2160
#define M3_DCPEXT_MAX_PITCH (M3_DCPEXT_MAX_WIDTH * 4)
#define M3_DCPEXT_MAX_PAYLOAD_KBPS 8640000
#define M3_DCPEXT_HBR2_4LANE_KBPS 17280000
#define M3_DCPEXT_HBR3_4LANE_KBPS 25920000

/* Mode data is supplied by DRM's EDID parser, never invented here. */
struct m3_dcpext_mode {
	u32 width, height, clock_khz, htotal, vtotal;
	bool interlaced, doublescan;
};

#define M3_DCPEXT_NATIVE_MAX_MODES 128
/* Total retained-property budget; assembly grows as chunks arrive. */
#define M3_DCPEXT_MAX_PROPERTY_BYTES 0x800000
struct m3_dcpext_native_mode {
	struct m3_dcpext_mode geometry;
	u32 hfront, hsync, vfront, vsync;
	u32 color_id, timing_id;
	bool hpositive, vpositive, preferred;
};

/* Decode the current native TimingElements publication. Only real, uncompressed
 * 8bpc SDR RGB or BT.709 limited-range YCbCr422 modes are admitted. RGB is
 * preferred; the bandwidth check conservatively budgets 24 wire bits/pixel.
 * No allocation; count is zero on error. IDs belong to this publication only. */
int m3_dcpext_native_modes_parse(const void *blob, u32 bytes,
			       struct m3_dcpext_native_mode *modes,
			       u32 capacity, u32 *count);
/* Explicit route bandwidth; USB-C remains two-lane HBR2. */
int m3_dcpext_native_modes_parse_link(const void *blob, u32 bytes,
                               struct m3_dcpext_native_mode *modes,
                               u32 capacity, u32 *count, u32 payload_kbps);
int m3_dcpext_mode_validate_link(const struct m3_dcpext_mode *mode,
                               u32 payload_kbps, u32 *fps_16_16);
/* Match full EDID timing geometry to a uniquely closest native timing. */
int m3_dcpext_native_mode_select(const struct m3_dcpext_native_mode *requested,
				const struct m3_dcpext_native_mode *modes,
				u32 count, u32 *selected);

/* The current hardware policy is four HBR lanes, 8bpc RGB. Mode records are
 * the complete firmware-provided 24-byte records, including reserved fields.
 * The selected record is returned unchanged. Unknown flags are not stripped.
 */
int m3_dcpext_mode_validate(const struct m3_dcpext_mode *mode, u32 *fps_16_16);
int m3_dcpext_mode_select(const struct m3_dcpext_mode *mode,
			 const u8 *timings, u32 count, u8 selected[24]);
/* One linear BGRA plane, caller-owned mapping in both external DARTs. */
int m3_dcpext_surface_validate(u32 width, u32 height, u32 pitch,
			      u64 dva, u64 mapped_bytes);
int m3_dcpext_native_dimensions(const void *,u32 bytes,u32 *width_mm,u32 *height_mm);
#endif
