#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check which device trees let the M5 hotspot fan assistance set fans.

drivers/hwmon/macsmc-hwmon.c owns all apple,smc-hwmon DT binding knowledge:
- macsmc_hwmon_parse_fan_assist(): requires exactly one key in
  apple,soc-die-keys (no hardcoded SMC key name) plus valid DT-supplied
  apple,soc-die-fan-trip-millicelsius, apple,soc-die-fan-hysteresis-millicelsius,
  and apple,soc-die-fan-hold-ms.
- macsmc_hwmon_hotspot_assist_available(): checks whether an available
  apple,smc-hwmon node (with all parents available) carries that policy and
  at least one settable fan with apple,fan-manual-control.
- macsmc_hwmon_fans_request_manual(): drives only fans marked with
  apple,fan-manual-control, leaving closed siblings untouched.
- drivers/gpu/drm/asahi/g17/fans.c: asahi_fans_request_described() gates
  boost requests on macsmc_hwmon_hotspot_assist_policy() and delegates to
  macsmc_hwmon_fans_request_manual().

This compiles the real functions from drivers/hwmon/macsmc-hwmon.c and
drivers/gpu/drm/asahi/g17/fans.c against libfdt and runs them on synthetic
trees and the T6050 board trees, including J716c without #2208, with #2208
alone (no trip properties -> must stay off), and with #2208 plus trip
properties, and also compiles FanBoost from drivers/gpu/drm/asahi/g17/thermal.rs
with rustc to test partial budget/hotspot request failure and handoff cleanup.
--mutation drops one condition, which a case must catch.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]

HWMON_FIRST = "static bool macsmc_hwmon_fan_settable(const struct device_node *fan)"
HWMON_LAST = "EXPORT_SYMBOL_GPL(macsmc_hwmon_hotspot_assist_available);"
FANS_FIRST = "int asahi_fans_request_described(struct device *dev, unsigned int permille,"

# Board trees: the exact fans the request may write there, or None when the tree
# does not select the assistance.
BOARDS = {
    "t6050-j714s": None,
    "t6050-j716s": None,
    "t6050-j775d": None,
}
J716C_FANS = ["fan-F0Ac", "fan-F1Ac"]

# J716c #2208 description without active trip properties (must stay off).
J716C_2208_NO_TRIP = """
#include "hwmon-fan-dual.dtsi"
&smc_hwmon {
	status = "okay";
	fan-F0Ac { apple,fan-mode = "F0md"; apple,fan-manual-control; };
	fan-F1Ac { apple,fan-mode = "F1md"; apple,fan-manual-control; };
};
"""

# J716c #2208 description with active trip properties on macsmc_soc_die.
J716C_2208_WITH_TRIP = """
#include "hwmon-fan-dual.dtsi"
&smc_hwmon {
	status = "okay";
	apple,soc-die-fan-trip-millicelsius = <98000>;
	apple,soc-die-fan-hysteresis-millicelsius = <16000>;
	apple,soc-die-fan-hold-ms = <30000>;
	fan-F0Ac { apple,fan-mode = "F0md"; apple,fan-manual-control; };
	fan-F1Ac { apple,fan-mode = "F1md"; apple,fan-manual-control; };
};
"""

KEYS = ('apple,key-id = "F{n}Ac"; apple,fan-minimum = "F{n}Mn"; apple,fan-maximum = "F{n}Mx"; '
        'apple,fan-target = "F{n}Tg"; ')
MODE = KEYS + 'apple,fan-mode = "F{n}md"; '
MANUAL = "apple,fan-manual-control;"
TRIP = ('apple,soc-die-fan-trip-millicelsius = <98000>; '
        'apple,soc-die-fan-hysteresis-millicelsius = <16000>; '
        'apple,soc-die-fan-hold-ms = <30000>;')
DIE_TCMB = 'apple,soc-die-keys = "TCMb"; ' + TRIP
DIE_OTHER = 'apple,soc-die-keys = "Tg0D"; ' + TRIP

