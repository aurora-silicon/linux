#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Unattended suspend-to-idle regression test for Apple silicon (J700 and others).
#
# Suspends with an RTC alarm (apple-pmu-rtc by default) several times and
# checks after every resume that:
#  - the time actually spent suspended (CLOCK_BOOTTIME minus CLOCK_MONOTONIC)
#    lasted until the alarm: no spurious wakeup, e.g. from a running xHC,
#  - suspend_stats reports no new failures,
#  - every USB device present before is back,
#  - the power/wakeup settings of USB devices, xHCI and DWC3 are unchanged,
#  - the kernel log has no xHCI resume errors, warnings or oopses.
#
# Usage (as root):
#   s2idle-rtc-regression.sh [-n] [-c cycles] [-s seconds] [-r rtc] [-o new-dir] [-i ignore-regex]
# -i takes an extended regex of known, unrelated log lines to leave out of the
# error check, e.g. -i dcp for a display driver failing its own resume.
# -n only checks the arguments and prints what would run.
# The kernel log is never cleared: it is saved before the first cycle, and each
# cycle checks what was logged since the previous one.
# Exit status: 0 when every requested cycle ran and passed, 1 otherwise, 2 for
# a usage or setup error.

set -u

# The PMU RTC, which can wake the system: rtc0 on J700, rtc1 on others.
pmu_rtc() {
	local name
	for name in /sys/class/rtc/rtc*/name; do
		[[ $(cat "$name" 2> /dev/null) == apple-pmu-rtc* ]] || continue
		name=${name%/name}
		echo "${name##*/}"
		return
	done
	echo rtc0
}

cycles=3
secs=20
rtc=$(pmu_rtc)
out=
ignore=
dry_run=0

usage() {
	echo "usage: $0 [-n] [-c cycles] [-s secs] [-r rtc] [-o outdir] [-i regex]" >&2
	exit 2
}

while getopts "nc:s:r:o:i:" opt; do
	case $opt in
	n) dry_run=1 ;;
	c) cycles=$OPTARG ;;
	s) secs=$OPTARG ;;
	r) rtc=$OPTARG ;;
	o) out=$OPTARG ;;
	i) ignore=$OPTARG ;;
	*) usage ;;
	esac
