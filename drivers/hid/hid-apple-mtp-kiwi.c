// SPDX-License-Identifier: GPL-2.0
/*
 * Apple MTP touch-screen (kiwi) interface.
 *
 * The kiwi interface of the dockchannel MTP carries its touches as a raw
 * report pipe. No other HID driver may claim it: the Apple wildcard entries of
 * hid-apple and hid-magicmouse make hid-generic stand off from every
 * BUS_HOST Apple device, and both reject the interface in probe. This driver
 * claims it by name, keeps the pipe on hidraw, and registers a direct
 * multi-touch input device. The interface is started (firmware, reset) when
 * the first user opens hidraw or the input device.
 *
 * The 29-byte descriptor declares one opaque Digitizer/TouchScreen input blob
 * (16384 bits), so the finger layout is not described by HID. Observed layout
 * of the 17861-byte report (see the pull request description):
 *
 *   0..60     header. Byte 0 is 0x5a, bytes 1..6 are 01 02 00 20 18 00,
 *             bytes 7..10 are a u32 LE microsecond timer (8333 us per frame)
 *   61..8916  54 rows x 82 columns of u16 LE raw capacitance, row major,
 *             row 0 at the bottom edge of the panel,
 *             idle level about 2500 (noise about 25 to 50 counts). A finger
 *             lowers the cells under it by 600..1300 counts. One cell is
 *             about 36.9 px on the 3024x1986 panel in both directions.
 *   8917..    further sections that are not decoded yet
 *
 * Every 2 s four whole-image shifts arrive (about +658, +902, -445 and -126
 * counts), each between two ordinary frames. A few cells on the top edge
 * (rows 52 and 53, columns 0, 41 and 81) move by 260 to 880 counts more than
 * the rest in each of them. Frames shifted by more than 64 counts are skipped
 * and a per-row mean is removed from the rest. The raw image has no
 * baseline, so the driver keeps one per cell, picks local maxima of
 * (baseline - raw), and centroids 3x3.
 */

#include <linux/debugfs.h>
#include <linux/hid.h>
#include <linux/input/mt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "hid-ids.h"

#define KIWI_HDR_LEN		61
#define KIWI_COLS		82
#define KIWI_ROWS		54
#define KIWI_CELLS		(KIWI_COLS * KIWI_ROWS)
#define KIWI_FRAME_SIZE		17861
#define KIWI_MAGIC		0x5a
#define KIWI_MAX_SLOTS		10
/* K114c is the only qualified panel and frame format. */
#define KIWI_MAX_X		3023
#define KIWI_MAX_Y		1985
/* Panel is about 10 units per mm. */
#define KIWI_RES		10
#define KIWI_FP			256
#define KIWI_GLITCH		64	/* counts: reference frames shift the mean by 137 to 899, idle frames by under 10 */
#define KIWI_SAME		200	/* counts: two ordinary frames differ less than this in every cell */
#define KIWI_GLITCH_MAX		10	/* skipped frames in a row before the baseline is retaken */
#define KIWI_MISS_MAX		2
#define KIWI_GATE_RING		3	/* the static periphery outliers all live inside this ring */
#define KIWI_MATCH_D2		(4 * KIWI_FP * 4 * KIWI_FP)

#define KIWI_THRESHOLD	250	/* counts below baseline that make a touch */
#define KIWI_HOLD_FRAMES	5400	/* 45 s at 120 Hz: a static contact past this is a phantom */
#define KIWI_SUP_REACH		6	/* shortcut: 6 cells from the drop, raise if a wider blob re-arms past the mark */
#define KIWI_LIFT_FRAMES	240	/* 2 s of quiet before a hold-dropped region re-arms: the breather's fp dips under the mask for a few frames at a time, a real lift stays quiet far longer */
#define KIWI_STATIC_FRAMES	120	/* 1 s: a contact this old whose level never breathed is a phantom */
#define KIWI_STILL_COUNTS	40	/* counts: real signal breathes far more (measured 854 on a real touch) */
#define KIWI_ROW_SHIFT	64	/* per-row mean shift that marks a whole-image reference frame */

#ifdef KIWI_DEBUG
#define KDBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define KDBG(...) do {} while (0)
#endif

struct kiwi_contact {
	bool on;
	int miss;
	int x, y;	/* 1/KIWI_FP cell, centre of cell 0 is KIWI_FP / 2 */
	int size;
	u32 born;	/* accepted-frame stamp of the first sight of this blob */
	int bx, by;	/* birth position: net drift past a cell over a sane hold = phantom */
	s16 b_sig;	/* signal at the birth cell when born: flatness marks a stuck cell */
	bool lively;	/* the birth-cell signal moved more than KIWI_STILL_COUNTS */
};

