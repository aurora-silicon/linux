#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test T6040 USB2/USB3/DP/CIO PHY firmware-sleep resume state resets on the host."""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "drivers/phy/apple/t6040-usb2.c"


def extract_block(source, pattern):
    match = re.search(pattern, source, re.M)
    if not match:
        raise ValueError(f"missing pattern: {pattern}")
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    while end < len(source) and source[end] in "; \t":
        end += 1
    return source[match.start():end]


def extract_function(source, name):
    return extract_block(source, r"^static [^\n]*\b" + name + r"\([^;]*?\n\{")


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
#define __iomem
#define T6040_RECOVERY_RETRIES 1
#define T6040_USB3_NR_TUNABLES 9

#define min_t(type, x, y) ({			\
	type __min1 = (x);			\
	type __min2 = (y);			\
	__min1 < __min2 ? __min1 : __min2; })

enum phy_mode {
	PHY_MODE_INVALID,
	PHY_MODE_USB_HOST,
	PHY_MODE_DP,
};

enum typec_orientation {
	TYPEC_ORIENTATION_NONE,
	TYPEC_ORIENTATION_NORMAL,
	TYPEC_ORIENTATION_REVERSE,
};

struct phy_configure_opts_dp {
	unsigned int link_rate;
	unsigned int lanes;
	unsigned int voltage[4];
	unsigned int pre[4];
	u8 set_rate : 1;
	u8 set_lanes : 1;
	u8 set_voltages : 1;
	u8 set_tunnel_rate : 1;
	u8 tunnel_dpin;
	u8 tunnel_rate;
};

union phy_configure_opts {
	struct phy_configure_opts_dp dp;
};

struct device {
	void *drvdata;
	int rpm_refs;
	bool awake_path;
};

struct mutex {
	int locked;
};

typedef struct {
	int counter;
} atomic_t;

struct completion {
	int done;
};

struct reset_controller_dev {
	int unused;
};

struct phy {
	void *drvdata;
};
struct resource;
struct apple_tunable;
struct apple_rtkit {
	bool running;
};
struct dentry;
struct typec_mux_dev;

static void *dev_get_drvdata(const struct device *dev)
{
	return dev->drvdata;
}

static void *phy_get_drvdata(struct phy *phy)
{
	return phy->drvdata;
}

static void mutex_lock(struct mutex *m)
{
	assert(m->locked == 0);
	m->locked = 1;
}

static bool mutex_trylock(struct mutex *m)
{
	if (m->locked)
		return false;
	m->locked = 1;
	return true;
}

static void mutex_unlock(struct mutex *m)
{
	assert(m->locked == 1);
	m->locked = 0;
}

static void lockdep_assert_held(const struct mutex *m)
{
	assert(m->locked == 1);
}

static struct mutex *mock_mutex_guard_enter(struct mutex *m)
{
	mutex_lock(m);
	return m;
}

static void mock_mutex_guard_exit(struct mutex **mp)
{
	mutex_unlock(*mp);
}

#define GUARD_CAT2(a, b) a##b
#define GUARD_CAT(a, b) GUARD_CAT2(a, b)
#define guard(type) \
	struct mutex *GUARD_CAT(__guard_m_, __COUNTER__) \
	__attribute__((cleanup(mock_mutex_guard_exit))) = mock_mutex_guard_enter
#define SCOPED_GUARD_IMPL(m, once, lock) \
	for (struct mutex *m __attribute__((cleanup(mock_mutex_guard_exit))) = \
		mock_mutex_guard_enter(lock), \
	     *once = (struct mutex *)1; once; once = NULL)
#define scoped_guard(type, lock) \
	SCOPED_GUARD_IMPL(GUARD_CAT(__sg_m_, __COUNTER__), \
			  GUARD_CAT(__sg_once_, __COUNTER__), lock)

static bool fake_suspend_no_platform;
static bool fake_resume_via_firmware;
static int fake_rpm_resume_err;
static int fake_dp_phy_init_err;
static int fake_dp_set_rate_err;
static bool fake_usb3_up_fail;
static unsigned int warn_calls;
static unsigned int rtkit_free_calls;
static struct apple_rtkit *last_freed_rtk;
static unsigned int rpm_put_noidle_calls;
static unsigned int rpm_resume_and_get_calls;
static unsigned int rpm_put_calls;
static unsigned int rpm_request_idle_calls;
static unsigned int usb3_up_calls;
static unsigned int usb3_down_calls;
static unsigned int usb3_reclaim_calls;
static unsigned int dp_phy_init_calls;
static bool last_dp_phy_init_flip;
static unsigned int dp_mode_setup_calls;
static bool last_dp_mode_setup_flip;
static unsigned int dp_aux_init_calls;
static unsigned int dp_set_rate_calls;
static int last_dp_set_rate_idx;
static unsigned int dp_drive_calls;
static unsigned int dp_tx_cmn_down_calls;
static unsigned int usb3_sleep_calls;
static unsigned int tun_rate_calls;
static const u32 dp_training_default[16] = { 0 };

static bool pm_suspend_no_platform(void)
{
	return fake_suspend_no_platform;
}

static bool pm_resume_via_firmware(void)
{
	return fake_resume_via_firmware;
}

static void device_set_awake_path(struct device *dev)
{
	dev->awake_path = true;
}

static bool apple_rtkit_is_running(struct apple_rtkit *rtk)
{
	return rtk && rtk->running;
}

static void apple_rtkit_free(struct apple_rtkit *rtk)
{
	assert(rtk != NULL);
	rtkit_free_calls++;
	last_freed_rtk = rtk;
}

static int pm_runtime_resume_and_get(struct device *dev)
{
	assert(dev != NULL);
	rpm_resume_and_get_calls++;
	if (fake_rpm_resume_err)
		return fake_rpm_resume_err;
	dev->rpm_refs++;
	return 0;
}

static void pm_runtime_put(struct device *dev)
{
	assert(dev != NULL);
	assert(dev->rpm_refs > 0);
	dev->rpm_refs--;
	rpm_put_calls++;
}

static void pm_runtime_put_noidle(struct device *dev)
{
	assert(dev != NULL);
	assert(dev->rpm_refs > 0);
	dev->rpm_refs--;
	rpm_put_noidle_calls++;
}

