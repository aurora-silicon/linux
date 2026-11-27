// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple silicon scaler (MSR23, M5): job builder.
 *
 * The engine's firmware streams a register image into the hardware for each
 * job: one block per register group (pipe, source and destination DMA,
 * chroma resampling, colour matrix, scaler, dither). This builds that image
 * from the job's parameters: the words that depend on the sizes, formats and
 * matrix are computed, the others hold the fixed values below.
 *
 * Crop is the source region's size and origin, rotation and mirroring a
 * field of the pipe word; the reader rotates, so the rest of the pipe works
 * in the destination's orientation. Compose writes a picture of the
 * rectangle's size at an offset into the destination.
 *
 * The firmware tiles the job itself, so no tile array is built.
 */

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/string.h>

#include <kunit/visibility.h>

#include "apple-scaler-hw.h"
#include "apple-scaler-msr23.h"

/*
 * The blocks of every job, in list order. Blocks 0, 1 and 14 hold the pipe,
 * the source and the destination.
 */
const struct msr23_block msr23_blocks[MSR23_NBLOCKS] = {
	{ 0, MSR23_B0, 0x00c00200 },	{ 1, MSR23_B1, 0x00c00200 },	{ 3, 0x1e00, 0x00400200 },
	{ 4, 0x1e80, 0x31c00200 },	{ 5, 0x8200, 0x00400200 },	{ 8, 0x9080, 0x03c00200 },
	{ 11, 0xa600, 0x23c00200 },	{ 12, 0xed80, 0x01400200 },	{ 13, 0xf000, 0x09000200 },
	{ 14, MSR23_B14, 0x00c00200 },
};
EXPORT_SYMBOL_IF_KUNIT(msr23_blocks);

#define MSR23_B3	0x1e00
#define MSR23_B8	0x9080
#define MSR23_B11	0xa600
#define MSR23_B12	0xed80

struct msr23_default {
	u8 block;
	u16 off;
	u32 val;
};

#define MSR23_FILTER_BINS	8
#define MSR23_FILTER_TAPS	144

struct msr23_filter {
	u32 ratio;
	s16 extra;
	s16 taps[MSR23_FILTER_TAPS];
};

/*
 * Register defaults: words every firmware-tiled job carries with the same
 * value (block, offset, value).
 */
