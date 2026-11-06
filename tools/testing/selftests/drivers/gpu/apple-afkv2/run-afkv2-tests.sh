#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

afkv2_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
afkv2_root=${AFKV2_SOURCE_ROOT:-$(CDPATH= cd -- "$afkv2_dir/../../../../../.." && pwd)}
afkv2_cc=${HOSTCC:-${CC:-cc}}
afkv2_cc_command=${afkv2_cc%% *}
afkv2_python=${PYTHON:-python3}
if ! command -v "$afkv2_cc_command" >/dev/null 2>&1 ||
   ! command -v "$afkv2_python" >/dev/null 2>&1; then
    echo "SKIP: a host C compiler and Python 3 are required"
    exit 4
fi
if [ ! -f "$afkv2_root/drivers/gpu/drm/apple/afkv2-codec.h" ]; then
    echo "SKIP: run from a source tree containing the AFKV2 codec"
    exit 4
fi
afkv2_tmp=$(mktemp -d "${TMPDIR:-/tmp}/apple-afkv2.XXXXXX")
trap 'rm -rf -- "$afkv2_tmp"' EXIT
trap 'exit 1' HUP INT TERM
afkv2_sanitizers=
if [ "${AFKV2_SANITIZERS:-0}" = 1 ]; then
    afkv2_sanitizers="-fsanitize=address,undefined -fno-omit-frame-pointer"
fi

# Compiler arguments, CFLAGS and sanitizers use normal compiler word splitting.
# shellcheck disable=SC2086
$afkv2_cc -std=c11 -Wall -Wextra -Werror -pedantic ${CFLAGS:-} $afkv2_sanitizers \
	-I"$afkv2_dir/host-include" -I"$afkv2_root/drivers/gpu/drm/apple" \
	"$afkv2_dir/codec.c" -o "$afkv2_tmp/codec"
"$afkv2_tmp/codec"
"$afkv2_python" "$afkv2_dir/extract-owned.py" \
	"$afkv2_root/drivers/gpu/drm/apple/afk.c" "$afkv2_tmp/owned.c"
# The extracted kernel function uses GNU C void-pointer arithmetic.
# shellcheck disable=SC2086
$afkv2_cc -std=gnu11 -Wall -Wextra -Werror ${CFLAGS:-} $afkv2_sanitizers \
	-I"$afkv2_dir/host-include" -I"$afkv2_root/drivers/gpu/drm/apple" \
	"$afkv2_tmp/owned.c" -o "$afkv2_tmp/owned"
"$afkv2_tmp/owned"

# Inline DPAV transport and actual-length EDID response validation.
# shellcheck disable=SC2086
$afkv2_cc -std=c11 -Wall -Wextra -Werror -pedantic ${CFLAGS:-} $afkv2_sanitizers \
    -I"$afkv2_dir/host-include" -I"$afkv2_root/drivers/gpu/drm/apple" \
    "$afkv2_dir/inline-edid.c" -o "$afkv2_tmp/inline-edid"
"$afkv2_tmp/inline-edid"
"$afkv2_python" "$afkv2_dir/extract-service.py" \
    "$afkv2_root/drivers/gpu/drm/apple/afk.c" "$afkv2_tmp/service.c"
# shellcheck disable=SC2086
$afkv2_cc -std=gnu11 -Wall -Wextra -Werror ${CFLAGS:-} $afkv2_sanitizers \
    -I"$afkv2_dir/host-include" -I"$afkv2_root/drivers/gpu/drm/apple" \
    "$afkv2_tmp/service.c" -o "$afkv2_tmp/service"
"$afkv2_tmp/service"

# The DPTX HPD request: only a complete reply carries the HPD echo.
"$afkv2_python" "$afkv2_dir/extract-hpd.py" \
    "$afkv2_root/drivers/gpu/drm/apple/afk.c" \
    "$afkv2_root/drivers/gpu/drm/apple/dptxep.c" "$afkv2_tmp/hpd.c"
# shellcheck disable=SC2086
$afkv2_cc -std=gnu11 -Wall -Wextra -Werror ${CFLAGS:-} $afkv2_sanitizers \
    -I"$afkv2_dir/host-include" -I"$afkv2_root/drivers/gpu/drm/apple" \
    "$afkv2_tmp/hpd.c" -o "$afkv2_tmp/hpd"
"$afkv2_tmp/hpd"

"$afkv2_python" "$afkv2_dir/extract-send.py" \
    "$afkv2_root/drivers/gpu/drm/apple/afk.c" "$afkv2_tmp/send.c"
# shellcheck disable=SC2086
$afkv2_cc -std=gnu11 -Wall -Wextra -Werror ${CFLAGS:-} $afkv2_sanitizers \
    -I"$afkv2_dir/host-include" -I"$afkv2_root/drivers/gpu/drm/apple" \
    "$afkv2_tmp/send.c" -o "$afkv2_tmp/send"
"$afkv2_tmp/send"

"$afkv2_python" "$afkv2_dir/extract-edid.py" \
    "$afkv2_root/drivers/gpu/drm/apple/epic/dpavservep.c" "$afkv2_tmp/edid.c"
# shellcheck disable=SC2086
$afkv2_cc -std=gnu11 -Wall -Wextra -Werror ${CFLAGS:-} $afkv2_sanitizers \
    -I"$afkv2_dir/host-include" -I"$afkv2_root/drivers/gpu/drm/apple" \
    "$afkv2_tmp/edid.c" -o "$afkv2_tmp/edid"
"$afkv2_tmp/edid"

"$afkv2_python" "$afkv2_dir/extract-ring.py" \
    "$afkv2_root/drivers/gpu/drm/apple/afk.c" "$afkv2_tmp/ring.c"
"$afkv2_python" "$afkv2_dir/extract-notify.py" \
    "$afkv2_root/drivers/gpu/drm/apple/epic/dpavservep.c" "$afkv2_tmp/notify.c"
"$afkv2_python" "$afkv2_dir/extract-compact.py" \
    "$afkv2_root/drivers/gpu/drm/apple/afk.c" "$afkv2_tmp/compact.c"
for afkv2_test in ring notify compact; do
    # shellcheck disable=SC2086
    $afkv2_cc -std=gnu11 -Wall -Wextra -Werror ${CFLAGS:-} $afkv2_sanitizers \
        -I"$afkv2_dir/host-include" -I"$afkv2_root/drivers/gpu/drm/apple" \
        "$afkv2_tmp/$afkv2_test.c" -o "$afkv2_tmp/$afkv2_test"
    "$afkv2_tmp/$afkv2_test"
done