static int pm_request_idle(struct device *dev);

#define dev_info(dev, fmt, ...) do { (void)(dev); (void)(fmt); } while (0)
#define dev_err(dev, fmt, ...) do { (void)(dev); (void)(fmt); } while (0)
#define dev_warn(dev, fmt, ...) do { (void)(dev); (void)(fmt); warn_calls++; } while (0)
#define dev_dbg(dev, fmt, ...) do { (void)(dev); (void)(fmt); } while (0)

struct t6040_usb2_phy;
static void t6040_usb3_up_locked(struct t6040_usb2_phy *tphy);
static void t6040_usb3_down_locked(struct t6040_usb2_phy *tphy);
static void t6040_usb3_reclaim_locked(struct t6040_usb2_phy *tphy);
static void t6040_phy_tx_cmn_down(struct t6040_usb2_phy *tphy);
static void t6040_usb3_sleep(struct t6040_usb2_phy *tphy);
static int t6040_dp_phy_init(struct t6040_usb2_phy *tphy, bool flip);
static void t6040_dp_mode_setup(struct t6040_usb2_phy *tphy, bool flip);
static void t6040_dp_aux_init(struct t6040_usb2_phy *tphy);
static int t6040_dp_set_rate(struct t6040_usb2_phy *tphy, int idx);
static void t6040_dp_drive(struct t6040_usb2_phy *tphy, unsigned int lane, u32 preset);
static int t6040_tun_rate_locked(struct t6040_usb2_phy *tphy, unsigned int dpin, u8 rate);
'''

TESTS = r'''
static struct device dev;
static struct phy dp_phy;
static struct t6040_usb2_phy phy;
static struct apple_rtkit fake_rtk_1, fake_rtk_2;

static int pm_request_idle(struct device *d)
{
	struct t6040_usb2_phy *tphy = dev_get_drvdata(d);

	assert(d != NULL);
	/* Must be called outside tphy->lock. */
	assert(tphy->lock.locked == 0);
	rpm_request_idle_calls++;
	return 0;
}

static void t6040_usb3_up_locked(struct t6040_usb2_phy *tphy)
{
	lockdep_assert_held(&tphy->lock);
	assert(tphy->dev->rpm_refs > 0);
	usb3_up_calls++;
	if (!tphy->usb3_ok || tphy->usb3_up || tphy->dp_up || tphy->cio_up)
		return;
	if (tphy->orientation != TYPEC_ORIENTATION_NORMAL &&
	    tphy->orientation != TYPEC_ORIENTATION_REVERSE)
		return;
	t6040_drop_stale_rtk_locked(tphy);
	if (fake_usb3_up_fail)
		return;
	tphy->rtk = &fake_rtk_2;
	tphy->usb3_up = true;
}

static void t6040_usb3_down_locked(struct t6040_usb2_phy *tphy)
{
	lockdep_assert_held(&tphy->lock);
	usb3_down_calls++;
	if (tphy->pipe_switched)
		tphy->pipe_switched = false;
	if (tphy->dp_up && tphy->dp_mode_two_lanes)
		return;
	if (tphy->usb3_up) {
		tphy->usb3_up = false;
		t6040_rtkit_stop_transport(tphy);
	}
}

static void t6040_usb3_reclaim_locked(struct t6040_usb2_phy *tphy)
{
	lockdep_assert_held(&tphy->lock);
	usb3_reclaim_calls++;
}

static void t6040_phy_tx_cmn_down(struct t6040_usb2_phy *tphy)
{
	lockdep_assert_held(&tphy->lock);
	dp_tx_cmn_down_calls++;
}

static void t6040_usb3_sleep(struct t6040_usb2_phy *tphy)
{
	lockdep_assert_held(&tphy->lock);
	usb3_sleep_calls++;
}

static int t6040_dp_phy_init(struct t6040_usb2_phy *tphy, bool flip)
{
	lockdep_assert_held(&tphy->lock);
	assert(tphy->dev->rpm_refs > 0);
	dp_phy_init_calls++;
	last_dp_phy_init_flip = flip;
	return fake_dp_phy_init_err;
}

static void t6040_dp_mode_setup(struct t6040_usb2_phy *tphy, bool flip)
{
	lockdep_assert_held(&tphy->lock);
	assert(tphy->dev->rpm_refs > 0);
	dp_mode_setup_calls++;
	last_dp_mode_setup_flip = flip;
}

static void t6040_dp_aux_init(struct t6040_usb2_phy *tphy)
{
	lockdep_assert_held(&tphy->lock);
	assert(tphy->dev->rpm_refs > 0);
	dp_aux_init_calls++;
}

static int t6040_dp_set_rate(struct t6040_usb2_phy *tphy, int idx)
{
	lockdep_assert_held(&tphy->lock);
	assert(tphy->dev->rpm_refs > 0);
	dp_set_rate_calls++;
	last_dp_set_rate_idx = idx;
	if (fake_dp_set_rate_err)
		return fake_dp_set_rate_err;
	tphy->dp_rate = idx;
	return 0;
}

static void t6040_dp_drive(struct t6040_usb2_phy *tphy, unsigned int lane, u32 preset)
{
	(void)lane;
	(void)preset;
	lockdep_assert_held(&tphy->lock);
	dp_drive_calls++;
}

static int t6040_tun_rate_locked(struct t6040_usb2_phy *tphy, unsigned int dpin, u8 rate)
{
	(void)dpin;
	(void)rate;
	lockdep_assert_held(&tphy->lock);
	tun_rate_calls++;
	return 0;
}