struct kiwi {
	struct input_dev *input;
	bool have_base;
	int rowmean[KIWI_ROWS];	/* per-row mean of the last frame, pre-removal */
	u8 cand_hot[(KIWI_CELLS + 7) / 8];	/* hot-cell map of the first baseline candidate */
	bool have_hot;
	u64 drop_ring, drop_hold, drop_still;	/* per-rule drop counters, debugfs */
	struct dentry *dbg;
	int bad;
	int agree;		/* agreeing pairs chained toward a baseline */
	u32 now;		/* accepted-frame counter, for the hold rule */
	bool sup;
	s16 sup_r, sup_c;	/* cell of the last hold-dropped phantom: re-arm guard */
	u8 sup_rad;		/* every drop masks the blob extent (2): a neighbour cell must not re-arm */
	u8 sup_map[(KIWI_CELLS + 7) / 8];	/* hold-dropped extents: every hold drop marks its blob; the map lifts only on a sustained quiet run */
	bool sup_map_on;
	u16 sup_map_clear;
	bool had;		/* a contact just ended here: its hold time carries over */
	bool last_still;	/* the last end was a stillness drop: a carry must not inherit lively */
	s16 last_r, last_c;
	u32 last_born;
	int last_bx, last_by;
	s16 last_bsig;
	u32 last_end;	/* frame of the last contact end: carry only across a miss-length gap */
	u8 cand[2 * KIWI_CELLS];	/* baseline candidate: raw frame, little endian */
	int cand_age;
	s16 sig[KIWI_CELLS];	/* baseline minus raw, positive under a finger */
	s32 base[KIWI_CELLS];
	s32 fp[KIWI_CELLS];
	struct kiwi_contact cont[KIWI_MAX_SLOTS];
};

static bool kiwi_match(struct hid_device *hdev, bool ignore_special_drivers)
{
	return hdev->bus == BUS_HOST && !strcmp(hdev->name, "Apple MTP kiwi") &&
	       of_machine_is_compatible("apple,k114c");
}

/*
 * The chip sends four reference frames in every 2 s cycle, each between two
 * ordinary frames. They differ from the ordinary frames and from each other, so
 * a frame is a baseline only if the frame two later matches it.
 */
/* Mark cells that lift more than a touch threshold off their row mean. The
 * panel's static periphery outliers land in this set on every frame, so a
 * frame is a baseline candidate only when its hot set matches the first one:
 * a live touch adds hot cells, static texture does not. */
static void kiwi_hot_set(const u8 *cells, u8 *hot)
{
	int r, c, i;

	memset(hot, 0, (KIWI_CELLS + 7) / 8);
	for (r = 0; r < KIWI_ROWS; r++) {
		s64 row = 0;
		int mean;

		for (c = 0; c < KIWI_COLS; c++)
			row += get_unaligned_le16(cells + 2 * (r * KIWI_COLS + c));
		mean = (int)div_s64(row, KIWI_COLS);
		for (c = 0; c < KIWI_COLS; c++) {
			i = r * KIWI_COLS + c;
			if (abs((int)get_unaligned_le16(cells + 2 * i) - mean) > KIWI_THRESHOLD)
				hot[i / 8] |= (u8)(1 << (i % 8));
		}
	}
}

