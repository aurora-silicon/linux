#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Run inside an already authorized test session. Does not mount debugfs.
set -eu
capacity_file=${1:-/sys/kernel/debug/aurora-g17/capacity}
interval=${2:-5}
case "$interval" in ''|*[!0-9]*|0) echo 'interval must be positive integer seconds' >&2; exit 2;; esac
test -r "$capacity_file"
while :; do
    printf 'sample_begin utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    cat "$capacity_file"
    printf 'sample_end\n'
    sleep "$interval"
done