static void reset_fixture(void)
{
	memset(&dev, 0, sizeof(dev));
	memset(&dp_phy, 0, sizeof(dp_phy));
	memset(&phy, 0, sizeof(phy));
	dev.drvdata = &phy;
	dev.rpm_refs = 1;
	dp_phy.drvdata = &phy;
	phy.dev = &dev;
	phy.dp = &dp_phy;
	phy.mode = PHY_MODE_USB_HOST;
	phy.usb3_ok = true;
	phy.dp_ok = true;
	phy.dp_rate = -1;
	fake_rtk_1 = (struct apple_rtkit){ .running = true };
	fake_rtk_2 = (struct apple_rtkit){ .running = true };
	fake_suspend_no_platform = false;
	fake_resume_via_firmware = false;
	fake_rpm_resume_err = 0;
	fake_dp_phy_init_err = 0;
	fake_dp_set_rate_err = 0;
	fake_usb3_up_fail = false;
	warn_calls = 0;
	rtkit_free_calls = 0;
	last_freed_rtk = NULL;
	rpm_put_noidle_calls = 0;
	rpm_resume_and_get_calls = 0;
	rpm_put_calls = 0;
	rpm_request_idle_calls = 0;
	usb3_up_calls = 0;
	usb3_down_calls = 0;
	usb3_reclaim_calls = 0;
	dp_phy_init_calls = 0;
	last_dp_phy_init_flip = false;
	dp_mode_setup_calls = 0;
	last_dp_mode_setup_flip = false;
	dp_aux_init_calls = 0;
	dp_set_rate_calls = 0;
	last_dp_set_rate_idx = -2;
	dp_drive_calls = 0;
	dp_tx_cmn_down_calls = 0;
	usb3_sleep_calls = 0;
	tun_rate_calls = 0;
}

static union phy_configure_opts dp_rate_opts(unsigned int rate)
{
	union phy_configure_opts opts;

	memset(&opts, 0, sizeof(opts));
	opts.dp.set_rate = 1;
	opts.dp.link_rate = rate;
	return opts;
}

static void test_pin_d_firmware_sleep_resets_stale_state(void)
{
	union phy_configure_opts opts;

	reset_fixture();
	fake_resume_via_firmware = true;

	/*
	 * Pin-D dock: dwc3 stopped USB3 before sleep, parking PIPE, but
	 * t6040_usb3_down_locked() returned early while dp_up && dp_mode_two_lanes
	 * stayed set, leaving usb3_up, dp_up and rtk live across power loss.
	 */
	phy.orientation = TYPEC_ORIENTATION_REVERSE;
	phy.dp_alt = true;
	phy.dp_two_lanes = true;
	phy.dp_flip = true;
	phy.usb3_up = true;
	phy.dp_on = true;
	phy.dp_up = true;
	phy.dp_mode_two_lanes = true;
	phy.dp_mode_flip = true;
	phy.dp_rate = 2;
	phy.rtk = &fake_rtk_1;
	t6040_recovery_power_down(&phy.recovery, false, false);
	assert(t6040_recovery_retry(&phy.recovery));
	phy.shutdown_failed = true;

	assert(t6040_usb2_resume_early(&dev) == 0);
	assert(warn_calls == 1);
	assert(!phy.usb3_up);
	assert(!phy.pipe_switched);
	assert(!phy.usb3_lent);
	assert(phy.dp_on);
	assert(!phy.dp_up);
	assert(!phy.dp_mode_two_lanes);
	assert(!phy.dp_mode_flip);
	assert(phy.dp_rate == -1);
	assert(!phy.recovery.cycle_pending);
	assert(phy.recovery.retries == 0);
	assert(!phy.shutdown_failed);
	/* RTKit teardown is deferred to .resume(), not run in .resume_early(). */
	assert(phy.sleep_rtk_stale);
	assert(phy.rtk == &fake_rtk_1);
	assert(rtkit_free_calls == 0);
	assert(rpm_put_noidle_calls == 0);
	/* Partner state stays intact for the next bring-up. */
	assert(phy.orientation == TYPEC_ORIENTATION_REVERSE);
	assert(phy.dp_alt);
	assert(phy.dp_two_lanes);
	assert(phy.dp_flip);

	/* .resume() tears down stale RTKit and restores pin-D (USB3 + DP aux). */
	assert(t6040_usb2_resume(&dev) == 0);
	assert(!phy.sleep_rtk_stale);
	assert(rtkit_free_calls == 1);
	assert(last_freed_rtk == &fake_rtk_1);
	assert(usb3_up_calls == 1);
	assert(phy.usb3_up);
	assert(phy.rtk == &fake_rtk_2);
	assert(dp_aux_init_calls == 1);
	assert(phy.dp_on);
	assert(phy.dp_up);
	assert(phy.dp_mode_two_lanes);
	assert(phy.dp_mode_flip);
	assert(phy.dp_rate == -1);
	assert(rpm_resume_and_get_calls == 1);
	assert(rpm_put_calls == 1);
	assert(rpm_request_idle_calls == 0);
	assert(dev.rpm_refs == 1);

	/* Post-resume DP configure succeeds directly on restored pin-D PHY. */
	opts = dp_rate_opts(5400);
	assert(t6040_dp_configure(&dp_phy, &opts) == 0);
	assert(dp_set_rate_calls == 1);
	assert(phy.dp_rate == 2);

	/* Repeated resume is idempotent. */
	assert(t6040_usb2_resume(&dev) == 0);
	assert(rtkit_free_calls == 1);
	assert(usb3_up_calls == 1);
	assert(dp_aux_init_calls == 1);
}

static void test_consumer_state_preserved_and_pipe_reset(void)
{
	reset_fixture();
	fake_resume_via_firmware = true;

	phy.powered = true;
	phy.dwc3_running = true;
	phy.usb3_on = true;
	phy.usb3_pipe = true;
	phy.dp_on = true;
	phy.orientation = TYPEC_ORIENTATION_NORMAL;
	phy.usb3_up = true;
	phy.pipe_switched = true;
	phy.usb3_lent = true;
	phy.dp_up = true;
	phy.dp_rate = 3;

	assert(t6040_usb2_resume_early(&dev) == 0);
	assert(warn_calls == 1);
	/* Hardware-live flags cleared. */
	assert(!phy.powered);
	assert(!phy.usb3_up);
	assert(!phy.pipe_switched);
	assert(!phy.usb3_lent);
	assert(!phy.dp_up);
	assert(phy.dp_rate == -1);
	/* Consumer ownership preserved so core refcounts stay balanced. */
	assert(phy.dwc3_running);
	assert(phy.usb3_on);
	assert(phy.usb3_pipe);
	assert(phy.dp_on);
	assert(phy.orientation == TYPEC_ORIENTATION_NORMAL);
	assert(phy.mode == PHY_MODE_USB_HOST);
}