static const struct msr23_default msr23_defaults[] = {
	{ 1, 0x84, 0x7d35d }, { 1, 0x88, 0x7d35d }, { 1, 0x8c, 0x7d35d }, { 1, 0x90, 0x7d35d },
	{ 1, 0x94, 0x708 }, { 1, 0x98, 0x708 }, { 1, 0x9c, 0x708 }, { 1, 0xa0, 0x708 },
	{ 1, 0xa4, 0x20 }, { 1, 0xa8, 0x20 }, { 1, 0xac, 0x20 }, { 1, 0xb0, 0x20 },
	{ 1, 0xd4, 0x1 }, { 1, 0xe4, 0xffffffff }, { 1, 0x10c, 0x12345678 }, { 14, 0xc, 0x8000 },
	{ 14, 0x64, 0xac25d }, { 14, 0x68, 0xac25d }, { 14, 0x6c, 0xac25d },
	{ 14, 0x70, 0xc0ac208 }, { 14, 0x74, 0xc0ac208 }, { 14, 0x78, 0xc0ac208 },
	{ 14, 0x7c, 0x4000600 }, { 14, 0x80, 0x4000600 }, { 14, 0x84, 0x4000600 },
	{ 14, 0xe4, 0x4048 }, { 14, 0xe8, 0x4048 }, { 14, 0xec, 0x4048 }, { 14, 0xf0, 0x4048 },
	{ 14, 0xf4, 0x4048 }, { 14, 0xf8, 0x4048 }, { 14, 0xfc, 0x4048 }, { 14, 0x100, 0x4048 },
	{ 14, 0x104, 0x4048 }, { 14, 0x108, 0x4048 }, { 14, 0x10c, 0x4048 }, { 14, 0x110, 0x4048 },
	{ 14, 0x114, 0x12402046 }, { 14, 0x118, 0x2000 }, { 14, 0x168, 0x12345678 },
	{ 3, 0x24, 0xc }, { 3, 0x64, 0x12345678 }, { 4, 0x6300, 0x2804008 },
	{ 4, 0x6304, 0xa0a0a0a0 }, { 4, 0x6308, 0x2550000 }, { 4, 0x630c, 0x2550255 },
	{ 4, 0x6314, 0x100008 }, { 4, 0x6318, 0x2550000 }, { 5, 0x20, 0xff }, { 11, 0x0, 0xe0000 },
	{ 11, 0x4, 0xc }, { 11, 0x50, 0x44000 }, { 11, 0x54, 0x44000 }, { 11, 0x58, 0x44000 },
	{ 11, 0x68, 0x12345678 }, { 11, 0x6c, 0xc }, { 11, 0xac, 0xc }, { 11, 0xec, 0x3d0 },
	{ 11, 0xf0, 0x6186318 }, { 11, 0xf4, 0x18 }, { 11, 0x134, 0xc30c00 },
	{ 11, 0x138, 0xffffffff }, { 11, 0x13c, 0x80000001 }, { 11, 0x140, 0x80000001 },
	{ 11, 0x144, 0x80000001 }, { 11, 0x148, 0x7fffffff }, { 11, 0x14c, 0x7fffffff },
	{ 11, 0x150, 0x7fffffff }, { 11, 0x154, 0x80000001 }, { 11, 0x158, 0x80000001 },
	{ 11, 0x15c, 0x80000001 }, { 11, 0x160, 0x7fffffff }, { 11, 0x164, 0x7fffffff },
	{ 11, 0x168, 0x7fffffff }, { 11, 0x16c, 0xffffffff }, { 11, 0x170, 0xffffffff },
	{ 11, 0x174, 0xffffffff }, { 11, 0x178, 0xffffffff }, { 11, 0x17c, 0xffffffff },
	{ 11, 0x180, 0xffffffff }, { 11, 0x184, 0x80000001 }, { 11, 0x188, 0x80000001 },
	{ 11, 0x18c, 0x80000001 }, { 11, 0x190, 0x7fffffff }, { 11, 0x194, 0x7fffffff },
	{ 11, 0x198, 0x7fffffff }, { 11, 0x19c, 0x80000001 }, { 11, 0x1a0, 0x80000001 },
	{ 11, 0x1a4, 0x80000001 }, { 11, 0x1a8, 0x7fffffff }, { 11, 0x1ac, 0x7fffffff },
	{ 11, 0x1b0, 0x7fffffff }, { 11, 0x1b4, 0x80000001 }, { 11, 0x1b8, 0x80000001 },
	{ 11, 0x1bc, 0x80000001 }, { 11, 0x1c0, 0x7fffffff }, { 11, 0x1c4, 0x7fffffff },
	{ 11, 0x1c8, 0x7fffffff }, { 11, 0x1cc, 0x183c }, { 11, 0x1fc, 0x1 }, { 11, 0x200, 0xc },
	{ 11, 0x240, 0xc }, { 11, 0x280, 0xc }, { 11, 0x2c0, 0x80000001 },
	{ 11, 0x2c4, 0x80000001 }, { 11, 0x2c8, 0x80000001 }, { 11, 0x2cc, 0x7fffffff },
	{ 11, 0x2d0, 0x7fffffff }, { 11, 0x2d4, 0x7fffffff }, { 11, 0x2d8, 0x80000001 },
	{ 11, 0x2dc, 0x80000001 }, { 11, 0x2e0, 0x80000001 }, { 11, 0x2e4, 0x7fffffff },
	{ 11, 0x2e8, 0x7fffffff }, { 11, 0x2ec, 0x7fffffff }, { 11, 0x2f0, 0x80000001 },
	{ 11, 0x2f4, 0x80000001 }, { 11, 0x2f8, 0x80000001 }, { 11, 0x2fc, 0x7fffffff },
	{ 11, 0x300, 0x7fffffff }, { 11, 0x304, 0x7fffffff }, { 11, 0x308, 0x80000001 },
	{ 11, 0x30c, 0x80000001 }, { 11, 0x310, 0x80000001 }, { 11, 0x314, 0x7fffffff },
	{ 11, 0x318, 0x7fffffff }, { 11, 0x31c, 0x7fffffff }, { 11, 0x320, 0x80000001 },
	{ 11, 0x324, 0x80000001 }, { 11, 0x328, 0x80000001 }, { 11, 0x32c, 0x7fffffff },
	{ 11, 0x330, 0x7fffffff }, { 11, 0x334, 0x7fffffff }, { 11, 0x338, 0x80000001 },
	{ 11, 0x33c, 0x80000001 }, { 11, 0x340, 0x80000001 }, { 11, 0x344, 0x7fffffff },
	{ 11, 0x348, 0x7fffffff }, { 11, 0x34c, 0x7fffffff }, { 11, 0x37c, 0x1 },
	{ 11, 0x2e80, 0x40000000 }, { 12, 0x4, 0x1 }, { 12, 0x120, 0x12345678 },
	{ 12, 0x184, 0xffff }, { 12, 0x188, 0xffff }, { 12, 0x18c, 0xffff }, { 12, 0x190, 0xffff },
	{ 12, 0x194, 0xffff }, { 12, 0x198, 0xffff }, { 12, 0x19c, 0xffff }, { 12, 0x1a0, 0xffff },
	{ 12, 0x1a4, 0xffff }, { 12, 0x1a8, 0xffff }, { 12, 0x1ac, 0xffff }, { 12, 0x1b0, 0xffff },
	{ 12, 0x1b4, 0xffff }, { 12, 0x1b8, 0xffff }, { 12, 0x1bc, 0xffff }, { 12, 0x1c0, 0xffff },
	{ 12, 0x1c4, 0xffff }, { 12, 0x1c8, 0xffff }, { 12, 0x1cc, 0xffff }, { 12, 0x1d0, 0xffff },
	{ 12, 0x1d4, 0xffff }, { 12, 0x1d8, 0xffff }, { 12, 0x1dc, 0xffff }, { 12, 0x1e0, 0xffff },
	{ 12, 0x1e4, 0xffff }, { 12, 0x1e8, 0xffff }, { 12, 0x1ec, 0xffff }, { 12, 0x1f0, 0xffff },
	{ 12, 0x1f4, 0xffff }, { 12, 0x1f8, 0xffff }, { 12, 0x1fc, 0xffff },
	{ 12, 0x240, 0xfff00000 }, { 12, 0x244, 0xfff00000 }, { 13, 0x80, 0x18e0 },
	{ 13, 0xc4, 0x80000001 }, { 13, 0xc8, 0x80000001 }, { 13, 0xcc, 0x80000001 },
	{ 13, 0xd0, 0x7fffffff }, { 13, 0xd4, 0x7fffffff }, { 13, 0xd8, 0x7fffffff },
	{ 13, 0xdc, 0x80000001 }, { 13, 0xe0, 0x80000001 }, { 13, 0xe4, 0x80000001 },
	{ 13, 0xe8, 0x7fffffff }, { 13, 0xec, 0x7fffffff }, { 13, 0xf0, 0x7fffffff },
};

