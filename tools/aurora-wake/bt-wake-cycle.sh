#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# One suspend-to-idle cycle for the MT7932 Bluetooth wake test (J700), as root.
#
# Sets power/wakeup of the Bluetooth PCI function, suspends with an RTC alarm as
# a backstop and records what ended the sleep. Something else (a person, or an
# emulated keyboard on another machine) is expected to make a wake-enabled
# Bluetooth HID device reconnect while the system sleeps.
#
# Usage: bt-wake-cycle.sh -w on|off [-s backstop-secs] [-r rtc] [-a bdaddr] [-o new-dir]
#   -w on   expect a Bluetooth wake well before the backstop
#   -w off  expect no wake: the RTC ends the sleep
#   -a      Bluetooth address that must be connected again after resume
#   -o      a new directory for the evidence (default: a fresh
#           /var/tmp/bt-wake-cycle.* directory)
# The file <outdir>/suspending appears right before the system suspends.
# Exit status: 0 PASS, 1 FAIL, 2 setup error.

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

mode=
secs=120
rtc=$(pmu_rtc)
addr=
out=
bdf=0000:01:00.1

while getopts "w:s:r:a:o:" opt; do
	case $opt in
	w) mode=$OPTARG ;;
	s) secs=$OPTARG ;;
	r) rtc=$OPTARG ;;
	a) addr=$OPTARG ;;
	o) out=$OPTARG ;;
	*) exit 2 ;;
	esac
done
case $mode in
on|off) ;;
*) echo "usage: $0 -w on|off [-s secs] [-r rtc] [-a bdaddr] [-o outdir]" >&2; exit 2 ;;
esac
[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 2; }
dev=/sys/bus/pci/devices/$bdf
[ -f "$dev/power/wakeup" ] || { echo "$bdf has no power/wakeup" >&2; exit 2; }

open_out "$out" /var/tmp bt-wake-cycle || exit 2
irq=$(awk -F: '/mt7932-bt/ { gsub(/ /, "", $1); print $1; exit }' /proc/interrupts)
[ -n "$irq" ] || { echo "no mt7932-bt interrupt" >&2; exit 2; }
# Recorded only: a PME would arrive on the root port's PME interrupt.
pme=$(awk -F: '/PCI-MSI-0000:00:00.0.*PME/ { gsub(/ /, "", $1); print $1; exit }' /proc/interrupts)

bt_irqs() {
	awk -v i="$irq" '$1 == i ":" {
		for (k = 2; k <= NF; k++) if ($k ~ /^[0-9]+$/) s += $k; else break
	} END { print s + 0 }' /proc/interrupts
}

orig_wakeup=$(cat "$dev/power/wakeup")
# Kernel attributes are the only files written with >|, past noclobber.
restore() { echo "$orig_wakeup" >| "$dev/power/wakeup"; }
trap restore EXIT
[ "$mode" = on ] && echo enabled >| "$dev/power/wakeup" || echo disabled >| "$dev/power/wakeup"

echo 'module mt7932_bt_pcie +p' >| /sys/kernel/debug/dynamic_debug/control 2> /dev/null
echo 1 >| /sys/power/pm_debug_messages

{
	echo "mode=$mode secs=$secs rtc=$rtc addr=$addr irq=$irq"
	uname -r
	echo "wakeup=$(cat "$dev/power/wakeup")" \
		"wakeup_count=$(cat "$dev/power/wakeup_count" 2>/dev/null)"
	echo "irq_wakeup=$(cat /sys/kernel/irq/"$irq"/wakeup 2>/dev/null)"
	echo "bt_irqs=$(bt_irqs)"
	bluetoothctl show | grep -E "Powered|PowerState"
	[ -n "$addr" ] && bluetoothctl info "$addr" | grep -E "Name|Connected|WakeAllowed"
} > before.txt 2>&1
fail_before=$(cat /sys/power/suspend_stats/fail)
dmesg -C
# HCI trace of the whole cycle: what the host sent at suspend, what woke it.
# btmon creates the capture itself, in this private directory.
btmon -w hci.btsnoop > /dev/null 2>&1 &
btmon_pid=$!
sleep 1
sync