static void test_cio_and_tunnel_state_drops_rpm_ref(void)
{
	reset_fixture();
	fake_resume_via_firmware = true;

	dev.rpm_refs = 2; /* 1 from dpm_prepare, 1 owned by cio_up */
	phy.cio_up = true;
	phy.tun_pll = true;
	phy.tun_pre = true;
	phy.tun_rate[0] = 0x1e;
	phy.tun_rate[1] = 0x14;
	phy.rtk = &fake_rtk_1;

	assert(t6040_usb2_resume_early(&dev) == 0);
	assert(warn_calls == 1);
	assert(!phy.cio_up);
	assert(!phy.tun_pll);
	assert(!phy.tun_pre);
	assert(phy.tun_rate[0] == 0);
	assert(phy.tun_rate[1] == 0);
	assert(rpm_put_noidle_calls == 1);
	assert(dev.rpm_refs == 1);
	assert(phy.sleep_rpm_idle_pending);
	assert(rpm_request_idle_calls == 0);
	assert(phy.sleep_rtk_stale);

	assert(t6040_usb2_resume(&dev) == 0);
	assert(!phy.sleep_rtk_stale);
	assert(!phy.sleep_rpm_idle_pending);
	assert(phy.rtk == NULL);
	assert(rtkit_free_calls == 1);
	assert(rpm_request_idle_calls == 1);

	/* Repeated resume does not re-request idle. */
	assert(t6040_usb2_resume(&dev) == 0);
	assert(rpm_request_idle_calls == 1);
}

static void test_clean_firmware_sleep_quiet_and_clears_recovery(void)
{
	reset_fixture();
	fake_resume_via_firmware = true;

	/*
	 * Clean sleep after an earlier 4-lane DP disconnect and failed power-down:
	 * dp_rate/dp_mode_flip/usb3_lent may still hold old values with dp_up=false.
	 * Power loss clears them and recovery/shutdown_failed without a false warning.
	 */
	phy.dp_alt = true;
	phy.dp_mode_flip = true;
	phy.dp_rate = 2;
	phy.usb3_lent = true;
	phy.shutdown_failed = true;
	t6040_recovery_power_down(&phy.recovery, false, true);

	assert(t6040_usb2_resume_early(&dev) == 0);
	assert(warn_calls == 0);
	assert(!phy.recovery.cycle_pending);
	assert(!phy.shutdown_failed);
	assert(!phy.dp_mode_flip);
	assert(phy.dp_rate == -1);
	assert(!phy.usb3_lent);
	assert(!phy.sleep_rtk_stale);
	assert(!phy.sleep_rpm_idle_pending);
	assert(phy.dp_alt);

	assert(t6040_usb2_resume(&dev) == 0);
	assert(rtkit_free_calls == 0);
	assert(rpm_put_noidle_calls == 0);
	assert(rpm_request_idle_calls == 0);
	assert(dp_phy_init_calls == 0);
}

static void test_dp_only_fixed_hdmi_sleep_restores_and_configures(void)
{
	union phy_configure_opts opts;

	reset_fixture();
	fake_resume_via_firmware = true;

	/* Fixed-HDMI PHY (dp_only) is powered on at probe even with no cable. */
	phy.usb3_ok = false;
	assert(t6040_dp_power_on(&dp_phy) == 0);
	assert(phy.dp_on);
	assert(phy.dp_up);
	assert(dp_phy_init_calls == 1);
	assert(dp_mode_setup_calls == 1);
	assert(!phy.usb3_lent);

	opts = dp_rate_opts(5400);
	assert(t6040_dp_configure(&dp_phy, &opts) == 0);
	assert(phy.dp_rate == 2);

	/* Deep sleep clears hardware-live dp_up quietly while preserving dp_on. */
	assert(t6040_usb2_resume_early(&dev) == 0);
	assert(warn_calls == 0);
	assert(phy.dp_on);
	assert(!phy.dp_up);
	assert(phy.dp_rate == -1);
	assert(!phy.sleep_rtk_stale);

	/* .resume() restores fixed HDMI so AUX and dp_configure() work after wake. */
	assert(t6040_usb2_resume(&dev) == 0);
	assert(warn_calls == 0);
	assert(rtkit_free_calls == 0);
	assert(phy.dp_up);
	assert(!phy.dp_mode_two_lanes);
	assert(!phy.usb3_lent);
	assert(dp_phy_init_calls == 2);
	assert(dp_mode_setup_calls == 2);
	assert(rpm_resume_and_get_calls == 1);
	assert(rpm_put_calls == 1);
	assert(dev.rpm_refs == 1);

	opts = dp_rate_opts(5400);
	opts.dp.set_voltages = 1;
	opts.dp.lanes = 4;
	assert(t6040_dp_configure(&dp_phy, &opts) == 0);
	assert(phy.dp_rate == 2);
	assert(dp_drive_calls == 4);

	/* Lazy fallback: if .resume() fails restore, dp_configure() restores it. */
	assert(t6040_usb2_resume_early(&dev) == 0);
	assert(!phy.dp_up);
	fake_dp_phy_init_err = -ETIMEDOUT;
	assert(t6040_usb2_resume(&dev) == 0);
	assert(warn_calls == 1);
	assert(phy.dp_on);
	assert(!phy.dp_up);

	fake_dp_phy_init_err = 0;
	opts = dp_rate_opts(2700);
	assert(t6040_dp_configure(&dp_phy, &opts) == 0);
	assert(phy.dp_up);
	assert(phy.dp_rate == 1);
	assert(dp_phy_init_calls == 4);
	assert(dp_mode_setup_calls == 3);
	assert(dev.rpm_refs == 1);
}