/*
 * The scaler's polyphase filters, one set per ratio bin (src/dst in Q19; a
 * step uses the first bin whose ratio is at least the step). Each set is a
 * symmetric kernel sampled at 9 source pixels around the output position,
 * 32 phases per pixel, in Q12 (4096 = 1.0), every phase summing to 4096:
 *
 *  - downscaling (ratio s > 1): sinc(x / s), a low-pass at the output's
 *    Nyquist frequency, under a Kaiser window of radius 3.5 s, beta 6.5;
 *  - upscaling (steps below 1): an interpolating 4-tap kernel, sinc(x) under
 *    a Kaiser window of radius 31/16, beta 5;
 *  - 1:1: the source passes through (centre tap 1.0, extra word 1.0).
 *
 * Phases 1-15 are stored as they are (index tap * 16 + phase); 17-31 are
 * their mirror images. Column 0 holds two phases: taps 4-8 are phase 0, taps
 * 0-3 are taps 1-4 of phase 16 (half a pixel), and .extra is that phase's
 * tap 0.
 *
 * Generated by tools/media/apple-scaler/msr23_filters.py, which documents
 * the design; regenerate rather than edit.
 */
static const struct msr23_filter msr23_filters[MSR23_FILTER_BINS] = {
	{ .ratio = 0x7ffff, .extra = 0, .taps = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, -177, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		2225, -63, -116, -160, -196, -223, -242, -255, -261, -262, -259, -251,
		-240, -227, -211, -194, 4096, 4086, 4056, 4015, 3953, 3873, 3776, 3665,
		3540, 3402, 3256, 3098, 2933, 2762, 2586, 2406, 0, 73, 156, 250,
		353, 467, 591, 724, 866, 1017, 1174, 1339, 1509, 1684, 1862, 2043,
		0, 0, 0, -9, -14, -21, -29, -38, -49, -61, -75, -90,
		-106, -123, -141, -159, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	} },
	{ .ratio = 0x80000, .extra = 4096, .taps = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096,
		4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	} },
	{ .ratio = 0x92492, .extra = 0, .taps = {
		-4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 79, 50, 45, 40, 35, 30, 25, 21,
		17, 13, 10, 7, 4, 2, -1, -2, -468, -181, -156, -130,
		-106, -83, -61, -40, -20, -2, 14, 29, 42, 54, 64, 72,
		2442, 312, 217, 128, 45, -32, -103, -167, -225, -276, -321, -360,
		-393, -420, -441, -457, 3578, 3575, 3560, 3535, 3500, 3457, 3404, 3341,
		3268, 3189, 3101, 3006, 2905, 2796, 2682, 2564, 412, 518, 629, 745,
		865, 989, 1116, 1246, 1379, 1512, 1647, 1782, 1917, 2051, 2184, 2313,
		-208, -235, -261, -288, -314, -339, -363, -386, -407, -426, -442, -456,
		-466, -473, -475, -474, 55, 61, 66, 71, 76, 80, 84, 88,
		91, 93, 94, 95, 94, 92, 89, 85, 0, -4, -4, -5,
		-5, -6, -6, -7, -7, -7, -7, -7, -7, -6, -6, -5,
	} },
	{ .ratio = 0xaaaaa, .extra = -5, .taps = {
		47, -1, -3, -4, -4, -5, -5, -6, -6, -6, -6, -6,
		-6, -6, -5, -5, -79, 79, 80, 81, 81, 80, 79, 77,
		75, 72, 69, 66, 63, 59, 55, 51, -242, -354, -339, -323,
		-306, -288, -269, -250, -231, -211, -191, -172, -152, -133, -115, -97,
		2325, 712, 625, 540, 458, 379, 304, 232, 164, 99, 38, -18,
		-71, -120, -165, -205, 3072, 3068, 3059, 3044, 3019, 2991, 2956, 2917,
		2870, 2819, 2762, 2700, 2632, 2562, 2487, 2408, 802, 895, 989, 1084,
		1182, 1280, 1378, 1478, 1577, 1675, 1773, 1870, 1965, 2059, 2150, 2239,
		-367, -379, -389, -397, -403, -406, -407, -406, -401, -393, -382, -368,
		-350, -329, -304, -275, 77, 74, 71, 66, 61, 55, 47, 39,
		30, 19, 8, -4, -17, -32, -47, -63, 0, 2, 3, 5,
		8, 10, 13, 15, 18, 22, 25, 28, 32, 36, 40, 43,
	} },
	{ .ratio = 0xccccc, .extra = 15, .taps = {
		57, 54, 51, 49, 46, 43, 41, 38, 35, 32, 30, 27,
		25, 22, 20, 18, -274, -54, -43, -32, -22, -13, -3, 5,
		13, 21, 27, 34, 39, 45, 49, 53, 137, -320, -327, -333,
		-336, -338, -339, -338, -335, -332, -326, -320, -313, -304, -295, -285,
		2120, 1023, 956, 889, 822, 757, 693, 630, 568, 508, 450, 393,
		338, 284, 233, 184, 2552, 2552, 2546, 2536, 2525, 2509, 2489, 2467,
		2441, 2412, 2379, 2341, 2304, 2263, 2218, 2171, 1092, 1160, 1229, 1298,
		1367, 1435, 1503, 1571, 1637, 1703, 1767, 1831, 1892, 1952, 2011, 2067,
		-310, -299, -286, -270, -253, -233, -211, -187, -160, -131, -100, -66,
		-30, 8, 48, 91, -66, -78, -90, -103, -116, -129, -143, -157,
		-170, -184, -198, -211, -225, -238, -250, -263, 56, 58, 60, 62,
		63, 65, 66, 67, 67, 67, 67, 67, 66, 64, 62, 60,
	} },
	{ .ratio = 0x100000, .extra = 53, .taps = {
		-121, 6, 11, 16, 20, 25, 29, 32, 36, 39, 42, 44,
		46, 48, 50, 51, -254, -244, -238, -231, -224, -216, -208, -200,
		-192, -183, -175, -166, -157, -148, -139, -130, 545, -25, -49, -71,
		-92, -112, -131, -149, -165, -180, -194, -207, -219, -229, -238, -247,
		1852, 1208, 1164, 1119, 1074, 1029, 984, 939, 894, 849, 804, 760,
		716, 673, 630, 587, 2092, 2090, 2088, 2082, 2077, 2067, 2055, 2044,
		2029, 2012, 1996, 1975, 1956, 1931, 1905, 1879, 1252, 1296, 1339, 1382,
		1424, 1465, 1506, 1545, 1584, 1622, 1658, 1694, 1728, 1761, 1793, 1823,
		0, 26, 53, 82, 111, 142, 174, 207, 241, 276, 312, 349,
		386, 425, 464, 505, -250, -255, -260, -265, -269, -272, -274, -276,
		-277, -277, -277, -275, -273, -270, -265, -260, 0, -6, -12, -18,
		-25, -32, -39, -46, -54, -62, -70, -78, -87, -95, -104, -112,
	} },
	{ .ratio = 0x155555, .extra = -115, .taps = {
		-200, -182, -179, -175, -171, -167, -163, -158, -154, -149, -145, -140,
		-135, -130, -125, -120, 82, -131, -139, -146, -153, -160, -166, -171,
		-177, -181, -185, -189, -192, -195, -197, -199, 789, 382, 360, 337,
		315, 294, 272, 251, 231, 211, 191, 171, 153, 134, 116, 99,
		1433, 1154, 1131, 1109, 1086, 1063, 1039, 1015, 991, 966, 941, 916,
		891, 865, 840, 814, 1550, 1550, 1549, 1547, 1543, 1538, 1535, 1527,
		1522, 1513, 1504, 1496, 1485, 1475, 1461, 1449, 1175, 1196, 1217, 1237,
		1256, 1275, 1293, 1311, 1327, 1344, 1359, 1374, 1388, 1401, 1413, 1425,
		405, 428, 452, 475, 499, 523, 547, 571, 595, 619, 644, 668,
		692, 716, 741, 765, -122, -113, -104, -94, -83, -72, -61, -48,
		-36, -23, -9, 5, 19, 34, 50, 65, -185, -188, -191, -194,
		-196, -198, -200, -202, -203, -204, -204, -205, -205, -204, -203, -202,
	} },
	{ .ratio = 0x200000, .extra = -72, .taps = {
		106, -5, -11, -16, -21, -26, -31, -36, -40, -45, -49, -53,
		-57, -61, -65, -69, 392, 229, 220, 211, 203, 194, 186, 177,
		169, 161, 152, 144, 136, 129, 121, 113, 695, 536, 527, 517,
		507, 498, 488, 479, 469, 459, 450, 440, 430, 421, 411, 402,
		891, 802, 796, 789, 783, 776, 769, 762, 755, 748, 741, 733,
		726, 718, 711, 703, 912, 912, 912, 912, 910, 910, 908, 908,
		905, 905, 904, 902, 901, 898, 896, 894, 808, 814, 820, 826,
		832, 838, 843, 849, 854, 859, 864, 869, 873, 878, 882, 887,
		546, 555, 565, 574, 584, 593, 603, 612, 622, 631, 640, 650,
		659, 668, 677, 686, 238, 247, 256, 266, 275, 284, 294, 303,
		313, 323, 332, 342, 352, 362, 372, 382, 0, 6, 11, 17,
		23, 29, 36, 42, 49, 55, 62, 69, 76, 83, 91, 98,
	} },
};