# name: (hwmon properties, [per-fan properties], fans written or None, expected policy or None)
SYNTHETIC = {
    "both": (DIE_TCMB, [MODE + MANUAL, MODE + MANUAL], ["fan-F0Ac", "fan-F1Ac"], (98000, 82000, 30000)),
    "one-fan": (DIE_TCMB, [MODE + MANUAL], ["fan-F0Ac"], (98000, 82000, 30000)),
    # Any single SoC die key works (no hardcoded TCMb key name in driver).
    "other-single-die-key": (DIE_OTHER, [MODE + MANUAL], ["fan-F0Ac"], (98000, 82000, 30000)),
    # Manual-only request writes only fans with apple,fan-manual-control; closed siblings stay untouched.
    "manual-and-monitor-only": (DIE_TCMB, [MODE + MANUAL, KEYS], ["fan-F0Ac"], (98000, 82000, 30000)),
    "second-fan-only": (DIE_TCMB, [MODE, MODE + MANUAL], ["fan-F1Ac"], (98000, 82000, 30000)),
    "first-fan-only": (DIE_TCMB, [MODE + MANUAL, MODE], ["fan-F0Ac"], (98000, 82000, 30000)),
    # Opened, but without the keys to set it: nothing the request could write.
    "manual-without-keys": (DIE_TCMB, ['apple,fan-mode = "F{n}md"; ' + MANUAL], None, None),
    "manual-without-mode": (DIE_TCMB, [KEYS + MANUAL], None, None),
    "no-manual-fan": (DIE_TCMB, [MODE], None, None),
    "no-fans": (DIE_TCMB, [], None, None),
    # Missing or invalid SoC die key list.
    "no-die-zone": (TRIP, [MODE + MANUAL], None, None),
    "die-zone-of-several": ('apple,soc-die-keys = "TCMb", "Tg0D"; ' + TRIP, [MODE + MANUAL], None, None),
    "temperature-node-only": (TRIP + ' temperature-TCMb { apple,key-id = "TCMb"; };',
                              [MODE + MANUAL], None, None),
    # Missing or invalid DT trip / hysteresis / hold properties (must stay off, no hardcoded fallback).
    "no-trip": ('apple,soc-die-keys = "TCMb";', [MODE + MANUAL], None, None),
    "missing-hyst": ('apple,soc-die-keys = "TCMb"; apple,soc-die-fan-trip-millicelsius = <98000>; '
                     'apple,soc-die-fan-hold-ms = <30000>;', [MODE + MANUAL], None, None),
    "missing-hold": ('apple,soc-die-keys = "TCMb"; apple,soc-die-fan-trip-millicelsius = <98000>; '
                     'apple,soc-die-fan-hysteresis-millicelsius = <16000>;', [MODE + MANUAL], None, None),
    "zero-hyst": ('apple,soc-die-keys = "TCMb"; apple,soc-die-fan-trip-millicelsius = <98000>; '
                  'apple,soc-die-fan-hysteresis-millicelsius = <0>; apple,soc-die-fan-hold-ms = <30000>;',
                  [MODE + MANUAL], None, None),
    "hyst-ge-trip": ('apple,soc-die-keys = "TCMb"; apple,soc-die-fan-trip-millicelsius = <98000>; '
                     'apple,soc-die-fan-hysteresis-millicelsius = <98000>; apple,soc-die-fan-hold-ms = <30000>;',
                     [MODE + MANUAL], None, None),
    "trip-too-high": ('apple,soc-die-keys = "TCMb"; apple,soc-die-fan-trip-millicelsius = <160000>; '
                      'apple,soc-die-fan-hysteresis-millicelsius = <16000>; apple,soc-die-fan-hold-ms = <30000>;',
                      [MODE + MANUAL], None, None),
    "zero-hold": ('apple,soc-die-keys = "TCMb"; apple,soc-die-fan-trip-millicelsius = <98000>; '
                  'apple,soc-die-fan-hysteresis-millicelsius = <16000>; apple,soc-die-fan-hold-ms = <0>;',
                  [MODE + MANUAL], None, None),
}

