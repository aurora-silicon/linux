#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the v0/v1 join constructors with and without AWDL."""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = Path("drivers/net/wireless/broadcom/brcm80211/brcmfmac")

PREFIX = r"""

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint16_t __le16;
typedef uint32_t __le32;
#define ETH_ALEN 6
#define IEEE80211_MAX_SSID_LEN 32
#define GFP_KERNEL 0
#define bphy_err(...) ((void)0)
#define brcmf_dbg(...) ((void)0)
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define struct_size(p, member, count) \
	(sizeof(*(p)) + sizeof((p)->member[0]) * (count))
static uint16_t cpu_to_le16(uint16_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return value;
#else
	return __builtin_bswap16(value);
#endif
}
static uint32_t cpu_to_le32(uint32_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return value;
#else
	return __builtin_bswap32(value);
#endif
}
#define le32_to_cpu(value) cpu_to_le32(value)
struct brcmf_pub {
	bool awdl;
};
struct brcmf_cfg80211_info {
	u32 channel;
	unsigned int d11inf;
	struct brcmf_pub *pub;
};
struct ieee80211_channel {
	u16 chanspec;
};
struct cfg80211_connect_params {
	const u8 *ssid;
	size_t ssid_len;
	const u8 *bssid;
	struct ieee80211_channel *channel, *channel_hint;
};
bool brcmf_has_awdl_interface(struct brcmf_pub *pub)
{
	return pub->awdl;
}
static void *kzalloc(size_t size, int flags)
{
	(void)flags;
	return calloc(1, size);
}
static void eth_broadcast_addr(u8 *address)
{
	memset(address, 0xff, ETH_ALEN);
}
static u16 channel_to_chanspec(unsigned int *d11inf,
			       struct ieee80211_channel *channel)
{
	(void)d11inf;
	return channel->chanspec;
}
"""

MAIN = r"""

int main(void)
{
	const u8 ssid[] = "test";
	const u8 bssid[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 1 };
	struct ieee80211_channel channel = { .chanspec = 0xd034 };
	unsigned int controls = 0;

	for (int version = 0; version <= 1; version++) {
		for (int selection = 0; selection < 3; selection++) {
			for (int awdl = 0; awdl <= 1; awdl++) {
				struct brcmf_pub pub = { .awdl = awdl };
				struct brcmf_cfg80211_info cfg = {
					.channel = selection ? 52 : 0,
					.pub = &pub,
				};
				struct cfg80211_connect_params params = {
					.ssid = ssid,
					.ssid_len = sizeof(ssid) - 1,
					.bssid = bssid,
					.channel_hint = selection == 1 ?
								&channel :
								NULL,
					.channel = selection == 2 ? &channel :
								    NULL,
				};
				u32 size;
				struct brcmf_ext_join_params_le *v0 = NULL;
				struct brcmf_ext_join_params_v1_le *v1 = NULL;
				struct brcmf_join_scan_params_le *scan;
				struct brcmf_ssid_le *sent_ssid;
				const u8 *sent_bssid;
				u32 channels;
				u16 chanspec = 0;
				bool extended = selection && !awdl;

				if (version) {
					v1 = brcmf_get_struct_for_connect_v1(
						&cfg, &size, &params);
					assert(v1);
					assert(v1->version == cpu_to_le16(1));
					assert(v1->assoc_le.version ==
					       cpu_to_le16(1));
					assert(v1->assoc_le.flags == 0);
					scan = &v1->scan_le;
					sent_ssid = &v1->ssid_le;
					sent_bssid = v1->assoc_le.bssid;
					channels = le32_to_cpu(
						v1->assoc_le.chanspec_num);
					if (selection)
						chanspec =
							v1->assoc_le
								.chanspec_list[0];
					assert(size ==
					       sizeof(*v1) +
						       (selection ?
								sizeof(u16) :
								0));
				} else {
					v0 = brcmf_get_struct_for_connect_v0(
						&cfg, &size, &params);
					assert(v0);
					scan = &v0->scan_le;
					sent_ssid = &v0->ssid_le;
					sent_bssid = v0->assoc_le.bssid;
					channels = le32_to_cpu(
						v0->assoc_le.chanspec_num);
					if (selection)
						chanspec =
							v0->assoc_le
								.chanspec_list[0];
					assert(size ==
					       sizeof(*v0) +
						       (selection ?
								sizeof(u16) :
								0));
				}

				assert(scan->scan_type == 0);
				assert(scan->home_time == cpu_to_le32(-1));
				assert(scan->active_time ==
				       cpu_to_le32(extended ? 320 : -1));
				assert(scan->passive_time ==
				       cpu_to_le32(extended ? 400 : -1));
				assert(scan->nprobes ==
				       cpu_to_le32(extended ? 16 : -1));
				assert(channels == (selection ? 1U : 0U));
				if (selection)
					assert(chanspec ==
					       cpu_to_le16(channel.chanspec));
				assert(le32_to_cpu(sent_ssid->SSID_len) ==
				       sizeof(ssid) - 1);
				assert(!memcmp(sent_ssid->SSID, ssid,
					       sizeof(ssid) - 1));
				assert(!memcmp(sent_bssid, bssid, ETH_ALEN));
				free(version ? (void *)v1 : (void *)v0);
				controls++;
			}
		}
	}
	printf("%u join scan controls passed\n", controls);
	return 0;
}
"""