/* De-ringing constants of a scaled axis, in pairs (two channels): downscale and upscale. */
static const u32 msr23_dering[2][13] = {
	{ 0xcd, 0x266, 0x39a, 0x4cd, 0x355e3a6, 0x400, 0xb33, 0x266, 0x733, 0x11c79b,
	  0x1aa98e, 0x666, 0x333 },
	{ 0xcd, 0x266, 0x39a, 0x99a, 0xaaaaab, 0x400, 0xb33, 0x266, 0x733, 0x11c79b,
	  0x1aa98e, 0x666, 0x333 },
};

/*
 * The 2x2 ordered dither (Bayer [[0, 2], [3, 1]], tiled 4x4) for a 10-bit
 * source written at 8 bits.
 */
static const u32 msr23_dither[16] = { 0, 2, 0, 2, 3, 1, 3, 1, 0, 2, 0, 2, 3, 1, 3, 1 };

/**
 * struct msr23_fmt - a pixel format as the engine sees it
 * @fourcc: V4L2 pixel format
 * @rgb: R'G'B' (else Y'CbCr or grey)
 * @grey: luma only
 * @sub: chroma subsampling code (0 4:4:4, 1 4:2:2, 3 4:2:0)
 * @code: memory format code of plane 0 and of the chroma plane
 * @swizzle: component order (0 identity, else CbCr)
 * @hs: horizontal chroma subsampling
 * @vs: vertical chroma subsampling
 * @bits: bits per component of the four channels (0: 8)
 */