HARNESS = r"""
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"

typedef uint32_t u32;
#define MACSMC_DIE_MAX_MC 150000
#define EXPORT_SYMBOL_GPL(sym)

struct macsmc_hwmon_fan_assist {
	int trigger_mc;
	int release_mc;
	unsigned int hold_ms;
};

static const void *blob;
struct device_node { int offset; };
struct device { struct device_node *of_node; };

static void free_node(struct device_node **node) { free(*node); }
#define __free(kind) __attribute__((cleanup(free_node)))

static struct device_node *node_at(int offset)
{
	struct device_node *node = malloc(sizeof(*node));
	if (!node)
		abort();
	node->offset = offset;
	return node;
}

static struct device_node *of_node_get(struct device_node *np)
{
	return np ? node_at(np->offset) : NULL;
}

static void of_node_put(struct device_node *np)
{
	free(np);
}

static struct device_node *of_get_parent(const struct device_node *np)
{
	int offset = fdt_parent_offset(blob, np->offset);

	return offset < 0 ? NULL : node_at(offset);
}

static struct device_node *of_get_next_child_with_prefix(const struct device_node *parent,
		struct device_node *prev, const char *prefix)
{
	int offset = prev ? fdt_next_subnode(blob, prev->offset)
			  : fdt_first_subnode(blob, parent->offset);
	free(prev);
	for (; offset >= 0; offset = fdt_next_subnode(blob, offset))
		if (!strncmp(fdt_get_name(blob, offset, NULL), prefix, strlen(prefix)))
			return node_at(offset);
	return NULL;
}

#define for_each_child_of_node_with_prefix(parent, child, prefix)	\
	for (struct device_node *child __free(device_node) =		\
	     of_get_next_child_with_prefix(parent, NULL, prefix);	\
	     child != NULL;						\
	     child = of_get_next_child_with_prefix(parent, child, prefix))

static struct device_node *of_find_compatible_node(struct device_node *from, const char *type,
						   const char *compatible)
{
	int offset = fdt_node_offset_by_compatible(blob, from ? from->offset : -1, compatible);

	(void)type;
	free(from);
	return offset < 0 ? NULL : node_at(offset);
}

#define for_each_compatible_node(dn, type, compatible)			\
	for (dn = of_find_compatible_node(NULL, type, compatible); dn;	\
	     dn = of_find_compatible_node(dn, type, compatible))

static bool of_device_is_available(const struct device_node *np)
{
	const char *status = fdt_getprop(blob, np->offset, "status", NULL);

	return !status || !strcmp(status, "okay") || !strcmp(status, "ok");
}

static bool of_property_read_bool(const struct device_node *np, const char *name)
{
	return fdt_getprop(blob, np->offset, name, NULL) != NULL;
}

static bool of_property_present(const struct device_node *np, const char *name)
{
	return of_property_read_bool(np, name);
}

static int of_property_count_strings(const struct device_node *np, const char *name)
{
	return fdt_stringlist_count(blob, np->offset, name);
}

static int of_property_read_u32(const struct device_node *np, const char *name, u32 *out)
{
	int len;
	const fdt32_t *val = fdt_getprop(blob, np->offset, name, &len);

	if (!val || len != 4)
		return -EINVAL;
	*out = fdt32_to_cpu(*val);
	return 0;
}

HWMON_BODY

static int macsmc_hwmon_hotspot_assist_policy(struct device *dev,
					      struct macsmc_hwmon_fan_assist *policy)
{
	if (!dev || !dev->of_node)
		return -ENODEV;
	return macsmc_hwmon_node_hotspot_assist(dev->of_node, policy) ? 0 : -ENODEV;
}

/*
 * Stand-in for macsmc_hwmon_fans_request_manual(): models the real manual_only
 * filter in drivers/hwmon/macsmc-hwmon.c (verified by assertion in extract()).
 */
static int requests;
static int macsmc_hwmon_fans_request_manual(struct device *dev, unsigned int permille,
					    unsigned int min_step_rpm)
{
	int fan;
	int count = 0;

	(void)permille;
	(void)min_step_rpm;
	requests++;
	fdt_for_each_subnode(fan, blob, dev->of_node->offset) {
		const char *name = fdt_get_name(blob, fan, NULL);
		bool manual = fdt_getprop(blob, fan, "apple,fan-manual-control", NULL) != NULL;

		if (strncmp(name, "fan-", 4) ||
		    !fdt_getprop(blob, fan, "apple,fan-mode", NULL) ||
		    !fdt_getprop(blob, fan, "apple,fan-target", NULL) ||
		    !fdt_getprop(blob, fan, "apple,fan-minimum", NULL) ||
		    !fdt_getprop(blob, fan, "apple,fan-maximum", NULL))
			continue;
		if (MANUAL_FILTER)
			continue;
		printf(" %s=%s", name, manual ? "manual" : "closed");
		count++;
	}
	return count ? 5000 : -ENODEV;
}

FANS_BODY

int main(int argc, char **argv)
{
	static char buffer[1 << 22];
	FILE *file = fopen(argv[1], "rb");
	int hwmon;

	if (argc != 2 || !file || fread(buffer, 1, sizeof(buffer), file) == 0)
		return 2;
	blob = buffer;
	printf("%s\n", macsmc_hwmon_hotspot_assist_available() ? "governs" : "idle");
	for (hwmon = fdt_node_offset_by_compatible(blob, -1, "apple,smc-hwmon"); hwmon >= 0;
	     hwmon = fdt_node_offset_by_compatible(blob, hwmon, "apple,smc-hwmon")) {
		char path[256];
		struct device_node node = { .offset = hwmon };
		struct device dev = { .of_node = &node };
		struct macsmc_hwmon_fan_assist policy = {};
		int pret, ret;

		fdt_get_path(blob, hwmon, path, sizeof(path));
		printf("%s", path);
		pret = macsmc_hwmon_hotspot_assist_policy(&dev, &policy);
		requests = 0;
		ret = asahi_fans_request_described(&dev, 1000, 0);
		if (ret < 0)
			printf(" refused\n");
		else if (pret == 0)
			printf(" policy=%d,%d,%u accepted\n",
			       policy.trigger_mc, policy.release_mc, policy.hold_ms);
		else
			printf(" policy=none accepted\n");
	}
	return 0;
}
"""