static void kiwi_take_base(struct kiwi *k, const u8 *data)
{
	const u8 *cells = data + KIWI_HDR_LEN;
	int i, r;

	if (!k->have_hot) {
		kiwi_hot_set(cells, k->cand_hot);
		k->have_hot = true;
	} else {
		u8 hot[(KIWI_CELLS + 7) / 8];

		kiwi_hot_set(cells, hot);
		if (memcmp(hot, k->cand_hot, sizeof(hot)) != 0)
			return;	/* a touch is down: keep the stale signal, wait for a quiet frame */
	}
	if (k->cand_age == 0) {
		memcpy(k->cand, cells, sizeof(k->cand));
		k->cand_age = 1;
		return;
	}
	if (k->cand_age == 1) {
		k->cand_age = 2;
		return;
	}
	k->cand_age = 0;
	for (i = 0; i < KIWI_CELLS; i++)
		if (abs(get_unaligned_le16(cells + 2 * i) - get_unaligned_le16(k->cand + 2 * i)) > KIWI_SAME) {
			k->agree = 0;	/* a broken chain restarts from the first pair */
			return;
		}
	if (++k->agree < 2) {	/* baseline only from agreeing frames: two pairs in a row */
		memcpy(k->cand, cells, sizeof(k->cand));
		k->cand_age = 1;
		return;
	}
	k->agree = 0;
	/* Bake every cell from its raw value, except interior hot cells: the
	 * panel's static periphery outliers are hot in every frame and must bake,
	 * but a finger resting on the glass at open is also hot, and baking it
	 * would eat a real touch for the whole session. Interior hot cells bake
	 * at their row's quiet level instead, so they read as a touch from the
	 * first frame; a dead-still interior block is then bounded by the static
	 * rule like any other flat phantom. */
	for (r = 0; r < KIWI_ROWS; r++) {
		s64 quiet = 0;
		int qn = 0, c, i2;

		for (c = 0; c < KIWI_COLS; c++) {
			i2 = r * KIWI_COLS + c;
			if (!(k->cand_hot[i2 / 8] & (u8)(1 << (i2 % 8)))) {
				quiet += get_unaligned_le16(cells + 2 * i2);
				qn++;
			}
		}
		if (qn)
			quiet = div_s64(quiet, qn);
		for (c = 0; c < KIWI_COLS; c++) {
			i2 = r * KIWI_COLS + c;
			if ((k->cand_hot[i2 / 8] & (u8)(1 << (i2 % 8))) &&
			    c >= KIWI_GATE_RING && c < KIWI_COLS - KIWI_GATE_RING &&
			    r >= KIWI_GATE_RING && r < KIWI_ROWS - KIWI_GATE_RING)
				k->base[i2] = (s32)quiet << 8;
			else
				k->base[i2] = get_unaligned_le16(cells + 2 * i2) << 8;
		}
	}
	k->have_base = true;
	k->bad = 0;
}

/*
 * Fixed point: base and fp are in 1/256 count. fp is baseline minus raw with the
 * frame-wide shift removed, positive under a finger.
 * Returns false for a frame that must be skipped: no baseline yet, or one of the
 * four reference frames of the 2 s cycle. None may reach the baseline or the
 * peak search: a baseline taken from the -126 frame, or a -126 frame passed
 * through, turns the top edge cells into a held touch.
 */
static bool kiwi_load(struct kiwi *k, const u8 *data)
{
	s64 sum = 0;
	s32 common;
	int rowmax;
	int i, r, c;

	if (!k->have_base) {
		kiwi_take_base(k, data);
		return false;
	}
	for (i = 0; i < KIWI_CELLS; i++) {
		k->fp[i] = k->base[i] - (get_unaligned_le16(data + KIWI_HDR_LEN + 2 * i) << 8);
		sum += k->fp[i];
	}
	common = div_s64(sum, KIWI_CELLS);
	if (abs(common >> 8) > KIWI_GLITCH) {
		if (++k->bad >= KIWI_GLITCH_MAX)
			k->have_base = false;
		return false;
	}
	k->bad = 0;
	/* the shift differs a little from row to row, so remove it per row */
	rowmax = 0;
	for (r = 0; r < KIWI_ROWS; r++) {
		s64 row = 0;

		for (c = 0; c < KIWI_COLS; c++)
			row += k->fp[r * KIWI_COLS + c];
		common = div_s64(row, KIWI_COLS);
		k->rowmean[r] = (int)common;
		if (abs(common >> 8) > rowmax)
			rowmax = abs(common >> 8);
		for (c = 0; c < KIWI_COLS; c++) {
			i = r * KIWI_COLS + c;
			k->fp[i] -= common;
			k->sig[i] = clamp(k->fp[i] >> 8, -32768, 32767);
		}
	}
	if (rowmax > KIWI_ROW_SHIFT) {
		/* a frame we cannot trust for contacts. A touch also leaves its rows
		 * a few tens of counts off (the slow adapt integrates the touch's row
		 * pressure); if skipped frames never healed, a lift would strand the
		 * row above the skip bound and blind the next touch. Heal mildly
		 * shifted rows here; true reference frames (hundreds of counts) and
		 * hot rows stay untouched. */
		for (r = 0; r < KIWI_ROWS; r++) {
			int step, rr, cc, worst = 0;

			if (abs(k->rowmean[r] >> 8) > KIWI_ROW_SHIFT + 32)
				continue;
			/* heal only rows with no touch-strength cell: a lit blob is the
			 * very pressure that drifted the row, and healing under it just
			 * chases the blob */
			for (rr = 0; rr < KIWI_COLS; rr++) {
				cc = k->sig[r * KIWI_COLS + rr];
				if (cc < 0)
					cc = -cc;
				if (cc > worst)
					worst = cc;
			}
			if (worst > KIWI_THRESHOLD)
				continue;
			step = k->rowmean[r] >> 10;
			for (c = 0; c < KIWI_COLS; c++)
				k->base[r * KIWI_COLS + c] -= step;
		}
		if (++k->bad >= KIWI_GLITCH_MAX)
			k->have_base = false;
		return false;
	}
	return true;
}

