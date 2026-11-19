// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple ProRes (APR) V4L2 memory-to-memory codec.
 *
 * The driver writes job descriptors into a 128 KiB DMA ring and advances the
 * producer head; the engine works through the ring and reports progress with
 * a consumer tail and a completion interrupt. Up to APR_MAX_INFLIGHT decode
 * jobs sit in the ring at once: a job's buffers leave the m2m queues when its
 * descriptor is published, so v4l2-m2m can run the next job while the engine
 * is busy. The tail moves when the engine fetches a descriptor, not when it
 * has finished the frame, so decode jobs complete in ring order once the
 * engine reports their job id as finished in ST1. An encode descriptor
 * carries no job id: it only enters an empty ring and completes on its
 * interrupt, as a lone job.
 * The encoder consumes semi-planar 4:2:2 or 4:4:4 frames and produces one
 * complete ProRes frame per buffer. The decoder exposes a second stateful
 * node with the queue directions reversed and linear color output.
 *
 * Based on Acelogic's J700 ProRes diagnostic, which first encoded on this
 * engine under Linux, and on the m1n1 proxyclient ProRes experiment (MIT).
 */
#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "../apple-video-nr.h"
#include "apple-prores.h"

#define APR_NAME		"apple-prores"
#define APR_MIN_W		16
#define APR_MIN_H		16
#define APR_MAX_W		U16_MAX
#define APR_MAX_H		U16_MAX
#define APR_MAX_PIXELS		0x10000000
#define APR_TIMEOUT_MS		1000
#define APR_POLL_MS		4
#define APR_AUTOSUSPEND_MS	500

struct apr_fmt {
	u32 fourcc;
	enum apr_input input;
	u8 bytes;	/* per sample */
	bool is444;
};

static const struct apr_fmt apr_raw_fmts[] = {
	{ V4L2_PIX_FMT_NV16, APR_INPUT_422_8, 1, false },
	{ V4L2_PIX_FMT_P216, APR_INPUT_422_16, 2, false },
	{ V4L2_PIX_FMT_NV24, APR_INPUT_444_8, 1, true },
	{ V4L2_PIX_FMT_P416, APR_INPUT_444_16, 2, true },
};


/*
 * Settings the engine and its AXI-to-fabric bridge need before the first
 * job, as read-modify-write (offset, mask, value), in order. T8140 values.
 */
static const struct apr_tunable apr_t8140_engine_tunables[] = {
	{ 0x008, 0x00000008, 0x00000008 },
	{ 0x118, 0x0fff0fff, 0x00000280 },
	{ 0x11c, 0x0fff0fff, 0x000001e0 },
	{ 0x148, 0x00000001, 0x00000001 },
	{ 0x180, 0x8fff3fff, 0x800a04ff },
	{ 0x184, 0x000000ff, 0x00000001 },
	{ 0x188, 0x0fff0fff, 0x02c00040 },
	{ 0x1c0, 0x8fff3fff, 0x800404ff },
	{ 0x1c4, 0x000000ff, 0x00000003 },
	{ 0x1c8, 0x0fff0fff, 0x00000140 },
	{ 0x200, 0x8fff3fff, 0x800504ff },
	{ 0x204, 0x000000ff, 0x00000002 },
	{ 0x208, 0x0fff0fff, 0x01400140 },
	{ 0x240, 0x8fff3fff, 0x800004ff },
	{ 0x244, 0x000000ff, 0x00000007 },
	{ 0x248, 0x000000ff, 0x00000040 },
	{ 0x280, 0x8fff3fff, 0x800104ff },
	{ 0x284, 0x000000ff, 0x00000006 },
	{ 0x288, 0x000000ff, 0x0000007e },
	{ 0x2c0, 0x8fff3fff, 0x800204ff },
	{ 0x2c4, 0x000000ff, 0x00000005 },
	{ 0x2c8, 0x000000ff, 0x00000040 },
	{ 0x300, 0x8fff3fff, 0x800304ff },
	{ 0x304, 0x000000ff, 0x00000004 },
	{ 0x308, 0x000000ff, 0x00000040 },
	{ 0x340, 0xbfffff86, 0x81000006 },
	{ 0x344, 0xffffffff, 0x0b0a0908 },
	{ 0x348, 0x0000ffff, 0x00000720 },
	{ 0x34c, 0x000fffff, 0x000014ff },
	{ 0x350, 0x00ffffff, 0x00928170 },
	{ 0x380, 0x8fff3fff, 0x800b08ff },
	{ 0x384, 0x000000ff, 0x0000000c },
	{ 0x388, 0x0fff0fff, 0x02800040 },
	{ 0x394, 0xffffffc0, 0x00200000 },
	{ 0x3c0, 0x8fff3fff, 0x800b08ff },
	{ 0x3c4, 0x000000ff, 0x00000019 },
	{ 0x3c8, 0x0fff0fff, 0x03000010 },
	{ 0x4c0, 0x800f3fff, 0x800b0880 },
	{ 0x4c4, 0x000000ff, 0x0000001b },
	{ 0x4c8, 0x0fff0fff, 0x01f80008 },
	{ 0x500, 0x800f3fff, 0x800604ff },
	{ 0x504, 0x000000ff, 0x0000000d },
	{ 0x508, 0x0fff0fff, 0x00000100 },
	{ 0x540, 0x800f3fff, 0x80000500 },
	{ 0x544, 0x000000ff, 0x00000011 },
	{ 0x548, 0x000000ff, 0x0000007e },
	{ 0x580, 0x800f3fff, 0x80010500 },
	{ 0x584, 0x000000ff, 0x00000010 },
	{ 0x588, 0x000000ff, 0x0000007e },
	{ 0x5c0, 0x800f3fff, 0x80020500 },
	{ 0x5c4, 0x000000ff, 0x0000000f },
	{ 0x5c8, 0x000000ff, 0x00000040 },
	{ 0x600, 0x800f3fff, 0x80030500 },
	{ 0x604, 0x000000ff, 0x0000000e },
	{ 0x608, 0x000000ff, 0x00000040 },
	{ 0x640, 0x802ff040, 0x80070040 },
	{ 0x644, 0x00ffffff, 0x00071716 },
	{ 0x648, 0xffff0003, 0x00ff0003 },
	{ 0x650, 0x0fff0fff, 0x01e00008 },
	{ 0x680, 0x802ff040, 0x80081040 },
	{ 0x684, 0x00ffffff, 0x00071514 },
	{ 0x688, 0xffff0003, 0x00ff0003 },
	{ 0x690, 0x0fff0fff, 0x01e80010 },
	{ 0x6c0, 0x802ff040, 0x80092040 },
	{ 0x6c4, 0x00ffffff, 0x00071312 },
	{ 0x6c8, 0xffff0003, 0x00ff0003 },
	{ 0x6d0, 0x0fff0fff, 0x01f80008 },
	{ 0x700, 0x800f3fff, 0x800b0880 },
	{ 0x704, 0x000000ff, 0x00000018 },
	{ 0x708, 0x0fff0fff, 0x01000080 },
	{ 0x714, 0xffffffc0, 0x00200000 },
	{ 0x740, 0x800f3fff, 0x800b0880 },
	{ 0x744, 0x000000ff, 0x0000001a },
	{ 0x748, 0x0fff0fff, 0x01800008 },
	{ 0x780, 0x800f3fff, 0x800b0880 },
	{ 0x784, 0x000000ff, 0x0000001c },
	{ 0x788, 0x0fff0fff, 0x01e00008 },
	{ 0x7c0, 0x800f3fff, 0x800b0880 },
	{ 0x7c4, 0x000000ff, 0x0000001d },
	{ 0x7c8, 0x0fff0fff, 0x01e80008 },
};

static const struct apr_tunable apr_t8140_wrapper_tunables[] = {
	{ 0x0000, 0x800000ff, 0x8000001a },
	{ 0x0004, 0x80ff00ff, 0x800b000c },
	{ 0x0008, 0x80ff00ff, 0x800b000c },
	{ 0x000c, 0x80ff00ff, 0x800b000c },
	{ 0x0010, 0x80ff00ff, 0x80000003 },
	{ 0x0014, 0x80ff00ff, 0x80000003 },
	{ 0x0018, 0x800000ff, 0x8000000f },
	{ 0x001c, 0x800000ff, 0x80000020 },
	{ 0x0020, 0x80ff00ff, 0x8014000c },
	{ 0x0024, 0x80ff00ff, 0x801a0006 },
	{ 0x0028, 0x80ff00ff, 0x80090003 },
	{ 0x002c, 0x80ff00ff, 0x80090003 },
	{ 0x0030, 0x800000ff, 0x80000013 },
	{ 0x0034, 0x800000ff, 0x8000000d },
	{ 0x0038, 0x800000ff, 0x8000000d },
	{ 0x0100, 0x000000ff, 0x000000ff },
	{ 0x0108, 0x00000006, 0x00000000 },
	{ 0x0304, 0x3f3f0000, 0x04040000 },
	{ 0x0314, 0x80000000, 0x80000000 },
	{ 0x031c, 0x80000000, 0x80000000 },
	{ 0x0408, 0xffff0020, 0x00100020 },
	{ 0x0700, 0x00000400, 0x00000400 },
	{ 0x0748, 0x07ff07ff, 0x00100010 },
	{ 0x0800, 0x00000400, 0x00000400 },
	{ 0x0848, 0x07ff07ff, 0x00800040 },
	{ 0x0a00, 0xffffffff, 0x7fffffff },
	{ 0x0a04, 0x00000100, 0x00000100 },
	{ 0x4008, 0x00000001, 0x00000000 },
	{ 0x400c, 0x00000001, 0x00000000 },
	{ 0x4014, 0x00000001, 0x00000000 },
	{ 0x4018, 0x00000001, 0x00000000 },
	{ 0x401c, 0x00000001, 0x00000001 },
};

/*
 * T6000 constants: Copyright The Asahi Linux Contributors.
 * MIT license: see m1n1-LICENSE.
 * MIT m1n1 f3ac615eb1376d3bbba24cd84c414b9e41892adb,
 * proxyclient/experiments/prores.py:42-89, in the published order.
 * APR a0 has no wrapper or SCODEC-system initialization table.
 */
static const struct apr_tunable apr_t6000_engine_tunables[] = {
	{ 0x118, 0x07ff07ff, 0x00000600 },
	{ 0x148, 0x00000001, 0x00000001 },
	{ 0x160, 0x800f3fff, 0x800a04ff },
	{ 0x164, 0x07ff07ff, 0x07800080 },
	{ 0x170, 0x800f3fff, 0x800404ff },
	{ 0x174, 0x07ff07ff, 0x06000080 },
	{ 0x180, 0x800f3fff, 0x800504ff },
	{ 0x184, 0x07ff07ff, 0x06800080 },
	{ 0x190, 0x800f3fff, 0x800004ff },
	{ 0x194, 0x000000ff, 0x00000040 },
	{ 0x1a0, 0x800f3fff, 0x800104ff },
	{ 0x1a4, 0x000000ff, 0x00000080 },
	{ 0x1b0, 0x800f3fff, 0x800204ff },
	{ 0x1b4, 0x000000ff, 0x00000040 },
	{ 0x1c0, 0x800f3fff, 0x800304ff },
	{ 0x1c4, 0x000000ff, 0x00000040 },
	{ 0x1d0, 0xbc00ff86, 0xa4000786 },
	{ 0x1d4, 0x000000ff, 0x00000020 },
	{ 0x1d8, 0x000000ff, 0x000000ff },
	{ 0x1dc, 0x00ffffff, 0x00928170 },
	{ 0x270, 0x800f3fff, 0x800b08ff },
	{ 0x274, 0x07ff07ff, 0x07000080 },
	{ 0x280, 0xffffffc0, 0x00180000 },
	{ 0x290, 0x800f3fff, 0x800004ff },
	{ 0x294, 0x000000ff, 0x00000080 },
	{ 0x2a0, 0x800f3fff, 0x800104ff },
	{ 0x2a4, 0x000000ff, 0x00000080 },
	{ 0x2b0, 0x800f3fff, 0x800204ff },
	{ 0x2b4, 0x000000ff, 0x00000040 },
	{ 0x2c0, 0x800f3fff, 0x800304ff },
	{ 0x2c4, 0x000000ff, 0x00000040 },
	{ 0x2d0, 0x802ff04c, 0x80070040 },
	{ 0x2d4, 0x00000001, 0x00000000 },
	{ 0x2d8, 0xffff0003, 0x00ff0003 },
	{ 0x2e0, 0x07ff07ff, 0x06000040 },
	{ 0x2f8, 0x802ff04c, 0x80081040 },
	{ 0x2fc, 0x00000001, 0x00000000 },
	{ 0x300, 0xffff0003, 0x00ff0003 },
	{ 0x308, 0x07ff07ff, 0x06400040 },
	{ 0x320, 0x802ff04c, 0x80092040 },
	{ 0x324, 0x00000001, 0x00000000 },
	{ 0x328, 0xffff0003, 0x00ff0003 },
	{ 0x330, 0x07ff07ff, 0x06800040 },
	{ 0x350, 0x800f3fff, 0x800b08ff },
	{ 0x354, 0x07ff07ff, 0x076000a0 },
	{ 0x360, 0xffffffc0, 0x00180000 },
	{ 0x370, 0x800f3fff, 0x800604ff },
	{ 0x374, 0x07ff07ff, 0x06c000a0 },
};

/*
 * T6041 (M4 Pro) power-up rows, in the order the engine needs them: the
 * SCODEC-system window (aux), then the engine (core), then the AXI-to-fabric
 * wrapper. Each row replaces the whole register with the value macOS 27.0
 * writes before its first ProRes job, from an MMIO trace on a J616s.
 */