def extract(tree, mutation):
    hwmon_src = (tree / "drivers/hwmon/macsmc-hwmon.c").read_text()
    fans_src = (tree / "drivers/gpu/drm/asahi/g17/fans.c").read_text()

    # Verify the GPU driver no longer hardcodes TCMb or walks macsmc-hwmon DT bindings.
    assert '"TCMb"' not in fans_src, "fans.c must not name an SMC key"
    assert '"apple,fan-manual-control"' not in fans_src, "fans.c must not walk macsmc-hwmon DT"
    assert '"apple,soc-die-keys"' not in fans_src, "fans.c must not walk macsmc-hwmon DT"
    # Verify macsmc-hwmon.c actually applies the manual_only filter in __macsmc_hwmon_fans_request.
    assert "if (manual_only && !fan->manual_control)\n\t\t\tcontinue;" in hwmon_src

    h_start = hwmon_src.index(HWMON_FIRST)
    h_end = hwmon_src.index(HWMON_LAST, h_start) + len(HWMON_LAST)
    hwmon_body = hwmon_src[h_start:h_end]

    f_start = fans_src.index(FANS_FIRST)
    f_end = fans_src.index("\n}\n", f_start) + 3
    fans_body = fans_src[f_start:f_end]

    manual_filter = "!manual"

    if mutation == "drop-fans":
        hwmon_body, n = re.subn(
            r'of_property_read_bool\(child, "apple,fan-manual-control"\)',
            "true",
            hwmon_body,
        )
        assert n == 1, f"mutation {mutation} did not apply"
        manual_filter = "false"
    elif mutation == "drop-siblings":
        manual_filter = "false"
    elif mutation == "drop-hotspot":
        hwmon_body, n = re.subn(
            r'of_property_count_strings\(np, "apple,soc-die-keys"\) != 1',
            "false",
            hwmon_body,
        )
        assert n == 1, f"mutation {mutation} did not apply"
    elif mutation == "drop-trip":
        hwmon_body, n = re.subn(
            r'if \(of_property_read_u32\(np, "apple,soc-die-fan-trip-millicelsius", &trip\) \|\|\n'
            r'\s*of_property_read_u32\(np, "apple,soc-die-fan-hysteresis-millicelsius", &hyst\) \|\|\n'
            r'\s*of_property_read_u32\(np, "apple,soc-die-fan-hold-ms", &hold_ms\)\)\n'
            r'\s*return false;',
            "trip = 98000; hyst = 16000; hold_ms = 30000;",
            hwmon_body,
        )
        assert n == 1, f"mutation {mutation} did not apply"
    elif mutation == "drop-settable":
        hwmon_body, n = re.subn(
            r'macsmc_hwmon_fan_settable\(child\)',
            "true",
            hwmon_body,
        )
        assert n == 1, f"mutation {mutation} did not apply"
    elif mutation == "drop-provider-check":
        fans_body, n = re.subn(
            r'if \(permille && macsmc_hwmon_hotspot_assist_policy\(dev, NULL\)\)\n\s*return -EPERM;\n',
            "",
            fans_body,
        )
        assert n == 1, f"mutation {mutation} did not apply"
    return hwmon_body, fans_body, manual_filter


