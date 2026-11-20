#!/usr/bin/env bash
# kiwi replay harness: build and run against a given driver file.
# usage: run-kiwi-replay.sh /path/to/hid-apple-mtp-kiwi.c [OUTDIR]
# Use CAPS to supply real captures. Without it, run synthetic captures only.
set -e
DRV=$1
OUT=${2:-/tmp/kiwi-replay}
H="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$H/replay.c" ]; then
	R=$H
else
	R=$H/replay
fi
if [ -f "$H/linux-shim.h" ]; then
	SHIM=$H
elif [ -f "$H/shim/linux-shim.h" ]; then
	SHIM=$H/shim
elif [ -f "$H/../shim/linux-shim.h" ]; then
	SHIM=$H/../shim
else
	echo "cannot locate linux-shim.h near $H" >&2
	exit 2
fi
CAPS=${CAPS:-$H/../kiwi-caps}
[ -d "$CAPS" ] || CAPS=$H/../../kiwi-caps
mkdir -p "$OUT"
python3 "$R/kiwi-synth.py" "$OUT/stuckedge.frames" stuckedge >/dev/null
python3 "$R/kiwi-synth.py" "$OUT/idlecycle.frames" idle >/dev/null
python3 "$R/kiwi-synth.py" "$OUT/tap.frames" tap >/dev/null
python3 "$R/kiwi-synth.py" "$OUT/tapedge.frames" tapedge >/dev/null
python3 "$R/kiwi-synth.py" "$OUT/stuckmid.frames" stuckmid >/dev/null
python3 "$R/kiwi-synth.py" "$OUT/flat2s.frames" flat2s >/dev/null
python3 "$R/kiwi-synth.py" "$OUT/bootfinger.frames" bootfinger >/dev/null
python3 "$R/kiwi-synth.py" "$OUT/breathelong.frames" breathelong >/dev/null
python3 "$R/kiwi-synth.py" "$OUT/breatheretap.frames" breatheretap >/dev/null
python3 "$R/kiwi-synth.py" "$OUT/holdrebirth.frames" holdrebirth >/dev/null
python3 "$R/kiwi-synth.py" "$OUT/lifteretap.frames" lifteretap >/dev/null
python3 "$R/kiwi-synth.py" "$OUT/bootfinger2.frames" bootfinger2 >/dev/null
python3 "$R/kiwi-synth.py" "$OUT/bootfinger4.frames" bootfinger4 >/dev/null
python3 "$R/kiwi-synth.py" "$OUT/bootfinger3.frames" bootfinger3 >/dev/null
python3 "$R/kiwi-synth.py" "$OUT/wander.frames" wander >/dev/null
python3 "$R/kiwi-synth.py" "$OUT/presshold.frames" presshold >/dev/null
cc -O2 -I"$SHIM" -include linux-shim.h -DKIWI_DRV="\"$DRV\"" "$R/replay.c" -o "$OUT/kiwi-replay"
echo "== driver: $DRV"
if [ -d "$CAPS" ]; then
	echo "== real captures (regression: no phantom ever; the real finger stays detected)"
	for cap in "$CAPS"/kiwi-full.frames "$CAPS"/kiwi-watch3.frames; do
		"$OUT/kiwi-replay" --expect strict --phases 240 "$cap" || true
	done
	"$OUT/kiwi-replay" --expect touch --phases 240 "$CAPS"/kiwi-watch2-final.frames || true
else
	echo "== real captures skipped (no $CAPS)"
fi
echo "== synthetic idle (must report nothing)"
"$OUT/kiwi-replay" --expect none --phases 60 --loop 40 "$OUT/idlecycle.frames" || true
echo "== synthetic stuck top-edge cells (the 16:12 phantom: RED until hardened)"
"$OUT/kiwi-replay" --expect strict --phases 60 --loop 40 "$OUT/stuckedge.frames" || true
echo "== synthetic stuck centre cells away from the edge (hold rule: RED until hardened)"
"$OUT/kiwi-replay" --expect none --phases 60 --loop 40 "$OUT/stuckmid.frames" || true
echo "== synthetic finger on the glass at boot (must be reported, not learned into the baseline)"
"$OUT/kiwi-replay" --expect touch --phases 1 --loop 1 "$OUT/bootfinger.frames" || true
echo "== synthetic finger on the glass at boot, lift, idle, then two taps (lifted finger leaves no ghost)"
"$OUT/kiwi-replay" --expect touch --phases 1 --loop 1 "$OUT/bootfinger2.frames" || true
echo "== synthetic flat blob on the boot-finger rows after the lift (the scar test: it must be seen)"
"$OUT/kiwi-replay" --expect touch --phases 1 --loop 1 "$OUT/bootfinger4.frames" || true
echo "== synthetic taps after a boot finger on the same rows (no scar: full-length taps, silent idle)"
"$OUT/kiwi-replay" --expect taps --taps 410:590,600:624,648:672 --tap-min 15 --phases 1 --loop 1 "$OUT/bootfinger3.frames" || true
echo "== synthetic stationary breather (the hold rule must drop it exactly once; a still-present blob must not re-arm)"
"$OUT/kiwi-replay" --expect one --phases 1 --loop 1 "$OUT/breathelong.frames" || true
echo "== synthetic always-lit breather (hold drops exactly 583 and 594; zero births after the drop)"
"$OUT/kiwi-replay" --expect reborn:583,594 --phases 1 --loop 1 "$OUT/holdrebirth.frames" || true
echo "== synthetic lift, full quiet, same-spot touch again (the lift must free the cells)"
"$OUT/kiwi-replay" --expect reborn:189,1074 --phases 1 --loop 1 "$OUT/lifteretap.frames" || true
echo "== synthetic breather that lifts and taps again (after a real lift the tap must register)"
"$OUT/kiwi-replay" --expect bound --phases 1 --loop 1 "$OUT/breatheretap.frames" || true
echo "== synthetic wandering breathing blob (a live touch by contract: it must stay reported)"
"$OUT/kiwi-replay" --expect touch --phases 60 --loop 40 "$OUT/wander.frames" || true
echo "== synthetic flat 2 s centre blob (the menu-open shape: RED until the static rule)"
"$OUT/kiwi-replay" --expect flat --phases 60 --loop 40 "$OUT/flat2s.frames" || true
echo "== synthetic press after a long touch (a fresh press must inherit nothing: RED until the carry gap check)"
"$OUT/kiwi-replay" --expect press --phases 1 --loop 1 "$OUT/presshold.frames" || true
echo "== synthetic finger taps (must stay detected and not over-hold)"
"$OUT/kiwi-replay" --expect touch --phases 60 --loop 5 "$OUT/tap.frames" || true
"$OUT/kiwi-replay" --expect touch --phases 60 --loop 5 "$OUT/tapedge.frames" || true
echo "== done (exit reflects failures; each block above is judged by its own PASS/FAIL lines)"