static const struct apr_tunable apr_t6041_aux_tunables[] = {
	{ 0x008, 0xffffffff, 0x00000001 },
	{ 0x00c, 0xffffffff, 0x7f7f0001 },
	{ 0x014, 0xffffffff, 0x01100110 },
	{ 0x018, 0xffffffff, 0x7f007f00 },
	{ 0x01c, 0xffffffff, 0xa000003f },
	{ 0x020, 0xffffffff, 0x001c0090 },
	{ 0x024, 0xffffffff, 0xa000003f },
	{ 0x028, 0xffffffff, 0x08000002 },
	{ 0x1000, 0xffffffff, 0x00007f00 },
	{ 0x1004, 0xffffffff, 0x00000000 },
	{ 0x1008, 0xffffffff, 0x00000000 },
	{ 0x100c, 0xffffffff, 0x00000000 },
	{ 0x1010, 0xffffffff, 0x00000000 },
	{ 0x1014, 0xffffffff, 0x00000000 },
	{ 0x1018, 0xffffffff, 0x00000000 },
	{ 0x101c, 0xffffffff, 0x00000000 },
	{ 0x1020, 0xffffffff, 0x00000000 },
	{ 0x1024, 0xffffffff, 0x00000000 },
	{ 0x1028, 0xffffffff, 0x00000000 },
	{ 0x102c, 0xffffffff, 0x00000000 },
	{ 0x1030, 0xffffffff, 0x00000000 },
	{ 0x1034, 0xffffffff, 0x00000000 },
	{ 0x1038, 0xffffffff, 0x00000000 },
	{ 0x103c, 0xffffffff, 0x00000000 },
	{ 0x1040, 0xffffffff, 0x00000000 },
	{ 0x1044, 0xffffffff, 0x00000000 },
	{ 0x1048, 0xffffffff, 0x00000000 },
	{ 0x104c, 0xffffffff, 0x00000000 },
	{ 0x1050, 0xffffffff, 0x00000000 },
	{ 0x1054, 0xffffffff, 0x00000000 },
	{ 0x1058, 0xffffffff, 0x00000000 },
	{ 0x105c, 0xffffffff, 0x00000000 },
	{ 0x1060, 0xffffffff, 0x00000000 },
	{ 0x1064, 0xffffffff, 0x00000000 },
	{ 0x1068, 0xffffffff, 0x00000000 },
	{ 0x106c, 0xffffffff, 0x00000000 },
	{ 0x1070, 0xffffffff, 0x00000000 },
	{ 0x1074, 0xffffffff, 0x00000000 },
	{ 0x1078, 0xffffffff, 0x00007f00 },
	{ 0x107c, 0xffffffff, 0x00000000 },
	{ 0x1080, 0xffffffff, 0x00000000 },
	{ 0x1084, 0xffffffff, 0x00000000 },
	{ 0x1088, 0xffffffff, 0x00000000 },
	{ 0x108c, 0xffffffff, 0x00000000 },
	{ 0x1090, 0xffffffff, 0x00000000 },
	{ 0x1094, 0xffffffff, 0x00000000 },
	{ 0x1098, 0xffffffff, 0x00000000 },
	{ 0x109c, 0xffffffff, 0x00000000 },
	{ 0x10a0, 0xffffffff, 0x00000000 },
	{ 0x10a4, 0xffffffff, 0x00000000 },
	{ 0x10a8, 0xffffffff, 0x00000000 },
	{ 0x10ac, 0xffffffff, 0x00000000 },
	{ 0x10b0, 0xffffffff, 0x00000000 },
	{ 0x10b4, 0xffffffff, 0x00000000 },
	{ 0x10b8, 0xffffffff, 0x00000000 },
	{ 0x10bc, 0xffffffff, 0x00000000 },
	{ 0x10c0, 0xffffffff, 0x00000000 },
	{ 0x10c4, 0xffffffff, 0x00000000 },
	{ 0x10c8, 0xffffffff, 0x00000000 },
	{ 0x10cc, 0xffffffff, 0x00000000 },
	{ 0x10d0, 0xffffffff, 0x00000000 },
	{ 0x10d4, 0xffffffff, 0x00000000 },
	{ 0x10d8, 0xffffffff, 0x00000000 },
	{ 0x10dc, 0xffffffff, 0x00000000 },
	{ 0x10e0, 0xffffffff, 0x00000000 },
	{ 0x10e4, 0xffffffff, 0x00000000 },
	{ 0x10e8, 0xffffffff, 0x00000000 },
	{ 0x10ec, 0xffffffff, 0x00000000 },
	{ 0x10fc, 0xffffffff, 0x00000000 },
	{ 0x1100, 0xffffffff, 0x00000000 },
	{ 0x1104, 0xffffffff, 0x00000000 },
	{ 0x1108, 0xffffffff, 0x00000000 },
	{ 0x110c, 0xffffffff, 0x00000000 },
	{ 0x1110, 0xffffffff, 0x00000000 },
	{ 0x1114, 0xffffffff, 0x00000000 },
	{ 0x1118, 0xffffffff, 0x00000000 },
	{ 0x111c, 0xffffffff, 0x00000000 },
	{ 0x1120, 0xffffffff, 0x00000000 },
	{ 0x1124, 0xffffffff, 0x00000000 },
	{ 0x1128, 0xffffffff, 0x00000000 },
	{ 0x112c, 0xffffffff, 0x00000000 },
	{ 0x1130, 0xffffffff, 0x00000000 },
};

static const struct apr_tunable apr_t6041_engine_tunables[] = {
	{ 0x008, 0xffffffff, 0x00000008 },
	{ 0x118, 0xffffffff, 0x00000600 },
	{ 0x11c, 0xffffffff, 0x00000600 },
	{ 0x148, 0xffffffff, 0x00000001 },
	{ 0x180, 0xffffffff, 0x800a0600 },
	{ 0x184, 0xffffffff, 0x00000001 },
	{ 0x188, 0xffffffff, 0x09b80038 },
	{ 0x1c0, 0xffffffff, 0x80040600 },
	{ 0x1c4, 0xffffffff, 0x00000003 },
	{ 0x1c8, 0xffffffff, 0x080000c0 },
	{ 0x200, 0xffffffff, 0x80050600 },
	{ 0x204, 0xffffffff, 0x00000002 },
	{ 0x208, 0xffffffff, 0x08c000c0 },
	{ 0x240, 0xffffffff, 0x800007ff },
	{ 0x244, 0xffffffff, 0x00000007 },
	{ 0x248, 0xffffffff, 0x00000040 },
	{ 0x280, 0xffffffff, 0x80010600 },
	{ 0x284, 0xffffffff, 0x00000006 },
	{ 0x288, 0xffffffff, 0x00000080 },
	{ 0x2c0, 0xffffffff, 0x80020600 },
	{ 0x2c4, 0xffffffff, 0x00000005 },
	{ 0x2c8, 0xffffffff, 0x00000040 },
	{ 0x300, 0xffffffff, 0x80030600 },
	{ 0x304, 0xffffffff, 0x00000004 },
	{ 0x308, 0xffffffff, 0x00000040 },
	{ 0x340, 0xffffffff, 0x81000047 },
	{ 0x344, 0xffffffff, 0x0b0a0908 },
	{ 0x348, 0xffffffff, 0x00000720 },
	{ 0x34c, 0xffffffff, 0x00000140 },
	{ 0x350, 0xffffffff, 0x00928170 },
	{ 0x380, 0xffffffff, 0x800b0a00 },
	{ 0x384, 0xffffffff, 0x0000000c },
	{ 0x388, 0xffffffff, 0x09800038 },
	{ 0x394, 0xffffffff, 0x00200000 },
	{ 0x3c0, 0xffffffff, 0x800b0a00 },
	{ 0x3c4, 0xffffffff, 0x00000019 },
	{ 0x3c8, 0xffffffff, 0x09f00010 },
	{ 0x4c0, 0xffffffff, 0x800b09ff },
	{ 0x4c4, 0xffffffff, 0x0000001b },
	{ 0x4c8, 0xffffffff, 0x09300010 },
	{ 0x500, 0xffffffff, 0x800605ff },
	{ 0x504, 0xffffffff, 0x0000000d },
	{ 0x508, 0xffffffff, 0x080000a0 },
	{ 0x540, 0xffffffff, 0x800005ff },
	{ 0x544, 0xffffffff, 0x00000011 },
	{ 0x548, 0xffffffff, 0x00000080 },
	{ 0x580, 0xffffffff, 0x800105ff },
	{ 0x584, 0xffffffff, 0x00000010 },
	{ 0x588, 0xffffffff, 0x00000080 },
	{ 0x5c0, 0xffffffff, 0x800205ff },
	{ 0x5c4, 0xffffffff, 0x0000000f },
	{ 0x5c8, 0xffffffff, 0x00000040 },
	{ 0x600, 0xffffffff, 0x800305ff },
	{ 0x604, 0xffffffff, 0x0000000e },
	{ 0x608, 0xffffffff, 0x00000040 },
	{ 0x640, 0xffffffff, 0x80070051 },
	{ 0x644, 0xffffffff, 0x00071716 },
	{ 0x648, 0xffffffff, 0x07ff0003 },
	{ 0x650, 0xffffffff, 0x06000040 },
	{ 0x680, 0xffffffff, 0x80081051 },
	{ 0x684, 0xffffffff, 0x00071514 },
	{ 0x688, 0xffffffff, 0x07ff0003 },
	{ 0x690, 0xffffffff, 0x06400040 },
	{ 0x6c0, 0xffffffff, 0x80092051 },
	{ 0x6c4, 0xffffffff, 0x00071312 },
	{ 0x6c8, 0xffffffff, 0x07ff0003 },
	{ 0x6d0, 0xffffffff, 0x06800040 },
	{ 0x700, 0xffffffff, 0x800b09ff },
	{ 0x704, 0xffffffff, 0x00000018 },
	{ 0x708, 0xffffffff, 0x08a00060 },
	{ 0x714, 0xffffffff, 0x00200000 },
	{ 0x740, 0xffffffff, 0x800b09ff },
	{ 0x744, 0xffffffff, 0x0000001a },
	{ 0x748, 0xffffffff, 0x09000010 },
	{ 0x780, 0xffffffff, 0x800b09ff },
	{ 0x784, 0xffffffff, 0x0000001c },
	{ 0x788, 0xffffffff, 0x09100010 },
	{ 0x7c0, 0xffffffff, 0x800b09ff },
	{ 0x7c4, 0xffffffff, 0x0000001d },
	{ 0x7c8, 0xffffffff, 0x09200010 },
};

static const struct apr_tunable apr_t6041_wrapper_tunables[] = {
	{ 0x000, 0xffffffff, 0x8000001d },
	{ 0x004, 0xffffffff, 0x801d0003 },
	{ 0x008, 0xffffffff, 0x801d000c },
	{ 0x00c, 0xffffffff, 0x801d0003 },
	{ 0x010, 0xffffffff, 0x80000020 },
	{ 0x014, 0xffffffff, 0x80000020 },
	{ 0x018, 0xffffffff, 0x801d0003 },
	{ 0x01c, 0xffffffff, 0x801d0003 },
	{ 0x020, 0xffffffff, 0x80000016 },
	{ 0x024, 0xffffffff, 0x8000001d },
	{ 0x028, 0xffffffff, 0x801d0003 },
	{ 0x02c, 0xffffffff, 0x801d0003 },
	{ 0x030, 0xffffffff, 0x80000020 },
	{ 0x034, 0xffffffff, 0x801d0003 },
	{ 0x038, 0xffffffff, 0x801d0003 },
	{ 0x03c, 0xffffffff, 0x80000016 },
	{ 0x040, 0xffffffff, 0x80000016 },
	{ 0x044, 0xffffffff, 0x80000016 },
	{ 0x100, 0xffffffff, 0x000000ff },
	{ 0x108, 0xffffffff, 0x00101330 },
	{ 0x304, 0xffffffff, 0x02020000 },
	{ 0x308, 0xffffffff, 0x00001681 },
	{ 0x30c, 0xffffffff, 0x00001680 },
	{ 0x310, 0xffffffff, 0x000001e0 },
	{ 0x314, 0xffffffff, 0x800001e0 },
	{ 0x318, 0xffffffff, 0x000005a0 },
	{ 0x31c, 0xffffffff, 0x800005a0 },
	{ 0x320, 0xffffffff, 0x000005a0 },
	{ 0x324, 0xffffffff, 0x800005a0 },
	{ 0x328, 0xffffffff, 0x0000003c },
	{ 0x32c, 0xffffffff, 0x8000003c },
	{ 0x330, 0xffffffff, 0x000005a0 },
	{ 0x334, 0xffffffff, 0x000005a0 },
	{ 0x338, 0xffffffff, 0x000005a0 },
	{ 0x344, 0xffffffff, 0x01000005 },
	{ 0x34c, 0xffffffff, 0x00050020 },
	{ 0x350, 0xffffffff, 0x000a0030 },
	{ 0x408, 0xffffffff, 0x00190031 },
	{ 0x40c, 0xffffffff, 0x00001681 },
	{ 0x410, 0xffffffff, 0x00001680 },
	{ 0x414, 0xffffffff, 0x000001e0 },
	{ 0x418, 0xffffffff, 0x000001e0 },
	{ 0x41c, 0xffffffff, 0x000005a0 },
	{ 0x420, 0xffffffff, 0x000005a0 },
	{ 0x424, 0xffffffff, 0x000005a0 },
	{ 0x428, 0xffffffff, 0x000005a0 },
	{ 0x42c, 0xffffffff, 0x0000003c },
	{ 0x430, 0xffffffff, 0x0000003c },
	{ 0x434, 0xffffffff, 0x000005a0 },
	{ 0x438, 0xffffffff, 0x000005a0 },
	{ 0x43c, 0xffffffff, 0x000005a0 },
	{ 0x448, 0xffffffff, 0x01000005 },
	{ 0x748, 0xffffffff, 0x00200000 },
	{ 0x848, 0xffffffff, 0x00100030 },
	{ 0x948, 0xffffffff, 0x01400100 },
	{ 0xa50, 0xffffffff, 0x01000100 },
	{ 0xc00, 0xffffffff, 0x7fffffff },
	{ 0x4008, 0xffffffff, 0x00000000 },
	{ 0x400c, 0xffffffff, 0x00000001 },
	{ 0x4014, 0xffffffff, 0x00000000 },
	{ 0x4018, 0xffffffff, 0x00000000 },
	{ 0x401c, 0xffffffff, 0x00000001 },
};