struct msr23_fmt {
	u32 fourcc;
	bool rgb, grey;
	u8 sub;
	u8 code[2];
	u8 swizzle, hs, vs;
	u8 bits[4];
};

static const struct msr23_fmt msr23_fmts[] = {
	{ V4L2_PIX_FMT_ABGR32, .rgb = true, .code = { 0x1e } },
	{ V4L2_PIX_FMT_NV12, .sub = 3, .code = { 1, 6 }, .swizzle = 2, .hs = 2, .vs = 2 },
	{ V4L2_PIX_FMT_NV12M, .sub = 3, .code = { 1, 6 }, .swizzle = 2, .hs = 2, .vs = 2 },
	{ V4L2_PIX_FMT_NV16, .sub = 1, .code = { 1, 6 }, .swizzle = 2, .hs = 2, .vs = 1 },
	{ V4L2_PIX_FMT_NV16M, .sub = 1, .code = { 1, 6 }, .swizzle = 2, .hs = 2, .vs = 1 },
	{ V4L2_PIX_FMT_NV24, .code = { 1, 6 }, .swizzle = 2, .hs = 1, .vs = 1 },
	{ V4L2_PIX_FMT_P010, .sub = 3, .code = { 2, 0x12 }, .swizzle = 2, .hs = 2, .vs = 2,
	  .bits = { 10, 10, 10, 0 } },
	{ V4L2_PIX_FMT_GREY, .grey = true, .code = { 1 }, .swizzle = 2 },
};

static const struct msr23_fmt *msr23_fmt(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(msr23_fmts); i++)
		if (msr23_fmts[i].fourcc == fourcc)
			return &msr23_fmts[i];
	return NULL;
}

bool msr23_format_ok(u32 fourcc)
{
	return msr23_fmt(fourcc);
}
EXPORT_SYMBOL_IF_KUNIT(msr23_format_ok);

static u8 msr23_bits(const struct msr23_fmt *f, unsigned int c)
{
	return f->bits[c] ?: 8;
}

#define W(img, o, v)		((img)[(o) / 4] = (v))

/*
 * One side's DMA words: plane codes, strides, the region's size and its
 * origin per plane, the planes' sizes, swizzle (block 1 for the source, block
 * 14 for the destination). The origins are in samples of each plane (y << 16
 * | x); the plane pointers stay at the start of the planes.
 */
static void msr23_side(u32 *img, const struct msr23_pic *p, const struct v4l2_rect *r,
		       const struct msr23_fmt *f, bool dst)
{
	u32 b = dst ? MSR23_B14 : MSR23_B1;
	u32 so = dst ? 0x40 : 0x58, wo = dst ? 0x5c : 0x7c, oo = dst ? 0x4c : 0x68;
	u32 po = dst ? 0x88 : 0xb4, ho = dst ? 0x94 : 0xc4;
	bool chroma = !f->rgb && !f->grey;
	u32 cw = chroma ? p->width / f->hs : p->width, ch = chroma ? p->height / f->vs : p->height;
	unsigned int i;

	for (i = 0; i < 2; i++)
		W(img, b + 4 + 4 * i, (dst ? 0x8000 : 0) | f->code[i]);
	W(img, b + so, p->stride[0]);
	W(img, b + so + 4, chroma ? p->stride[1] : 0);
	W(img, b + wo, r->width);
	W(img, b + wo + 4, r->height);
	W(img, b + oo, (u32)r->top << 16 | r->left);
	if (chroma)
		W(img, b + oo + 4, (u32)r->top / f->vs << 16 | (u32)r->left / f->hs);
	for (i = 0; i < 3; i++) {
		W(img, b + po + 4 * i, i == 1 ? cw : p->width);
		W(img, b + ho + 4 * i, i == 1 ? ch : p->height);
	}
	W(img, b + (dst ? 0x58 : 0x78), f->swizzle ? 0x03000102 : 0x03020100);
}

static u32 msr23_dup(s16 v)
{
	return (u16)v | (u32)(u16)v << 16;
}