def function(source, name):
    match = re.search(r"^static\s+void\s*\*?\s*" + name + r"\(", source, re.M)
    if not match:
        raise ValueError(name)
    opening = source.index("{", match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start() : end] + "\n"


def structure(source, name):
    match = re.search(r"struct " + name + r"\s*\{", source)
    if not match:
        raise ValueError(name)
    end = source.index("};", match.end()) + 2
    return source[match.start() : end] + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--ref", help="Git source revision to exercise")
    parser.add_argument("--out", type=Path, help="retain executable and results")
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    args = parser.parse_args()

    def read(name):
        path = DRIVER / name
        if args.ref:
            return subprocess.check_output(
                ["git", "show", f"{args.ref}:{path}"], cwd=args.root, text=True
            )
        return (args.root / path).read_text()

    source = read("join_param.c")
    types = read("fwil_types.h")
    constants = "\n".join(re.findall(r"^#define BRCMF_SCAN_JOIN_.*$", source, re.M))
    structures = "".join(
        structure(types, name)
        for name in [
            "brcmf_ssid_le",
            "brcmf_join_scan_params_le",
            "brcmf_assoc_params_le",
            "brcmf_assoc_params_v1_le",
            "brcmf_ext_join_params_le",
            "brcmf_ext_join_params_v1_le",
        ]
    )
    bodies = "".join(
        function(source, name)
        for name in [
            "brcmf_joinscan_set_ssid",
            "brcmf_joinscan_set_bssid",
            "brcmf_joinscan_set_single_chanspec_from_channel",
            "brcmf_joinscan_set_common_v0v1_params",
            "brcmf_get_struct_for_connect_v0",
            "brcmf_get_struct_for_connect_v1",
        ]
    )
    code = PREFIX + constants + "\n" + structures + bodies + MAIN

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        src, exe = out / "controls.c", out / "controls"
        src.write_text(code)
        subprocess.run(
            [
                args.cc,
                "-std=gnu11",
                "-O1",
                "-g",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all",
                str(src),
                "-o",
                str(exe),
            ],
            check=True,
        )
        result = subprocess.run([str(exe)], capture_output=True, text=True, cwd=out)
        receipt = {
            "exit": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
            "limits": "Join constructors run with host stubs; no firmware or radio.",
        }
        (out / "results.json").write_text(json.dumps(receipt, indent=2) + "\n")
        sys.stdout.write(result.stdout or result.stderr)
        return result.returncode != 0

    if args.out:
        return run(args.out.resolve())
    with tempfile.TemporaryDirectory(prefix="brcmf-join-scan-") as out:
        return run(Path(out))


if __name__ == "__main__":
    raise SystemExit(main())
