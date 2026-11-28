#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Active lane-count validation in dptxport_call_set_active_lane_count().

Builds the production dptxport_call_set_active_lane_count() from
drivers/gpu/drm/apple/dptxep.c and verifies that fixed non-ATC-PHY outputs
(dcp->dptx_phy > 3, such as HDMI-A ports without an ATC PHY) and USB4 DP
tunnels do not trigger the ATC PHY "unexpected lane count" error when
firmware sets lanes before a max-lane query or after a link-down, while
standard ATC PHY outputs (dptx_phy <= 3) still flag unexpected counts and
invalid lane counts are rejected for all outputs.

--mutation names a change to the production source that the matrix must kill.
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

MUTATIONS = {
    # Revert the fixed non-ATC-PHY (dptx_phy > 3) exemption.
    'drop-fixed-phy': (
        ' && !dcp_is_usb4_output(dcp) &&\n\t\t   dcp->dptx_phy <= 3)',
        ' && !dcp_is_usb4_output(dcp))',
    ),
    # Suppress the ATC PHY unexpected-lane-count check altogether.
    'skip-all-unexpected': (
        'dptx->lane_count < lane_count',
        '0',
    ),
}

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation', choices=sorted(MUTATIONS))
args = parser.parse_args()

root = Path(__file__).resolve().parents[2]
dptx_c = (root / 'drivers/gpu/drm/apple/dptxep.c').read_text()


