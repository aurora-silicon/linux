#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Cost of Bluetooth wake in suspend-to-idle on the J700, as root: alternates
# RTC-timed sleeps with the Bluetooth PCI function's wakeup disabled and
# enabled (no Bluetooth activity expected) and records, per sleep:
#  - Bluetooth MSIs and all interrupts taken across the sleep,
#  - how often the s2idle loop was re-entered (pm_debug "Timekeeping suspended"),
#  - the battery energy used, when running on battery (energy_now).
# Usage: bt-wake-power.sh [-s secs] [-n pairs] [-o outdir]
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
secs=300
pairs=2
out=/var/tmp/bt-wake/power-$(date +%Y%m%d-%H%M%S)
while getopts "s:n:o:" opt; do
	case $opt in s) secs=$OPTARG ;; n) pairs=$OPTARG ;; o) out=$OPTARG ;; *) exit 2 ;; esac
done
[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 2; }
dev=/sys/bus/pci/devices/0000:01:00.1
bat=/sys/class/power_supply/macsmc-battery
irq=$(awk -F: '/mt7932-bt/ { gsub(/ /, "", $1); print $1; exit }' /proc/interrupts)
rtc=$(pmu_rtc)
mkdir -p "$out"
orig=$(cat "$dev/power/wakeup")
trap 'echo "$orig" > "$dev/power/wakeup"' EXIT
echo 1 > /sys/power/pm_debug_messages
echo 'module mt7932_bt_pcie +p' > /sys/kernel/debug/dynamic_debug/control 2> /dev/null

irq_sum() { # $1: irq number or "all"
	awk -v i="$1" '$1 ~ /^[0-9]+:$/ && (i == "all" || $1 == i ":") {
		for (k = 2; k <= NF; k++) if ($k ~ /^[0-9]+$/) s += $k; else break
	} END { print s + 0 }' /proc/interrupts
}

echo "mode secs slept bt_msis all_irqs s2idle_entries energy_mWh status armed" |
	tee "$out/results.txt"
for p in $(seq 1 "$pairs"); do
	for mode in disabled enabled; do
		echo "$mode" > "$dev/power/wakeup"
		dmesg -C
		b0=$(irq_sum "$irq")
		a0=$(irq_sum all)
		e0=$(cat "$bat/energy_now")
		st=$(cat "$bat/status")
		t0=$(date +%s.%N)
		rtcwake -d "$rtc" -m freeze -s "$secs" > /dev/null 2>&1
		t1=$(date +%s.%N)
		b1=$(irq_sum "$irq"); a1=$(irq_sum all); e1=$(cat "$bat/energy_now")
		dmesg > "$out/dmesg-$p-$mode.txt"
		entries=$(grep -c "Timekeeping suspended" "$out/dmesg-$p-$mode.txt")
		armed=$(grep -c "system sleep with wakeup armed" "$out/dmesg-$p-$mode.txt")
		slept=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }')
		echo "$mode $secs $slept $((b1 - b0)) $((a1 - a0)) $entries" \
			"$(((e0 - e1) / 1000)) $st $armed" | tee -a "$out/results.txt"
		sleep 20
	done
done
echo "evidence: $out"
