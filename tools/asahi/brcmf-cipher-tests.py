#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Observe brcmfmac's cipher-selection commands at the firmware boundary."""

import argparse
import importlib.util
import json
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = Path("drivers/net/wireless/broadcom/brcm80211/brcmfmac")
spec = importlib.util.spec_from_file_location(
    "association_controls", ROOT / "tools/asahi/brcmf-assoc-tests.py"
)
association = importlib.util.module_from_spec(spec)
spec.loader.exec_module(association)

PREFIX = r"""

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
#define BIT(n) (1U << (n))
#define GFP_KERNEL 0
#define brcmf_dbg(...) ((void)0)
#define brcmf_err(...) ((void)0)
#define bphy_err(...) ((void)0)
#define WARN(test, ...) assert(!(test))
#define WARN_ON(test) assert(!(test))
#define WLAN_CIPHER_SUITE_WEP40 0x000fac01
#define WLAN_CIPHER_SUITE_TKIP 0x000fac02
#define WLAN_CIPHER_SUITE_CCMP 0x000fac04
#define WLAN_CIPHER_SUITE_WEP104 0x000fac05
#define WLAN_CIPHER_SUITE_AES_CMAC 0x000fac06
#define WLAN_CIPHER_SUITE_GCMP_256 0x000fac09
#define BRCMF_FEAT_GCMP 1
struct brcmf_pub {
	bool gcmp;
};
struct brcmf_if {
	struct brcmf_pub *drvr;
};
struct brcmf_cfg80211_security {
	u32 cipher_pairwise, cipher_group;
};
struct brcmf_cfg80211_profile {
	struct brcmf_cfg80211_security sec;
};
struct net_device {
	struct brcmf_if *ifp;
	struct brcmf_cfg80211_profile profile;
};
struct cfg80211_connect_params {
	struct {
		unsigned int n_ciphers_pairwise;
		u32 ciphers_pairwise[1];
		u32 cipher_group;
	} crypto;
	const u8 *ie;
	unsigned int ie_len;
	bool privacy;
};
static unsigned int wsec_calls, algorithm_calls, allocations;
static u32 observed_wsec, observed_algos, observed_mask, firmware_algos;
static int wsec_error, algorithm_error;
static bool allocation_error;
static void *netdev_priv(struct net_device *ndev)
{
	return ndev->ifp;
}
static struct brcmf_cfg80211_profile *ndev_to_prof(struct net_device *ndev)
{
	return &ndev->profile;
}
static bool brcmf_feat_is_enabled(struct brcmf_if *ifp, int feature)
{
	assert(feature == BRCMF_FEAT_GCMP);
	return ifp->drvr->gcmp;
}
static bool brcmf_find_wpsie(const u8 *ie, unsigned int len)
{
	assert(!ie && !len);
	return false;
}
static void *kzalloc(size_t size, int flags)
{
	assert(flags == GFP_KERNEL);
	if (allocation_error)
		return NULL;
	void *data = calloc(1, size);
	assert(data);
	allocations++;
	return data;
}
static void kfree(void *data)
{
	assert(allocations);
	allocations--;
	free(data);
}
static u32 read_le32(const u8 *data)
{
	return (u32)data[0] | (u32)data[1] << 8 | (u32)data[2] << 16 |
	       (u32)data[3] << 24;
}
static void put_unaligned_le16(u16 value, u8 *data)
{
	data[0] = value;
	data[1] = value >> 8;
}
static int brcmf_fil_bsscfg_int_set(struct brcmf_if *ifp, const char *name,
				    u32 value)
{
	(void)ifp;
	assert(!strcmp(name, "wsec"));
	assert(!wsec_calls && !algorithm_calls);
	wsec_calls++;
	observed_wsec = value;
	return wsec_error;
}
static int brcmf_fil_bsscfg_data_set(struct brcmf_if *ifp, const char *name,
				     const void *data, u32 len)
{
	const u8 *bytes = data;
	(void)ifp;
	assert(!strcmp(name, "wsec_info"));
	assert(wsec_calls == 1 && !algorithm_calls);
	/* Version 1, one BSS_ALGOS (0x106) TLV with two little-endian u32s. */
	assert(len == 16);
	assert(bytes[0] == 1 && !bytes[1] && !bytes[2] && bytes[3] == 1);
	assert(bytes[4] == 6 && bytes[5] == 1 && bytes[6] == 8 && !bytes[7]);
	observed_algos = read_le32(bytes + 8);
	observed_mask = read_le32(bytes + 12);
	algorithm_calls++;
	if (algorithm_error)
		return algorithm_error;
	/* Firmware ABI: algorithms outside the mask remain unchanged. */
	firmware_algos = (firmware_algos & ~observed_mask) |
			 (observed_algos & observed_mask);
	return 0;
}
"""

