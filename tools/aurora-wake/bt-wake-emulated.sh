#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Unattended Bluetooth wake test with an emulated BLE HID keyboard
# (hog-keyboard.py) on a second BlueZ host. Runs on a third machine that can
# reach both over SSH.
#
#   bt-wake-emulated.sh pair      pair the emulator with the target
#   bt-wake-emulated.sh run [N]   N wake-enabled cycles (default 5), then two
#                                 wake-disabled cycles
#   bt-wake-emulated.sh unpair    remove that pairing on both sides
#
# The lab is described by a local configuration file that is not part of the
# tree, ${BT_WAKE_CONF:-~/.config/aurora/bt-wake.conf}, a shell fragment with:
#   TARGET_SSH   ssh destination (an ssh_config alias) of the system under test
#   EMU_SSH      ssh destination of the BlueZ host running the emulator
#   EMU_ADDR     Bluetooth address of the emulator host's adapter
#   TARGET_ADDR  Bluetooth address of the target's adapter
#   EMU_TOOLS    directory holding hog-keyboard.py on the emulator host
#   TARGET_SSH_ARGS  optional extra ssh/scp options for the target, e.g. -i <key>
# Only the emulator is ever paired or removed; the paired lists of both sides
# are recorded before and after.
set -u

conf=${BT_WAKE_CONF:-$HOME/.config/aurora/bt-wake.conf}
[ -r "$conf" ] || { echo "no lab configuration: $conf" >&2; exit 2; }
# shellcheck source=/dev/null
. "$conf"
mac='^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$'
for v in TARGET_SSH EMU_SSH EMU_ADDR TARGET_ADDR EMU_TOOLS; do
	[ -n "${!v:-}" ] || { echo "$conf does not set $v" >&2; exit 2; }
done
[[ $EMU_ADDR =~ $mac && $TARGET_ADDR =~ $mac ]] ||
	{ echo "EMU_ADDR and TARGET_ADDR must be Bluetooth addresses" >&2; exit 2; }

ssh_opts=(-o ConnectTimeout=5 -o ServerAliveInterval=5 -o ServerAliveCountMax=2)
read -r -a target_args <<< "${TARGET_SSH_ARGS:-}"
tgt() { ssh "${ssh_opts[@]}" "${target_args[@]}" "$TARGET_SSH" "$@"; }
box() { ssh "${ssh_opts[@]}" "$EMU_SSH" "$@"; }
CTL=/run/hogkb/ctl
TGT_DIR=/var/tmp/bt-wake
BACKSTOP=120
TRIGGER_AFTER=20
out=${OUT:-./bt-wake-run-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$out"

say() { echo "$(date +%H:%M:%S) $*" | tee -a "$out/run.log"; }
emu() { box "echo '$*' | sudo -n tee $CTL > /dev/null"; }

emu_start() {
	box "pgrep -f '[h]og-keyboard.py' > /dev/null || {
		sudo -n mkdir -p /run/hogkb
		sudo -n setsid nohup $EMU_TOOLS/hog-keyboard.py > /dev/null 2>&1 < /dev/null &
		sleep 3
	}; sudo -n tail -2 /run/hogkb/log"
}

paired_lists() {
	tgt "bluetoothctl devices Paired" > "$out/target-paired-$1.txt"
	box "bluetoothctl devices Paired" > "$out/emulator-paired-$1.txt"
	say "paired ($1): target $(wc -l < "$out/target-paired-$1.txt")," \
		"emulator host $(wc -l < "$out/emulator-paired-$1.txt")"
}

case "${1:-}" in
pair)
	paired_lists before-pair
	emu_start
	emu pairable on
	sleep 2
	# Target side: discover, pair (Just Works), trust, allow wake.
	tgt "{ echo 'agent NoInputNoOutput'; echo default-agent; echo 'scan le'; sleep 12;
		echo 'scan off'; echo 'pair $EMU_ADDR'; sleep 15; echo 'trust $EMU_ADDR';
		sleep 1; echo 'connect $EMU_ADDR'; sleep 8; echo 'wake $EMU_ADDR on'; sleep 1;
		echo 'info $EMU_ADDR'; sleep 1; } | bluetoothctl" > "$out/pair-target.txt" 2>&1
	emu pairable off
	grep -E "Paired|Bonded|Trusted|Connected|WakeAllowed|UUID: Human" \
		"$out/pair-target.txt" | tail -8 | tee -a "$out/run.log"
	paired_lists after-pair
	;;
run)
	n=${2:-5}
	emu_start
	tgt "mkdir -p $TGT_DIR" &&
		scp -q "${target_args[@]}" "$(dirname "$0")/bt-wake-cycle.sh" \
			"$TARGET_SSH:$TGT_DIR/"
	pass=0
	plan=()
	for _ in $(seq 1 "$n"); do plan+=(on); done
	plan+=(off off)
	k=0
	for mode in "${plan[@]}"; do
		k=$((k + 1))
		d=$TGT_DIR/cycle-$k-$mode
		# Start each cycle with the keyboard connected, as in real use.
		emu key 0x2c 30
		sleep 8
		say "cycle $k wake=$mode: suspending the target (backstop ${BACKSTOP}s)"
		tgt "rm -rf $d; setsid nohup $TGT_DIR/bt-wake-cycle.sh -w $mode -s $BACKSTOP \
			-a $EMU_ADDR -o $d > $d.log 2>&1 < /dev/null &"
		for _ in $(seq 1 30); do
			tgt "test -f $d/suspending" 2> /dev/null && break
			sleep 1
		done
		sleep "$TRIGGER_AFTER"
		say "cycle $k: emulated key press"
		# Keep advertising past the backstop, so that the keyboard also
		# comes back after an RTC wake.
		emu key 0x2c $((BACKSTOP + 60))
		for _ in $(seq 1 $((BACKSTOP + 120))); do
			tgt "test -f $d/verdict.txt" 2> /dev/null && break
			sleep 1
		done
		tgt "cat $d/verdict.txt $d/after.txt" > "$out/cycle-$k-$mode.txt" 2>&1
		head -4 "$out/cycle-$k-$mode.txt" | tee -a "$out/run.log"
		grep -q "PASS" "$out/cycle-$k-$mode.txt" && pass=$((pass + 1))
		box "sudo -n tail -8 /run/hogkb/log" >> "$out/emulator.log"
		sleep 10
	done
	say "RESULT: $pass of ${#plan[@]} cycles passed (evidence $out, target $TGT_DIR)"
	tgt "tar -C $TGT_DIR -czf - ." > "$out/target-evidence.tgz"
	[ "$pass" = "${#plan[@]}" ]
	;;
unpair)
	paired_lists before-unpair
	if grep -qi "$EMU_ADDR" "$out/target-paired-before-unpair.txt"; then
		tgt "bluetoothctl remove $EMU_ADDR" | tee -a "$out/run.log"
	fi
	box "bluetoothctl devices Paired | grep -qi $TARGET_ADDR &&
		sudo -n bluetoothctl remove $TARGET_ADDR" | tee -a "$out/run.log"
	emu quit
	paired_lists after-unpair
	;;
*)
	echo "usage: $0 pair | run [cycles] | unpair" >&2
	exit 2
	;;
esac