/* Track the baseline: fast up when a finger left, slow drift otherwise.
 * Rows with a touch-strength cell never adapt: their idle cells would chase
 * the touch's row pressure and strand the row when the touch lifts. */
static void kiwi_adapt(struct kiwi *k)
{
	bool touched[KIWI_ROWS];
	int i, r, c;

	for (r = 0; r < KIWI_ROWS; r++) {
		touched[r] = false;
		for (c = 0; c < KIWI_COLS; c++)
			if (abs(k->sig[r * KIWI_COLS + c]) > KIWI_THRESHOLD / 2) {
				touched[r] = true;
				break;
			}
	}
	for (i = 0; i < KIWI_CELLS; i++) {
		int s = k->sig[i];

		if (touched[i / KIWI_COLS])
			continue;
		if (s < -100)
			k->base[i] -= k->fp[i] >> 3;
		else if (s < 100)
			k->base[i] -= k->fp[i] >> 10;
	}
}

static bool kiwi_is_peak(struct kiwi *k, int r, int c)
{
	int s = k->sig[r * KIWI_COLS + c];
	int dr, dc;

	for (dr = -1; dr <= 1; dr++) {
		for (dc = -1; dc <= 1; dc++) {
			int rr = r + dr, cc = c + dc, n;

			if ((!dr && !dc) || rr < 0 || rr >= KIWI_ROWS || cc < 0 || cc >= KIWI_COLS)
				continue;
			n = k->sig[rr * KIWI_COLS + cc];
			/* earlier in raster order must be strictly lower: one peak per plateau */
			if (((dr < 0) || (!dr && dc < 0)) ? n >= s : n > s)
				return false;
		}
	}
	return true;
}

static void kiwi_centroid(struct kiwi *k, int r, int c, struct kiwi_contact *out)
{
	s64 w = 0, wx = 0, wy = 0;
	int dr, dc, cells = 0;

	for (dr = -1; dr <= 1; dr++) {
		for (dc = -1; dc <= 1; dc++) {
			int rr = r + dr, cc = c + dc, s;

			if (rr < 0 || rr >= KIWI_ROWS || cc < 0 || cc >= KIWI_COLS)
				continue;
			s = k->sig[rr * KIWI_COLS + cc];
			if (s <= KIWI_THRESHOLD / 4)
				continue;
			w += s;
			wx += (s64)s * cc;
			wy += (s64)s * rr;
			cells++;
		}
	}
	out->x = (int)div64_s64(wx * KIWI_FP, w) + KIWI_FP / 2;
	out->y = (int)div64_s64(wy * KIWI_FP, w) + KIWI_FP / 2;
	out->size = cells;
	out->on = true;
	out->miss = 0;
}

static int kiwi_find(struct kiwi *k, const struct kiwi_contact *c, u32 *claimed)
{
	int best = -1, best_d2 = KIWI_MATCH_D2, j;

	for (j = 0; j < KIWI_MAX_SLOTS; j++) {
		int dx = k->cont[j].x - c->x, dy = k->cont[j].y - c->y, d2;

		if (!k->cont[j].on || (*claimed & BIT(j)))
			continue;
		d2 = dx * dx + dy * dy;
		if (d2 < best_d2) {
			best_d2 = d2;
			best = j;
		}
	}
	return best;
}

/* mark the dropped blob: the centroid box, then every still-lit cell that
 * touches it. A box on the centroid leaves the far edge of a wide blob
 * unmarked, and that edge re-arms a frame later. The map is the only clock:
 * it lifts after KIWI_LIFT_FRAMES of quiet, not on a dip and not on a timer. */