/*
 * One axis of the scaler (block 8: the vertical half at +0x80, the
 * horizontal half at +0x400): step and inverse in Q22 (the vertical half at
 * 20-bit precision), initial phase when upscaling, de-ringing, and the filter
 * of the first bin whose ratio is at least the step.
 * A Y'CbCr source at 1:1 runs no filter.
 */
static int msr23_axis(u32 *img, u32 src, u32 dst, bool horizontal, bool yuv_src)
{
	u32 b = MSR23_B8 + (horizontal ? 0x400 : 0x80);
	unsigned int prec = horizontal ? 22 : 20, i;
	u32 step = (u32)div_u64((u64)src << prec, dst) << (22 - prec);
	u32 inv = (u32)div_u64((u64)dst << prec, src) << (22 - prec);
	bool scaled = src != dst;

	W(img, b, yuv_src ? 2 : 0);
	if (step < BIT(22)) {
		s32 init = ((s32)step - (s32)BIT(22)) >> 1;

		W(img, b + 0x4, init & 0xffffff);
		W(img, b + 0x8, (yuv_src ? init >> 1 : init) & 0xffffff);
	}
	W(img, b + 0xc, step);
	W(img, b + 0x10, step);
	W(img, b + 0x14, inv);
	W(img, b + 0x18, inv);
	if (scaled)
		for (i = 0; i < ARRAY_SIZE(msr23_dering[0]); i++) {
			W(img, b + 0x1c + 8 * i, msr23_dering[step < BIT(22)][i]);
			W(img, b + 0x20 + 8 * i, msr23_dering[step < BIT(22)][i]);
		}
	W(img, b + (horizontal ? 0xe0 : 0xfc), 0x12345678);
	if (scaled || !yuv_src) {
		const struct msr23_filter *f = NULL;

		for (i = 0; i < MSR23_FILTER_BINS; i++)
			if (msr23_filters[i].ratio >= step >> 3) {
				f = &msr23_filters[i];
				break;
			}
		if (!f)
			return -EINVAL;
		for (i = 0; i < MSR23_FILTER_TAPS; i++)
			W(img, b + 0x100 + 4 * i, msr23_dup(f->taps[i]));
		W(img, b + 0x340, msr23_dup(f->extra));
	}
	return 0;
}

/*
 * Block 0 +0x78..+0x84: the channels' component sizes, source in the low
 * half, destination in the high half. A conversion at 1:1 (the matrix in
 * block 3) takes 0x30 on the source side; without a conversion, as with a
 * scaled one, the source's own sizes (with 0x30, values above half scale
 * come out 1 lower: v * 255 / 256).
 */
static void msr23_map(u32 *img, const struct msr23_fmt *s, const struct msr23_fmt *d, bool csc11)
{
	unsigned int c;

	for (c = 0; c < 4; c++) {
		u32 lo, hi;

		if (csc11)
			lo = 0x30;
		else if (!s->rgb)
			lo = c ? 0x20 | (c < 3 ? msr23_bits(s, c) : 0) : msr23_bits(s, c);
		else
			lo = s->bits[0] ? msr23_bits(s, c) : 8;
		if (d->rgb)
			hi = 8;
		else
			hi = c ? 0x20 | (c < 3 ? msr23_bits(d, c) : 0) : msr23_bits(d, c);
		W(img, MSR23_B0 + 0x78 + 4 * c, hi << 16 | lo);
	}
}

/* The colour matrix: block 3 at 1:1, block 11 when the job also scales. */
static int msr23_matrix(u32 *img, const struct msr23_job *job, bool to_yuv, bool scaled)
{
	struct asc_csc csc;
	unsigned int r, c;
	u32 b;

	if (asc_csc_compute(job->enc,
			    job->full ? V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE,
			    !to_yuv, &csc))
		return -EINVAL;
	b = scaled ? MSR23_B11 + 0x14 : MSR23_B3 + 0x34;
	for (r = 0; r < 3; r++)
		for (c = 0; c < 3; c++)
			W(img, b + 12 * r + 4 * c, (u16)csc.coef[r][c]);
	for (r = 0; r < 3; r++) {
		/* offsets: block 11 in 2^-24 of full scale (26 bits), block 3 in 2^-16 (17 bits) */
		if (to_yuv)
			W(img, b + 0x24 + 4 * r, scaled ? (u32)csc.post[r] : (u32)csc.post[r] >> 8);
		else
			W(img, b - 0xc + 4 * r, scaled ? (u32)csc.pre[r] & 0x3ffffff :
			  (u32)(csc.pre[r] >> 8) & 0x1ffff);
	}
	return 0;
}

static bool msr23_size_ok(const struct msr23_pic *p)
{
	return !((p->width | p->height) & 1) && min(p->width, p->height) >= MSR23_MIN_SIZE &&
	       p->width <= MSR23_MAX_WIDTH && (u64)p->width * p->height <= MSR23_MAX_PIXELS;
}

/* A job's crop or compose rectangle: all of the picture when its width is 0. */
static void msr23_region(const struct msr23_pic *p, const struct v4l2_rect *r,
			 struct v4l2_rect *out)
{
	if (r->width)
		*out = *r;
	else
		*out = (struct v4l2_rect){ 0, 0, p->width, p->height };
}