RUST_HARNESS = r"""
#![allow(dead_code)]

use std::sync::Mutex;

const FAN_HOLD_NS: u64 = 30_000_000_000;
const FAN_CURVE_RELEASE_MC: i32 = 82_000;
const FAN_CURVE_STEP_RPM: u32 = 150;

#[derive(Clone, Copy)]
struct HotspotPolicy {
    trigger_mc: i32,
    release_mc: i32,
    hold_ns: u64,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum FanCall {
    Budget(u32, u32),
    Hotspot(u32, u32),
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ReqMode {
    Ok,
    PartialFail,
}

struct MockState {
    enabled: bool,
    hotspot_fans_only: bool,
    fan_boost_enabled: bool,
    fan_curve_enabled: bool,
    budget_req: ReqMode,
    budget_rel_ok: bool,
    hotspot_req: ReqMode,
    hotspot_rel_ok: bool,
    budget_owned_mask: u32,
    hotspot_owned_mask: u32,
    calls: Vec<FanCall>,
}

impl MockState {
    const fn new() -> Self {
        Self {
            enabled: false,
            hotspot_fans_only: false,
            fan_boost_enabled: false,
            fan_curve_enabled: true,
            budget_req: ReqMode::Ok,
            budget_rel_ok: true,
            hotspot_req: ReqMode::Ok,
            hotspot_rel_ok: true,
            budget_owned_mask: 0,
            hotspot_owned_mask: 0,
            calls: Vec::new(),
        }
    }

    fn reset(&mut self) {
        *self = Self::new();
    }
}

static TEST_LOCK: Mutex<()> = Mutex::new(());
static MOCK: Mutex<MockState> = Mutex::new(MockState::new());

fn enabled() -> bool {
    MOCK.lock().unwrap().enabled
}

fn hotspot_fans_only() -> bool {
    let s = MOCK.lock().unwrap();
    !s.enabled && s.hotspot_fans_only
}

fn fan_boost_enabled() -> bool {
    MOCK.lock().unwrap().fan_boost_enabled
}

fn fan_curve_enabled() -> bool {
    MOCK.lock().unwrap().fan_curve_enabled
}

fn target_mc() -> Option<i32> {
    Some(90_000)
}

fn fan_curve_permille(temp_mc: i32, target_mc: i32) -> i32 {
    if temp_mc <= 70_000 {
        0
    } else if temp_mc <= target_mc {
        (temp_mc - 70_000) * 660 / (target_mc - 70_000).max(1000)
    } else {
        800
    }
}

fn hotspot_assist_policy() -> Option<HotspotPolicy> {
    Some(HotspotPolicy {
        trigger_mc: 98_000,
        release_mc: 82_000,
        hold_ns: 30_000_000_000,
    })
}

fn request_fans(permille: u32, min_step_rpm: u32) -> Option<u32> {
    let mut s = MOCK.lock().unwrap();
    s.calls.push(FanCall::Budget(permille, min_step_rpm));
    if permille == 0 {
        if s.budget_rel_ok {
            s.budget_owned_mask = 0;
            Some(0)
        } else {
            None
        }
    } else {
        match s.budget_req {
            ReqMode::Ok => {
                s.budget_owned_mask = 0b11;
                Some(5000)
            }
            ReqMode::PartialFail => {
                s.budget_owned_mask |= 0b01;
                None
            }
        }
    }
}

fn request_fans_described(permille: u32, min_step_rpm: u32) -> Option<u32> {
    let mut s = MOCK.lock().unwrap();
    s.calls.push(FanCall::Hotspot(permille, min_step_rpm));
    if permille == 0 {
        if s.hotspot_rel_ok {
            s.hotspot_owned_mask = 0;
            Some(0)
        } else {
            None
        }
    } else {
        match s.hotspot_req {
            ReqMode::Ok => {
                s.hotspot_owned_mask = 0b11;
                Some(5000)
            }
            ReqMode::PartialFail => {
                s.hotspot_owned_mask |= 0b01;
                None
            }
        }
    }
}

FAN_BOOST_BODY

fn take_calls() -> Vec<FanCall> {
    core::mem::take(&mut MOCK.lock().unwrap().calls)
}

#[test]
fn partial_budget_boost_failure_cleans_up_and_blocks_handoff_until_released() {
    let _guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    {
        let mut s = MOCK.lock().unwrap();
        s.reset();
        s.enabled = true;
        s.fan_boost_enabled = true;
        s.hotspot_fans_only = true;
        s.budget_req = ReqMode::PartialFail;
        s.budget_rel_ok = true;
    }

    let mut fans = FanBoost::new();
    fans.apply(true, Some(95_000), 1_000_000_000);
    assert_eq!(take_calls(), vec![FanCall::Budget(1000, 0), FanCall::Budget(0, 0)]);
    assert_eq!(MOCK.lock().unwrap().budget_owned_mask, 0);
    assert!(!fans.boosted());

    // Now fail both the partial budget boost and its immediate cleanup; verify
    // handoff keeps retrying unrestricted cleanup and blocks hotspot assist until
    // cleanup succeeds.
    {
        let mut s = MOCK.lock().unwrap();
        s.budget_rel_ok = false;
    }
    fans.apply(true, Some(95_000), 2_000_000_000);
    assert_eq!(take_calls(), vec![FanCall::Budget(1000, 0), FanCall::Budget(0, 0)]);
    assert_eq!(MOCK.lock().unwrap().budget_owned_mask, 0b01);
    assert!(fans.boosted());

    {
        let mut s = MOCK.lock().unwrap();
        s.enabled = false;
    }
    fans.apply(true, Some(99_000), 3_000_000_000);
    assert_eq!(take_calls(), vec![FanCall::Budget(0, 0)]);
    {
        let s = MOCK.lock().unwrap();
        assert_eq!(s.budget_owned_mask, 0b01);
        assert_eq!(s.hotspot_owned_mask, 0);
    }
    assert!(fans.boosted());

    {
        let mut s = MOCK.lock().unwrap();
        s.budget_rel_ok = true;
    }
    fans.apply(true, Some(99_000), 4_000_000_000);
    assert_eq!(take_calls(), vec![FanCall::Budget(0, 0), FanCall::Hotspot(1000, 0)]);
    {
        let s = MOCK.lock().unwrap();
        assert_eq!(s.budget_owned_mask, 0);
        assert_eq!(s.hotspot_owned_mask, 0b11);
    }
    assert!(fans.boosted());
    fans.release();
}

#[test]
fn partial_first_curve_request_failure_cleans_up_and_blocks_handoff_until_released() {
    let _guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    {
        let mut s = MOCK.lock().unwrap();
        s.reset();
        s.enabled = true;
        s.fan_boost_enabled = false;
        s.fan_curve_enabled = true;
        s.hotspot_fans_only = true;
        s.budget_req = ReqMode::PartialFail;
        s.budget_rel_ok = true;
    }

    let mut fans = FanBoost::new();
    fans.apply(true, Some(95_000), 1_000_000_000);
    let calls = take_calls();
    assert_eq!(calls.len(), 2);
    assert!(matches!(calls[0], FanCall::Budget(p, 0) if p > 0));
    assert_eq!(calls[1], FanCall::Budget(0, 0));
    assert_eq!(MOCK.lock().unwrap().budget_owned_mask, 0);
    assert!(!fans.boosted());

    // Fail immediate cleanup after partial first curve request, then verify both
    // a subsequent budget-mode step and handoff retry unrestricted cleanup first.
    {
        let mut s = MOCK.lock().unwrap();
        s.budget_rel_ok = false;
    }
    fans.apply(true, Some(95_000), 2_000_000_000);
    let calls = take_calls();
    assert_eq!(calls.len(), 2);
    assert!(matches!(calls[0], FanCall::Budget(p, 0) if p > 0));
    assert_eq!(calls[1], FanCall::Budget(0, 0));
    assert_eq!(MOCK.lock().unwrap().budget_owned_mask, 0b01);
    assert!(fans.boosted());

    fans.apply(true, Some(95_000), 3_000_000_000);
    assert_eq!(take_calls(), vec![FanCall::Budget(0, 0)]);
    assert_eq!(MOCK.lock().unwrap().budget_owned_mask, 0b01);
    assert!(fans.boosted());

    {
        let mut s = MOCK.lock().unwrap();
        s.enabled = false;
    }
    fans.apply(true, Some(99_000), 4_000_000_000);
    assert_eq!(take_calls(), vec![FanCall::Budget(0, 0)]);
    {
        let s = MOCK.lock().unwrap();
        assert_eq!(s.budget_owned_mask, 0b01);
        assert_eq!(s.hotspot_owned_mask, 0);
    }
    assert!(fans.boosted());

    {
        let mut s = MOCK.lock().unwrap();
        s.budget_rel_ok = true;
    }
    fans.apply(true, Some(99_000), 5_000_000_000);
    assert_eq!(take_calls(), vec![FanCall::Budget(0, 0), FanCall::Hotspot(1000, 0)]);
    {
        let s = MOCK.lock().unwrap();
        assert_eq!(s.budget_owned_mask, 0);
        assert_eq!(s.hotspot_owned_mask, 0b11);
    }
    assert!(fans.boosted());
    fans.release();
}

#[test]
fn budget_boost_is_reapplied_after_the_smc_takes_the_fans_back() {
    let _guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    {
        let mut s = MOCK.lock().unwrap();
        s.reset();
        s.enabled = true;
        s.fan_boost_enabled = true;
    }

    let mut fans = FanBoost::new();
    fans.apply(true, Some(99_000), 1_000_000_000);
    assert_eq!(take_calls(), vec![FanCall::Budget(1000, 0)]);
    assert_eq!(MOCK.lock().unwrap().budget_owned_mask, 0b11);
    assert!(fans.boosted());

    // Suspend, or the hwmon cell unbound and bound again, hands the fans back to the SMC behind
    // the cache: the next governed step must ask again (a fan already at its maximum keeps it).
    MOCK.lock().unwrap().budget_owned_mask = 0;
    fans.apply(true, Some(99_000), 2_000_000_000);
    assert_eq!(take_calls(), vec![FanCall::Budget(1000, FAN_CURVE_STEP_RPM)]);
    assert_eq!(MOCK.lock().unwrap().budget_owned_mask, 0b11);
    assert!(fans.boosted());
    fans.release();
}

#[test]
fn stop_releases_boosted_fans_inside_the_hold_and_reports_a_pending_release() {
    let _guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    {
        let mut s = MOCK.lock().unwrap();
        s.reset();
        s.enabled = true;
        s.fan_boost_enabled = true;
    }

    let mut fans = FanBoost::new();
    fans.apply(true, Some(99_000), 1_000_000_000);
    assert_eq!(take_calls(), vec![FanCall::Budget(1000, 0)]);

    // Not busy any more, inside the hold: a normal step keeps the fans boosted.
    fans.apply(false, Some(99_000), 2_000_000_000);
    assert_eq!(take_calls(), vec![]);
    assert!(fans.boosted());

    // The firmware stopped (fault, recovery or unbind): the fans go back at once.
    assert!(!fans.stop());
    assert_eq!(take_calls(), vec![FanCall::Budget(0, 0)]);
    assert_eq!(MOCK.lock().unwrap().budget_owned_mask, 0);
    assert!(!fans.boosted());

    // A release the SMC refuses stays pending, so the caller runs again until it lands.
    fans.apply(true, Some(99_000), 3_000_000_000);
    assert_eq!(take_calls(), vec![FanCall::Budget(1000, 0)]);
    MOCK.lock().unwrap().budget_rel_ok = false;
    assert!(fans.stop());
    assert_eq!(take_calls(), vec![FanCall::Budget(0, 0)]);
    MOCK.lock().unwrap().budget_rel_ok = true;
    assert!(!fans.stop());
    assert_eq!(take_calls(), vec![FanCall::Budget(0, 0)]);
    assert_eq!(MOCK.lock().unwrap().budget_owned_mask, 0);
}

#[test]
fn partial_hotspot_failure_blocks_budget_handoff_until_manual_cleanup_succeeds() {
    let _guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    {
        let mut s = MOCK.lock().unwrap();
        s.reset();
        s.enabled = false;
        s.hotspot_fans_only = true;
        s.hotspot_req = ReqMode::PartialFail;
        s.hotspot_rel_ok = false;
    }

    let mut fans = FanBoost::new();
    fans.apply(true, Some(99_000), 1_000_000_000);
    assert_eq!(take_calls(), vec![FanCall::Hotspot(1000, 0), FanCall::Hotspot(0, 0)]);
    assert_eq!(MOCK.lock().unwrap().hotspot_owned_mask, 0b01);
    assert!(fans.boosted());

    {
        let mut s = MOCK.lock().unwrap();
        s.enabled = true;
        s.fan_boost_enabled = true;
    }
    fans.apply(true, Some(95_000), 2_000_000_000);
    assert_eq!(take_calls(), vec![FanCall::Hotspot(0, 0)]);
    {
        let s = MOCK.lock().unwrap();
        assert_eq!(s.hotspot_owned_mask, 0b01);
        assert_eq!(s.budget_owned_mask, 0);
    }

    {
        let mut s = MOCK.lock().unwrap();
        s.hotspot_rel_ok = true;
    }
    fans.apply(true, Some(95_000), 3_000_000_000);
    assert_eq!(take_calls(), vec![FanCall::Hotspot(0, 0), FanCall::Budget(1000, 0)]);
    {
        let s = MOCK.lock().unwrap();
        assert_eq!(s.hotspot_owned_mask, 0);
        assert_eq!(s.budget_owned_mask, 0b11);
    }
    fans.release();
}
"""