date +%s.%N > suspending
t0=$(date +%s.%N)
rtcwake -d "$rtc" -m freeze -s "$secs" > rtcwake.txt 2>&1
rc=$?
t1=$(date +%s.%N)
slept=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }')
wirq=$(cat /sys/power/pm_wakeup_irq 2>&1)
irqs_after=$(bt_irqs)
wsrc=other
[ "$wirq" = "$irq" ] && wsrc=bt-msi
[ -n "$pme" ] && [ "$wirq" = "$pme" ] && wsrc=root-port-pme

# Bluetooth must come back: the controller up and, if given, the device connected.
reconnect=n/a
if [ -n "$addr" ]; then
	reconnect=timeout
	for i in $(seq 1 60); do
		if bluetoothctl info "$addr" 2> /dev/null | grep -q "Connected: yes"; then
			reconnect="${i}x0.5s"
			break
		fi
		sleep 0.5
	done
fi
powered=$(bluetoothctl show | awk '/Powered:/ { print $2 }')
kill "$btmon_pid" 2> /dev/null

dmesg > dmesg.txt
{
	echo "rc=$rc slept=${slept}s pm_wakeup_irq=$wirq ($wsrc) bt_irq=$irq pme_irq=$pme"
	echo "bt_irqs_after=$irqs_after wakeup_count=$(cat "$dev/power/wakeup_count" 2>/dev/null)"
	echo "irq_wakeup_after=$(cat /sys/kernel/irq/"$irq"/wakeup 2>/dev/null)"
	echo "reconnect=$reconnect powered=$powered"
	grep -E "mt7932|Bluetooth|hci0" dmesg.txt | grep -vE "Timekeeping|s2idle" | tail -20
} > after.txt

reasons=()
[ $rc = 0 ] || reasons+=("rtcwake exited $rc")
[ "$(cat /sys/power/suspend_stats/fail)" = "$fail_before" ] ||
	reasons+=("suspend_stats fail count rose")
grep -q "system sleep with wakeup armed" dmesg.txt
armed=$?
if [ "$mode" = on ]; then
	[ $armed = 0 ] || reasons+=("driver did not report wakeup armed")
	# The driver wakes through the function's MSI only; a root port PME is
	# recorded but is not the designed path.
	[ "$wsrc" = bt-msi ] ||
		reasons+=("pm_wakeup_irq $wirq ($wsrc) is not the Bluetooth MSI $irq")
	awk -v s="$slept" -v w="$secs" 'BEGIN { exit !(s < w - 10) }' ||
		reasons+=("slept ${slept}s of ${secs}s")
else
	[ $armed != 0 ] || reasons+=("wakeup armed although disabled")
	awk -v s="$slept" -v w="$secs" 'BEGIN { exit !(s + 1.5 >= w) }' ||
		reasons+=("woke early after ${slept}s (pm_wakeup_irq $wirq)")
fi
[ "$powered" = yes ] || reasons+=("controller not powered after resume")
[ "$reconnect" != timeout ] || reasons+=("$addr did not reconnect within 30 s")
bad='WARNING:|BUG:|Oops|Call trace:|terminal transport fault|failed to (suspend|resume)'
grep -E "$bad" dmesg.txt > bad-lines.txt
[ -s bad-lines.txt ] &&
	reasons+=("kernel log errors: $(head -2 bad-lines.txt | tr '\n' ' ')")

# The verdict file appears complete: bt-wake-emulated.sh waits for it.
{
	if [ ${#reasons[@]} = 0 ]; then
		echo "VERDICT wake=$mode: PASS slept=${slept}s pm_wakeup_irq=$wirq ($wsrc)" \
			"reconnect=$reconnect"
	else
		echo "VERDICT wake=$mode: FAIL slept=${slept}s pm_wakeup_irq=$wirq"
		printf '  - %s\n' "${reasons[@]}"
	fi
	echo "evidence: $out"
} > verdict.txt
cat verdict.txt
[ ${#reasons[@]} = 0 ] && status=0 || status=1
exit $status
