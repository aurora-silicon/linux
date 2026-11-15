#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Coordinator payload for an expendable, previously enrolled test account.
# Run "before", use the controller's approved reboot flow, then run "after".
set -euo pipefail
: "${EXPECTED_KERNEL_RELEASE:?set the exact candidate release}"
: "${SEP_TEST_USER:?set the disposable enrolled test account}"
test "$(id -u)" = 0
test "$(uname -r)" = "$EXPECTED_KERNEL_RELEASE"
test -c /dev/sep-bio
state_dir=/var/lib/aurora-unified-sep-check
count_helper=/run/aurora-checks/sep-live-count.py
test ! -L "$state_dir"
if test -e "$state_dir"; then
    test -d "$state_dir"
    test "$(stat -c %u "$state_dir")" = 0
fi
install -d -m 0700 "$state_dir"
trap 'systemctl start fprintd.service >/dev/null 2>&1 || true' EXIT
fingers() {
    busctl --system call net.reactivated.Fprint \
        /net/reactivated/Fprint/Device/0 net.reactivated.Fprint.Device \
        ListEnrolledFingers s "$SEP_TEST_USER" 2>/dev/null
}
case "${1:-}" in
    before)
        current=$(fingers)
        case "$current" in
            as\ [1-9]*) ;;
            *) exit 1 ;;
        esac
        removed=${current#as }
        removed=${removed%% *}
        case "$removed" in *[!0-9]*) exit 1 ;; esac
        systemctl stop fprintd.service
        python3 "$count_helper" prepare --state "$state_dir/live-count" --removed "$removed"
        fprintd-delete "$SEP_TEST_USER" >/dev/null 2>&1
        current=$(fingers)
        test "$current" = 'as 0'
        systemctl stop fprintd.service
        python3 "$count_helper" check --state "$state_dir/live-count"
        printf '%s\n' 'Deletion: PASS; controller reboot and after-check pending'
        ;;
    after)
        current=$(fingers)
        test "$current" = 'as 0'
        systemctl stop fprintd.service
        python3 "$count_helper" check --state "$state_dir/live-count"
        rm -f -- "$state_dir/live-count"
        printf '%s\n' 'Deletion persistence after reboot: PASS'
        ;;
    *) exit 2 ;;
esac
