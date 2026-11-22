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
# Usage: bt-wake-cycle.sh -w on|off [-s backstop-secs] [-r rtc] [-a bdaddr] [-o outdir]
#   -w on   expect a Bluetooth wake well before the backstop
#   -w off  expect no wake: the RTC ends the sleep
#   -a      Bluetooth address that must be connected again after resume
# The file <outdir>/suspending appears right before the system suspends.
# Exit status: 0 PASS, 1 FAIL, 2 setup error.

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

mode=
secs=120
rtc=$(pmu_rtc)
addr=
out=/var/tmp/bt-wake/cycle-$(date +%Y%m%d-%H%M%S)
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

mkdir -p "$out"
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
restore() { echo "$orig_wakeup" > "$dev/power/wakeup"; }
trap restore EXIT
[ "$mode" = on ] && echo enabled > "$dev/power/wakeup" || echo disabled > "$dev/power/wakeup"

echo 'module mt7932_bt_pcie +p' > /sys/kernel/debug/dynamic_debug/control 2> /dev/null
echo 1 > /sys/power/pm_debug_messages

{
	echo "mode=$mode secs=$secs rtc=$rtc addr=$addr irq=$irq"
	uname -r
	echo "wakeup=$(cat "$dev/power/wakeup")" \
		"wakeup_count=$(cat "$dev/power/wakeup_count" 2>/dev/null)"
	echo "irq_wakeup=$(cat /sys/kernel/irq/"$irq"/wakeup 2>/dev/null)"
	echo "bt_irqs=$(bt_irqs)"
	bluetoothctl show | grep -E "Powered|PowerState"
	[ -n "$addr" ] && bluetoothctl info "$addr" | grep -E "Name|Connected|WakeAllowed"
} > "$out/before.txt" 2>&1
fail_before=$(cat /sys/power/suspend_stats/fail)
dmesg -C
# HCI trace of the whole cycle: what the host sent at suspend, what woke it.
btmon -w "$out/hci.btsnoop" > /dev/null 2>&1 &
btmon_pid=$!
sleep 1
sync

date +%s.%N > "$out/suspending"
t0=$(date +%s.%N)
rtcwake -d "$rtc" -m freeze -s "$secs" > "$out/rtcwake.txt" 2>&1
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

dmesg > "$out/dmesg.txt"
{
	echo "rc=$rc slept=${slept}s pm_wakeup_irq=$wirq ($wsrc) bt_irq=$irq pme_irq=$pme"
	echo "bt_irqs_after=$irqs_after wakeup_count=$(cat "$dev/power/wakeup_count" 2>/dev/null)"
	echo "irq_wakeup_after=$(cat /sys/kernel/irq/"$irq"/wakeup 2>/dev/null)"
	echo "reconnect=$reconnect powered=$powered"
	grep -E "mt7932|Bluetooth|hci0" "$out/dmesg.txt" | grep -vE "Timekeeping|s2idle" | tail -20
} > "$out/after.txt"

reasons=()
[ $rc = 0 ] || reasons+=("rtcwake exited $rc")
[ "$(cat /sys/power/suspend_stats/fail)" = "$fail_before" ] ||
	reasons+=("suspend_stats fail count rose")
grep -q "system sleep with wakeup armed" "$out/dmesg.txt"
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
grep -E "$bad" "$out/dmesg.txt" > "$out/bad-lines.txt"
[ -s "$out/bad-lines.txt" ] &&
	reasons+=("kernel log errors: $(head -2 "$out/bad-lines.txt" | tr '\n' ' ')")

if [ ${#reasons[@]} = 0 ]; then
	echo "VERDICT wake=$mode: PASS slept=${slept}s pm_wakeup_irq=$wirq ($wsrc)" \
		"reconnect=$reconnect" | tee "$out/verdict.txt"
	status=0
else
	echo "VERDICT wake=$mode: FAIL slept=${slept}s pm_wakeup_irq=$wirq" | tee "$out/verdict.txt"
	printf '  - %s\n' "${reasons[@]}" | tee -a "$out/verdict.txt"
	status=1
fi
echo "evidence: $out" | tee -a "$out/verdict.txt"
exit $status