def extract_function(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


fn_src = extract_function(
    dptx_c,
    'static int dptxport_call_set_active_lane_count(',
)

if args.mutation:
    old, new = MUTATIONS[args.mutation]
    assert fn_src.count(old) == 1, args.mutation
    fn_src = fn_src.replace(old, new)

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef uint32_t __le32;
typedef unsigned long long __le64;

static inline u64 le64_to_cpu(__le64 v) { return (u64)v; }
static inline __le64 cpu_to_le64(u64 v) { return (__le64)v; }
static inline __le32 cpu_to_le32(u32 v) { return (__le32)v; }
static inline u32 le32_to_cpu(__le32 v) { return (u32)v; }

struct completion {
	unsigned int done;
};

struct phy {
	int dummy;
};

union phy_configure_opts {
	struct {
		u32 lanes;
		bool set_lanes;
	} dp;
};

struct dptxport_apcall_set_active_lane_count {
	__le32 retcode;
	u8 _unk0[12];
	__le64 lane_count;
	u8 _unk1[8];
} __attribute__((packed));

struct apple_dcp {
	void *dev;
	u32 dptx_phy;
	bool external_h17p;
	bool usb4_output;
	bool t8132_ext;
};

struct apple_dcp_afkep {
	struct apple_dcp *dcp;
};

struct dptx_port {
	struct phy *atcphy;
	union phy_configure_opts phy_ops;
	u32 lane_count;
	struct completion linkcfg_completion;
};

struct apple_epic_service {
	struct apple_dcp_afkep *ep;
	struct dptx_port *cookie;
};

static int unexpected_err_calls;
static int invalid_err_calls;
static int other_err_calls;
static int phy_configure_calls;
static int phy_configure_ret;
static int h17p_validate_calls;
static int h17p_validate_ret;
static int hotplug_calls;

static void record_err(const char *fmt, ...)
{
	if (strstr(fmt, "unexpected lane"))
		unexpected_err_calls++;
	else if (strstr(fmt, "invalid lane count"))
		invalid_err_calls++;
	else
		other_err_calls++;
}

#define dev_err(dev, fmt, ...) record_err(fmt, ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...) do { (void)(dev); } while (0)

static bool dcp_is_external_h17p(struct apple_dcp *dcp)
{
	return dcp->external_h17p;
}

static bool dcp_is_usb4_output(struct apple_dcp *dcp)
{
	return dcp->usb4_output;
}

static bool dcp_is_t8132_ext(struct apple_dcp *dcp)
{
	return dcp->t8132_ext;
}

static int dptx_j700_validate_lanes(struct dptx_port *dptx, u64 lane_count)
{
	(void)dptx;
	(void)lane_count;
	h17p_validate_calls++;
	return h17p_validate_ret;
}

static int phy_configure(struct phy *phy, union phy_configure_opts *opts)
{
	(void)phy;
	(void)opts;
	phy_configure_calls++;
	return phy_configure_ret;
}

static void t8132_ext_drm_hotplug(void *dev)
{
	(void)dev;
	hotplug_calls++;
}

static void complete(struct completion *c)
{
	c->done++;
}

''' + fn_src + r'''

static void reset_counters(void)
{
	unexpected_err_calls = 0;
	invalid_err_calls = 0;
	other_err_calls = 0;
	phy_configure_calls = 0;
	phy_configure_ret = 0;
	h17p_validate_calls = 0;
	h17p_validate_ret = 0;
	hotplug_calls = 0;
}

static int call_set_lanes(struct apple_epic_service *svc, u64 lanes,
			  struct dptxport_apcall_set_active_lane_count *reply)
{
	struct dptxport_apcall_set_active_lane_count req = {
		.lane_count = cpu_to_le64(lanes),
	};

	memset(reply, 0, sizeof(*reply));
	return dptxport_call_set_active_lane_count(svc, &req, sizeof(req),
						   reply, sizeof(*reply));
}

int main(void)
{
	struct phy dummy_phy = { 0 };
	struct dptxport_apcall_set_active_lane_count reply;
	unsigned int cases = 0;

	/*
	 * 1. Fixed non-ATC-PHY outputs (dptx_phy > 3, e.g. HDMI-A with phy 4 or 5):
	 *    bringing up 4 lanes from initial 0, dropping to 0, raising to 2 and 4
	 *    must never log "unexpected lane count" and must update dptx->lane_count.
	 */
	for (u32 phy_idx = 4; phy_idx <= 5; phy_idx++) {
		struct apple_dcp dcp = { .dptx_phy = phy_idx };
		struct apple_dcp_afkep ep = { .dcp = &dcp };
		struct dptx_port port = { .atcphy = NULL, .lane_count = 0 };
		struct apple_epic_service svc = { .ep = &ep, .cookie = &port };
		const u64 seq[] = { 4, 0, 2, 4, 1, 4 };

		for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
			unsigned int prev_done = port.linkcfg_completion.done;
			int ret;

			reset_counters();
			ret = call_set_lanes(&svc, seq[i], &reply);
			assert(ret == 0);
			assert(unexpected_err_calls == 0);
			assert(invalid_err_calls == 0);
			assert(other_err_calls == 0);
			assert(phy_configure_calls == 0);
			assert(hotplug_calls == 0);
			assert(le32_to_cpu(reply.retcode) == 0);
			assert(le64_to_cpu(reply.lane_count) == seq[i]);
			assert(port.lane_count == seq[i]);
			assert(port.linkcfg_completion.done ==
			       prev_done + (seq[i] > 0 ? 1u : 0u));
			cases++;
		}
	}

	/*
	 * 2. Standard ATC PHY outputs (dptx_phy <= 3, non-USB4, non-H17P):
	 *    requesting more lanes than dptx->lane_count must still emit the
	 *    "unexpected lane count" error; requesting <= dptx->lane_count must not.
	 */
	for (u32 phy_idx = 0; phy_idx <= 3; phy_idx++) {
		struct apple_dcp dcp = { .dptx_phy = phy_idx };
		struct apple_dcp_afkep ep = { .dcp = &dcp };
		struct dptx_port port = { .atcphy = &dummy_phy, .lane_count = 0 };
		struct apple_epic_service svc = { .ep = &ep, .cookie = &port };
		int ret;

		reset_counters();
		ret = call_set_lanes(&svc, 4, &reply);
		assert(ret == 0);
		assert(unexpected_err_calls == 1);
		assert(invalid_err_calls == 0);
		assert(phy_configure_calls == 0);
		assert(port.lane_count == 0);
		assert(le32_to_cpu(reply.retcode) == 0);
		assert(le64_to_cpu(reply.lane_count) == 4);
		cases++;

		port.lane_count = 4;
		reset_counters();
		ret = call_set_lanes(&svc, 2, &reply);
		assert(ret == 0);
		assert(unexpected_err_calls == 0);
		assert(invalid_err_calls == 0);
		assert(port.lane_count == 4);
		assert(le32_to_cpu(reply.retcode) == 0);
		assert(le64_to_cpu(reply.lane_count) == 2);
		cases++;
	}

	/*
	 * 3. USB4 DP tunnels on any dptx_phy (0..3):
	 *    no "unexpected lane count" error, updates dptx->lane_count, skips
	 *    phy_configure(), and triggers T8132 hotplug only on lane changes.
	 */
	{
		struct apple_dcp dcp = {
			.dptx_phy = 1,
			.usb4_output = true,
			.t8132_ext = true,
		};
		struct apple_dcp_afkep ep = { .dcp = &dcp };
		struct dptx_port port = { .atcphy = &dummy_phy, .lane_count = 0 };
		struct apple_epic_service svc = { .ep = &ep, .cookie = &port };
		int ret;

		reset_counters();
		ret = call_set_lanes(&svc, 4, &reply);
		assert(ret == 0);
		assert(unexpected_err_calls == 0);
		assert(phy_configure_calls == 0);
		assert(hotplug_calls == 1);
		assert(port.lane_count == 4);
		cases++;

		reset_counters();
		ret = call_set_lanes(&svc, 4, &reply);
		assert(ret == 0);
		assert(unexpected_err_calls == 0);
		assert(hotplug_calls == 0);
		assert(port.lane_count == 4);
		cases++;
	}

	/*
	 * 4. External H17P output:
	 *    uses dptx_j700_validate_lanes() and configures ATC PHY when present.
	 */
	{
		struct apple_dcp dcp = { .dptx_phy = 0, .external_h17p = true };
		struct apple_dcp_afkep ep = { .dcp = &dcp };
		struct dptx_port port = { .atcphy = &dummy_phy, .lane_count = 0 };
		struct apple_epic_service svc = { .ep = &ep, .cookie = &port };
		int ret;

		reset_counters();
		ret = call_set_lanes(&svc, 4, &reply);
		assert(ret == 0);
		assert(h17p_validate_calls == 1);
		assert(unexpected_err_calls == 0);
		assert(phy_configure_calls == 1);
		assert(port.lane_count == 4);
		cases++;
	}

	/*
	 * 5. Invalid lane counts (3, 5, 8) are rejected even on dptx_phy > 3.
	 */
	{
		struct apple_dcp dcp = { .dptx_phy = 4 };
		struct apple_dcp_afkep ep = { .dcp = &dcp };
		struct dptx_port port = { .atcphy = NULL, .lane_count = 4 };
		struct apple_epic_service svc = { .ep = &ep, .cookie = &port };
		const u64 bad[] = { 3, 5, 8 };

		for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			unsigned int prev_done = port.linkcfg_completion.done;
			int ret;

			reset_counters();
			ret = call_set_lanes(&svc, bad[i], &reply);
			assert(ret == 0);
			assert(unexpected_err_calls == 0);
			assert(invalid_err_calls == 1);
			assert(le32_to_cpu(reply.retcode) == 1);
			assert(le64_to_cpu(reply.lane_count) == 0);
			assert(port.linkcfg_completion.done == prev_done);
			cases++;
		}
	}

	/*
	 * 6. Truncated request or reply buffers fail with -1.
	 */
	{
		struct apple_dcp dcp = { .dptx_phy = 4 };
		struct apple_dcp_afkep ep = { .dcp = &dcp };
		struct dptx_port port = { .atcphy = NULL, .lane_count = 0 };
		struct apple_epic_service svc = { .ep = &ep, .cookie = &port };
		struct dptxport_apcall_set_active_lane_count req = { 0 };

		assert(dptxport_call_set_active_lane_count(&svc, &req, sizeof(req) - 1,
							   &reply, sizeof(reply)) == -1);
		assert(dptxport_call_set_active_lane_count(&svc, &req, sizeof(req),
							   &reply, sizeof(reply) - 1) == -1);
		cases += 2;
	}

	printf("%u cases\n", cases);
	return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'test.c'
    src.write_text(program)
    exe = src.with_suffix('')
    subprocess.run(
        ['cc', '-Werror', '-Wall', '-Wno-unused-function',
         '-fsanitize=address,undefined', str(src), '-o', str(exe)],
        check=True,
    )
    run = subprocess.run([str(exe)])
    if run.returncode:
        sys.exit(f'FAIL: dptx set_active_lane_count matrix exited {run.returncode}')
print('PASS fixed non-ATC-PHY (dptx_phy > 3) and USB4 tunnels skip ATC PHY lane-count error; ATC PHY and invalid counts preserved')