static void test_typec_4lane_dp_sleep_restores_and_configures(void)
{
	union phy_configure_opts opts;

	reset_fixture();
	fake_resume_via_firmware = true;

	/* Type-C 4-lane DP alt mode (pin C/E, reverse orientation). */
	phy.orientation = TYPEC_ORIENTATION_REVERSE;
	phy.dp_alt = true;
	phy.dp_two_lanes = false;
	phy.dp_flip = true;

	assert(t6040_dp_power_on(&dp_phy) == 0);
	assert(phy.dp_on);
	assert(phy.dp_up);
	assert(phy.usb3_lent);
	assert(!phy.dp_mode_two_lanes);
	assert(last_dp_phy_init_flip);
	assert(last_dp_mode_setup_flip);

	opts = dp_rate_opts(8100);
	assert(t6040_dp_configure(&dp_phy, &opts) == 0);
	assert(phy.dp_rate == 3);

	/* Deep sleep clears hardware-live state; dp_on and partner state stay. */
	assert(t6040_usb2_resume_early(&dev) == 0);
	assert(warn_calls == 1);
	assert(phy.dp_on);
	assert(!phy.dp_up);
	assert(!phy.usb3_lent);
	assert(phy.dp_rate == -1);

	/* .resume() eagerly restores 4-lane DP and re-claims USB3 lanes. */
	assert(t6040_usb2_resume(&dev) == 0);
	assert(phy.dp_up);
	assert(phy.usb3_lent);
	assert(!phy.dp_mode_two_lanes);
	assert(dp_phy_init_calls == 2);
	assert(dp_mode_setup_calls == 2);
	assert(last_dp_phy_init_flip);
	assert(last_dp_mode_setup_flip);

	opts = dp_rate_opts(8100);
	assert(t6040_dp_configure(&dp_phy, &opts) == 0);
	assert(phy.dp_rate == 3);

	/* Lazy fallback in dp_configure() if .resume() failed and USB3 came up first. */
	assert(t6040_usb2_resume_early(&dev) == 0);
	fake_dp_phy_init_err = -ETIMEDOUT;
	assert(t6040_usb2_resume(&dev) == 0);
	assert(!phy.dp_up);
	phy.usb3_up = true;
	phy.pipe_switched = true;

	fake_dp_phy_init_err = 0;
	opts = dp_rate_opts(5400);
	assert(t6040_dp_configure(&dp_phy, &opts) == 0);
	assert(!phy.usb3_up);
	assert(!phy.pipe_switched);
	assert(phy.usb3_lent);
	assert(phy.dp_up);
	assert(phy.dp_rate == 2);

	/* Unplugged Type-C port (dp_alt cleared) must not restore 4-lane DP. */
	assert(t6040_usb2_resume_early(&dev) == 0);
	phy.dp_alt = false;
	phy.usb3_up = true;
	unsigned int prev_down = usb3_down_calls;
	unsigned int prev_init = dp_phy_init_calls;

	opts = dp_rate_opts(5400);
	assert(t6040_dp_configure(&dp_phy, &opts) == -ENOLINK);
	assert(!phy.dp_up);
	assert(phy.usb3_up);
	assert(usb3_down_calls == prev_down);
	assert(dp_phy_init_calls == prev_init);

	/* Power-off after failed restore cleanly drops dp_on without HW teardown. */
	assert(t6040_dp_power_off(&dp_phy) == 0);
	assert(!phy.dp_on);
	assert(!phy.dp_up);
	assert(dp_tx_cmn_down_calls == 0);
	assert(usb3_sleep_calls == 0);
}

static void test_pin_d_dp_configure_after_usb3_recovery_and_first(void)
{
	union phy_configure_opts opts;

	reset_fixture();
	fake_resume_via_firmware = true;

	phy.orientation = TYPEC_ORIENTATION_REVERSE;
	phy.dp_alt = true;
	phy.dp_two_lanes = true;
	phy.dp_flip = true;

	assert(t6040_dp_power_on(&dp_phy) == 0);
	assert(phy.dp_on);
	assert(phy.dp_up);
	assert(phy.usb3_up);
	assert(phy.dp_mode_two_lanes);
	assert(phy.dp_mode_flip);

	/*
	 * Subcase A: USB3 recovers before DP configure (after .resume() failed
	 * to boot firmware). Lazy restore keeps the running USB3/PIPE intact
	 * and only runs DP aux init.
	 */
	assert(t6040_usb2_resume_early(&dev) == 0);
	fake_usb3_up_fail = true;
	assert(t6040_usb2_resume(&dev) == 0);
	assert(phy.dp_on);
	assert(!phy.dp_up);
	assert(!phy.usb3_up);

	fake_usb3_up_fail = false;
	phy.usb3_up = true;
	phy.pipe_switched = true;
	unsigned int prev_usb3_up = usb3_up_calls;
	unsigned int prev_aux = dp_aux_init_calls;

	opts = dp_rate_opts(5400);
	opts.dp.set_voltages = 1;
	opts.dp.lanes = 2;
	assert(t6040_dp_configure(&dp_phy, &opts) == 0);
	assert(usb3_up_calls == prev_usb3_up);
	assert(dp_aux_init_calls == prev_aux + 1);
	assert(phy.usb3_up);
	assert(phy.pipe_switched);
	assert(phy.dp_up);
	assert(phy.dp_mode_two_lanes);
	assert(phy.dp_mode_flip);
	assert(phy.dp_rate == 2);
	assert(dp_drive_calls == 2);

	/* 4 lanes rejected on restored pin-D PHY. */
	opts = dp_rate_opts(5400);
	opts.dp.set_lanes = 1;
	opts.dp.lanes = 4;
	assert(t6040_dp_configure(&dp_phy, &opts) == -EINVAL);

	/*
	 * Subcase B: DP configure arrives FIRST before USB3 recovers (and even
	 * before .resume() drops stale RTKit). Lazy restore tears down stale
	 * RTKit, brings up USB3, runs DP aux init and programs the link rate.
	 */
	phy.rtk = &fake_rtk_1;
	assert(t6040_usb2_resume_early(&dev) == 0);
	assert(phy.sleep_rtk_stale);
	assert(!phy.usb3_up);
	assert(!phy.dp_up);
	unsigned int prev_free = rtkit_free_calls;

	prev_usb3_up = usb3_up_calls;
	prev_aux = dp_aux_init_calls;
	opts = dp_rate_opts(2700);
	assert(t6040_dp_configure(&dp_phy, &opts) == 0);
	assert(!phy.sleep_rtk_stale);
	assert(rtkit_free_calls == prev_free + 1);
	assert(last_freed_rtk == &fake_rtk_1);
	assert(usb3_up_calls == prev_usb3_up + 1);
	assert(dp_aux_init_calls == prev_aux + 1);
	assert(phy.usb3_up);
	assert(!phy.pipe_switched);
	assert(phy.dp_up);
	assert(phy.dp_mode_two_lanes);
	assert(phy.dp_mode_flip);
	assert(phy.dp_rate == 1);

	/* Subcase C: firmware boot failure during DP restore returns -ENOLINK. */
	assert(t6040_usb2_resume_early(&dev) == 0);
	fake_usb3_up_fail = true;
	unsigned int prev_rate_calls = dp_set_rate_calls;

	opts = dp_rate_opts(2700);
	assert(t6040_dp_configure(&dp_phy, &opts) == -ENOLINK);
	assert(!phy.dp_up);
	assert(phy.dp_on);
	assert(dp_set_rate_calls == prev_rate_calls);
}

