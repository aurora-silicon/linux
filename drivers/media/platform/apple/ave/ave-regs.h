/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple AVE video encoder: registers and firmware interface constants
 *
 * Copyright The Aurora Silicon Contributors
 */
#ifndef _APPLE_AVE_REGS_H_
#define _APPLE_AVE_REGS_H_

#include <linux/bits.h>

/* Mailbox window */
#define AVE_MBOX_DOORBELL		0x0c
#define AVE_MBOX_STATUS			0x10
#define AVE_MBOX_STATUS_PENDING		BIT(0)
#define AVE_MBOX_SCRATCH(n)		(0x18 + (n) * 4)
#define AVE_MBOX_SCRATCH_COUNT		8
#define AVE_MBOX_READY			AVE_MBOX_SCRATCH(3)
#define AVE_MBOX_TIME_LO		AVE_MBOX_SCRATCH(4)
#define AVE_MBOX_TIME_HI		AVE_MBOX_SCRATCH(5)
#define AVE_MBOX_TIMEOUT_US		5000000

/* Coprocessor window */
#define AVE_COPROC_VCBAR		0x50000
#define AVE_COPROC_VCBAR_VALID		BIT(0)
#define AVE_COPROC_TIMEBASE_FREQ	0x360020
#define AVE_COPROC_TIMEBASE		0x378000
#define AVE_COPROC_CTRL			0x600044
#define AVE_COPROC_CTRL_RUN		BIT(4)
#define AVE_COPROC_STATUS		0x600048
#define AVE_COPROC_BOOT_ARG		0x600400
#define AVE_COPROC_BOOT_ARG_VALUE	0x10000
#define AVE_COPROC_KEEP_MAPPINGS	0x600808

/* Boot request and the configuration block it points at */
#define AVE_BOOT_MAGIC			0x08042006
#define AVE_BOOT_CFG_BYTES		0x40
#define AVE_BOOT_CFG_DEVICE		1	/* word index */
#define AVE_BOOT_CFG_UNITS		2
#define AVE_BOOT_CFG_GROUPS		3
#define AVE_BOOT_CFG_SUBID_MASK		4
#define AVE_BOOT_CFG_LOG_LO		8
#define AVE_BOOT_CFG_LOG_HI		9
#define AVE_BOOT_CFG_LOG_BYTES		10
#define AVE_BOOT_CFG_OPTIONS		12
#define AVE_BOOT_CFG_OPTIONS_VALUE	0x77

/* Firmware log: 512-byte header, then a 128 KiB ring. Mapping is page sized. */
#define AVE_LOG_BYTES			0x24000
#define AVE_LOG_HDR_USED		0x20200
#define AVE_LOG_HDR_BYTES		0x200
#define AVE_LOG_RING_BYTES		0x20000

/* Hello reply: {version, channel area bytes, descriptor bytes, heap bytes} */
#define AVE_HELLO_VERSION		2
#define AVE_HELLO_DESC_BYTES		0x100
#define AVE_HELLO_EXTRA_MAX		0x13c000

/* Handshake block in the IPC window */
#define AVE_HS_BYTES			0x50
#define AVE_HS_CHANNELS			0x08
#define AVE_HS_SHARED			0x10
#define AVE_HS_SHARED_BYTES		0x18
#define AVE_HS_HEAP			0x1c
#define AVE_HS_HEAP_BYTES		0x24
#define AVE_HS_DEVICE_TYPE		0x28

/* Channel descriptors the firmware writes into the channel area */
#define AVE_CHAN_DESC_STRIDE		0x100
#define AVE_CHAN_DESC_NAME_LEN		0x40
#define AVE_CHAN_DESC_DIR		0x40
#define AVE_CHAN_DESC_IRQ		0x44
#define AVE_CHAN_DESC_SLOTS		0x48
#define AVE_CHAN_DESC_ADDR		0x4c
#define AVE_RING_SLOT_BYTES		64

/* Common command header */
#define AVE_CMD_OPCODE			0x00
#define AVE_CMD_TOKEN			0x08
#define AVE_CMD_CLIENT			0x10
#define AVE_CMD_UNIT			0x18
#define AVE_CMD_CODEC			0x1c
#define AVE_CMD_OPCODE_ECHO		0x20
#define AVE_CMD_ARG0			0x24
#define AVE_CMD_ARG1			0x28
#define AVE_CMD_ARG2			0x2c
#define AVE_CMD_TIMEOUT			0x30
#define AVE_CMD_CREATED			0x38
#define AVE_CMD_HDR_BYTES		0x48

/* Timeouts the firmware is given, and how long the host waits */
#define AVE_CMD_GLOBAL_TIMEOUT_US	3000000ULL
#define AVE_CMD_SESSION_TIMEOUT_US	94400000ULL
#define AVE_CMD_WAIT_MS			5000
#define AVE_START_WAIT_MS		10000
#define AVE_FRAME_WAIT_MS		5000
/* Halt is never answered; give the firmware this long before power-off. */
#define AVE_HALT_SETTLE_MS		20

enum ave_opcode {
	AVE_OP_CONFIG = 1,
	AVE_OP_HALT = 2,
	AVE_OP_OPEN = 3,
	AVE_OP_CLOSE = 4,
	AVE_OP_START = 6,
	AVE_OP_STOP = 7,
	AVE_OP_PROCESS = 8,
	AVE_OP_COMPLETE = 9,
	AVE_OP_PRIORITY = 10,
};

/* Firmware-to-host messages */
#define AVE_REPLY_ID			0x00
#define AVE_REPLY_CLIENT		0x10
#define AVE_REPLY_SUBTYPE		0x20
#define AVE_REPLY_STATUS		0x40
#define AVE_REPLY_MIN_BYTES		0x48
#define AVE_REPLY_PROCESS_BYTES		0x50

#define AVE_HALT_ALL			0xffffffff

/* Session command header values */
#define AVE_SESSION_UNIT		1
#define AVE_SESSION_ARG0		0xc8
#define AVE_SESSION_ARG1		0x36
#define AVE_SESSION_ARG2_SETUP		0x1e0	/* Open and Start */
#define AVE_SESSION_ARG2_SETUP_SHARED	0x140	/* the same, another session open */
#define AVE_SESSION_ARG2_H264		0x40	/* every other session command */
#define AVE_SESSION_ARG2_HEVC		0x20
#define AVE_TOKEN_STEP			100
#define AVE_TOKEN_BASE			300	/* the first Open carries 400 */
#define AVE_CLIENT_STEP			10

/* Completes sent to drain a session */
#define AVE_DRAIN_COMPLETES		4

#define AVE_CONFIG_BYTES		0x78
#define AVE_CONFIG_ALL_UNITS		0x20
#define AVE_CONFIG_OPT_A		0x49
#define AVE_CONFIG_OPT_B		0x4a
#define AVE_CONFIG_MCC_DSID		0x60
#define AVE_CONFIG_MCC_DSID_T8140	0x12

#endif
