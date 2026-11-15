#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Attended Bluetooth keyboard wake check (J700), about two minutes, as root.
#
# Usage: attended-bt-wake.sh <bdaddr>
#   <bdaddr> is a paired keyboard with WakeAllowed set (bluetoothctl wake <bdaddr> on).
#
# 1. Press a key on the keyboard so that it connects; the script waits for it.
# 2. At SUSPENDING, wait for the screen to go dark, count to ten, and press one
#    key on the keyboard. The RTC ends the sleep after 60 s otherwise.
set -u

addr=${1:-}
[[ $addr =~ ^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$ ]] ||
	{ echo "usage: $0 <bdaddr>" >&2; exit 2; }
here=$(dirname "$(readlink -f "$0")")
out=/var/tmp/bt-wake/attended-$(date +%H%M%S)
mkdir -p "$out"

bluetoothctl info "$addr" > "$out/device.txt" 2>&1 ||
	{ echo "$addr is not known to BlueZ" >&2; exit 2; }
grep -E "Name|Paired|WakeAllowed|Connected" "$out/device.txt"
grep -q "WakeAllowed: yes" "$out/device.txt" ||
	{ echo "wake is not allowed: bluetoothctl wake $addr on" >&2; exit 2; }

echo ">>> Press a key on the Bluetooth keyboard so that it connects (60 s)."
for _ in $(seq 1 120); do
	bluetoothctl info "$addr" | grep -q "Connected: yes" && break
	sleep 0.5
done
bluetoothctl info "$addr" | grep -q "Connected: yes" ||
	{ echo "the keyboard did not connect" >&2; exit 2; }
echo "connected."

echo ">>> SUSPENDING. When the screen is dark, count to ten and press ONE key."
"$here/bt-wake-cycle.sh" -w on -s 60 -a "$addr" -o "$out/cycle"
rc=$?
if [ $rc = 0 ]; then
	echo "VERDICT: PASS (the Bluetooth keyboard woke the system)"
else
	echo "VERDICT: FAIL (see $out/cycle/verdict.txt)"
fi
exit $rc