static void kiwi_sup_mark(struct kiwi *k, int row, int col)
{
	int dr, dc, pass;

	for (dr = -2; dr <= 2; dr++)
		for (dc = -2; dc <= 2; dc++) {
			int rr = row + dr, cc = col + dc, i;

			if (rr < 0 || rr >= KIWI_ROWS || cc < 0 || cc >= KIWI_COLS)
				continue;
			i = rr * KIWI_COLS + cc;
			k->sup_map[i / 8] |= (u8)(1 << (i % 8));
		}
	/* grow into the lit blob, one ring per pass, bounded by KIWI_SUP_REACH */
	for (pass = 0; pass < KIWI_SUP_REACH; pass++)
		for (dr = -KIWI_SUP_REACH; dr <= KIWI_SUP_REACH; dr++)
			for (dc = -KIWI_SUP_REACH; dc <= KIWI_SUP_REACH; dc++) {
				int rr = row + dr, cc = col + dc, i, a, b, near = 0;

				if (rr < 0 || rr >= KIWI_ROWS || cc < 0 || cc >= KIWI_COLS)
					continue;
				i = rr * KIWI_COLS + cc;
				if (k->sup_map[i / 8] & (u8)(1 << (i % 8)))
					continue;
				if (k->sig[i] < KIWI_THRESHOLD / 2)
					continue;
				for (a = -1; a <= 1 && !near; a++)
					for (b = -1; b <= 1; b++) {
						int r2 = rr + a, c2 = cc + b, i2;

						if (!a && !b)
							continue;
						if (r2 < 0 || r2 >= KIWI_ROWS ||
						    c2 < 0 || c2 >= KIWI_COLS)
							continue;
						i2 = r2 * KIWI_COLS + c2;
						if (k->sup_map[i2 / 8] & (u8)(1 << (i2 % 8)))
							near = 1;
					}
				if (near)
					k->sup_map[i / 8] |= (u8)(1 << (i % 8));
			}
	k->sup_map_on = true;
	k->sup_map_clear = 0;
}

static void kiwi_report(struct kiwi *k)
{
	struct input_dev *input = k->input;
	int j, j2;

	for (j = 0; j < KIWI_MAX_SLOTS; j++) {
		struct kiwi_contact *c = &k->cont[j];
		int x, y;

		if (c->on && k->now - c->born >= KIWI_STATIC_FRAMES && !c->lively) {
			/* a 1 s old contact whose birth cell never breathed is a stuck cell */
			KDBG("f%u static-drop slot%d cell(row%d,col%d)\n", k->now, j,
			     c->y / KIWI_FP, c->x / KIWI_FP);
			k->drop_still++;
			c->on = false;
			k->sup = true;
			k->sup_rad = 2;
			k->sup_r = clamp(c->y / KIWI_FP, 0, KIWI_ROWS - 1);
			k->sup_c = clamp(c->x / KIWI_FP, 0, KIWI_COLS - 1);
			/* an old flat contact dying of stillness is the stuck blob at
			 * the end of its hold life: mark the hold map, or its fp dips
			 * seed a fresh clock within frames. Young still drops keep
			 * today's burst semantics exactly. */
			if (k->now - c->born > KIWI_HOLD_FRAMES - KIWI_LIFT_FRAMES) {
				kiwi_sup_mark(k, c->y / KIWI_FP, c->x / KIWI_FP);
				for (j2 = 0; j2 < KIWI_MAX_SLOTS; j2++)
					if (k->cont[j2].on && j2 != j &&
					    abs(k->cont[j2].y / KIWI_FP - k->sup_r) <= k->sup_rad &&
					    abs(k->cont[j2].x / KIWI_FP - k->sup_c) <= k->sup_rad)
						k->cont[j2].on = false;	/* one blob, one contact */
			}
			k->had = true;
			k->last_r = k->sup_r;
			k->last_c = k->sup_c;
			k->last_born = c->born;
			k->last_bx = c->bx;
			k->last_by = c->by;
			k->last_bsig = c->b_sig;
			k->last_end = k->now;
			k->last_still = true;
		} else if (c->on && k->now - c->born > KIWI_HOLD_FRAMES &&
		    abs(c->x - c->bx) + abs(c->y - c->by) < 3 * KIWI_FP) {
			/* a contact that outlives a sane hold in one spot is a phantom */
			KDBG("f%u hold-drop slot%d cell(row%d,col%d) drift(%d,%d)\n", k->now, j,
			     c->y / KIWI_FP, c->x / KIWI_FP, (c->y - c->by) / KIWI_FP, (c->x - c->bx) / KIWI_FP);
			k->drop_hold++;
			c->on = false;
			k->sup = true;
			k->sup_rad = 2;	/* the dropped blob still spans its cells; mask them all */
			k->sup_r = clamp(c->y / KIWI_FP, 0, KIWI_ROWS - 1);
			k->sup_c = clamp(c->x / KIWI_FP, 0, KIWI_COLS - 1);
			kiwi_sup_mark(k, c->y / KIWI_FP, c->x / KIWI_FP);
			for (j2 = 0; j2 < KIWI_MAX_SLOTS; j2++)
				if (k->cont[j2].on && j2 != j &&
				    abs(k->cont[j2].y / KIWI_FP - k->sup_r) <= k->sup_rad &&
				    abs(k->cont[j2].x / KIWI_FP - k->sup_c) <= k->sup_rad)
					k->cont[j2].on = false;	/* one blob, one contact */
			k->had = true;
			k->last_r = k->sup_r;
			k->last_c = k->sup_c;
			k->last_born = c->born;
			k->last_bx = c->bx;
			k->last_by = c->by;
			k->last_end = k->now;
		}
		input_mt_slot(input, j);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, c->on);
		if (!c->on)
			continue;
		x = div_s64((s64)c->x * (KIWI_MAX_X + 1), KIWI_COLS * KIWI_FP);
		y = div_s64((s64)c->y * (KIWI_MAX_Y + 1), KIWI_ROWS * KIWI_FP);
		/* Row 0 is the bottom edge of the panel. */
		x = clamp(x, 0, KIWI_MAX_X);
		y = clamp(KIWI_MAX_Y - y, 0, KIWI_MAX_Y);
		input_report_abs(input, ABS_MT_POSITION_X, x);
		input_report_abs(input, ABS_MT_POSITION_Y, y);
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, c->size * 16);
	}
	input_mt_sync_frame(input);
	input_sync(input);
}

