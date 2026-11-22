// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Aurora Silicon */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "afkv2-codec.h"
#define MAX_PENDING_CMDS 16
#define SZ_1M 1048576U
#define EACCES 13
#define spin_lock_irqsave(lock, flags) \
	do {                           \
		(void)(lock);          \
		(flags) = 0;           \
	} while (0)
#define spin_unlock_irqrestore(lock, flags) \
	do {                                \
		(void)(lock);               \
		(void)(flags);              \
	} while (0)
#define dma_rmb() \
	do {      \
	} while (0)
#define dma_wmb() \
	do {      \
	} while (0)
#define dev_warn_ratelimited(...) \
	do {                      \
	} while (0)
static int test_bit(unsigned int bit, const unsigned long *map)
{
	return !!(*map & (1UL << bit));
}

struct epic_cmd_info {
	bool done;
	void *rxbuf, *txbuf;
	u64 rxbuf_dma, txbuf_dma;
	size_t rxlen, txlen;
};

enum dcp_firmware_version {
	DCP_FIRMWARE_UNKNOWN,
	DCP_FIRMWARE_V_27_0,
};

struct apple_dcp {
	enum dcp_firmware_version fw_compat;
};

struct apple_dcp_afkep {
	bool wire_v2;
	struct apple_dcp *dcp;
};

/* Firmware 27's retained-DART fallback: record the call, return the verdict. */
static int remote_calls, remote_ret = -EACCES;
static u64 remote_dva;
static size_t remote_size;
static bool remote_to_remote, remote_copy;

static int afk_v27_access_remote(struct apple_dcp_afkep *ep, u64 dva,
				 void *buffer, size_t size, bool to_remote,
				 bool copy)
{
	(void)ep;
	(void)buffer;
	remote_calls++;
	remote_dva = dva;
	remote_size = size;
	remote_to_remote = to_remote;
	remote_copy = copy;
	return remote_ret;
}

struct apple_epic_service {
	int lock;
	struct apple_dcp_afkep *ep;
	unsigned long cmd_map[1];
	struct epic_cmd_info cmds[MAX_PENDING_CMDS];
};
