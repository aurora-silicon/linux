/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Runtime notch geometry of a built-in panel.
 *
 * A panel with a camera housing has apple,notch-height rows at its top that
 * are only partly visible.  The panel always runs its native timing; what
 * changes is how much of it the OS draws on:
 *
 *  - full:       the mode covers the whole panel, notch rows included;
 *  - notch-free: the mode leaves out the notch rows.  They are folded into
 *                the vertical sync, so the timing (clock, totals, rate) is
 *                the same, and the frame is scanned out below them; the
 *                rows stay black.
 *
 * Both geometries of a timing are in the mode catalogue, so the mode a commit
 * carries chooses one, and switching between the two needs no mode set on the
 * panel.  Which one is offered to userspace follows a policy (the boot
 * loader's choice, then the "notch" connector property or the connector's
 * notch sysfs attribute) and, when no DRM master owns the device, the
 * console: the console never draws under the notch.
 */
#ifndef APPLE_DCP_NOTCH_H
#define APPLE_DCP_NOTCH_H

#ifndef DCP_NOTCH_HOST
#include <drm/drm_modes.h>
#include <linux/types.h>
#endif

/* Values of the "notch" connector property. */
enum dcp_notch_policy {
	DCP_NOTCH_HIDDEN = 0,
	DCP_NOTCH_SHOWN = 1,
};

/*
 * Turn the full-panel @mode into its notch-free twin: @rows fewer active
 * rows, folded into the vertical sync pulse.  Returns false if the mode has
 * no room for it.
 */
static inline bool dcp_notch_free_mode(struct drm_display_mode *mode, u32 rows)
{
	if (!rows || mode->vdisplay <= (int)rows || mode->vsync_start < mode->vdisplay)
		return false;
	mode->vdisplay -= rows;
	mode->vsync_start -= rows;
	return true;
}

/*
 * True if @free is the notch-free twin of the full-panel mode @full.  DCP
 * timings carry no skew or scan doubling, so those are not compared.
 */
static inline bool dcp_notch_twin_of(const struct drm_display_mode *full,
				     const struct drm_display_mode *free, u32 rows)
{
	return rows && full->clock == free->clock &&
	       full->hdisplay == free->hdisplay &&
	       full->hsync_start == free->hsync_start &&
	       full->hsync_end == free->hsync_end &&
	       full->htotal == free->htotal &&
	       full->vdisplay == free->vdisplay + (int)rows &&
	       full->vsync_start == free->vsync_start + (int)rows &&
	       full->vsync_end == free->vsync_end &&
	       full->vtotal == free->vtotal &&
	       full->flags == free->flags;
}

/* True if @a and @b are the two geometries of one timing (either order). */
static inline bool dcp_notch_twins(const struct drm_display_mode *a,
				   const struct drm_display_mode *b, u32 rows)
{
	return dcp_notch_twin_of(a, b, rows) || dcp_notch_twin_of(b, a, rows);
}

/*
 * Rows to scan a frame of @mode out below: the notch rows for a notch-free
 * mode of a panel @panel_rows high, 0 for a full one.
 */
static inline u32 dcp_notch_offset(const struct drm_display_mode *mode,
				   u32 panel_rows, u32 rows)
{
	return rows && panel_rows > rows &&
	       mode->vdisplay == (int)(panel_rows - rows) ? rows : 0;
}

/*
 * The geometry userspace and the console are offered: the policy while a
 * DRM master (a compositor, a boot splash) owns the device, notch-free
 * otherwise, so that the text console never draws under the notch.
 */
static inline enum dcp_notch_policy
dcp_notch_effective(enum dcp_notch_policy policy, bool owned)
{
	return owned ? policy : DCP_NOTCH_HIDDEN;
}

/*
 * Whether a catalogue mode is offered, and as preferred: the modes of the
 * effective geometry, so that a compositor follows a change of policy, and
 * the notch-free ones also to a client that cannot take the full panel
 * (@full_fits false: the console, whose surface is notch-free, re-probing
 * while a compositor owns the device).  The firmware's preferred timing
 * stays preferred among what is offered.
 */
static inline bool dcp_notch_offer(bool notch_free, enum dcp_notch_policy effective,
				   bool full_fits, bool *preferred)
{
	bool offered = notch_free ? effective == DCP_NOTCH_HIDDEN || !full_fits :
				    effective == DCP_NOTCH_SHOWN && full_fits;

	*preferred = *preferred && offered;
	return offered;
}

/*
 * Whether a probe offers only the notch-free geometry, which then sets the
 * physical height it reports.  A bounded probe (the console) gets the
 * notch-free size whatever the policy, so that the size it keeps in its
 * screen info does not change with the compositor's geometry.
 */
static inline bool dcp_notch_free_only(enum dcp_notch_policy effective, bool full_fits)
{
	return effective == DCP_NOTCH_HIDDEN || !full_fits;
}

/*
 * The physical height of the notch-free geometry: the panel's adj-height-mm
 * if it gives one, else the full height scaled by the rows.
 */
static inline int dcp_notch_free_mm(int height_mm, u32 adj_mm, u32 full_rows, u32 free_rows)
{
	if (adj_mm)
		return adj_mm;
	if (!full_rows)
		return height_mm;
	return (int)(((u64)height_mm * free_rows + full_rows / 2) / full_rows);
}

/* A probe limited to @max_y rows (0: none) can take the @panel_rows panel. */
static inline bool dcp_notch_full_fits(u32 max_y, u32 panel_rows)
{
	return !max_y || !panel_rows || max_y >= panel_rows;
}

#endif