static void kiwi_track(struct kiwi *k, struct kiwi_contact *found, int n)
{
	u32 claimed = 0;
	int i, i2, j, r, c;

	for (i = 0; i < n; i++) {
		j = kiwi_find(k, &found[i], &claimed);
		if (j < 0)
			continue;
		claimed |= BIT(j);
		{
			u32 b = k->cont[j].born;
			int bx = k->cont[j].bx, by = k->cont[j].by;
			s16 bs = k->cont[j].b_sig;
			bool lv = k->cont[j].lively;

			k->cont[j] = found[i];
			k->cont[j].born = b;
			k->cont[j].bx = bx;
			k->cont[j].by = by;
			k->cont[j].b_sig = bs;
			k->cont[j].lively = lv;	/* liveliness is sticky: the claim copy must not wipe it */
			if (!k->cont[j].lively) {
				int d = k->sig[(by / KIWI_FP) * KIWI_COLS + (bx / KIWI_FP)] - bs;

				if (d < 0)
					d = -d;
				if (d > KIWI_STILL_COUNTS) {
					k->cont[j].lively = true;
					KDBG("f%u lively slot%d d%d\n", k->now, j, d);
				}
			}
		}
		found[i].on = false;	/* consumed */
	}
	for (j = 0; j < KIWI_MAX_SLOTS; j++) {
		if (k->cont[j].on && !(claimed & BIT(j)) && ++k->cont[j].miss > KIWI_MISS_MAX) {
			k->cont[j].on = false;
			/* a contact near the hold age that dips out misses into its
			 * own drop: mark the hold map here, or the same blob seeds a
			 * fresh slot with a fresh clock one frame later. The window
			 * opens one lift-run early: measured on the breather, its
			 * last dip lands 2 frames shy of the full hold age. */
			if (k->now - k->cont[j].born > KIWI_HOLD_FRAMES - KIWI_LIFT_FRAMES) {
				k->sup = true;
				k->sup_rad = 2;
				k->sup_r = clamp(k->cont[j].y / KIWI_FP, 0, KIWI_ROWS - 1);
				k->sup_c = clamp(k->cont[j].x / KIWI_FP, 0, KIWI_COLS - 1);
				kiwi_sup_mark(k, k->cont[j].y / KIWI_FP, k->cont[j].x / KIWI_FP);
				k->drop_hold++;
			}
			KDBG("f%u miss-drop slot%d cell(row%d,col%d) held%u\n", k->now, j,
			     k->cont[j].y / KIWI_FP, k->cont[j].x / KIWI_FP, k->now - k->cont[j].born);
			k->had = true;
			k->last_r = clamp(k->cont[j].y / KIWI_FP, 0, KIWI_ROWS - 1);
			k->last_c = clamp(k->cont[j].x / KIWI_FP, 0, KIWI_COLS - 1);
			k->last_born = k->cont[j].born;
			k->last_bx = k->cont[j].bx;
			k->last_by = k->cont[j].by;
			k->last_bsig = k->cont[j].b_sig;
			k->last_end = k->now;
			k->last_still = false;
		}
	}
	for (i = 0; i < n; i++) {
		if (!found[i].on)
			continue;
		for (j = 0; j < KIWI_MAX_SLOTS; j++) {
			int dx = k->cont[j].x - found[i].x, dy = k->cont[j].y - found[i].y;

			if (k->cont[j].on && dx * dx + dy * dy < KIWI_MATCH_D2) {
				found[i].on = false;	/* already tracked: one blob, one contact */
				break;
			}
		}
		if (!found[i].on)
			continue;
		r = found[i].x / KIWI_FP;
		c = found[i].y / KIWI_FP;
		if (k->sup && abs(c - k->sup_r) <= k->sup_rad && abs(r - k->sup_c) <= k->sup_rad)
			continue;	/* the dropped phantom's cell is still lit */
		i2 = c * KIWI_COLS + r;
		if (k->sup_map_on && (k->sup_map[i2 / 8] & (u8)(1 << (i2 % 8))))
			continue;	/* a hold-dropped region: silent until it lifts */
		for (j = 0; j < KIWI_MAX_SLOTS && k->cont[j].on; j++)
			;
		if (j < KIWI_MAX_SLOTS) {
			KDBG("f%u open slot%d cell(row%d,col%d) carry%d\n", k->now, j, c, r,
			     k->had && abs(c - k->last_r) <= 2 && abs(r - k->last_c) <= 2);
			k->cont[j] = found[i];
			if (k->had && abs(c - k->last_r) <= 2 && abs(r - k->last_c) <= 2 &&
			    k->now - k->last_born <= KIWI_HOLD_FRAMES + KIWI_MISS_MAX &&
			    k->now - k->last_end <= KIWI_MISS_MAX) {
				k->cont[j].born = k->last_born;	/* same hold, still going */
				k->cont[j].bx = k->last_bx;
				k->cont[j].by = k->last_by;
				k->cont[j].b_sig = k->last_bsig;
				/* a carried chain has earned its breath, unless the chain
				 * just died of stillness: the same flat blob lit a neighbour */
				k->cont[j].lively = !k->last_still;
			} else {
				int i2;

				k->cont[j].born = k->now;
				k->cont[j].bx = found[i].x;
				k->cont[j].by = found[i].y;
				k->cont[j].lively = false;
				i2 = (k->cont[j].by / KIWI_FP) * KIWI_COLS + (k->cont[j].bx / KIWI_FP);
				k->cont[j].b_sig = (k->cont[j].by >= 0 && k->cont[j].by / KIWI_FP < KIWI_ROWS &&
						    k->cont[j].bx >= 0 && k->cont[j].bx / KIWI_FP < KIWI_COLS)
						   ? k->sig[i2] : 0;
			}
		}
	}
	/* the guard clears once its cell goes quiet, so a real finger works again */
	if (k->sup && !k->sup_map_on) {
		i = k->sup_r * KIWI_COLS + k->sup_c;
		if (k->sup_r < 0 || k->sup_r >= KIWI_ROWS || k->sup_c < 0 || k->sup_c >= KIWI_COLS ||
		    k->sig[i] < KIWI_THRESHOLD / 2)
			k->sup = false;
	}
	/* the hold map lifts only after a sustained quiet run: the blob's fp
	 * dips cannot re-arm a dropped region with a fresh clock, while a real
	 * lift (quiet well past KIWI_LIFT_FRAMES) frees the cells for the next
	 * touch */
	if (k->sup_map_on) {
		bool lit = false;

		for (i2 = 0; i2 < KIWI_CELLS && !lit; i2++)
			if ((k->sup_map[i2 / 8] & (u8)(1 << (i2 % 8))) &&
			    k->sig[i2] >= KIWI_THRESHOLD / 2)
				lit = true;
		if (lit) {
			k->sup_map_clear = 0;
		} else if (++k->sup_map_clear >= KIWI_LIFT_FRAMES) {
			memset(k->sup_map, 0, sizeof(k->sup_map));
			k->sup_map_on = false;
			k->sup_map_clear = 0;
			k->sup = false;
		}
	}
}

