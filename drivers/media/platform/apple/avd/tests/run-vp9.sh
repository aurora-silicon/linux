#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only OR MIT
set -eu
src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
# CFLAGS is split intentionally, as with conventional build flags.
# shellcheck disable=SC2086
"${CC:-cc}" ${CFLAGS:-} -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter \
    "$src_dir/test-vp9-fp.c" -o "$test_dir/test-vp9-fp"
"$test_dir/test-vp9-fp"
python3 "$src_dir/test-vp9-builder.py"
python3 "$src_dir/test-vp9-codec.py"
python3 "$src_dir/test-vp9-legacy.py"
python3 "$src_dir/test-vp9-differential.py"