done
[ $OPTIND -gt $# ] || usage

# A count that runs no cycle must not report a pass.
[[ $cycles =~ ^[1-9][0-9]{0,3}$ ]] || { echo "cycles must be 1-9999: $cycles" >&2; usage; }
[[ $secs =~ ^[1-9][0-9]{0,4}$ ]] && [ "$secs" -ge 5 ] ||
	{ echo "seconds must be 5-99999: $secs" >&2; usage; }
# An invalid expression would make the filter fail and hide every error.
if [ -n "$ignore" ]; then
	grep -Eq -- "$ignore" < /dev/null
	[ $? -le 1 ] || { echo "invalid -i expression: $ignore" >&2; usage; }
fi

# The evidence goes to a directory this script creates for itself, with private
# permissions: a fresh one under /var/tmp by default, or the absolute path given
# with -o, which must not exist yet. It never writes through an existing path,
# so a planted symlink cannot redirect what root writes.
if [ -n "$out" ]; then
	[[ $out == /* ]] || { echo "the output directory must be an absolute path" >&2; usage; }
	[ ! -e "$out" ] && [ ! -L "$out" ] ||
		{ echo "the output directory must not exist yet: $out" >&2; usage; }
fi

if [ $dry_run = 1 ]; then
	echo "would run $cycles cycles of $secs s on $rtc into ${out:-a new /var/tmp directory}"
	exit 0
fi

[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 2; }
command -v rtcwake > /dev/null || { echo "rtcwake not found" >&2; exit 2; }
command -v python3 > /dev/null || { echo "python3 not found" >&2; exit 2; }
[ -e "/sys/class/rtc/$rtc/wakealarm" ] || { echo "$rtc has no wakealarm" >&2; exit 2; }
grep -qw s2idle /sys/power/mem_sleep 2> /dev/null || grep -qw freeze /sys/power/state ||
	{ echo "suspend-to-idle not available" >&2; exit 2; }

if [ -n "$out" ]; then
	mkdir -m 700 -- "$out" || { echo "cannot create $out" >&2; exit 2; }
else
	out=$(mktemp -d /var/tmp/s2idle-rtc-regression.XXXXXXXX) ||
		{ echo "cannot create an output directory" >&2; exit 2; }
fi

# Errors that must never appear in the log of a suspend/resume cycle.
bad='Host halt failed|xHC restore state timeout|xHC error in resume'
bad+='|Controller not ready at resume|HC died|Clearing Run/Stop bit failed'
bad+='|Controller Save State failed|PM: Some devices failed'
bad+='|PM: .*failed to suspend|PM: .*failed to resume|Freezing .* failed'
bad+='|unable to enumerate USB device|WARNING:|BUG:|Oops|Call trace:'

usb_devices() {
	local d
	for d in /sys/bus/usb/devices/*; do
		[ -f "$d/idVendor" ] || continue
		echo "$(basename "$d") $(cat "$d/idVendor"):$(cat "$d/idProduct")"
	done | sort
}

wakeup_state() {
	local d
	for d in /sys/bus/usb/devices/* /sys/bus/platform/devices/{*.usb,xhci-hcd.*}; do
		[ -f "$d/power/wakeup" ] && echo "$(basename "$d") $(cat "$d/power/wakeup")"
	done | sort
}

# The lines that differ between two files, on one line
changes() {
	diff "$1" "$2" | grep '^[<>]' | tr '\n' ' '
}

# Drop the log lines the caller declared as known and unrelated
known_filter() {
	if [ -n "$ignore" ]; then grep -Ev "$ignore"; else cat; fi
}

# The suspend failure count, or "unavailable": a missing count is not zero.
stat_fail() {
	local n

	n=$(cat /sys/power/suspend_stats/fail 2> /dev/null)
	[[ $n =~ ^[0-9]+$ ]] && echo "$n" || echo unavailable
}

# CLOCK_BOOTTIME and CLOCK_MONOTONIC: only the first one runs while suspended.
clocks() {
	python3 -c 'import time; print(time.clock_gettime(time.CLOCK_BOOTTIME),
			time.clock_gettime(time.CLOCK_MONOTONIC))'
}

# --- kernel log collection (also run by test-s2idle-rtc-regression.sh) ---
# Save the kernel log since $mark (a line count) into $1 without clearing it, and
# move $mark past exactly what was saved, from the same read, so that nothing
# logged in between is skipped. A log that wrapped meanwhile no longer starts
# with $log_first: then all of it is saved. A failed read fails.
log_take() {
	dmesg > "$out/.dmesg" || return 1
	if [ "$(head -n 1 "$out/.dmesg")" = "$log_first" ]; then
		tail -n +"$((mark + 1))" "$out/.dmesg" > "$1" || return 1
	else
		{ echo "(kernel log wrapped; the whole log follows)"; cat "$out/.dmesg"; } > "$1" ||
			return 1
	fi
	mark=$(wc -l < "$out/.dmesg")
}
# --- end of kernel log collection ---

usb_devices > "$out/usb-before.txt"
wakeup_state > "$out/wakeup-before.txt"
uname -a > "$out/uname.txt"
echo "cycles=$cycles secs=$secs rtc=$rtc ignore=$ignore" > "$out/params.txt"
dmesg > "$out/dmesg-before.txt" || { echo "cannot read the kernel log" >&2; exit 2; }
log_first=$(head -n 1 "$out/dmesg-before.txt")
mark=$(wc -l < "$out/dmesg-before.txt")

failed=0
completed=0
for i in $(seq 1 "$cycles"); do
	c="$out/cycle-$i"
	mkdir -p "$c"
	reasons=()
	fail_before=$(stat_fail)
	sync

	read -r b0 m0 < <(clocks)
	rtcwake -d "$rtc" -m freeze -s "$secs" > "$c/rtcwake.txt" 2>&1
	rc=$?
	read -r b1 m1 < <(clocks)
	# Time spent suspended, and the whole command with entry and resume
	slept=$(awk -v b="$b1" -v a="$b0" -v n="$m1" -v m="$m0" \
		'BEGIN { printf "%.1f", (b - a) - (n - m) }')
	took=$(awk -v b="$b1" -v a="$b0" 'BEGIN { printf "%.1f", b - a }')

	[ $rc = 0 ] || reasons+=("rtcwake exited $rc")
	# The alarm counts from before suspend entry: allow 3 s for entry.
	if awk -v s="$slept" -v w="$secs" 'BEGIN { exit !(s + 3 < w) }'; then
		wirq=$(cat /sys/power/pm_wakeup_irq 2>&1)
		reasons+=("woke early after ${slept}s of ${secs}s (pm_wakeup_irq $wirq)")
	fi
	fail_after=$(stat_fail)
	if [ "$fail_before" = unavailable ] || [ "$fail_after" = unavailable ]; then
		reasons+=("suspend_stats fail count unavailable")
	elif [ "$fail_after" != "$fail_before" ]; then
		reasons+=("suspend_stats fail count rose")
	fi

	# Give USB up to 15 s to come back.
	for _ in $(seq 1 30); do
		usb_devices > "$c/usb-after.txt"
		cmp -s "$out/usb-before.txt" "$c/usb-after.txt" && break
		sleep 0.5
	done
	cmp -s "$out/usb-before.txt" "$c/usb-after.txt" ||
		reasons+=("USB devices differ: $(changes "$out/usb-before.txt" \
			"$c/usb-after.txt")")

	wakeup_state > "$c/wakeup-after.txt"
	cmp -s "$out/wakeup-before.txt" "$c/wakeup-after.txt" ||
		reasons+=("power/wakeup changed: $(changes "$out/wakeup-before.txt" \
			"$c/wakeup-after.txt")")

	log_take "$c/dmesg.txt" || reasons+=("kernel log unavailable")
	grep -E "$bad" "$c/dmesg.txt" | known_filter > "$c/bad-lines.txt"
	[ -s "$c/bad-lines.txt" ] &&
		reasons+=("kernel log errors: $(head -3 "$c/bad-lines.txt" | tr '\n' ' ')")

	times="suspended ${slept}s, ${took}s in all"
	if [ ${#reasons[@]} = 0 ]; then
		echo "cycle $i: PASS ($times)" | tee -a "$out/summary.txt"
	else
		failed=1
		echo "cycle $i: FAIL ($times)" | tee -a "$out/summary.txt"
		printf '  - %s\n' "${reasons[@]}" | tee -a "$out/summary.txt"
	fi
	completed=$((completed + 1))
done

# Whatever was logged after the last check
if ! log_take "$out/dmesg-after.txt"; then
	failed=1
	echo "kernel log unavailable after the last cycle" | tee -a "$out/summary.txt"
elif grep -E "$bad" "$out/dmesg-after.txt" | known_filter | grep -q .; then
	failed=1
	echo "kernel log errors after the last cycle, see dmesg-after.txt" |
		tee -a "$out/summary.txt"
fi
rm -f "$out/.dmesg"

if [ "$completed" != "$cycles" ]; then
	failed=1
	echo "only $completed of $cycles cycles ran" | tee -a "$out/summary.txt"
fi

if [ $failed = 0 ]; then
	echo "RESULT: PASS ($cycles cycles) evidence: $out" | tee -a "$out/summary.txt"
else
	echo "RESULT: FAIL evidence: $out" | tee -a "$out/summary.txt"
fi
exit $failed