/*
 * T6050 (M5 Pro/Max) power-up rows: the SCODEC-system window (aux), the
 * SCODEC control window (ctrl), the engine (core) and the AXI-to-fabric
 * wrapper, applied in that order. Each row replaces the whole register with
 * the value macOS 27.0 writes before its first ProRes job, from an MMIO trace
 * of the four apr0 windows on a J714s. The system and engine rows sit at
 * T6041's offsets; the wrapper rows do not.
 */
static const struct apr_tunable apr_t6050_aux_tunables[] = {
	{ 0x008, 0xffffffff, 0x00000001 },
	{ 0x00c, 0xffffffff, 0x7f7f0001 },
	{ 0x014, 0xffffffff, 0x01100110 },
	{ 0x018, 0xffffffff, 0x7f007f00 },
	{ 0x01c, 0xffffffff, 0xa0000041 },
	{ 0x020, 0xffffffff, 0x001c0090 },
	{ 0x024, 0xffffffff, 0xa0000041 },
	{ 0x028, 0xffffffff, 0x08000002 },
	{ 0x1000, 0xffffffff, 0x00007f00 },
	{ 0x1004, 0xffffffff, 0x00000000 },
	{ 0x1008, 0xffffffff, 0x00000000 },
	{ 0x100c, 0xffffffff, 0x00000000 },
	{ 0x1010, 0xffffffff, 0x00000000 },
	{ 0x1014, 0xffffffff, 0x00000000 },
	{ 0x1018, 0xffffffff, 0x00000000 },
	{ 0x101c, 0xffffffff, 0x00000000 },
	{ 0x1020, 0xffffffff, 0x00000000 },
	{ 0x1024, 0xffffffff, 0x00000000 },
	{ 0x1028, 0xffffffff, 0x00000000 },
	{ 0x102c, 0xffffffff, 0x00000000 },
	{ 0x1030, 0xffffffff, 0x00000000 },
	{ 0x1034, 0xffffffff, 0x00000000 },
	{ 0x1038, 0xffffffff, 0x00000000 },
	{ 0x103c, 0xffffffff, 0x00000000 },
	{ 0x1040, 0xffffffff, 0x00000000 },
	{ 0x1044, 0xffffffff, 0x00000000 },
	{ 0x1048, 0xffffffff, 0x00000000 },
	{ 0x104c, 0xffffffff, 0x00000000 },
	{ 0x1050, 0xffffffff, 0x00000000 },
	{ 0x1054, 0xffffffff, 0x00000000 },
	{ 0x1058, 0xffffffff, 0x00000000 },
	{ 0x105c, 0xffffffff, 0x00000000 },
	{ 0x1060, 0xffffffff, 0x00000000 },
	{ 0x1064, 0xffffffff, 0x00000000 },
	{ 0x1068, 0xffffffff, 0x00000000 },
	{ 0x106c, 0xffffffff, 0x00000000 },
	{ 0x1070, 0xffffffff, 0x00000000 },
	{ 0x1074, 0xffffffff, 0x00000000 },
	{ 0x1078, 0xffffffff, 0x00007f00 },
	{ 0x107c, 0xffffffff, 0x00000000 },
	{ 0x1080, 0xffffffff, 0x00000000 },
	{ 0x1084, 0xffffffff, 0x00000000 },
	{ 0x1088, 0xffffffff, 0x00000000 },
	{ 0x108c, 0xffffffff, 0x00000000 },
	{ 0x1090, 0xffffffff, 0x00000000 },
	{ 0x1094, 0xffffffff, 0x00000000 },
	{ 0x1098, 0xffffffff, 0x00000000 },
	{ 0x109c, 0xffffffff, 0x00000000 },
	{ 0x10a0, 0xffffffff, 0x00000000 },
	{ 0x10a4, 0xffffffff, 0x00000000 },
	{ 0x10a8, 0xffffffff, 0x00000000 },
	{ 0x10ac, 0xffffffff, 0x00000000 },
	{ 0x10b0, 0xffffffff, 0x00000000 },
	{ 0x10b4, 0xffffffff, 0x00000000 },
	{ 0x10b8, 0xffffffff, 0x00000000 },
	{ 0x10bc, 0xffffffff, 0x00000000 },
	{ 0x10c0, 0xffffffff, 0x00000000 },
	{ 0x10c4, 0xffffffff, 0x00000000 },
	{ 0x10c8, 0xffffffff, 0x00000000 },
	{ 0x10cc, 0xffffffff, 0x00000000 },
	{ 0x10d0, 0xffffffff, 0x00000000 },
	{ 0x10d4, 0xffffffff, 0x00000000 },
	{ 0x10d8, 0xffffffff, 0x00000000 },
	{ 0x10dc, 0xffffffff, 0x00000000 },
	{ 0x10e0, 0xffffffff, 0x00000000 },
	{ 0x10e4, 0xffffffff, 0x00000000 },
	{ 0x10e8, 0xffffffff, 0x00000000 },
	{ 0x10ec, 0xffffffff, 0x00000000 },
	{ 0x10fc, 0xffffffff, 0x00000000 },
	{ 0x1100, 0xffffffff, 0x00000000 },
	{ 0x1104, 0xffffffff, 0x00000000 },
	{ 0x1108, 0xffffffff, 0x00000000 },
	{ 0x110c, 0xffffffff, 0x00000000 },
	{ 0x1110, 0xffffffff, 0x00000000 },
	{ 0x1114, 0xffffffff, 0x00000000 },
	{ 0x1118, 0xffffffff, 0x00000000 },
	{ 0x111c, 0xffffffff, 0x00000000 },
	{ 0x1120, 0xffffffff, 0x00000000 },
	{ 0x1124, 0xffffffff, 0x00000000 },
	{ 0x1128, 0xffffffff, 0x00000000 },
	{ 0x112c, 0xffffffff, 0x00000000 },
	{ 0x1130, 0xffffffff, 0x00000000 },
};

static const struct apr_tunable apr_t6050_engine_tunables[] = {
	{ 0x008, 0xffffffff, 0x00000008 },
	{ 0x118, 0xffffffff, 0x00000800 },
	{ 0x11c, 0xffffffff, 0x00000740 },
	{ 0x148, 0xffffffff, 0x00000001 },
	{ 0x180, 0xffffffff, 0x800a0600 },
	{ 0x184, 0xffffffff, 0x00000001 },
	{ 0x188, 0xffffffff, 0x0bb80038 },
	{ 0x1c0, 0xffffffff, 0x80040600 },
	{ 0x1c4, 0xffffffff, 0x00000003 },
	{ 0x1c8, 0xffffffff, 0x0a0000c0 },
	{ 0x200, 0xffffffff, 0x80050600 },
	{ 0x204, 0xffffffff, 0x00000002 },
	{ 0x208, 0xffffffff, 0x0ac000c0 },
	{ 0x240, 0xffffffff, 0x800007ff },
	{ 0x244, 0xffffffff, 0x00000007 },
	{ 0x248, 0xffffffff, 0x00000040 },
	{ 0x280, 0xffffffff, 0x80010600 },
	{ 0x284, 0xffffffff, 0x00000006 },
	{ 0x288, 0xffffffff, 0x00000080 },
	{ 0x2c0, 0xffffffff, 0x80020600 },
	{ 0x2c4, 0xffffffff, 0x00000005 },
	{ 0x2c8, 0xffffffff, 0x00000040 },
	{ 0x300, 0xffffffff, 0x80030600 },
	{ 0x304, 0xffffffff, 0x00000004 },
	{ 0x308, 0xffffffff, 0x00000040 },
	{ 0x340, 0xffffffff, 0x81000047 },
	{ 0x344, 0xffffffff, 0x0b0a0908 },
	{ 0x348, 0xffffffff, 0x00000820 },
	{ 0x34c, 0xffffffff, 0x00001580 },
	{ 0x350, 0xffffffff, 0x00928170 },
	{ 0x380, 0xffffffff, 0x800b0a00 },
	{ 0x384, 0xffffffff, 0x0000000c },
	{ 0x388, 0xffffffff, 0x0b800038 },
	{ 0x394, 0xffffffff, 0x00200000 },
	{ 0x3c0, 0xffffffff, 0x800b0a00 },
	{ 0x3c4, 0xffffffff, 0x00000019 },
	{ 0x3c8, 0xffffffff, 0x0bf00010 },
	{ 0x4c0, 0xffffffff, 0x800b09ff },
	{ 0x4c4, 0xffffffff, 0x0000001b },
	{ 0x4c8, 0xffffffff, 0x0b300010 },
	{ 0x500, 0xffffffff, 0x800605ff },
	{ 0x504, 0xffffffff, 0x0000000d },
	{ 0x508, 0xffffffff, 0x0a0000a0 },
	{ 0x540, 0xffffffff, 0x800005ff },
	{ 0x544, 0xffffffff, 0x00000011 },
	{ 0x548, 0xffffffff, 0x00000080 },
	{ 0x580, 0xffffffff, 0x800105ff },
	{ 0x584, 0xffffffff, 0x00000010 },
	{ 0x588, 0xffffffff, 0x00000080 },
	{ 0x5c0, 0xffffffff, 0x800205ff },
	{ 0x5c4, 0xffffffff, 0x0000000f },
	{ 0x5c8, 0xffffffff, 0x00000040 },
	{ 0x600, 0xffffffff, 0x800305ff },
	{ 0x604, 0xffffffff, 0x0000000e },
	{ 0x608, 0xffffffff, 0x00000040 },
	{ 0x640, 0xffffffff, 0x80070051 },
	{ 0x644, 0xffffffff, 0x00081716 },
	{ 0x648, 0xffffffff, 0x07ff0003 },
	{ 0x650, 0xffffffff, 0x07400040 },
	{ 0x680, 0xffffffff, 0x80081051 },
	{ 0x684, 0xffffffff, 0x00081514 },
	{ 0x688, 0xffffffff, 0x07ff0003 },
	{ 0x690, 0xffffffff, 0x07800040 },
	{ 0x6c0, 0xffffffff, 0x80092051 },
	{ 0x6c4, 0xffffffff, 0x00081312 },
	{ 0x6c8, 0xffffffff, 0x07ff0003 },
	{ 0x6d0, 0xffffffff, 0x07c00040 },
	{ 0x700, 0xffffffff, 0x800b09ff },
	{ 0x704, 0xffffffff, 0x00000018 },
	{ 0x708, 0xffffffff, 0x0aa00060 },
	{ 0x714, 0xffffffff, 0x00200000 },
	{ 0x740, 0xffffffff, 0x800b09ff },
	{ 0x744, 0xffffffff, 0x0000001a },
	{ 0x748, 0xffffffff, 0x0b000010 },
	{ 0x780, 0xffffffff, 0x800b09ff },
	{ 0x784, 0xffffffff, 0x0000001c },
	{ 0x788, 0xffffffff, 0x0b100010 },
	{ 0x7c0, 0xffffffff, 0x800b09ff },
	{ 0x7c4, 0xffffffff, 0x0000001d },
	{ 0x7c8, 0xffffffff, 0x0b200010 },
};

static const struct apr_tunable apr_t6050_wrapper_tunables[] = {
	{ 0x000, 0xffffffff, 0x80000027 },
	{ 0x004, 0xffffffff, 0x80270002 },
	{ 0x008, 0xffffffff, 0x80270009 },
	{ 0x00c, 0xffffffff, 0x80270002 },
	{ 0x010, 0xffffffff, 0x80000027 },
	{ 0x014, 0xffffffff, 0x80000027 },
	{ 0x018, 0xffffffff, 0x80270002 },
	{ 0x01c, 0xffffffff, 0x80270002 },
	{ 0x020, 0xffffffff, 0x8000001e },
	{ 0x024, 0xffffffff, 0x80000027 },
	{ 0x028, 0xffffffff, 0x80270002 },
	{ 0x02c, 0xffffffff, 0x80000027 },
	{ 0x030, 0xffffffff, 0x80270002 },
	{ 0x034, 0xffffffff, 0x80270002 },
	{ 0x038, 0xffffffff, 0x8000001e },
	{ 0x03c, 0xffffffff, 0x9000001e },
	{ 0x040, 0xffffffff, 0x90000021 },
	{ 0x100, 0xffffffff, 0x000000ff },
	{ 0x104, 0xffffffff, 0x00010002 },
	{ 0x108, 0xffffffff, 0x001013b0 },
	{ 0x300, 0xffffffff, 0x00000211 },
	{ 0x304, 0xffffffff, 0x02020000 },
	{ 0x308, 0xffffffff, 0x00000061 },
	{ 0x30c, 0xffffffff, 0x00000060 },
	{ 0x310, 0xffffffff, 0x00800008 },
	{ 0x314, 0xffffffff, 0x80800008 },
	{ 0x318, 0xffffffff, 0x00800018 },
	{ 0x31c, 0xffffffff, 0x80800018 },
	{ 0x320, 0xffffffff, 0x00800018 },
	{ 0x324, 0xffffffff, 0x80800018 },
	{ 0x328, 0xffffffff, 0x00800001 },
	{ 0x32c, 0xffffffff, 0x80800001 },
	{ 0x330, 0xffffffff, 0x00800018 },
	{ 0x334, 0xffffffff, 0x00800018 },
	{ 0x338, 0xffffffff, 0x00800018 },
	{ 0x344, 0xffffffff, 0x01000005 },
	{ 0x404, 0xffffffff, 0x00210031 },
	{ 0x408, 0xffffffff, 0x00000061 },
	{ 0x40c, 0xffffffff, 0x00000060 },
	{ 0x410, 0xffffffff, 0x80000008 },
	{ 0x414, 0xffffffff, 0x80000008 },
	{ 0x418, 0xffffffff, 0x80000018 },
	{ 0x41c, 0xffffffff, 0x80000018 },
	{ 0x420, 0xffffffff, 0x80000018 },
	{ 0x424, 0xffffffff, 0x80000018 },
	{ 0x428, 0xffffffff, 0x80000001 },
	{ 0x42c, 0xffffffff, 0x80000001 },
	{ 0x430, 0xffffffff, 0x80000018 },
	{ 0x434, 0xffffffff, 0x80000018 },
	{ 0x438, 0xffffffff, 0x80000018 },
	{ 0x444, 0xffffffff, 0x01000005 },
	{ 0x748, 0xffffffff, 0x00200000 },
	{ 0x848, 0xffffffff, 0x00100030 },
	{ 0x948, 0xffffffff, 0x01800100 },
	{ 0xa50, 0xffffffff, 0x01000100 },
	{ 0xc00, 0xffffffff, 0x7fffffff },
	{ 0xc04, 0xffffffff, 0x00000101 },
	{ 0x4008, 0xffffffff, 0x00000000 },
	{ 0x400c, 0xffffffff, 0x00000001 },
	{ 0x4014, 0xffffffff, 0x00000000 },
	{ 0x4018, 0xffffffff, 0x00000000 },
	{ 0x401c, 0xffffffff, 0x00000001 },
};

