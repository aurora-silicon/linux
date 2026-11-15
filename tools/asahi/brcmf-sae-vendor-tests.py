#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise SAE offload RSNE handling through the firmware vendor callbacks."""

import argparse
import importlib.util
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = Path("drivers/net/wireless/broadcom/brcm80211/brcmfmac")
spec = importlib.util.spec_from_file_location(
    "association", ROOT / "tools/asahi/brcmf-assoc-tests.py"
)
association = importlib.util.module_from_spec(spec)
spec.loader.exec_module(association)

PREFIX = r"""

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint16_t __le16;
#define BIT(n) (1U << (n))
#define WARN_ON(condition) (condition)
#define brcmf_dbg(...) ((void)0)
#define bphy_err(context, ...) ((void)(context))
#define BRCMF_FEAT_FWSUP 1
enum {
	NL80211_IFTYPE_STATION,
	NL80211_IFTYPE_P2P_CLIENT,
	NL80211_IFTYPE_AP,
	NL80211_IFTYPE_P2P_GO
};
struct cfg80211_crypto_settings {
	const u8 *sae_pwd, *psk;
	u16 sae_pwd_len;
};
struct cfg80211_connect_params {
	struct cfg80211_crypto_settings crypto;
};
struct brcmf_if;
struct brcmf_fwvid_ops {
	int (*set_sae_password)(struct brcmf_if *,
				struct cfg80211_crypto_settings *);
};
struct brcmf_pub {
	const struct brcmf_fwvid_ops *vops;
};
struct brcmf_profile {
	int use_fwsup;
};
struct brcmf_vif {
	struct {
		int iftype;
	} wdev;
	struct brcmf_profile profile;
};
struct brcmf_if {
	struct brcmf_pub *drvr;
	struct brcmf_vif *vif;
};
enum { CLEAR_RSNE, CYW_PASSWORD, WCC_PASSWORD, SET_PMK };
static int commands[8], count;
static int clear_error, password_error, pmk_error, sup_error;
static bool fwsup;
static u16 cpu_to_le16(u16 value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return value;
#else
	return __builtin_bswap16(value);
#endif
}
static bool brcmf_feat_is_enabled(struct brcmf_if *ifp, int feature)
{
	(void)ifp;
	assert(feature == BRCMF_FEAT_FWSUP);
	return fwsup;
}
static int brcmf_fil_iovar_int_set(struct brcmf_if *ifp, const char *name,
				   int value)
{
	(void)ifp;
	assert(!strcmp(name, "sup_wpa") && (value == 0 || value == 1));
	return sup_error;
}
static int brcmf_fil_iovar_data_set(struct brcmf_if *ifp, const char *name,
				    const void *data, unsigned int length)
{
	(void)ifp;
	assert(count < 8);
	if (!strcmp(name, "wpaie")) {
		assert(!data && !length);
		commands[count++] = CLEAR_RSNE;
		return clear_error;
	}
	assert(!strcmp(name, "sae_password") && data && length);
	commands[count++] = CYW_PASSWORD;
	return password_error;
}
static int brcmf_set_wsec(struct brcmf_if *ifp, const u8 *password, u16 length,
			  unsigned int flags)
{
	(void)ifp;
	assert(password && length == 8 && flags == BRCMF_WSEC_PASSPHRASE);
	commands[count++] = WCC_PASSWORD;
	return password_error;
}
static int brcmf_set_pmk(struct brcmf_if *ifp, const u8 *pmk, u16 length)
{
	(void)ifp;
	assert(pmk && length == BRCMF_WSEC_MAX_PSK_LEN);
	commands[count++] = SET_PMK;
	return pmk_error;
}
"""

