#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Physical J813 check. Save stdout and the payload SHA-256 manifest together.
# Usage: sh j813-peripherals.sh [event-capture-seconds]
set -eu

duration=${1:-30}
case "$duration" in *[!0-9]*|'') exit 2;; esac
[ "$duration" -ge 1 ] && [ "$duration" -le 600 ] || exit 2
[ -r /proc/device-tree/compatible ] || exit 4
tr '\000' '\n' < /proc/device-tree/compatible | grep -qx apple,j813 || exit 4
[ "$(id -u)" -eq 0 ] || { echo 'SKIP: run as root'; exit 4; }

work=$(mktemp -d)
led=/sys/class/leds/kbd_backlight
saved_brightness=
cleanup()
{
	if [ -n "$saved_brightness" ]; then
		echo "$saved_brightness" > "$led/brightness"
	fi
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

echo J813_PERIPHERALS_BEGIN
uname -r
zcat /proc/config.gz | sha256sum
cat /sys/devices/system/cpu/online

hwmon=
for h in /sys/class/hwmon/hwmon*; do
	[ -r "$h/name" ] || continue
	[ "$(cat "$h/name")" = macsmc_hwmon ] || continue
	hwmon=$h
done
[ -n "$hwmon" ] || { echo 'FAIL: no SMC hwmon'; exit 1; }
for sample in 1 2; do
	count=0
	for input in "$hwmon"/temp*_input; do
		[ -r "$input" ] || continue
		value=$(cat "$input")
		label=$(cat "${input%_input}_label")
		[ "$value" -ge -40000 ] && [ "$value" -le 125000 ] || exit 1
		printf 'temperature sample=%s key=%s millidegrees=%s\n' "$sample" "$label" "$value"
		count=$((count + 1))
	done
	[ "$count" -gt 0 ] || { echo 'FAIL: no temperature channels'; exit 1; }
	sleep 1
done
echo "PASS: $count readable SMC temperature channels"

[ -r "$led/brightness" ] || { echo 'FAIL: no keyboard backlight'; exit 1; }
saved_brightness=$(cat "$led/brightness")
maximum=$(cat "$led/max_brightness")
for level in 0 $((maximum / 2)) "$maximum"; do
	echo "$level" > "$led/brightness"
	[ "$(cat "$led/brightness")" = "$level" ] || exit 1
	printf 'backlight requested=%s readback=%s\n' "$level" "$(cat "$led/brightness")"
	sleep 5
done
echo "$saved_brightness" > "$led/brightness"
echo 'PASS: backlight control and restore (visual confirmation is separate)'

keyboard=
trackpad=
for e in /sys/class/input/event*; do
	[ -r "$e/device/name" ] || continue
	case "$(cat "$e/device/name")" in
		'Apple MTP keyboard') keyboard=/dev/input/${e##*/} ;;
		'Apple MTP multi-touch') trackpad=/dev/input/${e##*/} ;;
	esac
done
[ -n "$keyboard" ] && [ -n "$trackpad" ] || {
	echo 'FAIL: keyboard or trackpad missing'; exit 1;
}
echo "Move/click the built-in trackpad and press/release keys for $duration seconds."
# Count bytes only. Do not retain key codes, pointer coordinates, or raw events.
timeout "$duration" cat "$keyboard" | wc -c > "$work/keyboard" &
kp=$!
timeout "$duration" cat "$trackpad" | wc -c > "$work/trackpad" &
tp=$!
wait "$kp"
wait "$tp"
failed=0
for name in keyboard trackpad; do
	bytes=$(cat "$work/$name")
	[ "$bytes" -gt 0 ] && [ $((bytes % 24)) -eq 0 ] || {
		echo "FAIL: $name produced no complete arm64 input events"
		failed=1
		continue
	}
	printf 'PASS: %s events=%s\n' "$name" "$((bytes / 24))"
done
dmesg > "$work/dmesg"
fault_pattern='Kernel panic|Oops:|(^|[[:space:]])SError([[:space:]:]|$)'
fault_pattern="$fault_pattern|(^|[[:space:]])BUG:|apple-dart .*:.*(fault|error)"
if grep -E "$fault_pattern" "$work/dmesg"; then
	echo 'FAIL: kernel fault detected'
	exit 1
fi
[ "$failed" -eq 0 ] || exit 1
echo J813_PERIPHERALS_PASS