static void test_dp_configure_without_dp_on_returns_einval(void)
{
	union phy_configure_opts opts = dp_rate_opts(5400);

	reset_fixture();

	/* Type-C PHY with dp_alt set but consumer never powered DP on. */
	phy.dp_alt = true;
	assert(t6040_dp_configure(&dp_phy, &opts) == -EINVAL);
	assert(!phy.dp_up);
	assert(dp_phy_init_calls == 0);
	assert(usb3_up_calls == 0);
	assert(rpm_resume_and_get_calls == 0);

	/* Fixed HDMI PHY with dp_on false also rejects dp_configure(). */
	phy.usb3_ok = false;
	phy.dp_alt = false;
	assert(t6040_dp_configure(&dp_phy, &opts) == -EINVAL);
	assert(!phy.dp_up);
	assert(dp_phy_init_calls == 0);

	/* Failed power_on must NOT set dp_on. */
	fake_dp_phy_init_err = -ETIMEDOUT;
	assert(t6040_dp_power_on(&dp_phy) == -ETIMEDOUT);
	assert(!phy.dp_on);
	assert(!phy.dp_up);
	fake_dp_phy_init_err = 0;
	assert(t6040_dp_configure(&dp_phy, &opts) == -EINVAL);
	assert(!phy.dp_up);

	/* Power-on followed by power-off clears dp_on; configure returns -EINVAL. */
	assert(t6040_dp_power_on(&dp_phy) == 0);
	assert(phy.dp_on);
	assert(t6040_dp_power_off(&dp_phy) == 0);
	assert(!phy.dp_on);
	assert(!phy.dp_up);
	assert(t6040_dp_configure(&dp_phy, &opts) == -EINVAL);
}

static void test_dp_configure_link_down_short_circuits_without_restore(void)
{
	union phy_configure_opts opts = {0};

	reset_fixture();
	fake_resume_via_firmware = true;

	/*
	 * Simulate a Type-C DP route whose .resume() restore failed and which
	 * was then unplugged (dp_alt cleared) before DCP's SET_LINK_RATE(0)
	 * teardown call arrives. Pure link-down must succeed without trying to
	 * restore the PHY or returning -ENOLINK (which would stall DCP).
	 */
	phy.usb3_ok = true;
	phy.dp_alt = true;
	phy.dp_two_lanes = false;
	assert(t6040_dp_power_on(&dp_phy) == 0);
	assert(phy.dp_on && phy.dp_up);
	assert(t6040_usb2_resume_early(&dev) == 0);
	assert(phy.dp_on && !phy.dp_up && phy.dp_rate == -1);

	phy.dp_alt = false;
	opts.dp.set_rate = 1;
	opts.dp.link_rate = 0;
	assert(t6040_dp_configure(&dp_phy, &opts) == 0);
	assert(!phy.dp_up);
	assert(dp_phy_init_calls == 1);
	assert(usb3_up_calls == 0);
	assert(rpm_resume_and_get_calls == 0);

	/* Non-zero rate or lane/voltage changes still require a live PHY. */
	opts.dp.set_lanes = 1;
	opts.dp.lanes = 4;
	assert(t6040_dp_configure(&dp_phy, &opts) == -ENOLINK);
}

static void test_dp_set_mode_restores_lazily(void)
{
	reset_fixture();
	fake_resume_via_firmware = true;

	phy.usb3_ok = false;
	/* Before power_on: PHY_MODE_DP is a no-op, invalid mode fails. */
	assert(t6040_dp_set_mode(&dp_phy, PHY_MODE_DP, 0) == 0);
	assert(!phy.dp_up);
	assert(t6040_dp_set_mode(&dp_phy, PHY_MODE_USB_HOST, 0) == -EOPNOTSUPP);

	assert(t6040_dp_power_on(&dp_phy) == 0);
	assert(t6040_usb2_resume_early(&dev) == 0);
	assert(!phy.dp_up);

	/* PHY_MODE_INVALID must not restore. */
	assert(t6040_dp_set_mode(&dp_phy, PHY_MODE_INVALID, 0) == 0);
	assert(!phy.dp_up);

	/* PHY_MODE_DP lazily restores when dp_on && !dp_up. */
	assert(t6040_dp_set_mode(&dp_phy, PHY_MODE_DP, 0) == 0);
	assert(phy.dp_up);
	assert(dp_phy_init_calls == 2);
	assert(dp_mode_setup_calls == 2);
}

static void test_early_consumer_drop_beats_resume(void)
{
	reset_fixture();
	fake_resume_via_firmware = true;

	phy.usb3_up = true;
	phy.rtk = &fake_rtk_1;

	assert(t6040_usb2_resume_early(&dev) == 0);
	assert(phy.sleep_rtk_stale);
	assert(phy.rtk == &fake_rtk_1);

	/* Consumer starts USB3/CIO before t6040_usb2_resume() runs. */
	mutex_lock(&phy.lock);
	t6040_drop_stale_rtk_locked(&phy);
	mutex_unlock(&phy.lock);
	assert(!phy.sleep_rtk_stale);
	assert(phy.rtk == NULL);
	assert(rtkit_free_calls == 1);
	assert(last_freed_rtk == &fake_rtk_1);

	/* Consumer brings up a fresh RTKit session before .resume(). */
	phy.rtk = &fake_rtk_2;
	assert(t6040_usb2_resume(&dev) == 0);
	assert(phy.rtk == &fake_rtk_2);
	assert(rtkit_free_calls == 1);
}