MAIN = r"""

static const struct brcmf_fwvid_ops wcc = { .set_sae_password =
						    brcmf_wcc_set_sae_pwd };
static const struct brcmf_fwvid_ops cyw = { .set_sae_password =
						    brcmf_cyw_set_sae_pwd };
static struct brcmf_pub pub;
static struct brcmf_vif vif;
static struct brcmf_if ifp = { &pub, &vif };
static struct cfg80211_connect_params params;
static const u8 password[] = "test-sae";
static const u8 pmk[BRCMF_WSEC_MAX_PSK_LEN];
static void reset(const struct brcmf_fwvid_ops *vendor, int type)
{
	pub.vops = vendor;
	vif.wdev.iftype = type;
	vif.profile.use_fwsup = BRCMF_PROFILE_FWSUP_SAE;
	params.crypto = (struct cfg80211_crypto_settings){
		.sae_pwd = password,
		.sae_pwd_len = sizeof(password) - 1,
	};
	count = clear_error = password_error = pmk_error = sup_error = 0;
	fwsup = true;
}
int main(void)
{
	unsigned int controls = 0;
	reset(&wcc, NL80211_IFTYPE_STATION);
	assert(!offload_setup(&ifp, &params));
	assert(count == 1 && commands[0] == WCC_PASSWORD);
	controls++;

	const int types[] = { NL80211_IFTYPE_STATION, NL80211_IFTYPE_P2P_CLIENT,
			      NL80211_IFTYPE_AP, NL80211_IFTYPE_P2P_GO };
	for (unsigned int i = 0; i < 4; i++) {
		reset(&cyw, types[i]);
		if (i < 2) {
			assert(!offload_setup(&ifp, &params));
			assert(count == 2 && commands[0] == CLEAR_RSNE &&
			       commands[1] == CYW_PASSWORD);
		} else {
			assert(!brcmf_fwvid_set_sae_password(&ifp,
							     &params.crypto));
			assert(count == 1 && commands[0] == CYW_PASSWORD);
		}
		controls++;
	}
	reset(&cyw, NL80211_IFTYPE_STATION);
	clear_error = -EIO;
	assert(offload_setup(&ifp, &params) == -EIO);
	assert(count == 1 && commands[0] == CLEAR_RSNE);
	controls++;

	reset(&cyw, NL80211_IFTYPE_STATION);
	password_error = -EIO;
	params.crypto.psk = pmk;
	assert(offload_setup(&ifp, &params) == -EIO);
	assert(count == 2 && commands[1] == CYW_PASSWORD);
	controls++;

	reset(&wcc, NL80211_IFTYPE_STATION);
	password_error = -EIO;
	assert(offload_setup(&ifp, &params) == -EIO);
	assert(count == 1 && commands[0] == WCC_PASSWORD);
	controls++;

	reset(&wcc, NL80211_IFTYPE_STATION);
	params.crypto.psk = pmk;
	assert(!offload_setup(&ifp, &params));
	assert(count == 2 && commands[0] == WCC_PASSWORD &&
	       commands[1] == SET_PMK);
	controls++;

	reset(&wcc, NL80211_IFTYPE_STATION);
	params.crypto.psk = pmk;
	pmk_error = -EIO;
	assert(offload_setup(&ifp, &params) == -EIO);
	assert(count == 2 && commands[1] == SET_PMK);
	controls++;

	reset(&wcc, NL80211_IFTYPE_STATION);
	vif.profile.use_fwsup = BRCMF_PROFILE_FWSUP_NONE;
	params.crypto.psk = pmk;
	assert(!offload_setup(&ifp, &params));
	assert(count == 1 && commands[0] == SET_PMK);
	controls++;

	reset(NULL, NL80211_IFTYPE_STATION);
	assert(offload_setup(&ifp, &params) == -EOPNOTSUPP);
	assert(!count);
	controls++;

	reset(&wcc, NL80211_IFTYPE_STATION);
	sup_error = -EIO;
	assert(offload_setup(&ifp, &params) == -EIO && !count);
	controls++;

	printf("%u SAE vendor setup controls passed\n", controls);
	return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()

    def read(name):
        path = DRIVER / name
        if args.ref:
            return subprocess.check_output(
                ["git", "show", f"{args.ref}:{path}"], cwd=ROOT, text=True
            )
        return (ROOT / path).read_text()

    cfg = read("cfg80211.c")
    header = read("cfg80211.h")
    types = read("fwil_types.h")
    start = header.index("enum brcmf_profile_fwsup {")
    constants = re.findall(
        r"^#define\s+(?:BRCMF_WSEC_MAX_(?:PSK_LEN|SAE_PASSWORD_LEN)|BRCMF_WSEC_PASSPHRASE)\s+.*$",
        types,
        re.M,
    )
    # Execute the production CONNECT offload stage, including its error exits.
    connect = association.function(cfg, "brcmf_cfg80211_connect")
    start_block = connect.index("\tif (brcmf_feat_is_enabled(ifp, BRCMF_FEAT_FWSUP)) {")
    opening = connect.index("{", start_block)
    depth, end = 1, opening + 1
    while depth:
        depth += (connect[end] == "{") - (connect[end] == "}")
        end += 1
    setup = """
static int offload_setup(struct brcmf_if *ifp, struct cfg80211_connect_params *params)
{
    struct brcmf_pub *drvr = ifp->drvr;
    struct brcmf_profile *profile = &ifp->vif->profile;
    int err = 0;
""" + connect[start_block:end] + "\ndone:\n    return err;\n}\n"
    parts = [
        "\n".join(constants),
        PREFIX,
        header[start : header.index("};", start) + 2],
        association.structure(types, "brcmf_wsec_sae_pwd_le"),
        association.function(read("cyw/core.c"), "brcmf_cyw_set_sae_pwd"),
        association.function(read("wcc/core.c"), "brcmf_wcc_set_sae_pwd"),
        association.function(read("fwvid.h"), "brcmf_fwvid_set_sae_password"),
        setup,
        MAIN,
    ]

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        source, binary = out / "controls.c", out / "controls"
        source.write_text("\n".join(parts))
        subprocess.run(
            [
                os.environ.get("CC", "clang"),
                "-std=gnu11",
                "-O1",
                "-g",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all",
                str(source),
                "-o",
                str(binary),
            ],
            check=True,
        )
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        (out / "results.json").write_text(
            json.dumps(
                {
                    "exit": result.returncode,
                    "stdout": result.stdout,
                    "stderr": result.stderr,
                    "limits": "CONNECT offload stage and vendor callbacks with host firmware stubs; no radio.",
                },
                indent=2,
            )
            + "\n"
        )
        print(result.stdout or result.stderr, end="")
        return result.returncode != 0

    if args.out:
        return run(args.out.resolve())
    with tempfile.TemporaryDirectory(prefix="brcmf-sae-vendor-") as out:
        return run(Path(out))


if __name__ == "__main__":
    raise SystemExit(main())