def extract_fan_boost(tree, mutation):
    thermal_src = (tree / "drivers/gpu/drm/asahi/g17/thermal.rs").read_text()
    start = thermal_src.index("pub(crate) struct FanBoost {")
    end_marker = "impl Drop for FanBoost {\n    fn drop(&mut self) {\n        self.release();\n    }\n}"
    end = thermal_src.index(end_marker, start) + len(end_marker)
    body = thermal_src[start:end]

    if mutation == "drop-budget-release-pending":
        body, n1 = re.subn(
            r'if let Some\(rpm\) = self\.set_curve_if_moved\(permille\) \{\n'
            r'\s*self\.curve_rpm = rpm;\n'
            r'\s*self\.budget_release_pending = false;\n'
            r'\s*\} else if self\.curve_rpm == 0 \{\n'
            r'(?:\s*//[^\n]*\n)*'
            r'\s*self\.budget_release_pending = !self\.set_budget\(false\);\n'
            r'\s*\}',
            'if let Some(rpm) = self.set_curve_if_moved(permille) {\n                self.curve_rpm = rpm;\n            }',
            body,
        )
        assert n1 == 1, f"mutation {mutation} curve replace failed ({n1})"
        body, n2 = re.subn(
            r'if self\.budget_boosted \{\n'
            r'\s*self\.budget_release_pending = false;\n'
            r'\s*\} else \{\n'
            r'(?:\s*//[^\n]*\n)*'
            r'\s*self\.budget_release_pending = !self\.set_budget\(false\);\n'
            r'\s*\}',
            'if self.budget_boosted {\n                self.budget_release_pending = false;\n            }',
            body,
        )
        assert n2 == 1, f"mutation {mutation} boost replace failed ({n2})"
    return body


