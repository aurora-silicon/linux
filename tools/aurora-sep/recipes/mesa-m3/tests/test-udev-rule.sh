#!/bin/sh
# The render-node rules: valid udev syntax, one rule each for the T6030 and
# T8122 GPUs' render nodes, and they only set group render, mode 0660.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
R=$HERE/../72-mesa-m3-render-node.rules
fail=0
if command -v udevadm >/dev/null; then
	udevadm verify --no-style "$R" >/dev/null 2>&1 && echo "ok    udevadm verify" || { echo "FAIL  udevadm verify"; fail=1; }
fi
rules=$(grep -v '^#' "$R" | grep -v '^$')
[ "$(printf '%s\n' "$rules" | wc -l)" = 2 ] && echo "ok    two rules" || { echo "FAIL  not two rules"; fail=1; }
for gpu in apple,agx-t6030 apple,agx-t8122; do
	want="SUBSYSTEM==\"drm\", KERNEL==\"renderD*\", DRIVERS==\"asahi\", ATTR{device/of_node/compatible}==\"$gpu\", GROUP=\"render\", MODE=\"0660\""
	printf '%s\n' "$rules" | grep -qxF "$want" && echo "ok    rule for $gpu" || { echo "FAIL  no exact rule for $gpu"; fail=1; }
done
printf '%s\n' "$rules" | grep -q -E 'OWNER|TAG|RUN|PROGRAM|IMPORT' && { echo "FAIL  sets more than group and mode"; fail=1; } ||
	echo "ok    no owner, tag, run, program or import"
[ "$fail" = 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit "$fail"
