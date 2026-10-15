#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Unattended suspend-to-idle regression test for Apple silicon (J700 and others).
#
# Suspends with an RTC alarm (rtc1, apple-pmu-rtc, by default) several times and
# checks after every resume that:
#  - the sleep lasted until the alarm (no spurious wakeup, e.g. from a running xHC),
#  - suspend_stats reports no new failures,
#  - every USB device present before is back,
#  - the power/wakeup settings of USB devices, xHCI and DWC3 are unchanged,
#  - the kernel log has no xHCI resume errors, warnings or oopses.
#
# Usage (as root):
#   s2idle-rtc-regression.sh [-c cycles] [-s seconds] [-r rtc] [-o outdir] [-i ignore-regex]
# -i takes an extended regex of known, unrelated log lines to leave out of the
# error check, e.g. -i dcp for a display driver failing its own resume.
# Exit status: 0 when every cycle passed, 1 otherwise, 2 for a setup error.

set -u

cycles=3
secs=20
rtc=rtc1
out=/var/tmp/s2idle-rtc-regression-$(date +%Y%m%d-%H%M%S)
ignore=

while getopts "c:s:r:o:i:" opt; do
	case $opt in
	c) cycles=$OPTARG ;;
	s) secs=$OPTARG ;;
	r) rtc=$OPTARG ;;
	o) out=$OPTARG ;;
	i) ignore=$OPTARG ;;
	*) echo "usage: $0 [-c cycles] [-s secs] [-r rtc] [-o outdir] [-i regex]" >&2; exit 2 ;;
	esac
done

[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 2; }
command -v rtcwake > /dev/null || { echo "rtcwake not found" >&2; exit 2; }
[ -e "/sys/class/rtc/$rtc/wakealarm" ] || { echo "$rtc has no wakealarm" >&2; exit 2; }
grep -qw s2idle /sys/power/mem_sleep 2> /dev/null || grep -qw freeze /sys/power/state ||
	{ echo "suspend-to-idle not available" >&2; exit 2; }

mkdir -p "$out"

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

stat_fail() {
	cat /sys/power/suspend_stats/fail 2> /dev/null || echo 0
}

usb_devices > "$out/usb-before.txt"
wakeup_state > "$out/wakeup-before.txt"
uname -a > "$out/uname.txt"
echo "cycles=$cycles secs=$secs rtc=$rtc ignore=$ignore" > "$out/params.txt"

failed=0
for i in $(seq 1 "$cycles"); do
	c="$out/cycle-$i"
	mkdir -p "$c"
	reasons=()
	fail_before=$(stat_fail)
	dmesg -C
	sync

	t0=$(date +%s.%N)
	rtcwake -d "$rtc" -m freeze -s "$secs" > "$c/rtcwake.txt" 2>&1
	rc=$?
	t1=$(date +%s.%N)
	slept=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }')

	[ $rc = 0 ] || reasons+=("rtcwake exited $rc")
	if awk -v s="$slept" -v w="$secs" 'BEGIN { exit !(s + 1.5 < w) }'; then
		wirq=$(cat /sys/power/pm_wakeup_irq 2>&1)
		reasons+=("woke early after ${slept}s of ${secs}s (pm_wakeup_irq $wirq)")
	fi
	[ "$(stat_fail)" = "$fail_before" ] || reasons+=("suspend_stats fail count rose")

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

	dmesg > "$c/dmesg.txt"
	grep -E "$bad" "$c/dmesg.txt" | known_filter > "$c/bad-lines.txt"
	[ -s "$c/bad-lines.txt" ] &&
		reasons+=("kernel log errors: $(head -3 "$c/bad-lines.txt" | tr '\n' ' ')")

	if [ ${#reasons[@]} = 0 ]; then
		echo "cycle $i: PASS (slept ${slept}s)" | tee -a "$out/summary.txt"
	else
		failed=1
		echo "cycle $i: FAIL (slept ${slept}s)" | tee -a "$out/summary.txt"
		printf '  - %s\n' "${reasons[@]}" | tee -a "$out/summary.txt"
	fi
done

if [ $failed = 0 ]; then
	echo "RESULT: PASS ($cycles cycles) evidence: $out" | tee -a "$out/summary.txt"
else
	echo "RESULT: FAIL evidence: $out" | tee -a "$out/summary.txt"
fi
exit $failed