static const struct apr_tunable apr_t6050_ctrl_tunables[] = {
	{ 0x004, 0xffffffff, 0x00000001 },
	{ 0x00c, 0xffffffff, 0x00000001 },
	{ 0x010, 0xffffffff, 0x00000003 },
	{ 0x014, 0xffffffff, 0x00000000 },
	{ 0x018, 0xffffffff, 0x00000029 },
	{ 0x01c, 0xffffffff, 0x0000007f },
	{ 0x020, 0xffffffff, 0x00000000 },
	{ 0x024, 0xffffffff, 0x000000f6 },
	{ 0x028, 0xffffffff, 0x00000000 },
	{ 0x02c, 0xffffffff, 0x00001c20 },
	{ 0x034, 0xffffffff, 0x00000003 },
	{ 0x1000, 0xffffffff, 0x00000001 },
};

struct apr_stats_regs {
	u32 lo, hi, size;
};

/* Selected only by the DT compatible, before mapping any resources. */
struct apr_variant {
	int (*build_desc)(const struct apr_frame *f, u8 *desc);
	struct apr_stats_regs stats[2];
	const struct apr_tunable *engine_tunables;
	size_t num_engine_tunables;
	const struct apr_tunable *wrapper_tunables;
	size_t num_wrapper_tunables;
	/* SCODEC-system rows, applied to the "aux" window before the engine. */
	const struct apr_tunable *aux_tunables;
	size_t num_aux_tunables;
	/* SCODEC control rows (T6050), applied to "ctrl" after the system rows. */
	const struct apr_tunable *ctrl_tunables;
	size_t num_ctrl_tunables;
	const char *reg_names[4];
	unsigned int num_regs;
	unsigned int num_irqs;
	u32 ring_bytes;
	u32 desc_bytes;
	/* Optional per-machine APRI file that replaces the built-in rows. */
	const char *firmware_name;
	const struct apri_layout *apri;
};

static const struct apr_variant apr_t8140 = {
	.build_desc = apr_build_desc,
	.stats = {
		{ APR_REG_STATS0_LO, APR_REG_STATS0_HI, APR_REG_STATS0_SIZE },
		{ APR_REG_STATS1_LO, APR_REG_STATS1_HI, APR_REG_STATS1_SIZE },
	},
	.engine_tunables = apr_t8140_engine_tunables,
	.num_engine_tunables = ARRAY_SIZE(apr_t8140_engine_tunables),
	.wrapper_tunables = apr_t8140_wrapper_tunables,
	.num_wrapper_tunables = ARRAY_SIZE(apr_t8140_wrapper_tunables),
	.reg_names = { "core", "wrapper" },
	.num_regs = 2,
	.num_irqs = 2,
	.ring_bytes = APR_RING_BYTES,
	.desc_bytes = APR_ENCODE_DESC_BYTES,
};

static const struct apr_variant apr_t6000 = {
	.build_desc = apr_build_desc_v0,
	.stats = { { 0x278, 0x27c, 0x280 }, { 0x358, 0x35c, 0x360 } },
	.engine_tunables = apr_t6000_engine_tunables,
	.num_engine_tunables = ARRAY_SIZE(apr_t6000_engine_tunables),
	.reg_names = { "core", "wrapper" },
	.num_regs = 2,
	.num_irqs = 1,
	.ring_bytes = APR_RING_BYTES,
	.desc_bytes = APR_ENCODE_DESC_BYTES,
};

static const struct apr_variant apr_t6041 = {
	.build_desc = apr_build_desc,
	.stats = {
		{ APR_REG_STATS0_LO, APR_REG_STATS0_HI, APR_REG_STATS0_SIZE },
		{ APR_REG_STATS1_LO, APR_REG_STATS1_HI, APR_REG_STATS1_SIZE },
	},
	.engine_tunables = apr_t6041_engine_tunables,
	.num_engine_tunables = ARRAY_SIZE(apr_t6041_engine_tunables),
	.wrapper_tunables = apr_t6041_wrapper_tunables,
	.num_wrapper_tunables = ARRAY_SIZE(apr_t6041_wrapper_tunables),
	.aux_tunables = apr_t6041_aux_tunables,
	.num_aux_tunables = ARRAY_SIZE(apr_t6041_aux_tunables),
	.reg_names = { "core", "wrapper", "aux" },
	.num_regs = 3,
	.num_irqs = 1,
	.ring_bytes = APR_RING_BYTES,
	.desc_bytes = APR_DESC_BYTES,
	.firmware_name = APRI_T6041_FIRMWARE_NAME,
	.apri = &apri_t6041,
};

/* T6041's engine plus the SCODEC control window. */
static const struct apr_variant apr_t6050 = {
	.build_desc = apr_build_desc,
	.stats = {
		{ APR_REG_STATS0_LO, APR_REG_STATS0_HI, APR_REG_STATS0_SIZE },
		{ APR_REG_STATS1_LO, APR_REG_STATS1_HI, APR_REG_STATS1_SIZE },
	},
	.engine_tunables = apr_t6050_engine_tunables,
	.num_engine_tunables = ARRAY_SIZE(apr_t6050_engine_tunables),
	.wrapper_tunables = apr_t6050_wrapper_tunables,
	.num_wrapper_tunables = ARRAY_SIZE(apr_t6050_wrapper_tunables),
	.aux_tunables = apr_t6050_aux_tunables,
	.num_aux_tunables = ARRAY_SIZE(apr_t6050_aux_tunables),
	.ctrl_tunables = apr_t6050_ctrl_tunables,
	.num_ctrl_tunables = ARRAY_SIZE(apr_t6050_ctrl_tunables),
	.reg_names = { "core", "wrapper", "aux", "ctrl" },
	.num_regs = 4,
	.num_irqs = 1,
	.ring_bytes = APR_RING_BYTES,
	.desc_bytes = APR_DESC_BYTES,
	.firmware_name = APRI_T6050_FIRMWARE_NAME,
	.apri = &apri_t6050,
};

struct apr_ctx;

/*
 * A source buffer of the decoder. The frame is parsed once, when it is
 * queued; job_ready, the source-change check and device_run then use the
 * cached result instead of walking the slice table of the (uncached) buffer
 * again each time.
 */
struct apr_buf {
	struct v4l2_m2m_buffer m2m;	/* first: vb2 and m2m embed this */
	struct apr_prores_info info;
	int parse_err;
};

static struct apr_buf *to_apr_buf(struct vb2_v4l2_buffer *vbuf)
{
	return container_of(vbuf, struct apr_buf, m2m.vb);
}

/* A job in the ring; its buffers left the m2m queues when it was published. */
struct apr_job {
	struct apr_ctx *ctx;
	struct vb2_v4l2_buffer *src;
	struct vb2_v4l2_buffer *dst;
	u32 end;		/* ring pointer just past its descriptor */
	u16 id;			/* decode job id in the descriptor */
	u32 sequence;
	u32 bytes;		/* decoded payload */
	int err;		/* forced result: engine error or cancellation */
};

struct apr_dev {
	const struct apr_variant *variant;
	struct device *dev;
	void __iomem *regs;
	void __iomem *wrapper;
	void __iomem *aux;
	void __iomem *ctrl;
	struct apr_init_data init;
	/* Rows applied at power-up, per APRI block: built-in or the file's. */
	const struct apr_tunable *tunables[APRI_MAX_BLOCKS];
	size_t num_tunables[APRI_MAX_BLOCKS];
	struct reset_control *reset;
	struct dev_pm_domain_list *pd_list;
	int irq[2];

	struct v4l2_device v4l2_dev;
	struct video_device enc_vdev;
	struct video_device dec_vdev;
	struct v4l2_m2m_dev *m2m_dev;
	struct mutex vdev_lock;	/* serialises ioctls and queue state */

	/* Descriptor ring, written by the CPU and read by the engine. */
	u8 *ring;
	dma_addr_t ring_dma;
	/* Statistics area the engine owns; size from its capacity registers. */
	void *stats;
	dma_addr_t stats_dma;
	size_t stats_size;

	spinlock_t hw_lock;	/* protects the fields below */
	struct apr_job jobs[APR_MAX_INFLIGHT];	/* in ring order */
	unsigned int job_first;	/* oldest job in jobs[] */
	unsigned int njobs;	/* jobs the engine owns */
	unsigned int nencodes;	/* encode jobs among them */
	struct apr_ctx *held;	/* m2m job left running while the ring is full */
	u32 head;		/* next free ring offset */
	u32 done;		/* the engine has consumed the ring up to here */
	u16 next_id;		/* id for the next decode job, never 0 */
	bool ring_ready;	/* engine registers programmed since power-up */
	bool broken;		/* timed out with no way to reset */
	bool hung;		/* ring state is inconsistent, reset now */
	bool resetting;		/* the watchdog is resetting the engine */
	unsigned long progress;	/* jiffies of the last start or completion */
	struct delayed_work watchdog;

	atomic_t outstanding;	/* jobs not yet given back to vb2 */
	wait_queue_head_t idle_wq;	/* woken when jobs are given back */
};

struct apr_ctx {
	struct v4l2_fh fh;
	struct apr_dev *apr;
	struct v4l2_ctrl_handler hdl;
	struct v4l2_ctrl *profile;
	struct v4l2_pix_format_mplane src_fmt;
	struct v4l2_pix_format_mplane dst_fmt;
	const struct apr_fmt *raw;
	bool decode;
	bool source_valid;
	bool decode_444;
	atomic_t inflight;	/* jobs in the ring not yet given back to vb2 */
	enum apr_profile stream_profile;	/* fixed while OUTPUT streams */
	u32 sequence;
};

static struct apr_ctx *apr_file_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct apr_ctx, fh);
}

/* ---- hardware ---------------------------------------------------------- */

static void apr_write(struct apr_dev *apr, u32 reg, u32 val)
{
	writel_relaxed(val, apr->regs + reg);
}

static u32 apr_read(struct apr_dev *apr, u32 reg)
{
	return readl_relaxed(apr->regs + reg);
}

/*
 * Program the engine from power-up state. The ring must be installed before
 * MODE enables descriptor execution, otherwise the engine fetches from IOVA 0.
 */
static void apr_apply(void __iomem *base, const struct apr_tunable *t, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		u32 v = readl_relaxed(base + t[i].offset);

		writel_relaxed((v & ~t[i].mask) | (t[i].value & t[i].mask), base + t[i].offset);
	}
}

static void apr_hw_init(struct apr_dev *apr)
{
	const struct apr_variant *v = apr->variant;
	unsigned int t, i;

	apr_write(apr, APR_REG_MODE, APR_MODE_OFF);
	apr_write(apr, APR_REG_IRQ_ENABLE, 0);
	if (apr->aux)
		apr_apply(apr->aux, apr->tunables[APRI_SYSTEM_BLOCK],
			  apr->num_tunables[APRI_SYSTEM_BLOCK]);
	if (apr->ctrl)
		apr_apply(apr->ctrl, apr->tunables[APRI_CONTROL_BLOCK],
			  apr->num_tunables[APRI_CONTROL_BLOCK]);
	apr_apply(apr->regs, apr->tunables[APRI_CORE_BLOCK],
		  apr->num_tunables[APRI_CORE_BLOCK]);
	if (apr->wrapper)
		apr_apply(apr->wrapper, apr->tunables[APRI_WRAPPER_BLOCK],
			  apr->num_tunables[APRI_WRAPPER_BLOCK]);
	apr_write(apr, APR_REG_RING_HEAD, 0);
	apr_write(apr, APR_REG_RING_TAIL, 0);
	apr_write(apr, APR_REG_RING_SIZE, v->ring_bytes);
	apr_write(apr, APR_REG_RING_LO, lower_32_bits(apr->ring_dma));
	apr_write(apr, APR_REG_RING_HI, upper_32_bits(apr->ring_dma));
	/* Both statistics consumers share one area. */
	for (i = 0; i < ARRAY_SIZE(v->stats); i++) {
		apr_write(apr, v->stats[i].lo, lower_32_bits(apr->stats_dma) & ~0x3fU);
		apr_write(apr, v->stats[i].hi, upper_32_bits(apr->stats_dma) & 0x3ff);
	}
	apr_write(apr, APR_REG_MODE, APR_MODE_RUN);

	for (t = 0; t < APR_QTABLES; t++) {
		for (i = 0; i < 32; i++) {
			apr_write(apr, APR_REG_QUANT_LUMA(t) + 4 * i,
				  apr_qmat_reg(apr_qmat_luma[t], i));
			apr_write(apr, APR_REG_QUANT_CHROMA(t) + 4 * i,
				  apr_qmat_reg(apr_qmat_chroma[t], i));
		}
	}
	for (i = 0; i < APR_DC_SCALE_ENTRIES; i++)
		apr_write(apr, APR_REG_DC_SCALE(i), apr_dc_scale_reg(i));

	apr_write(apr, APR_REG_IRQ_STATUS, apr_read(apr, APR_REG_IRQ_STATUS));
	apr_write(apr, APR_REG_IRQ_ENABLE, APR_IRQ_DONE | APR_IRQ_ERROR);
	/* Order the setup before any descriptor publication. */
	wmb();
	apr->head = 0;
	apr->next_id = 1;
	apr->done = 0;
	apr->ring_ready = true;
}