/* Even, at least the minimum size, and inside the picture. */
static bool msr23_region_ok(const struct msr23_pic *p, const struct v4l2_rect *r)
{
	return r->left >= 0 && r->top >= 0 && !((r->left | r->top | r->width | r->height) & 1) &&
	       min(r->width, r->height) >= MSR23_MIN_SIZE &&
	       (u64)r->left + r->width <= p->width && (u64)r->top + r->height <= p->height;
}

/*
 * Block 0 +0x64 bits 3-6: the rotation (bits 3-4: 1, 2, 3 quarter turns
 * clockwise), then a mirror of the result left to right (bit 5) and top to
 * bottom (bit 6). A mirror both ways is a half turn, and a half turn and one
 * mirror is the other mirror: each orientation is sent in one form only.
 */
static u32 msr23_orient(const struct msr23_job *job)
{
	unsigned int rot = job->rotate / 90;
	bool h = job->hflip, v = job->vflip;

	if (h && v) {
		rot = (rot + 2) & 3;
		h = false;
		v = false;
	}
	if (rot == 2 && (h || v)) {
		rot = 0;
		swap(h, v);
	}
	return rot << 3 | h << 5 | v << 6;
}

/* The source region's size as the pipe sees it: after the rotation. */
static void msr23_turned(const struct msr23_job *job, const struct v4l2_rect *c, u32 *w, u32 *h)
{
	bool quarter = job->rotate == 90 || job->rotate == 270;

	*w = quarter ? c->height : c->width;
	*h = quarter ? c->width : c->height;
}

/**
 * msr23_check - whether the builder covers a job
 * @job: the conversion
 *
 * Return: 0, or -EINVAL for a format, size, rectangle, rotation or ratio the
 * builder was not checked over.
 */