CONTROLS = r"""

static struct brcmf_pub pub;
static struct brcmf_if ifp = { &pub };
static struct net_device ndev = { .ifp = &ifp };
static struct cfg80211_connect_params params;

static void reset(bool gcmp, u32 pairwise, u32 group)
{
	assert(!allocations);
	pub.gcmp = gcmp;
	memset(&params, 0, sizeof(params));
	params.crypto.n_ciphers_pairwise = 1;
	params.crypto.ciphers_pairwise[0] = pairwise;
	params.crypto.cipher_group = group;
	params.privacy = true;
	memset(&ndev.profile, 0, sizeof(ndev.profile));
	observed_wsec = observed_algos = observed_mask = 0;
	wsec_calls = algorithm_calls = 0;
	wsec_error = algorithm_error = 0;
	allocation_error = false;
	/* GCMP-256 starts enabled; bit 31 represents an unrelated algorithm. */
	firmware_algos = 0x80010000U;
}

int main(void)
{
	unsigned int controls = 0;
	reset(true, WLAN_CIPHER_SUITE_CCMP, WLAN_CIPHER_SUITE_CCMP);
	assert(!brcmf_set_wsec_mode(&ndev, &params));
	assert(observed_wsec == 4 && algorithm_calls == 1);
	assert(observed_algos == 0x10 && observed_mask == 0x10010);
	assert(firmware_algos == 0x80000010U);
	assert(ndev.profile.sec.cipher_pairwise == WLAN_CIPHER_SUITE_CCMP);
	assert(ndev.profile.sec.cipher_group == WLAN_CIPHER_SUITE_CCMP);
	assert(!allocations);
	controls++;

	reset(false, WLAN_CIPHER_SUITE_CCMP, WLAN_CIPHER_SUITE_CCMP);
	assert(!brcmf_set_wsec_mode(&ndev, &params));
	assert(observed_wsec == 4 && !algorithm_calls);
	assert(firmware_algos == 0x80010000U);
	controls++;

	const u32 groups[] = { WLAN_CIPHER_SUITE_CCMP,
			       WLAN_CIPHER_SUITE_GCMP_256 };
	for (unsigned int i = 0; i < 2; i++) {
		reset(true, WLAN_CIPHER_SUITE_GCMP_256, groups[i]);
		assert(!brcmf_set_wsec_mode(&ndev, &params));
		assert(observed_wsec == 4 && algorithm_calls == 1);
		assert(observed_algos == 0x10000 && observed_mask == 0x10010);
		assert(firmware_algos == 0x80010000U);
		assert(!allocations);
		controls++;
	}

	const u32 legacy[] = { WLAN_CIPHER_SUITE_TKIP, WLAN_CIPHER_SUITE_WEP40,
			       WLAN_CIPHER_SUITE_WEP104 };
	const u32 flags[] = { 2, 1, 1 };
	for (unsigned int i = 0; i < 3; i++) {
		reset(true, legacy[i], legacy[i]);
		assert(!brcmf_set_wsec_mode(&ndev, &params));
		assert(observed_wsec == flags[i] && algorithm_calls == 1);
		assert(!observed_algos && !observed_mask);
		assert(firmware_algos == 0x80010000U && !allocations);
		controls++;
	}

	reset(false, WLAN_CIPHER_SUITE_GCMP_256, WLAN_CIPHER_SUITE_CCMP);
	assert(brcmf_set_wsec_mode(&ndev, &params) == -EOPNOTSUPP);
	assert(!wsec_calls && !algorithm_calls);
	controls++;

	reset(true, WLAN_CIPHER_SUITE_CCMP, WLAN_CIPHER_SUITE_CCMP);
	wsec_error = -EIO;
	assert(brcmf_set_wsec_mode(&ndev, &params) == -EIO);
	assert(wsec_calls == 1 && !algorithm_calls && !allocations);
	assert(!ndev.profile.sec.cipher_pairwise);
	controls++;

	reset(true, WLAN_CIPHER_SUITE_CCMP, WLAN_CIPHER_SUITE_CCMP);
	algorithm_error = -EIO;
	assert(brcmf_set_wsec_mode(&ndev, &params) == -EIO);
	assert(algorithm_calls == 1 && firmware_algos == 0x80010000U);
	assert(!ndev.profile.sec.cipher_pairwise && !allocations);
	controls++;

	reset(true, WLAN_CIPHER_SUITE_CCMP, WLAN_CIPHER_SUITE_CCMP);
	allocation_error = true;
	assert(brcmf_set_wsec_mode(&ndev, &params) == -ENOMEM);
	assert(!algorithm_calls && !allocations);
	assert(!ndev.profile.sec.cipher_pairwise);
	controls++;

	printf("%u production cipher-command controls passed\n", controls);
	return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref", help="Git source revision to exercise")
    parser.add_argument("--out", type=Path, help="retain executable and results")
    args = parser.parse_args()

    def read(path):
        if args.ref:
            return subprocess.check_output(
                ["git", "show", f"{args.ref}:{path}"], cwd=ROOT, text=True
            )
        return (ROOT / path).read_text()

    cfg = read(DRIVER / "cfg80211.c")
    types = read(DRIVER / "fwil_types.h")
    xtlv = read(DRIVER / "xtlv.h")
    wifi = read(DRIVER.parent / "include/brcmu_wifi.h")
    constants = "\n".join(
        line
        for source in (cfg, types, wifi)
        for line in source.splitlines()
        if re.match(
            r"#define\s+(KEY_ALGO_MASK\(|WL_WSEC_INFO_BSS_(?:BASE|ALGOS)\s|"
            r"BRCMF_WSEC_INFO_VER\s|(?:AES|TKIP|WEP)_ENABLED\s|"
            r"CRYPTO_ALGO_AES_(?:CCM|GCM256)\s)",
            line,
        )
    )
    parts = [PREFIX, constants]
    for name in ("brcmf_wsec_info_tlv", "brcmf_wsec_info"):
        parts.append(association.structure(types, name))
    parts.append(association.structure(xtlv, "brcmf_xtlv"))
    start = xtlv.index("enum brcmf_xtlv_option {")
    parts.append(xtlv[start : xtlv.index("};", start) + 2])
    parts.append(
        association.function(read(DRIVER / "xtlv.c"), "brcmf_xtlv_pack_header")
    )
    for name in ("brcmf_set_wsec_info_algos", "brcmf_set_wsec_mode"):
        parts.append(association.function(cfg, name))
    code = "\n".join(parts) + CONTROLS

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        source = out / "controls.c"
        source.write_text(code)
        executable = out / "controls"
        subprocess.run(
            [
                "clang",
                "-std=gnu11",
                "-O1",
                "-g",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-variable",
                "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all",
                str(source),
                "-o",
                str(executable),
            ],
            check=True,
        )
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        receipt = {
            "exit": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
            "ref": args.ref,
            "limits": (
                "Production command encoding at an observing host boundary; "
                "no firmware or hardware test."
            ),
        }
        (out / "results.json").write_text(json.dumps(receipt, indent=2) + "\n")
        print(result.stdout or result.stderr, end="")
        return result.returncode != 0

    if args.out:
        return run(args.out)
    with tempfile.TemporaryDirectory(prefix="brcmf-cipher-") as out:
        return run(Path(out))


if __name__ == "__main__":
    raise SystemExit(main())