static u32 apr_coded_size(u32 width, u32 height, bool is444)
{
	/* Worst case for 4444 XQ at the lowest quantiser, plus headers. */
	u64 size = (u64)ALIGN(width, 16) * ALIGN(height, 16) * (is444 ? 4 : 3) + SZ_64K;

	return min_t(u64, PAGE_ALIGN(size), SZ_256M);
}

static u8 apr_color_primaries(const struct v4l2_pix_format_mplane *f)
{
	switch (f->colorspace) {
	case V4L2_COLORSPACE_REC709:
	case V4L2_COLORSPACE_SRGB:
		return 1;
	case V4L2_COLORSPACE_SMPTE170M:
		return 6;
	case V4L2_COLORSPACE_470_SYSTEM_BG:
		return 5;
	case V4L2_COLORSPACE_BT2020:
		return 9;
	case V4L2_COLORSPACE_DCI_P3:
		return 11;
	default:
		return 2;	/* unspecified */
	}
}

static u8 apr_color_transfer(const struct v4l2_pix_format_mplane *f)
{
	switch (f->xfer_func) {
	case V4L2_XFER_FUNC_709:
		return 1;
	case V4L2_XFER_FUNC_SMPTE2084:
		return 16;
	default:
		return f->colorspace == V4L2_COLORSPACE_REC709 ? 1 : 2;
	}
}

static u8 apr_color_matrix(const struct v4l2_pix_format_mplane *f)
{
	switch (f->ycbcr_enc) {
	case V4L2_YCBCR_ENC_709:
		return 1;
	case V4L2_YCBCR_ENC_601:
		return 6;
	case V4L2_YCBCR_ENC_BT2020:
		return 9;
	default:
		return f->colorspace == V4L2_COLORSPACE_REC709 ? 1 : 2;
	}
}

static void apr_fill_frame(struct apr_ctx *ctx, struct vb2_v4l2_buffer *src,
			   struct vb2_v4l2_buffer *dst, struct apr_frame *f)
{
	const struct v4l2_pix_format_mplane *s = &ctx->src_fmt;
	u32 luma_stride = s->plane_fmt[0].bytesperline;
	dma_addr_t in = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);

	*f = (struct apr_frame){
		.width = s->width,
		.height = s->height,
		.input = ctx->raw->input,
		.profile = ctx->stream_profile,
		.luma_stride = luma_stride,
		.chroma_stride = luma_stride * (ctx->raw->is444 ? 2 : 1),
		.luma = in,
		.chroma = in + (u64)luma_stride * s->height,
		.output = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0),
		.output_size = vb2_plane_size(&dst->vb2_buf, 0),
		.primaries = apr_color_primaries(s),
		.transfer = apr_color_transfer(s),
		.matrix = apr_color_matrix(s),
	};
}

static int apr_parse_src(struct vb2_v4l2_buffer *src,
			 struct apr_prores_info *info)
{
	struct apr_buf *buf = to_apr_buf(src);

	if (buf->parse_err)
		return buf->parse_err;
	*info = buf->info;
	return 0;
}

static int apr_fill_decode_frame(struct apr_ctx *ctx,
				 struct vb2_v4l2_buffer *src,
				 struct vb2_v4l2_buffer *dst,
				 struct apr_decode_frame *f)
{
	const struct v4l2_pix_format_mplane *d = &ctx->dst_fmt;
	struct apr_prores_info info;
	dma_addr_t output;
	u32 luma_stride;
	int ret;

	ret = apr_parse_src(src, &info);
	if (ret)
		return ret;
	/* The non-RAW descriptor accepts one picture; field repacking is later. */
	if (info.pictures != 1)
		return -EOPNOTSUPP;
	if (info.width != d->width || info.height != d->height ||
	    info.chroma == 3 != ctx->raw->is444)
		return -EINVAL;

	luma_stride = d->plane_fmt[0].bytesperline;
	output = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);
	*f = (struct apr_decode_frame){
		.width = info.width,
		.height = info.height,
		.output = ctx->raw->input,
		.luma_stride = luma_stride,
		.chroma_stride = luma_stride * (ctx->raw->is444 ? 2 : 1),
		.input = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0) +
			 src->vb2_buf.planes[0].data_offset,
		.luma = output,
		.chroma = output + (u64)luma_stride * d->height,
	};
	return 0;
}

/*
 * Give a job's buffers back to vb2. Safe in interrupt context. Not inlined,
 * so ftrace can time completions against apr_device_run and apr_irq.
 */
static noinline void apr_complete(struct apr_dev *apr, struct apr_job *job)
{
	struct apr_ctx *ctx = job->ctx;
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *src = job->src, *dst = job->dst;
	int err = job->err;
	u32 bytes = 0;

	if (!err && !ctx->decode) {
		u8 *out = vb2_plane_vaddr(&dst->vb2_buf, 0);

		/* The engine wrote the frame through the DART; see it now. */
		dma_rmb();
		err = out ? apr_check_output(out, vb2_plane_size(&dst->vb2_buf, 0),
					     &bytes) : -EIO;
		if (err)
			dev_err_ratelimited(apr->dev, "engine returned no frame\n");
	}
	if (!err && ctx->decode) {
		/* The ACK and tail read complete before output ownership returns. */
		dma_rmb();
		bytes = job->bytes;
	}
	vb2_set_plane_payload(&dst->vb2_buf, 0, err ? 0 : bytes);
	v4l2_m2m_buf_copy_metadata(src, dst);
	dst->sequence = job->sequence;
	dst->flags |= V4L2_BUF_FLAG_KEYFRAME;
	if (v4l2_m2m_is_last_draining_src_buf(m2m, src)) {
		static const struct v4l2_event eos = { .type = V4L2_EVENT_EOS };

		dst->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_event_queue_fh(&ctx->fh, &eos);
		v4l2_m2m_mark_stopped(m2m);
	}
	v4l2_m2m_buf_done(src, err ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	v4l2_m2m_buf_done(dst, err ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
}

/*
 * Give back jobs taken off the ring, in ring order. A context may be freed
 * as soon as its in-flight count drops, so it is not touched after that.
 */
static void apr_retire(struct apr_dev *apr, struct apr_job *jobs,
				unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		apr_complete(apr, &jobs[i]);
		pm_runtime_mark_last_busy(apr->dev);
		pm_runtime_put_autosuspend(apr->dev);
		atomic_dec(&jobs[i].ctx->inflight);
		atomic_dec(&apr->outstanding);
	}
	if (n)
		wake_up_all(&apr->idle_wq);
}

/* Fail the current m2m job before its descriptor reached the ring. */
static void apr_finish_job(struct apr_dev *apr, struct apr_ctx *ctx, int err)
{
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct apr_job job = {
		.ctx = ctx,
		.err = err,
		.sequence = ctx->sequence++,
	};

	job.src = v4l2_m2m_src_buf_remove(m2m);
	job.dst = v4l2_m2m_dst_buf_remove(m2m);
	if (!WARN_ON(!job.src || !job.dst))
		apr_complete(apr, &job);
	pm_runtime_mark_last_busy(apr->dev);
	pm_runtime_put_autosuspend(apr->dev);
	v4l2_m2m_job_finish(apr->m2m_dev, m2m);
}

/*
 * Move the jobs the engine has finished, oldest first, to @out (room for
 * APR_MAX_INFLIGHT). Returns false if the engine raised nothing. Called with
 * hw_lock held and the engine powered.
 *
 * DONE and ERROR are latched per engine, not per descriptor: the status and
 * detail registers do not say which descriptor failed. An ERROR therefore
 * fails every job in the ring at that moment; the engine keeps running and
 * the watchdog resets it if the tail stops moving. Several descriptors may
 * finish behind one interrupt, and a DONE may arrive for a job already taken
 * here on the tail alone; both are fine.
 */
static bool apr_take_done(struct apr_dev *apr, struct apr_job *out,
			  unsigned int *n)
{
	u32 ring_bytes = apr->variant->ring_bytes;
	u32 status, tail, st0, st1, st2;
	u32 detail[6];
	unsigned int i;
	u16 done_id;

	*n = 0;
	status = apr_read(apr, APR_REG_IRQ_STATUS);
	if (!(status & (APR_IRQ_DONE | APR_IRQ_ERROR)))
		return false;
	/* Snapshot before the W1C, as for a single job. */
	st0 = apr_read(apr, APR_REG_ST0);
	st1 = apr_read(apr, APR_REG_ST1);
	st2 = apr_read(apr, APR_REG_ST2);
	detail[0] = apr_read(apr, APR_REG_DETAIL0);
	detail[1] = apr_read(apr, APR_REG_DETAIL1_LO);
	detail[2] = apr_read(apr, APR_REG_DETAIL1_HI);
	detail[3] = apr_read(apr, APR_REG_DETAIL2_LO);
	detail[4] = apr_read(apr, APR_REG_DETAIL2_HI);
	detail[5] = apr_read(apr, APR_REG_DETAIL3);
	if (status & APR_IRQ_ERROR) {
		dev_err_ratelimited(apr->dev,
				    "job failed: irq %#x head %#x done %#x jobs %u st %#x/%#x/%#x detail %#x/%#x/%#x/%#x/%#x/%#x\n",
				    status, apr->head, apr->done, apr->njobs,
				    st0, st1, st2, detail[0], detail[1], detail[2],
				    detail[3], detail[4], detail[5]);
		for (i = 0; i < apr->njobs; i++) {
			struct apr_job *job = &apr->jobs[(apr->job_first + i) %
							 APR_MAX_INFLIGHT];

			if (!job->err)
				job->err = -EIO;
		}
	}
	apr_write(apr, APR_REG_IRQ_STATUS, status);
	/* The tail read flushes the posted W1C before buffers are released. */
	tail = apr_read(apr, APR_REG_RING_TAIL) & APR_RING_PTR_MASK;
	if (!apr_ring_tail_valid(apr->done, tail, apr->head, ring_bytes)) {
		dev_err_ratelimited(apr->dev, "tail %#x outside %#x..%#x\n",
				    tail, apr->done, apr->head);
		apr->hung = true;
		return true;
	}
	/* Read after the W1C: a job finishing later raises DONE again. */
	done_id = FIELD_GET(APR_ST1_DONE_ID, apr_read(apr, APR_REG_ST1));
	while (apr->njobs) {
		struct apr_job *job = &apr->jobs[apr->job_first];

		if (job->ctx->decode ?
		    !apr_job_id_done(done_id, job->id) :
		    !(status & (APR_IRQ_DONE | APR_IRQ_ERROR)) ||
		    !apr_ring_passed(apr->done, job->end, tail, ring_bytes))
			break;
		out[(*n)++] = *job;
		apr->done = job->end;
		apr->job_first = (apr->job_first + 1) % APR_MAX_INFLIGHT;
		apr->njobs--;
		if (!job->ctx->decode)
			apr->nencodes--;
	}
	if (*n)
		apr->progress = jiffies;
	return true;
}

/*
 * The m2m job left running because the ring was full may finish once there
 * is room again. Called with hw_lock held; the caller finishes it unlocked.
 */
static struct apr_ctx *apr_take_held(struct apr_dev *apr)
{
	struct apr_ctx *ctx = apr->held;

	if (!ctx || apr_jobs_full(apr->njobs, apr->nencodes))
		return NULL;
	apr->held = NULL;
	return ctx;
}

static irqreturn_t apr_irq(int irq, void *data)
{
	struct apr_dev *apr = data;
	struct apr_job done[APR_MAX_INFLIGHT];
	struct apr_ctx *held = NULL;
	unsigned int n = 0;
	bool handled = false, hung;

	spin_lock(&apr->hw_lock);
	/* Registers are only reachable while the engine is powered. */
	if (apr->ring_ready)
		handled = apr_take_done(apr, done, &n);
	held = apr_take_held(apr);
	hung = apr->hung;
	if (!apr->njobs)
		cancel_delayed_work(&apr->watchdog);
	spin_unlock(&apr->hw_lock);
	if (hung)
		mod_delayed_work(system_wq, &apr->watchdog, 0);
	apr_retire(apr, done, n);
	if (held)
		v4l2_m2m_job_finish(apr->m2m_dev, held->fh.m2m_ctx);
	return handled ? IRQ_HANDLED : IRQ_NONE;
}

/*
 * Backstop for a lost interrupt and for hangs. A ring that stops moving for
 * APR_TIMEOUT_MS fails every job in it. A hung engine is reset through its
 * power domain and reprogrammed; without a reset line the device refuses
 * further jobs rather than reuse memory it may still own.
 */
static void apr_watchdog(struct work_struct *work)
{
	struct apr_dev *apr = container_of(work, struct apr_dev, watchdog.work);
	struct apr_job done[APR_MAX_INFLIGHT];
	struct apr_ctx *held;
	unsigned long flags;
	unsigned int n = 0;

	spin_lock_irqsave(&apr->hw_lock, flags);
	if (!apr->njobs) {
		spin_unlock_irqrestore(&apr->hw_lock, flags);
		return;
	}
	if (apr->ring_ready && apr_take_done(apr, done, &n) && n)
		dev_warn_once(apr->dev, "completion found without an interrupt\n");
	if (!apr->hung && (!apr->njobs ||
			   time_before(jiffies, apr->progress +
				       msecs_to_jiffies(APR_TIMEOUT_MS)))) {
		held = apr_take_held(apr);
		if (apr->njobs)
			schedule_delayed_work(&apr->watchdog,
					      msecs_to_jiffies(APR_POLL_MS));
		spin_unlock_irqrestore(&apr->hw_lock, flags);
		apr_retire(apr, done, n);
		if (held)
			v4l2_m2m_job_finish(apr->m2m_dev, held->fh.m2m_ctx);
		return;
	}
	dev_err(apr->dev, "%s: head %#x done %#x tail %#x jobs %u st %#x/%#x\n",
		apr->hung ? "ring state invalid" : "job timed out",
		apr->head, apr->done, apr_read(apr, APR_REG_RING_TAIL),
		apr->njobs, apr_read(apr, APR_REG_ST0), apr_read(apr, APR_REG_ST1));
	/* Every job still in the ring fails; the engine may own none of them. */
	while (apr->njobs) {
		struct apr_job *job = &apr->jobs[apr->job_first];

		done[n] = *job;
		if (!done[n].err)
			done[n].err = -ETIMEDOUT;
		n++;
		apr->job_first = (apr->job_first + 1) % APR_MAX_INFLIGHT;
		apr->njobs--;
	}
	apr->nencodes = 0;
	apr->hung = false;
	held = apr->held;
	apr->held = NULL;
	apr_write(apr, APR_REG_IRQ_ENABLE, 0);
	apr_write(apr, APR_REG_MODE, APR_MODE_OFF);
	apr->ring_ready = false;
	/* device_run waits until the reset below is over. */
	apr->resetting = true;
	spin_unlock_irqrestore(&apr->hw_lock, flags);
	if (!apr->reset || reset_control_reset(apr->reset)) {
		dev_err(apr->dev, "engine cannot be reset, refusing further jobs\n");
		spin_lock_irqsave(&apr->hw_lock, flags);
		apr->broken = true;
		spin_unlock_irqrestore(&apr->hw_lock, flags);
	}
	spin_lock_irqsave(&apr->hw_lock, flags);
	apr->resetting = false;
	spin_unlock_irqrestore(&apr->hw_lock, flags);
	apr_retire(apr, done, n);
	/* apr_retire() woke idle_wq only if there was a job to give back. */
	wake_up_all(&apr->idle_wq);
	if (held)
		v4l2_m2m_job_finish(apr->m2m_dev, held->fh.m2m_ctx);
}

/*
 * Publish one job and, while the ring has room for another, finish the m2m
 * job at once so the next one is prepared while the engine works. When the
 * ring is full the m2m job stays running until a completion makes room, so
 * device_run is only entered with room for one more descriptor.
 */
static void apr_device_run(void *priv)
{
	struct apr_ctx *ctx = priv;
	struct apr_dev *apr = ctx->apr;
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	u32 ring_bytes = apr->variant->ring_bytes;
	struct vb2_v4l2_buffer *src, *dst;
	struct apr_frame f;
	struct apr_decode_frame decode;
	struct apr_job *job;
	u8 desc[APR_ENCODE_DESC_BYTES];
	u32 desc_bytes;
	unsigned long flags;
	bool hold;
	void *out;
	int ret;

	src = v4l2_m2m_next_src_buf(m2m);
	dst = v4l2_m2m_next_dst_buf(m2m);

	ret = pm_runtime_resume_and_get(apr->dev);
	if (ret < 0) {
		pm_runtime_get_noresume(apr->dev);
		apr_finish_job(apr, ctx, ret);
		return;
	}
	if (ctx->decode) {
		ret = apr_fill_decode_frame(ctx, src, dst, &decode);
		desc_bytes = APR_DECODE_DESC_BYTES;
	} else {
		apr_fill_frame(ctx, src, dst, &f);
		ret = 0;
		desc_bytes = apr->variant->desc_bytes;
		/* Mark output invalid so a frame the engine never wrote is caught. */
		out = vb2_plane_vaddr(&dst->vb2_buf, 0);
		if (!out)
			ret = -EINVAL;
		else
			memset(out, 0, 8);
	}
	if (ret) {
		apr_finish_job(apr, ctx, ret);
		return;
	}

	spin_lock_irqsave(&apr->hw_lock, flags);
	/* An encode has no job id, so it only runs alone in the ring. */
	while (apr->resetting || (!ctx->decode && apr->njobs)) {
		spin_unlock_irqrestore(&apr->hw_lock, flags);
		wait_event(apr->idle_wq, !READ_ONCE(apr->resetting) &&
			   (ctx->decode || !READ_ONCE(apr->njobs)));
		spin_lock_irqsave(&apr->hw_lock, flags);
	}
	if (apr->broken) {
		spin_unlock_irqrestore(&apr->hw_lock, flags);
		apr_finish_job(apr, ctx, -EIO);
		return;
	}
	if (!apr->ring_ready && !WARN_ON(apr->njobs))
		apr_hw_init(apr);
	if (WARN_ON(apr_jobs_full(apr->njobs, apr->nencodes) ||
		    apr_ring_free(apr->done, apr->head, ring_bytes) <= desc_bytes)) {
		spin_unlock_irqrestore(&apr->hw_lock, flags);
		apr_finish_job(apr, ctx, -EBUSY);
		return;
	}
	if (ctx->decode)
		decode.job_id = apr->next_id;
	ret = ctx->decode ? apr_build_decode_desc(&decode, desc) :
			    apr->variant->build_desc(&f, desc);
	if (ret) {
		spin_unlock_irqrestore(&apr->hw_lock, flags);
		apr_finish_job(apr, ctx, ret);
		return;
	}
	apr_ring_write(apr->ring, ring_bytes, apr->head, desc, desc_bytes);
	apr->head = apr_ring_advance(apr->head, desc_bytes, ring_bytes);
	job = &apr->jobs[(apr->job_first + apr->njobs) % APR_MAX_INFLIGHT];
	*job = (struct apr_job){
		.ctx = ctx,
		.src = v4l2_m2m_src_buf_remove(m2m),
		.dst = v4l2_m2m_dst_buf_remove(m2m),
		.end = apr->head,
		.id = ctx->decode ? apr->next_id : 0,
		.sequence = ctx->sequence++,
		.bytes = ctx->dst_fmt.plane_fmt[0].sizeimage,
	};
	WARN_ON(job->src != src || job->dst != dst);
	if (!apr->njobs)
		apr->progress = jiffies;
	apr->njobs++;
	if (!ctx->decode)
		apr->nencodes++;
	if (ctx->decode)
		apr->next_id = apr_next_job_id(apr->next_id);
	atomic_inc(&ctx->inflight);
	atomic_inc(&apr->outstanding);
	/* Descriptor and buffers must be visible before the head moves. */
	dma_wmb();
	writel(apr->head, apr->regs + APR_REG_RING_HEAD);
	hold = apr_jobs_full(apr->njobs, apr->nencodes);
	if (hold)
		apr->held = ctx;
	schedule_delayed_work(&apr->watchdog, msecs_to_jiffies(APR_POLL_MS));
	spin_unlock_irqrestore(&apr->hw_lock, flags);
	if (!hold)
		v4l2_m2m_job_finish(apr->m2m_dev, m2m);
}

static int apr_job_ready(void *priv)
{
	struct apr_ctx *ctx = priv;
	struct vb2_v4l2_buffer *src;
	struct apr_prores_info info;

	if (!ctx->decode)
		return 1;
	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	if (!src || apr_parse_src(src, &info))
		return 1;
	return info.width == ctx->dst_fmt.width &&
	       info.height == ctx->dst_fmt.height &&
	       (info.chroma == 3) == ctx->raw->is444;
}

static void apr_job_abort(void *priv)
{
	struct apr_ctx *ctx = priv;
	struct apr_dev *apr = ctx->apr;
	unsigned long flags;

	unsigned int i;

	/*
	 * There is no cancellation opcode: the context's jobs still run to the
	 * end of the ring and only report cancelled. stop_streaming waits for
	 * them; other contexts' jobs in the same ring are not touched.
	 */
	spin_lock_irqsave(&apr->hw_lock, flags);
	for (i = 0; i < apr->njobs; i++) {
		struct apr_job *job = &apr->jobs[(apr->job_first + i) %
						 APR_MAX_INFLIGHT];

		if (job->ctx == ctx)
			job->err = -ECANCELED;
	}
	spin_unlock_irqrestore(&apr->hw_lock, flags);
}

static const struct v4l2_m2m_ops apr_m2m_ops = {
	.device_run = apr_device_run,
	.job_ready = apr_job_ready,
	.job_abort = apr_job_abort,
};

/* ---- formats ----------------------------------------------------------- */

static const struct apr_fmt *apr_find_raw(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(apr_raw_fmts); i++)
		if (apr_raw_fmts[i].fourcc == fourcc)
			return &apr_raw_fmts[i];
	return NULL;
}