def run_fan_boost_tests(tree, output, mutation):
    body = extract_fan_boost(tree, mutation)
    src = output / "fan_boost_test.rs"
    binary = output / "fan_boost_test"
    src.write_text(RUST_HARNESS.replace("FAN_BOOST_BODY", body))
    rustc = os.environ.get("RUSTC", "rustc")
    subprocess.run([rustc, "--edition=2021", "--test", "-Dwarnings", str(src), "-o", str(binary)],
                   check=True)
    res = subprocess.run([str(binary)], capture_output=True, text=True)
    if res.stdout:
        print(res.stdout, end="")
    if res.stderr:
        print(res.stderr, end="", file=sys.stderr)
    return res.returncode == 0


def build(tree, output, mutation):
    libfdt = tree / "scripts/dtc/libfdt"
    hwmon_body, fans_body, manual_filter = extract(tree, mutation)
    harness = output / "harness.c"
    code = (HARNESS
            .replace("HWMON_BODY", hwmon_body)
            .replace("FANS_BODY", fans_body)
            .replace("MANUAL_FILTER", manual_filter))
    harness.write_text(code)
    binary = output / "harness"
    sources = [libfdt / f for f in ("fdt.c", "fdt_ro.c", "fdt_strerror.c")]
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                    "-I", str(libfdt), "-o", str(binary), str(harness)] + [str(s) for s in sources],
                   check=True)
    return binary


def run(binary, dtb):
    """Returns (governs, {provider path: (written fans, policy_str) or None when refused})."""
    lines = subprocess.check_output([str(binary), str(dtb)], text=True).splitlines()
    providers = {}
    for line in lines[1:]:
        path, *rest = line.split()
        verdict = rest[-1]
        if verdict != "accepted":
            providers[path] = None
        else:
            policy_tok = rest[-2]
            assert policy_tok.startswith("policy="), line
            providers[path] = (rest[:-2], policy_tok.split("=", 1)[1])
    return lines[0] == "governs", providers


def dtc_compile(dtc, source, dtb):
    subprocess.run([dtc, "-q", "-I", "dts", "-O", "dtb", "-o", str(dtb)], input=source, check=True)


def compile_board(tree, output, dtc, board, extra="", suffix=""):
    apple = tree / "arch/arm64/boot/dts/apple"
    source = (apple / f"{board}.dts").read_text() + extra
    command = ["cpp", "-nostdinc", "-undef", "-D__DTS__",
               "-I", str(tree / "scripts/dtc/include-prefixes"), "-I", str(apple),
               "-x", "assembler-with-cpp", "-"]
    dtb = output / f"{board}{suffix}.dtb"
    dtc_compile(dtc, subprocess.check_output(command, input=source.encode(), cwd=apple), dtb)
    return dtb