static void test_s2idle_is_untouched(void)
{
	reset_fixture();
	fake_suspend_no_platform = true;
	fake_resume_via_firmware = false;

	dev.rpm_refs = 2;
	phy.orientation = TYPEC_ORIENTATION_NORMAL;
	phy.dp_alt = true;
	phy.dp_two_lanes = true;
	phy.usb3_up = true;
	phy.pipe_switched = true;
	phy.usb3_lent = true;
	phy.dp_on = true;
	phy.dp_up = true;
	phy.dp_mode_two_lanes = true;
	phy.dp_mode_flip = true;
	phy.dp_rate = 2;
	phy.cio_up = true;
	phy.tun_pll = true;
	phy.tun_pre = true;
	phy.tun_rate[0] = 0x1e;
	phy.rtk = &fake_rtk_1;
	t6040_recovery_power_down(&phy.recovery, false, true);
	phy.shutdown_failed = true;

	assert(t6040_usb2_suspend(&dev) == 0);
	assert(dev.awake_path);
	assert(t6040_usb2_suspend_noirq(&dev) == 0);
	assert(t6040_usb2_resume_early(&dev) == 0);
	assert(t6040_usb2_resume(&dev) == 0);

	assert(warn_calls == 0);
	assert(rtkit_free_calls == 0);
	assert(rpm_put_noidle_calls == 0);
	assert(rpm_request_idle_calls == 0);
	assert(rpm_resume_and_get_calls == 0);
	assert(dev.rpm_refs == 2);
	assert(phy.usb3_up);
	assert(phy.pipe_switched);
	assert(phy.usb3_lent);
	assert(phy.dp_on);
	assert(phy.dp_up);
	assert(phy.dp_mode_two_lanes);
	assert(phy.dp_mode_flip);
	assert(phy.dp_rate == 2);
	assert(phy.cio_up);
	assert(phy.tun_pll);
	assert(phy.tun_pre);
	assert(phy.tun_rate[0] == 0x1e);
	assert(phy.rtk == &fake_rtk_1);
	assert(!phy.sleep_rtk_stale);
	assert(!phy.sleep_rpm_idle_pending);
	assert(phy.recovery.cycle_pending);
	assert(phy.shutdown_failed);
}

static void test_aborted_deep_sleep_is_untouched(void)
{
	reset_fixture();
	fake_suspend_no_platform = false;
	fake_resume_via_firmware = false;

	dev.rpm_refs = 2;
	phy.cio_up = true;
	phy.usb3_up = true;
	phy.dp_on = true;
	phy.dp_up = true;
	phy.rtk = &fake_rtk_1;

	assert(t6040_usb2_suspend(&dev) == 0);
	assert(!dev.awake_path);
	assert(t6040_usb2_resume_early(&dev) == 0);
	assert(t6040_usb2_resume(&dev) == 0);

	assert(warn_calls == 0);
	assert(rtkit_free_calls == 0);
	assert(rpm_put_noidle_calls == 0);
	assert(rpm_request_idle_calls == 0);
	assert(rpm_resume_and_get_calls == 0);
	assert(phy.cio_up);
	assert(phy.usb3_up);
	assert(phy.dp_on);
	assert(phy.dp_up);
	assert(phy.rtk == &fake_rtk_1);
	assert(!phy.sleep_rtk_stale);
	assert(!phy.sleep_rpm_idle_pending);
}