/*
 * One contiguous plane: Y rows, then interleaved CbCr rows. The engine
 * reads whole 16-row macroblocks, so space for the rows that round the
 * height up follows the chroma region.
 */
static void apr_fill_raw(struct v4l2_pix_format_mplane *pix, const struct apr_fmt *fmt)
{
	u32 width = clamp_t(u32, pix->width, APR_MIN_W, APR_MAX_W);
	u32 height = clamp_t(u32, pix->height, APR_MIN_H, APR_MAX_H);
	u32 stride_align;
	u32 min_stride;
	u32 stride;
	u32 cstride;

	if (!fmt->is444)
		width = min_t(u32, ALIGN(width, 2), APR_MAX_W - 1);
	if ((u64)width * height > APR_MAX_PIXELS)
		height = APR_MAX_PIXELS / width;
	/* An odd row count needs a 128-byte stride to align plane 1. */
	stride_align = height & 1 ? 128 : 64;
	min_stride = ALIGN(width * fmt->bytes, stride_align);
	stride = ALIGN(pix->plane_fmt[0].bytesperline, stride_align);

	/* Accept padding up to one extra row width, as other drivers do. */
	if (stride < min_stride || stride > 2 * min_stride)
		stride = min_stride;
	cstride = stride * (fmt->is444 ? 2 : 1);
	pix->pixelformat = fmt->fourcc;
	pix->width = width;
	pix->height = height;
	pix->field = V4L2_FIELD_NONE;
	pix->num_planes = 1;
	pix->flags = 0;
	memset(pix->reserved, 0, sizeof(pix->reserved));
	memset(pix->plane_fmt, 0, sizeof(pix->plane_fmt));
	pix->plane_fmt[0].bytesperline = stride;
	pix->plane_fmt[0].sizeimage = (stride + cstride) * ALIGN(height, 16);
	if (pix->colorspace == V4L2_COLORSPACE_DEFAULT)
		pix->colorspace = V4L2_COLORSPACE_REC709;
	if (pix->xfer_func == V4L2_XFER_FUNC_DEFAULT)
		pix->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(pix->colorspace);
	if (pix->ycbcr_enc == V4L2_YCBCR_ENC_DEFAULT)
		pix->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(pix->colorspace);
	if (pix->quantization == V4L2_QUANTIZATION_DEFAULT)
		pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
}

static void apr_fill_coded(struct v4l2_pix_format_mplane *dst,
			   const struct v4l2_pix_format_mplane *src, bool is444)
{
	*dst = *src;
	dst->pixelformat = V4L2_PIX_FMT_PRORES;
	dst->num_planes = 1;
	memset(dst->plane_fmt, 0, sizeof(dst->plane_fmt));
	dst->plane_fmt[0].sizeimage = apr_coded_size(src->width, src->height, is444);
}

static void apr_fill_decode_coded(struct v4l2_pix_format_mplane *pix)
{
	u32 size = pix->plane_fmt[0].sizeimage;

	pix->width = clamp_t(u32, pix->width, APR_MIN_W, APR_MAX_W);
	pix->height = clamp_t(u32, pix->height, APR_MIN_H, APR_MAX_H);
	if ((u64)pix->width * pix->height > APR_MAX_PIXELS)
		pix->height = APR_MAX_PIXELS / pix->width;
	pix->pixelformat = V4L2_PIX_FMT_PRORES;
	pix->field = V4L2_FIELD_NONE;
	pix->num_planes = 1;
	pix->flags = 0;
	memset(pix->reserved, 0, sizeof(pix->reserved));
	memset(pix->plane_fmt, 0, sizeof(pix->plane_fmt));
	pix->plane_fmt[0].sizeimage = clamp_t(u32, size, SZ_4K, SZ_256M);
}

static void apr_reset_fmts(struct apr_ctx *ctx)
{
	ctx->raw = ctx->decode ? &apr_raw_fmts[1] : &apr_raw_fmts[0];
	memset(&ctx->src_fmt, 0, sizeof(ctx->src_fmt));
	ctx->src_fmt.width = 1920;
	ctx->src_fmt.height = 1080;
	if (ctx->decode) {
		ctx->src_fmt.plane_fmt[0].sizeimage =
			apr_coded_size(1920, 1080, false);
		apr_fill_decode_coded(&ctx->src_fmt);
		ctx->dst_fmt = ctx->src_fmt;
		apr_fill_raw(&ctx->dst_fmt, ctx->raw);
	} else {
		apr_fill_raw(&ctx->src_fmt, ctx->raw);
		apr_fill_coded(&ctx->dst_fmt, &ctx->src_fmt, ctx->raw->is444);
	}
}

static int apr_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct apr_ctx *ctx = apr_file_to_ctx(file);

	strscpy(cap->driver, APR_NAME, sizeof(cap->driver));
	strscpy(cap->card, ctx->decode ? "Apple ProRes decoder" :
					  "Apple ProRes encoder", sizeof(cap->card));
	return 0;
}

static int apr_enum_fmt_out(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	struct apr_ctx *ctx = apr_file_to_ctx(file);

	if (ctx->decode) {
		if (f->index)
			return -EINVAL;
		f->pixelformat = V4L2_PIX_FMT_PRORES;
		f->flags = V4L2_FMT_FLAG_COMPRESSED;
		return 0;
	}
	if (f->index >= ARRAY_SIZE(apr_raw_fmts))
		return -EINVAL;
	f->pixelformat = apr_raw_fmts[f->index].fourcc;
	return 0;
}