int msr23_check(const struct msr23_job *job)
{
	const struct msr23_pic *sp = &job->src, *dp = &job->dst;
	const struct msr23_fmt *s = msr23_fmt(sp->fourcc), *d = msr23_fmt(dp->fourcc);
	struct v4l2_rect c, o;
	u32 sw, sh, dw, dh;

	if (!s || !d || !msr23_size_ok(sp) || !msr23_size_ok(dp) || s->grey != d->grey)
		return -EINVAL;
	if (job->rotate % 90 || job->rotate > 270)
		return -EINVAL;
	/* a quarter turn of 4:2:2 moves its subsampling to the other axis: not checked */
	if (job->rotate % 180 && (s->sub == 1 || d->sub == 1))
		return -EINVAL;
	msr23_region(sp, &job->crop, &c);
	msr23_region(dp, &job->compose, &o);
	if (!msr23_region_ok(sp, &c) || !msr23_region_ok(dp, &o))
		return -EINVAL;
	msr23_turned(job, &c, &sw, &sh);
	dw = o.width;
	dh = o.height;
	if (sw > MSR23_MAX_RATIO * dw || dw > MSR23_MAX_RATIO * sw ||
	    sh > MSR23_MAX_RATIO * dh || dh > MSR23_MAX_RATIO * sh)
		return -EINVAL;
	/*
	 * A conversion at 1:1 with a mirror or a half turn: not checked on
	 * hardware (with a quarter turn it is; see msr23_build()).
	 */
	if (s->rgb != d->rgb && sw == dw && sh == dh && !(job->rotate % 180) &&
	    (msr23_orient(job) & 0x70))
		return -EINVAL;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(msr23_check);

/**
 * msr23_build - the register image of a job
 * @job: the conversion
 * @img: MSR23_IMG_LEN bytes, zeroed by the caller
 * @out: NC sizes and plane counts
 *
 * The pointers and the frame id are left zero: the caller writes them
 * (MSR23_PTR_*, MSR23_FRAME_ID).
 *
 * Return: 0, or -EINVAL for a job the builder does not cover.
 */
int msr23_build(const struct msr23_job *job, u32 *img, struct msr23_built *out)
{
	const struct msr23_pic *sp = &job->src, *dp = &job->dst;
	const struct msr23_fmt *s = msr23_fmt(sp->fourcc), *d = msr23_fmt(dp->fourcc);
	struct v4l2_rect c, o, whole;
	struct msr23_pic sub;
	u32 sw, sh, dw, dh, ybpp;
	bool yuv_src, yuv_dst, csc_on, scaled, csc_pre, ups, wide;
	u64 px;
	u32 cds, k, rows;
	unsigned int i;
	int ret;

	ret = msr23_check(job);
	if (ret)
		return ret;
	/*
	 * The source is read rotated: from the reader on, the pipe (scaler,
	 * window) works in the destination's orientation.
	 */
	msr23_region(sp, &job->crop, &c);
	msr23_region(dp, &job->compose, &o);
	msr23_turned(job, &c, &sw, &sh);
	dw = o.width;
	dh = o.height;

	yuv_src = !s->rgb;
	yuv_dst = !d->rgb;
	csc_on = yuv_src != yuv_dst;
	scaled = sw != dw || sh != dh;
	/*
	 * A conversion at 1:1 runs its matrix in block 3, ahead of the scaler;
	 * with a quarter turn it runs where a scaled job's does (block 11): in
	 * block 3 the rotated output is wrong.
	 */
	csc_pre = csc_on && !scaled && !(job->rotate % 180);
	/* chroma upsampling: a subsampled Y'CbCr source */
	ups = yuv_src && (s->hs > 1 || s->vs > 1);
	px = max((u64)sw * sh, (u64)dw * dh);

	for (i = 0; i < ARRAY_SIZE(msr23_defaults); i++) {
		const struct msr23_block *b = NULL;
		unsigned int j;

		for (j = 0; j < MSR23_NBLOCKS && !b; j++)
			if (msr23_blocks[j].id == msr23_defaults[i].block)
				b = &msr23_blocks[j];
		W(img, b->off + msr23_defaults[i].off, msr23_defaults[i].val);
	}

	/* Block 0: the pipe. */
	W(img, MSR23_B0 + 0xc, 0xffff | (yuv_src && csc_on ? BIT(31) : 0));
	/* a time budget: 40e6 (80e6 above 1080p) + 8/15 per pixel of the larger side */
	W(img, MSR23_B0 + 0x18, 40000000U * (px > 1920 * 1080 ? 2 : 1) + (u32)div_u64(px * 8, 15));
	W(img, MSR23_B0 + 0x64, 0x38d800 | s->rgb | s->sub << 1 | d->rgb << 7 | d->sub << 8 |
	  (csc_pre ? BIT(16) : 0) | (ups ? BIT(13) : 0) | msr23_orient(job));
	/* scaler enables per axis: off at 1:1, the low 3 bits upscaling, all 5 downscaling */
	W(img, MSR23_B0 + 0x68, (sw == dw ? 0 : (dw > sw ? 0x7 : 0x1f) << 11) |
	  (sh == dh ? 0 : (dh > sh ? 0x7 : 0x1f) << 6));
	/* chroma downsampling of a subsampled Y'CbCr destination */
	cds = 0xc | (yuv_dst && d->hs == 2 ? 1 : 0) | (yuv_dst && d->vs == 2 ? 2 : 0);
	W(img, MSR23_B0 + 0x6c, cds << 16 | (csc_on && !csc_pre ? 3 : 0));
	W(img, MSR23_B0 + 0x70, (dh - 1) << 16 | (dw - 1));
	msr23_map(img, s, d, csc_pre);

	msr23_side(img, sp, &c, s, false);
	/*
	 * The destination rectangle is written as a picture of its own: its
	 * size, at an offset into the planes the caller adds to the pointers.
	 */
	sub = *dp;
	sub.width = o.width;
	sub.height = o.height;
	whole = (struct v4l2_rect){ 0, 0, o.width, o.height };
	msr23_side(img, &sub, &whole, d, true);
	ybpp = d->rgb ? 4 : msr23_bits(d, 0) > 8 ? 2 : 1;
	out->dst_off[0] = (u32)o.top * dp->stride[0] + (u32)o.left * ybpp;
	out->dst_off[1] = d->rgb || d->grey ? 0 :
			  (u32)o.top / d->vs * dp->stride[1] + (u32)o.left / d->hs * 2 * ybpp;

	ret = msr23_axis(img, sh, dh, false, yuv_src);
	if (!ret)
		ret = msr23_axis(img, sw, dw, true, yuv_src);
	if (ret)
		return ret;
	W(img, MSR23_B8 + 0x4e4, 1);

	/*
	 * Block 3: chroma upsampling terms; the matrix at 1:1. Block 11: the
	 * matrix when scaling too.
	 */
	for (i = 0; i < 4; i++)
		W(img, MSR23_B3 + (i < 2 ? 0x8 : 0x10) + 4 * i, ups ? 0x800 : 0);
	if (csc_on) {
		ret = msr23_matrix(img, job, yuv_dst, !csc_pre);
		if (ret)
			return ret;
	}

	/*
	 * Block 12: chroma downsampling per subsampled axis, and the dither of a
	 * 10-bit source written at 8 bits.
	 */
	W(img, MSR23_B12 + 0xc, yuv_dst && d->hs == 2 ? 0x1000 : 0);
	W(img, MSR23_B12 + 0x1c, yuv_dst && d->vs == 2 ? 0x1000 : 0);
	if (msr23_bits(s, 0) > msr23_bits(d, 0)) {
		W(img, MSR23_B12 + 0x180, 0x02020200);
		for (i = 0; i < ARRAY_SIZE(msr23_dither); i++)
			W(img, MSR23_B12 + 0x200 + 4 * i, msr23_dither[i]);
	}

	/*
	 * Spill (NC) buffers in 16 KiB pages, for the units' line buffers:
	 * about 0.018 page per source row and 0.004 per destination row at
	 * 8 bits (24 pages at 1080p, 43 for 4K -> 1080p), twice that with
	 * 16-bit samples. Larger than needed is harmless (the jobs give the
	 * same output), so round generously, and never below 48 pages per
	 * byte of sample.
	 */
	wide = s->bits[0] || d->bits[0];
	k = wide ? 2 : 1;
	rows = 320 * sh + 64 * dh;
	out->nc0 = max(DIV_ROUND_UP(rows * k, MSR23_NC_PAGE) + 2, 48 * k);
	out->nc1 = wide ? 8 : 4;
	W(img, MSR23_B0 + 0xec, out->nc1 << 16 | out->nc0);
	out->planes[0] = s->code[1] ? 2 : 1;
	out->planes[1] = d->code[1] ? 2 : 1;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(msr23_build);