def hwmon_node(name, zone, fans, status=""):
    nodes = "".join(f"\t\tfan-F{n}Ac {{ {props.format(n=n)} }};\n" for n, props in enumerate(fans))
    return (f"\t{name} {{ {status} hwmon {{ compatible = \"apple,smc-hwmon\";\n"
            f"\t\t{zone}\n{nodes}\t}}; }};\n")


def compile_synthetic(output, dtc, name, nodes):
    dtb = output / f"synthetic-{name}.dtb"
    dtc_compile(dtc, ("/dts-v1/;\n/ {\n" + nodes + "};\n").encode(), dtb)
    return dtb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, default=ROOT)
    parser.add_argument("--dtc", required=True, help="this kernel's built scripts/dtc/dtc")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mutation", choices=("drop-fans", "drop-siblings", "drop-hotspot",
                                               "drop-trip", "drop-settable", "drop-provider-check",
                                               "drop-budget-release-pending"))
    args = parser.parse_args()
    tree = args.tree.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    failures = 0

    if args.mutation != "drop-budget-release-pending":
        binary = build(tree, args.output, args.mutation)

    def check(label, dtb, want, want_policy=None, governs=None):
        """want: {provider path suffix: exact fans written, or None when refused}."""
        nonlocal failures
        got_governs, providers = run(binary, dtb)
        if governs is None:
            governs = any(fans for fans in want.values())
        problems = []
        if got_governs != governs:
            problems.append(f"governs {got_governs}, expected {governs}")
        for suffix in want:
            if not any(path.endswith(suffix) for path in providers):
                problems.append(f"no provider ending in {suffix} was reported")
        for path, res in providers.items():
            expected = next((fans for suffix, fans in want.items() if path.endswith(suffix)), None)
            written = None if res is None else res[0]
            pol = None if res is None else res[1]
            names = None if written is None else [w.split("=")[0] for w in written]
            if written is not None and (not written or any(w.endswith("=closed") for w in written)):
                problems.append(f"{path} accepted and writes {written or 'no fan'}")
            if names != expected:
                problems.append(f"{path} writes {names}, expected {expected}")
            if want_policy is not None and expected is not None:
                want_str = f"{want_policy[0]},{want_policy[1]},{want_policy[2]}"
                if pol != want_str:
                    problems.append(f"{path} policy {pol}, expected {want_str}")
        if problems:
            failures += 1
            print(f"FAIL line {sys._getframe(1).f_lineno}: {label}: {'; '.join(problems)}")
        else:
            print(f"ok {label}: {'governs' if got_governs else 'idle'} {providers}")

    if args.mutation != "drop-budget-release-pending":
        for name, (zone, fans, written, pol) in SYNTHETIC.items():
            check(f"synthetic {name}",
                  compile_synthetic(args.output, args.dtc, name, hwmon_node("smc", zone, fans)),
                  {"/smc/hwmon": written}, want_policy=pol)
        check("synthetic opted-in-under-disabled-parent",
              compile_synthetic(args.output, args.dtc, "disabled-parent",
                                hwmon_node("smc0", DIE_TCMB, [MODE + MANUAL], 'status = "disabled";')
                                + hwmon_node("smc1", DIE_TCMB, [MODE])),
              {"/smc0/hwmon": ["fan-F0Ac"], "/smc1/hwmon": None}, governs=False)
        check("synthetic disabled-hwmon-node",
              compile_synthetic(args.output, args.dtc, "disabled-node",
                                hwmon_node("smc", DIE_TCMB + ' status = "disabled";', [MODE + MANUAL])),
              {"/hwmon": ["fan-F0Ac"]}, governs=False)
        for board, written in BOARDS.items():
            check(board, compile_board(tree, args.output, args.dtc, board), {"/hwmon": written})

        j716c_dts = (tree / "arch/arm64/boot/dts/apple/t6050-j716c.dts").read_text()
        has_2208 = "hwmon-fan-dual.dtsi" in j716c_dts
        has_trip = "apple,soc-die-fan-trip-millicelsius" in j716c_dts
        check("t6050-j716c (in-tree)",
              compile_board(tree, args.output, args.dtc, "t6050-j716c"),
              {"/hwmon": J716C_FANS if (has_2208 and has_trip) else None})
        if not has_2208:
            check("t6050-j716c with #2208 (no trip -> stays off)",
                  compile_board(tree, args.output, args.dtc, "t6050-j716c", J716C_2208_NO_TRIP, "-2208-notrip"),
                  {"/hwmon": None})
            check("t6050-j716c with #2208 + DT trip",
                  compile_board(tree, args.output, args.dtc, "t6050-j716c", J716C_2208_WITH_TRIP, "-2208-trip"),
                  {"/hwmon": J716C_FANS}, want_policy=(98000, 82000, 30000))
    if args.mutation in (None, "drop-budget-release-pending"):
        if not run_fan_boost_tests(tree, args.output, args.mutation):
            failures += 1
    if failures:
        raise SystemExit(1)
    print("PASS")


if __name__ == "__main__":
    main()