static void kiwi_frame(struct kiwi *k, const u8 *data)
{
	struct kiwi_contact found[KIWI_MAX_SLOTS];
	int r, c, n = 0;

	if (!kiwi_load(k, data))
		return;
	k->now++;

	for (r = 0; r < KIWI_ROWS && n < KIWI_MAX_SLOTS; r++) {
		for (c = 0; c < KIWI_COLS && n < KIWI_MAX_SLOTS; c++) {
			/* the 2-cell edge ring is where stuck-cell phantoms live; a real
			 * finger is never centred that close to the bezel */
			if (k->sig[r * KIWI_COLS + c] >= KIWI_THRESHOLD && kiwi_is_peak(k, r, c)) {
				if (r > 1 && r < KIWI_ROWS - 2 && c > 1 && c < KIWI_COLS - 2)
					kiwi_centroid(k, r, c, &found[n++]);
				else
					k->drop_ring++;	/* the 2-cell edge ring rejected a peak */
			}
		}
	}
	kiwi_adapt(k);
	kiwi_track(k, found, n);
	kiwi_report(k);
}

static int kiwi_raw_event(struct hid_device *hdev, struct hid_report *report,
			  u8 *data, int size)
{
	struct kiwi *k = hid_get_drvdata(hdev);

	if (size == KIWI_FRAME_SIZE && data[0] == KIWI_MAGIC)
		kiwi_frame(k, data);
	return 0;
}

