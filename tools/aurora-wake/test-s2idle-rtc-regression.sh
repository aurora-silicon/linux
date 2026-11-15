#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Hardware-free checks of the argument handling of s2idle-rtc-regression.sh: a
# count that would run no cycle must be refused, not reported as a pass, and the
# output must go to a fresh directory of its own.

script="$(dirname "$0")/s2idle-rtc-regression.sh"
fails=0

expect() {
	local want=$1 got
	shift
	bash "$script" "$@" > /dev/null 2>&1
	got=$?
	if [ "$got" = "$want" ]; then
		echo "ok: $* -> $got"
	else
		echo "FAIL: $* -> $got, expected $want"
		fails=1
	fi
}

expect 2 -n -c 0
expect 2 -n -c -1
expect 2 -n -c abc
expect 2 -n -c ''
expect 2 -n -s 0
expect 2 -n -s 2
expect 2 -n -s x
expect 2 -n extra-argument
expect 0 -n -c 3 -s 20
expect 0 -n
# An invalid filter would hide every kernel log error.
expect 2 -n -i '['
expect 0 -n -i 'dcp|DCP'

# Output only goes to a directory the script creates itself, never to an
# existing path that may be a symlink planted by someone else.
existing=$(mktemp -d)
ln -s "$existing" "$existing.link"
expect 2 -n -o "$existing"
expect 2 -n -o "$existing.link"
expect 2 -n -o relative-dir
expect 0 -n -o "$existing/new"
rm -rf "$existing" "$existing.link"

# Kernel log collection. A message logged right after a read must land in the
# next collection, and a failed read must fail, not pass as an empty log.
tmp=$(mktemp -d)
sed -n '/^# --- kernel log collection/,/^# --- end of kernel log collection/p' \
	"$script" > "$tmp/lib.sh"
fakelog=$tmp/kmsg
late=0
dmesg() {
	cat "$fakelog" || return 1
	late=$((late + 1))
	echo "WARNING: late $late" >> "$fakelog"
}
printf 'first line\nsecond line\n' > "$fakelog"
# shellcheck source=/dev/null
. "$tmp/lib.sh"
out=$tmp log_first='first line' mark=2
if log_take "$tmp/c1" && log_take "$tmp/c2" && grep -q 'late 1' "$tmp/c2"; then
	echo "ok: a message logged right after a read is in the next collection"
else
	echo "FAIL: a message logged between collections was skipped"
	fails=1
fi
fakelog=$tmp/missing
if log_take "$tmp/c3" 2> /dev/null; then
	echo "FAIL: a failed kernel log read passed"
	fails=1
else
	echo "ok: a failed kernel log read fails"
fi
rm -rf "$tmp"

exit $fails