int main(void)
{
	test_pin_d_firmware_sleep_resets_stale_state();
	test_consumer_state_preserved_and_pipe_reset();
	test_cio_and_tunnel_state_drops_rpm_ref();
	test_clean_firmware_sleep_quiet_and_clears_recovery();
	test_dp_only_fixed_hdmi_sleep_restores_and_configures();
	test_typec_4lane_dp_sleep_restores_and_configures();
	test_pin_d_dp_configure_after_usb3_recovery_and_first();
	test_dp_configure_without_dp_on_returns_einval();
	test_dp_configure_link_down_short_circuits_without_restore();
	test_dp_set_mode_restores_lazily();
	test_early_consumer_drop_beats_resume();
	assert(EARLY_USB3_DROP_OK);
	assert(EARLY_CIO_DROP_OK);
	test_s2idle_is_untouched();
	test_aborted_deep_sleep_is_untouched();
	printf("PASS 13 T6040 PHY firmware-sleep resume cases\n");
	return 0;
}
'''

MUTATIONS = {
    "ignore-fw-resume": (
        "if (!tphy || !pm_resume_via_firmware())",
        "if (!tphy)",
    ),
    "keep-powered": (
        "\ttphy->powered = false;\n",
        "",
    ),
    "keep-usb3-up": (
        "\ttphy->usb3_up = false;\n",
        "",
    ),
    "keep-dp-up": (
        "\ttphy->dp_up = false;\n\ttphy->dp_mode_two_lanes = false;\n",
        "\ttphy->dp_mode_two_lanes = false;\n",
    ),
    "warn-dp-only": (
        "(tphy->dp_up && !t6040_dp_only(tphy))",
        "tphy->dp_up",
    ),
    "keep-pipe-switched": (
        "\ttphy->pipe_switched = false;\n",
        "",
    ),
    "keep-rtk-stale": (
        "\tif (tphy->rtk)\n\t\ttphy->sleep_rtk_stale = true;\n",
        "",
    ),
    "leak-cio-rpm": (
        "\tif (cio_was_up) {\n\t\ttphy->sleep_rpm_idle_pending = true;\n"
        "\t\tpm_runtime_put_noidle(tphy->dev);\n\t}\n",
        "\t(void)cio_was_up;\n",
    ),
    "drop-cio-request-idle": (
        "\tif (idle_pending)\n\t\tpm_request_idle(tphy->dev);\n",
        "\t(void)idle_pending;\n",
    ),
    "keep-recovery": (
        "\tt6040_power_lost(tphy);\n",
        "",
    ),
    "clobber-partner": (
        "\ttphy->dp_rate = -1;\n\n\tif (cio_was_up)",
        "\ttphy->dp_rate = -1;\n\ttphy->orientation = TYPEC_ORIENTATION_NONE;\n\n\tif (cio_was_up)",
    ),
    "drop-dp-on": (
        "\tif (!ret)\n\t\ttphy->dp_on = true;\n",
        "",
    ),
    "clear-dp-on-sleep": (
        "\ttphy->dp_up = false;\n\ttphy->dp_mode_two_lanes = false;\n",
        "\ttphy->dp_on = false;\n\ttphy->dp_up = false;\n\ttphy->dp_mode_two_lanes = false;\n",
    ),
    "drop-dp-resume-restore": (
        "\t\tif (tphy->dp_on && !tphy->dp_up)\n"
        "\t\t\tret = t6040_dp_restore_locked(tphy);\n",
        "",
    ),
    "drop-dp-configure-restore": (
        "\tif (!tphy->dp_up) {\n"
        "\t\tif (!tphy->dp_on)\n"
        "\t\t\treturn -EINVAL;\n"
        "\t\tif (opts->set_rate && !opts->link_rate &&\n"
        "\t\t    !opts->set_lanes && !opts->set_voltages)\n"
        "\t\t\treturn 0;\n"
        "\t\tret = t6040_dp_restore_locked(tphy);\n"
        "\t\tif (ret)\n"
        "\t\t\treturn ret;\n"
        "\t\tif (!tphy->dp_up)\n"
        "\t\t\treturn -EINVAL;\n"
        "\t}\n",
        "\tif (!tphy->dp_up)\n\t\treturn -EINVAL;\n",
    ),
    "allow-dp-configure-without-dp-on": (
        "\tif (!tphy->dp_up) {\n"
        "\t\tif (!tphy->dp_on)\n"
        "\t\t\treturn -EINVAL;\n"
        "\t\tif (opts->set_rate && !opts->link_rate &&\n"
        "\t\t    !opts->set_lanes && !opts->set_voltages)\n"
        "\t\t\treturn 0;\n"
        "\t\tret = t6040_dp_restore_locked(tphy);\n",
        "\tif (!tphy->dp_up) {\n"
        "\t\tret = t6040_dp_up_locked(tphy);\n",
    ),
    "restore-on-dp-link-down": (
        "\t\tif (opts->set_rate && !opts->link_rate &&\n"
        "\t\t    !opts->set_lanes && !opts->set_voltages)\n"
        "\t\t\treturn 0;\n",
        "",
    ),
    "drop-dp-alt-guard": (
        "\tif (!t6040_dp_only(tphy)) {\n"
        "\t\tif (!tphy->dp_alt)\n"
        "\t\t\treturn -ENOLINK;\n",
        "\tif (!t6040_dp_only(tphy)) {\n",
    ),
    "drop-usb3-early-rtk": None,
    "drop-cio-early-rtk": None,
}


def early_drop_defines(source, mutation):
    usb3_fn = extract_function(source, "t6040_usb3_up_locked")
    cio_fn = extract_function(source, "t6040_cio_up_locked")
    if mutation == "drop-usb3-early-rtk":
        usb3_fn = usb3_fn.replace("\tt6040_drop_stale_rtk_locked(tphy);\n", "", 1)
    elif mutation == "drop-cio-early-rtk":
        cio_fn = cio_fn.replace("\tt6040_drop_stale_rtk_locked(tphy);\n", "", 1)

    def has_early_drop(fn):
        drop_pos = fn.find("t6040_drop_stale_rtk_locked(tphy);")
        rec_pos = fn.find("t6040_recovery_start(&tphy->recovery)")
        return 1 if (drop_pos >= 0 and rec_pos >= 0 and drop_pos < rec_pos) else 0

    return (
        f"#define EARLY_USB3_DROP_OK {has_early_drop(usb3_fn)}\n"
        f"#define EARLY_CIO_DROP_OK {has_early_drop(cio_fn)}\n"
    )


def build_unit(source, mutation):
    parts = [
        extract_block(source, r"^struct t6040_recovery_state \{"),
        extract_function(source, "t6040_recovery_power_down"),
        extract_function(source, "t6040_recovery_start"),
        extract_function(source, "t6040_recovery_retry"),
        extract_function(source, "t6040_recovery_complete"),
        extract_function(source, "t6040_recovery_power_lost"),
        extract_block(source, r"^enum t6040_usb3_region \{"),
        extract_block(source, r"^struct t6040_usb2_phy \{"),
        extract_function(source, "t6040_rtkit_stop_transport"),
        extract_function(source, "t6040_drop_stale_rtk_locked"),
        extract_function(source, "t6040_dp_only"),
        extract_function(source, "t6040_dp_up_locked"),
        extract_function(source, "t6040_dp_restore_locked"),
        extract_function(source, "t6040_dp_power_on"),
        extract_function(source, "t6040_dp_power_off"),
        extract_function(source, "t6040_dp_configure"),
        extract_function(source, "t6040_dp_set_mode"),
        extract_function(source, "t6040_keep_running_cpu_awake"),
        extract_function(source, "t6040_power_lost"),
        extract_function(source, "t6040_sleep_power_lost_locked"),
        extract_function(source, "t6040_usb2_suspend"),
        extract_function(source, "t6040_usb2_suspend_noirq"),
        extract_function(source, "t6040_usb2_resume_early"),
        extract_function(source, "t6040_usb2_resume"),
    ]
    extracted = "\n\n".join(parts) + "\n"
    if mutation is not None and MUTATIONS[mutation] is not None:
        old, new = MUTATIONS[mutation]
        if old not in extracted:
            raise ValueError(f"mutation {mutation!r} did not match extracted code")
        extracted = extracted.replace(old, new, 1)
    return PRELUDE + "\n" + early_drop_defines(source, mutation) + "\n" + extracted + "\n" + TESTS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation", choices=sorted(MUTATIONS))
    args = parser.parse_args()

    source = DRIVER.read_text()
    unit_src = build_unit(source, args.mutation)
    with tempfile.TemporaryDirectory(prefix="t6040-usb2-s2r-") as tmp:
        unit = Path(tmp) / "test.c"
        binary = Path(tmp) / "test"
        unit.write_text(unit_src)
        subprocess.run(
            [
                "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-Wno-unused-parameter",
                "-g", "-fsanitize=address,undefined",
                str(unit), "-o", str(binary),
            ],
            check=True,
        )
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
        if run.stdout:
            sys.stdout.write(run.stdout)
        if run.stderr:
            sys.stderr.write(run.stderr)
        sys.exit(run.returncode)


if __name__ == "__main__":
    main()