static int kiwi_input_open(struct input_dev *input)
{
	struct kiwi *k = input_get_drvdata(input);

	k->have_base = false;
	k->cand_age = 0;
	k->agree = 0;
	k->now = 0;
	k->sup = false;
	k->had = false;
	return hid_hw_open(to_hid_device(input->dev.parent));
}

static void kiwi_input_close(struct input_dev *input)
{
	hid_hw_close(to_hid_device(input->dev.parent));
}

static struct input_dev *kiwi_alloc_input(struct hid_device *hdev, struct kiwi *k)
{
	struct input_dev *input = devm_input_allocate_device(&hdev->dev);

	if (!input)
		return NULL;
	input->name = "Apple MTP touch screen";
	input->id.bustype = hdev->bus;
	input->id.vendor = hdev->vendor;
	input->id.product = hdev->product;
	input->phys = hdev->phys;
	input->dev.parent = &hdev->dev;
	input->open = kiwi_input_open;
	input->close = kiwi_input_close;
	input_set_drvdata(input, k);
	__set_bit(INPUT_PROP_DIRECT, input->propbit);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, KIWI_MAX_X, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, KIWI_MAX_Y, 0, 0);
	input_abs_set_res(input, ABS_MT_POSITION_X, KIWI_RES);
	input_abs_set_res(input, ABS_MT_POSITION_Y, KIWI_RES);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	if (input_mt_init_slots(input, KIWI_MAX_SLOTS, INPUT_MT_DIRECT))
		return NULL;
	return input;
}

static int kiwi_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct kiwi *kiwi;
	int ret;

	kiwi = devm_kzalloc(&hdev->dev, sizeof(*kiwi), GFP_KERNEL);
	if (!kiwi)
		return -ENOMEM;
	hid_set_drvdata(hdev, kiwi);

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	kiwi->input = kiwi_alloc_input(hdev, kiwi);
	if (!kiwi->input)
		return -ENOMEM;
	ret = input_register_device(kiwi->input);
	if (ret)
		return ret;

	kiwi->dbg = debugfs_create_dir("hid-apple-mtp-kiwi", NULL);
	debugfs_create_u64("drop_edge_ring", 0444, kiwi->dbg, &kiwi->drop_ring);
	debugfs_create_u64("drop_hold", 0444, kiwi->dbg, &kiwi->drop_hold);
	debugfs_create_u64("drop_still", 0444, kiwi->dbg, &kiwi->drop_still);

	return hid_hw_start(hdev, HID_CONNECT_HIDRAW);
}

static void kiwi_remove(struct hid_device *hdev)
{
	struct kiwi *k = hid_get_drvdata(hdev);

	debugfs_remove_recursive(k->dbg);
	input_unregister_device(k->input);
	hid_hw_stop(hdev);
}

static const struct hid_device_id kiwi_devices[] = {
	{ HID_DEVICE(BUS_HOST, HID_GROUP_ANY, HOST_VENDOR_ID_APPLE, HID_ANY_ID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, kiwi_devices);

static struct hid_driver kiwi_driver = {
	.name = "hid-apple-mtp-kiwi",
	.id_table = kiwi_devices,
	.match = kiwi_match,
	.probe = kiwi_probe,
	.remove = kiwi_remove,
	.raw_event = kiwi_raw_event,
};
module_hid_driver(kiwi_driver);

MODULE_DESCRIPTION("Apple MTP touch-screen (kiwi) interface");
MODULE_LICENSE("GPL");