static int apr_enum_fmt_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	struct apr_ctx *ctx = apr_file_to_ctx(file);

	if (ctx->decode) {
		if (f->index >= ARRAY_SIZE(apr_raw_fmts))
			return -EINVAL;
		f->pixelformat = apr_raw_fmts[f->index].fourcc;
		return 0;
	}
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_PRORES;
	f->flags = V4L2_FMT_FLAG_COMPRESSED;
	return 0;
}

static int apr_enum_framesizes(struct file *file, void *priv,
			       struct v4l2_frmsizeenum *fs)
{
	const struct apr_fmt *fmt = apr_find_raw(fs->pixel_format);

	if (fs->index || (fs->pixel_format != V4L2_PIX_FMT_PRORES &&
			  !fmt))
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fs->stepwise = (struct v4l2_frmsize_stepwise){
		APR_MIN_W, APR_MAX_W, fmt && !fmt->is444 ? 2 : 1,
		APR_MIN_H, APR_MAX_H, 1,
	};
	return 0;
}

static int apr_g_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = apr_file_to_ctx(file)->src_fmt;
	return 0;
}

static int apr_g_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = apr_file_to_ctx(file)->dst_fmt;
	return 0;
}

static int apr_try_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct apr_ctx *ctx = apr_file_to_ctx(file);
	const struct apr_fmt *fmt = apr_find_raw(f->fmt.pix_mp.pixelformat);

	if (ctx->decode) {
		apr_fill_decode_coded(&f->fmt.pix_mp);
		return 0;
	}
	apr_fill_raw(&f->fmt.pix_mp, fmt ?: &apr_raw_fmts[0]);
	return 0;
}

/* The coded size follows the raw format, as for other stateful encoders. */
static int apr_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct apr_ctx *ctx = apr_file_to_ctx(file);
	const struct apr_fmt *fmt;

	if (ctx->decode) {
		fmt = apr_find_raw(f->fmt.pix_mp.pixelformat);
		if (!fmt || (ctx->source_valid && fmt->is444 != ctx->decode_444))
			fmt = &apr_raw_fmts[ctx->decode_444 ? 3 : 1];
		f->fmt.pix_mp.width = ctx->src_fmt.width;
		f->fmt.pix_mp.height = ctx->src_fmt.height;
		apr_fill_raw(&f->fmt.pix_mp, fmt);
		return 0;
	}
	apr_fill_coded(&f->fmt.pix_mp, &ctx->src_fmt, ctx->raw->is444);
	return 0;
}

static int apr_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct apr_ctx *ctx = apr_file_to_ctx(file);
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;

	if (vb2_is_busy(v4l2_m2m_get_src_vq(m2m)) ||
	    vb2_is_busy(v4l2_m2m_get_dst_vq(m2m)))
		return -EBUSY;
	apr_try_fmt_out(file, priv, f);
	ctx->src_fmt = f->fmt.pix_mp;
	if (ctx->decode)
		return 0;
	ctx->raw = apr_find_raw(ctx->src_fmt.pixelformat);
	apr_fill_coded(&ctx->dst_fmt, &ctx->src_fmt, ctx->raw->is444);
	/* Keep the profile consistent with the chroma format. */
	if (ctx->raw->is444 != apr_profile_is_444(v4l2_ctrl_g_ctrl(ctx->profile)))
		v4l2_ctrl_s_ctrl(ctx->profile, ctx->raw->is444 ?
				   V4L2_MPEG_VIDEO_PRORES_PROFILE_4444 :
				   V4L2_MPEG_VIDEO_PRORES_PROFILE_STANDARD);
	return 0;
}

static int apr_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct apr_ctx *ctx = apr_file_to_ctx(file);

	if (vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;
	apr_try_fmt_cap(file, priv, f);
	ctx->dst_fmt = f->fmt.pix_mp;
	if (ctx->decode)
		ctx->raw = apr_find_raw(ctx->dst_fmt.pixelformat);
	return 0;
}

static int apr_subscribe_event(struct v4l2_fh *fh, const struct v4l2_event_subscription *sub)
{
	if (sub->type == V4L2_EVENT_EOS)
		return v4l2_event_subscribe(fh, sub, 0, NULL);
	if (sub->type == V4L2_EVENT_SOURCE_CHANGE)
		return v4l2_src_change_event_subscribe(fh, sub);
	return v4l2_ctrl_subscribe_event(fh, sub);
}

/*
 * A STOP with no OUTPUT buffer left in the m2m queue makes the m2m helper
 * mark the next CAPTURE buffer LAST at once. With this context's jobs still
 * in the ring their frames would follow that LAST buffer, so wait for them
 * first. The check and the helper run under hw_lock: device_run moves a
 * job's buffers into the ring under the same lock, so the helper sees the
 * last OUTPUT buffer either queued or already given back.
 */
static int apr_stop_cmd(struct file *file, struct apr_ctx *ctx, void *cmd)
{
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct apr_dev *apr = ctx->apr;
	unsigned long flags;
	int ret;

	for (;;) {
		spin_lock_irqsave(&apr->hw_lock, flags);
		if (v4l2_m2m_num_src_bufs_ready(m2m) ||
		    !atomic_read(&ctx->inflight))
			break;
		spin_unlock_irqrestore(&apr->hw_lock, flags);
		wait_event(apr->idle_wq, !atomic_read(&ctx->inflight));
	}
	ret = ctx->decode ? v4l2_m2m_decoder_cmd(file, m2m, cmd) :
			    v4l2_m2m_encoder_cmd(file, m2m, cmd);
	spin_unlock_irqrestore(&apr->hw_lock, flags);
	return ret;
}

static int apr_encoder_cmd(struct file *file, void *priv,
			   struct v4l2_encoder_cmd *ec)
{
	struct apr_ctx *ctx = apr_file_to_ctx(file);
	int ret;

	ret = v4l2_m2m_ioctl_try_encoder_cmd(file, priv, ec);
	if (ret)
		return ret;
	if (ec->cmd == V4L2_ENC_CMD_STOP)
		return apr_stop_cmd(file, ctx, ec);
	return v4l2_m2m_ioctl_encoder_cmd(file, priv, ec);
}

static int apr_decoder_cmd(struct file *file, void *priv,
			   struct v4l2_decoder_cmd *dc)
{
	struct apr_ctx *ctx = apr_file_to_ctx(file);
	int ret;

	ret = v4l2_m2m_ioctl_try_decoder_cmd(file, priv, dc);
	if (ret)
		return ret;
	if (dc->cmd == V4L2_DEC_CMD_STOP)
		return apr_stop_cmd(file, ctx, dc);
	return v4l2_m2m_ioctl_decoder_cmd(file, priv, dc);
}

static const struct v4l2_ioctl_ops apr_ioctl_ops = {
	.vidioc_querycap = apr_querycap,
	.vidioc_enum_fmt_vid_out = apr_enum_fmt_out,
	.vidioc_enum_fmt_vid_cap = apr_enum_fmt_cap,
	.vidioc_enum_framesizes = apr_enum_framesizes,
	.vidioc_g_fmt_vid_out_mplane = apr_g_fmt_out,
	.vidioc_g_fmt_vid_cap_mplane = apr_g_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane = apr_try_fmt_out,
	.vidioc_try_fmt_vid_cap_mplane = apr_try_fmt_cap,
	.vidioc_s_fmt_vid_out_mplane = apr_s_fmt_out,
	.vidioc_s_fmt_vid_cap_mplane = apr_s_fmt_cap,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_try_encoder_cmd = v4l2_m2m_ioctl_try_encoder_cmd,
	.vidioc_encoder_cmd = apr_encoder_cmd,

	.vidioc_subscribe_event = apr_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_ioctl_ops apr_decoder_ioctl_ops = {
	.vidioc_querycap = apr_querycap,
	.vidioc_enum_fmt_vid_out = apr_enum_fmt_out,
	.vidioc_enum_fmt_vid_cap = apr_enum_fmt_cap,
	.vidioc_enum_framesizes = apr_enum_framesizes,
	.vidioc_g_fmt_vid_out_mplane = apr_g_fmt_out,
	.vidioc_g_fmt_vid_cap_mplane = apr_g_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane = apr_try_fmt_out,
	.vidioc_try_fmt_vid_cap_mplane = apr_try_fmt_cap,
	.vidioc_s_fmt_vid_out_mplane = apr_s_fmt_out,
	.vidioc_s_fmt_vid_cap_mplane = apr_s_fmt_cap,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_try_decoder_cmd = v4l2_m2m_ioctl_try_decoder_cmd,
	.vidioc_decoder_cmd = apr_decoder_cmd,

	.vidioc_subscribe_event = apr_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* ---- queues ------------------------------------------------------------ */

static struct v4l2_pix_format_mplane *apr_q_fmt(struct apr_ctx *ctx, u32 type)
{
	return V4L2_TYPE_IS_OUTPUT(type) ? &ctx->src_fmt : &ctx->dst_fmt;
}

static int apr_queue_setup(struct vb2_queue *q, unsigned int *nbufs,
			   unsigned int *nplanes, unsigned int sizes[],
			   struct device *alloc_devs[])
{
	struct apr_ctx *ctx = vb2_get_drv_priv(q);
	u32 size = apr_q_fmt(ctx, q->type)->plane_fmt[0].sizeimage;

	if (*nplanes)
		return *nplanes == 1 && sizes[0] >= size ? 0 : -EINVAL;
	*nplanes = 1;
	sizes[0] = size;
	return 0;
}

static int apr_buf_prepare(struct vb2_buffer *vb)
{
	struct apr_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	u32 size = apr_q_fmt(ctx, vb->type)->plane_fmt[0].sizeimage;

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;
	if (V4L2_TYPE_IS_OUTPUT(vb->type)) {
		if (ctx->decode) {
			struct apr_buf *buf = to_apr_buf(to_vb2_v4l2_buffer(vb));
			u32 payload = vb2_get_plane_payload(vb, 0);
			u32 offset = vb->planes[0].data_offset;
			u8 *data = vb2_plane_vaddr(vb, 0);

			if (payload < 28 || offset > payload)
				return -EINVAL;
			/* A frame that does not parse fails its job, not QBUF. */
			buf->parse_err = data ?
				apr_parse_frame(data + offset, payload - offset,
						&buf->info) : -EINVAL;
		} else {
			/* The encoder always reads the whole padded surface. */
			vb2_set_plane_payload(vb, 0, size);
		}
	} else {
		vb2_set_plane_payload(vb, 0, 0);
	}
	return 0;
}

static void apr_decode_source_change(struct apr_ctx *ctx,
				     struct vb2_v4l2_buffer *vbuf)
{
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct apr_prores_info info;
	bool changed;
	unsigned int i;

	if (apr_parse_src(vbuf, &info))
		return;
	changed = !ctx->source_valid || ctx->src_fmt.width != info.width ||
		  ctx->src_fmt.height != info.height ||
		  ctx->decode_444 != (info.chroma == 3);
	if (!changed)
		return;
	ctx->source_valid = true;
	ctx->decode_444 = info.chroma == 3;
	ctx->src_fmt.width = info.width;
	ctx->src_fmt.height = info.height;
	if (!vb2_is_busy(v4l2_m2m_get_dst_vq(m2m))) {
		for (i = 0; i < ARRAY_SIZE(apr_raw_fmts); i++) {
			if (apr_raw_fmts[i].is444 == ctx->decode_444 &&
			    apr_raw_fmts[i].bytes == ctx->raw->bytes) {
				ctx->raw = &apr_raw_fmts[i];
				break;
			}
		}
		ctx->dst_fmt.width = info.width;
		ctx->dst_fmt.height = info.height;
		apr_fill_raw(&ctx->dst_fmt, ctx->raw);
	}
	{
		const struct v4l2_event event = {
			.type = V4L2_EVENT_SOURCE_CHANGE,
			.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
		};

		v4l2_event_queue_fh(&ctx->fh, &event);
	}
}

static void apr_buf_queue(struct vb2_buffer *vb)
{
	struct apr_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	if (ctx->decode && V4L2_TYPE_IS_OUTPUT(vb->type))
		apr_decode_source_change(ctx, vbuf);
	if (!V4L2_TYPE_IS_OUTPUT(vb->type) &&
	    vb2_is_streaming(vb->vb2_queue) &&
	    v4l2_m2m_dst_buf_is_last(ctx->fh.m2m_ctx)) {
		vb2_set_plane_payload(vb, 0, 0);
		vbuf->field = V4L2_FIELD_NONE;
		v4l2_m2m_last_buffer_done(ctx->fh.m2m_ctx, vbuf);
		return;
	}
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static int apr_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct apr_ctx *ctx = vb2_get_drv_priv(q);

	if (V4L2_TYPE_IS_OUTPUT(q->type) && !ctx->decode) {
		if (ctx->raw->is444 != apr_profile_is_444(v4l2_ctrl_g_ctrl(ctx->profile))) {
			struct vb2_v4l2_buffer *vbuf;

			while ((vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx)))
				v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_QUEUED);
			return -EINVAL;
		}
		ctx->stream_profile = v4l2_ctrl_g_ctrl(ctx->profile);
		v4l2_ctrl_grab(ctx->profile, true);
		ctx->sequence = 0;
	}
	v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, q);
	return 0;
}

static void apr_stop_streaming(struct vb2_queue *q)
{
	struct apr_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *vbuf;

	/*
	 * The engine still owns this context's jobs in the ring. The m2m core
	 * cancelled the job being prepared; wait for the ring ones, which the
	 * watchdog completes or fails within APR_TIMEOUT_MS of no progress.
	 */
	wait_event(ctx->apr->idle_wq, !atomic_read(&ctx->inflight));
	for (;;) {
		vbuf = V4L2_TYPE_IS_OUTPUT(q->type) ?
			v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
			v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, q);
	if (V4L2_TYPE_IS_OUTPUT(q->type) && !ctx->decode)
		v4l2_ctrl_grab(ctx->profile, false);
	if (V4L2_TYPE_IS_OUTPUT(q->type) && v4l2_m2m_has_stopped(ctx->fh.m2m_ctx)) {
		static const struct v4l2_event eos = { .type = V4L2_EVENT_EOS };

		v4l2_event_queue_fh(&ctx->fh, &eos);
	}
}

