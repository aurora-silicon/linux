#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only OR MIT
set -eu
avd_test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
avd_fw_dir=${1:?usage: run-multi-vp.sh AVD_FW_DIR OUTPUT_BINARY}
avd_test_out=${2:?usage: run-multi-vp.sh AVD_FW_DIR OUTPUT_BINARY}
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O2 \
	-I"$avd_test_dir/include" -I"$avd_fw_dir/src" \
	"$avd_test_dir/multi-vp-host.c" "$avd_fw_dir/src/queue.c" -o "$avd_test_out"
"$avd_test_out"
