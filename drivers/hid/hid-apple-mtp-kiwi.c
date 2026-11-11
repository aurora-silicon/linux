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
 * counts, slightly different per row). Frames shifted by more than 200 counts
 * are skipped and a per-row mean is removed from the rest. The raw image has no
 * baseline, so the driver keeps one per cell, picks local maxima of
 * (baseline - raw), and centroids 3x3.
 */

#include <linux/hid.h>
#include <linux/input/mt.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "hid-ids.h"

#define KIWI_HDR_LEN		61
#define KIWI_COLS		82
#define KIWI_ROWS		54
#define KIWI_CELLS		(KIWI_COLS * KIWI_ROWS)
#define KIWI_MIN_FRAME		(KIWI_HDR_LEN + 2 * KIWI_CELLS)
#define KIWI_MAGIC		0x5a
#define KIWI_MAX_SLOTS		10
/* eDP-1 panel of the k114c, 3024x1986. */
#define KIWI_MAX_X		3023
#define KIWI_MAX_Y		1985
/* Panel is about 10 units per mm. */
#define KIWI_RES		10
#define KIWI_FP			256
#define KIWI_GLITCH		200	/* mean shift of the 2 s reference frame is about 900 */
#define KIWI_GLITCH_MAX		10	/* skipped frames in a row before the baseline is retaken */
#define KIWI_MISS_MAX		2
#define KIWI_MATCH_D2		(4 * KIWI_FP * 4 * KIWI_FP)

#define KIWI_THRESHOLD	250	/* counts below baseline that make a touch */

struct kiwi_contact {
	bool on;
	int miss;
	int x, y;	/* 1/KIWI_FP cell, centre of cell 0 is KIWI_FP / 2 */
	int size;
};

struct kiwi {
	struct input_dev *input;
	bool have_base;
	int bad;
	s16 sig[KIWI_CELLS];	/* baseline minus raw, positive under a finger */
	s32 base[KIWI_CELLS];
	s32 fp[KIWI_CELLS];
	struct kiwi_contact cont[KIWI_MAX_SLOTS];
};

static bool kiwi_match(struct hid_device *hdev, bool ignore_special_drivers)
{
	return hdev->bus == BUS_HOST && !strcmp(hdev->name, "Apple MTP kiwi");
}

/*
 * Fixed point: base and fp are in 1/256 count. fp is baseline minus raw with the
 * frame-wide shift removed, positive under a finger.
 * Returns false for a frame that must be skipped (big reference frame).
 * Besides the two big reference frames (+658 and +902 counts) every 2 s there is a
 * -445 and a -126 count frame: the whole image moves, so remove the mean.
 */
static bool kiwi_load(struct kiwi *k, const u8 *data)
{
	s64 sum = 0;
	s32 common;
	int i, r, c;

	if (!k->have_base) {
		for (i = 0; i < KIWI_CELLS; i++)
			k->base[i] = get_unaligned_le16(data + KIWI_HDR_LEN + 2 * i) << 8;
		k->have_base = true;
		k->bad = 0;
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
	for (r = 0; r < KIWI_ROWS; r++) {
		s64 row = 0;

		for (c = 0; c < KIWI_COLS; c++)
			row += k->fp[r * KIWI_COLS + c];
		common = div_s64(row, KIWI_COLS);
		for (c = 0; c < KIWI_COLS; c++) {
			i = r * KIWI_COLS + c;
			k->fp[i] -= common;
			k->sig[i] = clamp(k->fp[i] >> 8, -32768, 32767);
		}
	}
	return true;
}

/* Track the baseline: fast up when a finger left, slow drift otherwise. */
static void kiwi_adapt(struct kiwi *k)
{
	int i;

	for (i = 0; i < KIWI_CELLS; i++) {
		int s = k->sig[i];

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

static void kiwi_report(struct kiwi *k)
{
	struct input_dev *input = k->input;
	int j;

	for (j = 0; j < KIWI_MAX_SLOTS; j++) {
		struct kiwi_contact *c = &k->cont[j];
		int x, y;

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
	int i, j;

	for (i = 0; i < n; i++) {
		j = kiwi_find(k, &found[i], &claimed);
		if (j < 0)
			continue;
		claimed |= BIT(j);
		k->cont[j] = found[i];
		found[i].on = false;	/* consumed */
	}
	for (j = 0; j < KIWI_MAX_SLOTS; j++) {
		if (k->cont[j].on && !(claimed & BIT(j)) && ++k->cont[j].miss > KIWI_MISS_MAX)
			k->cont[j].on = false;
	}
	for (i = 0; i < n; i++) {
		if (!found[i].on)
			continue;
		for (j = 0; j < KIWI_MAX_SLOTS && k->cont[j].on; j++)
			;
		if (j < KIWI_MAX_SLOTS)
			k->cont[j] = found[i];
	}
}

static void kiwi_frame(struct kiwi *k, const u8 *data)
{
	struct kiwi_contact found[KIWI_MAX_SLOTS];
	int r, c, n = 0;

	if (!kiwi_load(k, data))
		return;
	for (r = 0; r < KIWI_ROWS && n < KIWI_MAX_SLOTS; r++) {
		for (c = 0; c < KIWI_COLS && n < KIWI_MAX_SLOTS; c++) {
			if (k->sig[r * KIWI_COLS + c] >= KIWI_THRESHOLD && kiwi_is_peak(k, r, c))
				kiwi_centroid(k, r, c, &found[n++]);
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

	if (size >= KIWI_MIN_FRAME && data[0] == KIWI_MAGIC)
		kiwi_frame(k, data);
	return 0;
}

static int kiwi_input_open(struct input_dev *input)
{
	struct kiwi *k = input_get_drvdata(input);

	k->have_base = false;
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

	return hid_hw_start(hdev, HID_CONNECT_HIDRAW);
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
	.raw_event = kiwi_raw_event,
};
module_hid_driver(kiwi_driver);

MODULE_DESCRIPTION("Apple MTP touch-screen (kiwi) interface");
MODULE_LICENSE("GPL");
