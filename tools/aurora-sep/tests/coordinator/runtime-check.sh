#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Coordinator payload: execute through an approved, explicitly named device.
set -euo pipefail
: "${EXPECTED_KERNEL_RELEASE:?set the exact candidate release}"
: "${EXPECTED_SEP_PROFILE:?set the profile expected for this device and firmware}"
: "${SEP_TEST_USER:?set the existing test account}"
test "$(id -u)" = 0
test "$(uname -r)" = "$EXPECTED_KERNEL_RELEASE"
test -c /dev/sep-bio
tr ' ' '\n' </proc/cmdline | grep -Fx 'trusted.source=applesep' >/dev/null
umask 077
scratch=$(mktemp -d /run/unified-sep-check.XXXXXXXX)
old_rng=$(cat /sys/class/misc/hw_random/rng_current)
new_key=
loaded_key=
cleanup() {
    test -z "$new_key" || keyctl revoke "$new_key" >/dev/null 2>&1 || true
    test -z "$loaded_key" || keyctl revoke "$loaded_key" >/dev/null 2>&1 || true
    printf '%s\n' "$old_rng" > /sys/class/misc/hw_random/rng_current || true
    rm -rf -- "$scratch"
}
trap cleanup EXIT
since=$(date --iso-8601=seconds)
diag=
for path in /sys/bus/platform/devices/*.sep/diag; do
    test -d "$path" || continue
    test -z "$diag"
    diag=$path
done
test -n "$diag"
test "$(cat "$diag/profile")" = "$EXPECTED_SEP_PROFILE"
test "$(cat "$diag/attach")" = attached
test "$(cat "$diag/keystore")" = open

# Account and enrollment output stays in a private transient directory.
fprintd-list "$SEP_TEST_USER" > "$scratch/before" 2>&1
for iteration in $(seq 1 20); do
    rc=0
    timeout -s INT 2s fprintd-verify "$SEP_TEST_USER" > "$scratch/verify" 2>&1 || rc=$?
    case "$rc" in 0|124|130) ;; *) exit 1 ;; esac
    if grep -Eiq 'busy|not register|transfer failed|unknown-error' "$scratch/verify"; then
        exit 1
    fi
done
test "$(cat "$diag/touchid")" = ready
fprintd-list "$SEP_TEST_USER" > "$scratch/after" 2>&1
cmp -s "$scratch/before" "$scratch/after"

# Exercise private-key admission. No key, blob or keyring ID is printed.
new_key=$(keyctl add trusted unified-sep-check 'new 32' @s 2>/dev/null)
keyctl pipe "$new_key" > "$scratch/blob" 2>/dev/null
loaded_key=$({ printf 'load '; cat "$scratch/blob"; } |
    keyctl padd trusted unified-sep-reload @s 2>/dev/null)
keyctl revoke "$new_key" >/dev/null 2>&1
new_key=
keyctl revoke "$loaded_key" >/dev/null 2>&1
loaded_key=

# Select the SEP source, discard all entropy bytes, and check it after sleep.
printf '%s\n' apple-sep > /sys/class/misc/hw_random/rng_current
for iteration in 1 2; do
    timeout 5s dd if=/dev/hwrng of=/dev/null bs=4 count=1 status=none 2>/dev/null
    rtcwake -m freeze -s 5 >/dev/null 2>&1
    timeout 5s dd if=/dev/hwrng of=/dev/null bs=4 count=1 status=none 2>/dev/null
    test "$(cat "$diag/attach")" = attached
    test "$(cat "$diag/keystore")" = open
done
set +e
journalctl -k --since "$since" --no-pager -o cat 2>/dev/null |
    grep -Ei 'capture did not drain|hwrng: read produced nothing|sbio:.*timed out' >/dev/null
scan_status=("${PIPESTATUS[@]}")
set -e
test "${scan_status[0]}" = 0
test "${scan_status[1]}" = 1
printf '%s\n' 'SEP runtime checks: PASS'