static const struct vb2_ops apr_qops = {
	.queue_setup = apr_queue_setup,
	.buf_prepare = apr_buf_prepare,
	.buf_queue = apr_buf_queue,
	.start_streaming = apr_start_streaming,
	.stop_streaming = apr_stop_streaming,
};

static int apr_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct apr_ctx *ctx = priv;
	int ret;

	src->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src->io_modes = VB2_MMAP | VB2_DMABUF;
	src->drv_priv = ctx;
	src->buf_struct_size = sizeof(struct apr_buf);
	src->ops = &apr_qops;
	src->mem_ops = &vb2_dma_contig_memops;
	src->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src->lock = &ctx->apr->vdev_lock;
	src->dev = ctx->apr->dev;
	ret = vb2_queue_init(src);
	if (ret)
		return ret;

	dst->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst->io_modes = VB2_MMAP | VB2_DMABUF;
	dst->drv_priv = ctx;
	dst->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst->ops = &apr_qops;
	dst->mem_ops = &vb2_dma_contig_memops;
	dst->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst->lock = &ctx->apr->vdev_lock;
	dst->dev = ctx->apr->dev;
	return vb2_queue_init(dst);
}

/* ---- controls and file operations -------------------------------------- */

/* Controls are read when a job is built; nothing to apply on change. */
static int apr_s_ctrl(struct v4l2_ctrl *ctrl)
{
	return 0;
}

static const struct v4l2_ctrl_ops apr_ctrl_ops = {
	.s_ctrl = apr_s_ctrl,
};

static int apr_init_ctrls(struct apr_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->hdl;

	v4l2_ctrl_handler_init(hdl, ctx->decode ? 0 : 1);
	if (ctx->decode) {
		ctx->fh.ctrl_handler = hdl;
		return 0;
	}
	/* The profile must match the raw format's chroma; checked at STREAMON. */
	ctx->profile = v4l2_ctrl_new_std_menu(hdl, &apr_ctrl_ops,
					      V4L2_CID_MPEG_VIDEO_PRORES_PROFILE,
					      V4L2_MPEG_VIDEO_PRORES_PROFILE_4444_XQ, 0,
					      V4L2_MPEG_VIDEO_PRORES_PROFILE_STANDARD);
	if (hdl->error) {
		int ret = hdl->error;

		v4l2_ctrl_handler_free(hdl);
		return ret;
	}
	ctx->fh.ctrl_handler = hdl;
	return 0;
}

static int apr_open(struct file *file)
{
	struct apr_dev *apr = video_drvdata(file);
	struct apr_ctx *ctx;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;
	ctx->apr = apr;
	ctx->decode = video_devdata(file) == &apr->dec_vdev;
	apr_reset_fmts(ctx);
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ret = apr_init_ctrls(ctx);
	if (ret)
		goto err_fh;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(apr->m2m_dev, ctx, apr_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_ctrls;
	}
	v4l2_fh_add(&ctx->fh, file);
	return 0;

err_ctrls:
	v4l2_ctrl_handler_free(&ctx->hdl);
err_fh:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int apr_release(struct file *file)
{
	struct apr_ctx *ctx = apr_file_to_ctx(file);
	struct apr_dev *apr = ctx->apr;

	mutex_lock(&apr->vdev_lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	mutex_unlock(&apr->vdev_lock);
	v4l2_ctrl_handler_free(&ctx->hdl);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations apr_fops = {
	.owner = THIS_MODULE,
	.open = apr_open,
	.release = apr_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

/* ---- power management and probe ---------------------------------------- */

static int apr_runtime_suspend(struct device *dev)
{
	struct apr_dev *apr = dev_get_drvdata(dev);
	unsigned long flags;

	/* Registers do not survive the power domain going off. */
	spin_lock_irqsave(&apr->hw_lock, flags);
	apr->ring_ready = false;
	spin_unlock_irqrestore(&apr->hw_lock, flags);
	return 0;
}

static int apr_runtime_resume(struct device *dev)
{
	return 0;
}

static int apr_suspend(struct device *dev)
{
	struct apr_dev *apr = dev_get_drvdata(dev);

	v4l2_m2m_suspend(apr->m2m_dev);
	/* No new job starts now; let the ring drain. */
	wait_event(apr->idle_wq, !atomic_read(&apr->outstanding));
	return pm_runtime_force_suspend(dev);
}

static int apr_resume(struct device *dev)
{
	struct apr_dev *apr = dev_get_drvdata(dev);
	int ret;

	ret = pm_runtime_force_resume(dev);
	if (ret)
		return ret;
	v4l2_m2m_resume(apr->m2m_dev);
	return 0;
}

static const struct dev_pm_ops apr_pm_ops = {
	RUNTIME_PM_OPS(apr_runtime_suspend, apr_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(apr_suspend, apr_resume)
};

static void apr_cleanup(void *data)
{
	struct apr_dev *apr = data;

	cancel_delayed_work_sync(&apr->watchdog);
	if (apr->m2m_dev)
		v4l2_m2m_release(apr->m2m_dev);
	v4l2_device_unregister(&apr->v4l2_dev);
}

/*
 * Pick the power-up rows before touching the hardware. The built-in tables
 * are the default. A variant with a firmware name also accepts a per-machine
 * APRI file that replaces them; a file that is present but malformed fails
 * the probe rather than silently falling back.
 */
static int apr_select_tunables(struct apr_dev *apr)
{
	const struct apr_variant *v = apr->variant;
	const struct firmware *firmware;
	struct device *dev = apr->dev;
	unsigned int i;
	int ret;

	apr->tunables[APRI_SYSTEM_BLOCK] = v->aux_tunables;
	apr->num_tunables[APRI_SYSTEM_BLOCK] = v->num_aux_tunables;
	apr->tunables[APRI_CORE_BLOCK] = v->engine_tunables;
	apr->num_tunables[APRI_CORE_BLOCK] = v->num_engine_tunables;
	apr->tunables[APRI_WRAPPER_BLOCK] = v->wrapper_tunables;
	apr->num_tunables[APRI_WRAPPER_BLOCK] = v->num_wrapper_tunables;
	apr->tunables[APRI_CONTROL_BLOCK] = v->ctrl_tunables;
	apr->num_tunables[APRI_CONTROL_BLOCK] = v->num_ctrl_tunables;

	if (v->firmware_name &&
	    !firmware_request_nowarn(&firmware, v->firmware_name, dev)) {
		ret = apr_parse_init(v->apri, firmware->data, firmware->size,
				     &apr->init);
		release_firmware(firmware);
		if (ret)
			return dev_err_probe(dev, ret, "invalid %s\n",
					     v->firmware_name);
		for (i = 0; i < v->apri->blocks; i++) {
			apr->tunables[i] = apr->init.blocks[i].entries;
			apr->num_tunables[i] = apr->init.blocks[i].count;
		}
		dev_info(dev, "initialization rows from %s\n", v->firmware_name);
	}

	/*
	 * A variant whose rows come only from the optional file has nothing to
	 * program without it. The file is optional, so report its absence once
	 * at info level and leave the engine off.
	 */
	if (!apr->num_tunables[APRI_CORE_BLOCK]) {
		if (v->firmware_name) {
			dev_info(dev, "no %s and no built-in rows, engine not used\n",
				 v->firmware_name);
			return -ENODEV;
		}
		return dev_err_probe(dev, -ENOENT,
				     "no initialization rows for this engine\n");
	}

	return 0;
}

static int apr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct video_device *vdev;
	void __iomem *regs[4] = {};
	struct apr_dev *apr;
	unsigned int i;
	int ret;

	apr = devm_kzalloc(dev, sizeof(*apr), GFP_KERNEL);
	if (!apr)
		return -ENOMEM;
	apr->dev = dev;
	apr->variant = of_device_get_match_data(dev);
	if (!apr->variant)
		return -ENODEV;
	ret = apr_select_tunables(apr);
	if (ret)
		return ret;
	mutex_init(&apr->vdev_lock);
	spin_lock_init(&apr->hw_lock);
	INIT_DELAYED_WORK(&apr->watchdog, apr_watchdog);
	init_waitqueue_head(&apr->idle_wq);
	/* The ring always has room for every job the driver keeps in flight. */
	BUILD_BUG_ON(APR_MAX_INFLIGHT * APR_ENCODE_DESC_BYTES >= APR_RING_BYTES);
	platform_set_drvdata(pdev, apr);

	for (i = 0; i < apr->variant->num_regs; i++) {
		regs[i] = devm_platform_ioremap_resource_byname(pdev,
							     apr->variant->reg_names[i]);
		if (IS_ERR(regs[i]))
			return PTR_ERR(regs[i]);
	}
	apr->regs = regs[0];
	apr->wrapper = regs[1];
	apr->aux = regs[2];
	apr->ctrl = regs[3];
	/*
	 * A second power domain, "perf", is the SoC performance request the
	 * engine needs for full speed. With more than one domain the core
	 * attaches none, so attach both with runtime PM links: the request is
	 * held exactly while the engine is runtime-active.
	 */
	if (of_count_phandle_with_args(dev->of_node, "power-domains",
				       "#power-domain-cells") > 1) {
		static const struct dev_pm_domain_attach_data pd_data = {
			.pd_flags = PD_FLAG_DEV_LINK_ON,
		};

		ret = devm_pm_domain_attach_list(dev, &pd_data, &apr->pd_list);
		if (ret < 0)
			return dev_err_probe(dev, ret, "failed to attach power domains\n");
	}
	apr->reset = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(apr->reset))
		return dev_err_probe(dev, PTR_ERR(apr->reset), "reset\n");

	/* Every buffer the engine touches goes through its DART. */
	if (!device_iommu_mapped(dev))
		return dev_err_probe(dev, -ENODEV, "no IOMMU\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;
	apr->ring = dmam_alloc_coherent(dev, apr->variant->ring_bytes, &apr->ring_dma,
					GFP_KERNEL);
	if (!apr->ring)
		return -ENOMEM;
	apr->stats_size = APR_STATS_BYTES;
	apr->stats = dmam_alloc_coherent(dev, apr->stats_size, &apr->stats_dma, GFP_KERNEL);
	if (!apr->stats)
		return -ENOMEM;

	for (i = 0; i < apr->variant->num_irqs; i++) {
		apr->irq[i] = platform_get_irq_optional(pdev, i);
		if (apr->irq[i] == -ENXIO && i)
			continue;
		if (apr->irq[i] < 0)
			return apr->irq[i];
		ret = devm_request_irq(dev, apr->irq[i], apr_irq, 0,
				       dev_name(dev), apr);
		if (ret)
			return ret;
	}

	ret = v4l2_device_register(dev, &apr->v4l2_dev);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, apr_cleanup, apr);
	if (ret)
		return ret;
	apr->m2m_dev = v4l2_m2m_init(&apr_m2m_ops);
	if (IS_ERR(apr->m2m_dev)) {
		ret = PTR_ERR(apr->m2m_dev);
		apr->m2m_dev = NULL;
		return ret;
	}

	pm_runtime_set_autosuspend_delay(dev, APR_AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;
	/* The engine reports how much statistics space it writes. */
	if (max(apr_read(apr, apr->variant->stats[0].size),
		apr_read(apr, apr->variant->stats[1].size)) >
	    apr->stats_size) {
		pm_runtime_put_autosuspend(dev);
		return dev_err_probe(dev, -EINVAL, "statistics area larger than %zu bytes\n",
				     apr->stats_size);
	}
	dev_info(dev, "ProRes codec, id %#x\n", apr_read(apr, APR_REG_ID));
	pm_runtime_put_autosuspend(dev);

	vdev = &apr->enc_vdev;
	strscpy(vdev->name, APR_NAME, sizeof(vdev->name));
	vdev->fops = &apr_fops;
	vdev->ioctl_ops = &apr_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &apr->vdev_lock;
	vdev->v4l2_dev = &apr->v4l2_dev;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(vdev, apr);
	ret = video_register_device(vdev, VFL_TYPE_VIDEO, APPLE_M2M_VIDEO_NR);
	if (ret)
		return ret;

	/*
	 * Every APR engine is a ProRes encode and decode engine (the decode
	 * descriptor shares the encoder's ring), so each one gets a decoder node.
	 */
	vdev = &apr->dec_vdev;
	strscpy(vdev->name, APR_NAME "-decode", sizeof(vdev->name));
	vdev->fops = &apr_fops;
	vdev->ioctl_ops = &apr_decoder_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &apr->vdev_lock;
	vdev->v4l2_dev = &apr->v4l2_dev;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(vdev, apr);
	ret = video_register_device(vdev, VFL_TYPE_VIDEO, APPLE_M2M_VIDEO_NR);
	if (ret)
		video_unregister_device(&apr->enc_vdev);
	return ret;
}

static void apr_remove(struct platform_device *pdev)
{
	struct apr_dev *apr = platform_get_drvdata(pdev);

	video_unregister_device(&apr->dec_vdev);
	video_unregister_device(&apr->enc_vdev);
}

static const struct of_device_id apr_of_match[] = {
	{ .compatible = "apple,t6000-prores", .data = &apr_t6000 },
	{ .compatible = "apple,t6041-prores", .data = &apr_t6041 },
	{ .compatible = "apple,t6050-prores", .data = &apr_t6050 },
	{ .compatible = "apple,t8140-prores", .data = &apr_t8140 },
	{ }
};
MODULE_DEVICE_TABLE(of, apr_of_match);

static struct platform_driver apr_driver = {
	.probe = apr_probe,
	.remove = apr_remove,
	.driver = {
		.name = APR_NAME,
		.of_match_table = apr_of_match,
		.pm = pm_ptr(&apr_pm_ops),
	},
};
module_platform_driver(apr_driver);

MODULE_DESCRIPTION("Apple ProRes codec");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(APRI_T6041_FIRMWARE_NAME);
MODULE_FIRMWARE(APRI_T6050_FIRMWARE_NAME);
