#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Cost of Bluetooth wake in suspend-to-idle on the J700, as root: alternates
# RTC-timed sleeps with the Bluetooth PCI function's wakeup disabled and
# enabled (no Bluetooth activity expected) and records, per sleep:
#  - Bluetooth MSIs and all interrupts taken across the sleep,
#  - how often the s2idle loop was re-entered (pm_debug "Timekeeping suspended"),
#  - the battery energy used, when running on battery (energy_now).
# Usage: bt-wake-power.sh [-s secs] [-n pairs] [-o new-dir]
#   -o  a new directory for the evidence (default: a fresh
#       /var/tmp/bt-wake-power.* directory)
set -u

# --- private output directory
# The evidence goes to a directory this script creates for itself, mode 0700:
# a fresh one under $2 by default, or the absolute path $1 given with -o, which
# must not exist yet, below parents that only root, this user or the sticky bit
# let others change. The script then works inside it with noclobber set, so a
# planted or swapped path cannot redirect what it writes as root.
open_out() { # $1: -o path or empty, $2: base directory, $3: name prefix
	local leaf parent p owner mode
	if [ -z "$1" ]; then
		out=$(mktemp -d "$2/$3.XXXXXXXX") ||
			{ echo "cannot create an output directory in $2" >&2; return 1; }
	else
		[[ $1 == /* ]] ||
			{ echo "the output directory must be an absolute path: $1" >&2; return 1; }
		leaf=$(basename -- "$1")
		case $leaf in . | .. | /) echo "not a new directory: $1" >&2; return 1 ;; esac
		parent=$(readlink -e -- "$(dirname -- "$1")") ||
			{ echo "the parent of $1 does not exist" >&2; return 1; }
		p=$parent
		while :; do
			read -r owner mode < <(stat -c '%u %a' -- "$p") || return 1
			if { [ "$owner" != 0 ] && [ "$owner" != "$(id -u)" ]; } ||
				{ ((8#$mode & 8#022)) && ! ((8#$mode & 8#1000)); }; then
				echo "others can change $p; not writing below it" >&2
				return 1
			fi
			[ "$p" = / ] && break
			p=$(dirname -- "$p")
		done
		out=${parent%/}/$leaf
		mkdir -m 700 -- "$out" 2> /dev/null ||
			{ echo "the output directory must not exist yet: $1" >&2; return 1; }
	fi
	cd -- "$out" || return 1
	[ "$(stat -c '%u %a' .)" = "$(id -u) 700" ] ||
		{ echo "$out is not a private directory" >&2; return 1; }
	set -C
}
# --- end of private output directory
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
out=
while getopts "s:n:o:" opt; do
	case $opt in s) secs=$OPTARG ;; n) pairs=$OPTARG ;; o) out=$OPTARG ;; *) exit 2 ;; esac
done
[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 2; }
dev=/sys/bus/pci/devices/0000:01:00.1
bat=/sys/class/power_supply/macsmc-battery
irq=$(awk -F: '/mt7932-bt/ { gsub(/ /, "", $1); print $1; exit }' /proc/interrupts)
rtc=$(pmu_rtc)
open_out "$out" /var/tmp bt-wake-power || exit 2
orig=$(cat "$dev/power/wakeup")
# Kernel attributes are the only files written with >|, past noclobber.
trap 'echo "$orig" >| "$dev/power/wakeup"' EXIT
echo 1 >| /sys/power/pm_debug_messages
echo 'module mt7932_bt_pcie +p' >| /sys/kernel/debug/dynamic_debug/control 2> /dev/null

irq_sum() { # $1: irq number or "all"
	awk -v i="$1" '$1 ~ /^[0-9]+:$/ && (i == "all" || $1 == i ":") {
		for (k = 2; k <= NF; k++) if ($k ~ /^[0-9]+$/) s += $k; else break
	} END { print s + 0 }' /proc/interrupts
}

echo "mode secs slept bt_msis all_irqs s2idle_entries energy_mWh status armed" > results.txt
cat results.txt
for p in $(seq 1 "$pairs"); do
	for mode in disabled enabled; do
		echo "$mode" >| "$dev/power/wakeup"
		dmesg -C
		b0=$(irq_sum "$irq")
		a0=$(irq_sum all)
		e0=$(cat "$bat/energy_now")
		st=$(cat "$bat/status")
		t0=$(date +%s.%N)
		rtcwake -d "$rtc" -m freeze -s "$secs" > /dev/null 2>&1
		t1=$(date +%s.%N)
		b1=$(irq_sum "$irq"); a1=$(irq_sum all); e1=$(cat "$bat/energy_now")
		dmesg > "dmesg-$p-$mode.txt"
		entries=$(grep -c "Timekeeping suspended" "dmesg-$p-$mode.txt")
		armed=$(grep -c "system sleep with wakeup armed" "dmesg-$p-$mode.txt")
		slept=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }')
		echo "$mode $secs $slept $((b1 - b0)) $((a1 - a0)) $entries" \
			"$(((e0 - e1) / 1000)) $st $armed" | tee -a results.txt
		sleep 20
	done
done
echo "evidence: $out"
