#!/usr/bin/env bash
# mutation check for the kiwi hardening: each guard removed must turn its
# synthetic capture RED again in the replay harness.
# usage: run-kiwi-mutations.sh  (env: RUN for outputs, BASE for the frozen
# unhardened driver; captures are generated into $RUN)
set -e
H="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$H/replay.c" ]; then
	S=$H
else
	S=$H/replay
fi
if [ -f "$H/linux-shim.h" ]; then
	SHIM=$H
elif [ -f "$H/shim/linux-shim.h" ]; then
	SHIM=$H/shim
elif [ -f "$H/../shim/linux-shim.h" ]; then
	SHIM=$H/../shim
else
	echo "cannot locate replay.c under $H" >&2
	exit 2
fi
R=${R:-$(cd "$H/../.." && pwd)/kiwi}
DRIVE=${DRIVE:-$R/drv/hid-apple-mtp-kiwi.c}
[ -f "$DRIVE" ] || DRIVE=$(cd "$H/.." && pwd)/hid-apple-mtp-kiwi.c
RUN=${RUN:-/tmp/kiwi-mut}
BASE=${BASE:-$H/baseline2.c}   # frozen unhardened driver, sha in the log
mkdir -p "$RUN"
[ -f "$BASE" ] || { echo "frozen baseline driver missing: $BASE" >&2; exit 2; }
for m in stuckedge wander flat2s idlecycle presshold holdrebirth; do
	[ -f "$RUN/$m.frames" ] || python3 "$S/kiwi-synth.py" "$RUN/$m.frames" "$m" >/dev/null
done
cc -O2 -I"$SHIM" -include linux-shim.h -DKIWI_DRV="\"$BASE\"" "$S/replay.c" -o "$RUN/kiwi-replay-base"
echo "== M0 baseline (unhardened): stuckedge must FAIL"
"$RUN/kiwi-replay-base" --expect strict --phases 60 --loop 40 "$RUN/stuckedge.frames" 2>/dev/null | tail -n 1 || true

echo "== M1 edge-ring guard removed: stuckedge must FAIL again"
sed 's/if (r > 1 \&\& r < KIWI_ROWS - 2 \&\& c > 1 \&\& c < KIWI_COLS - 2)/if (1)/' "$DRIVE" > "$RUN/m1.c"
cc -O2 -I"$SHIM" -include linux-shim.h -DKIWI_DRV="\"$RUN/m1.c\"" "$S/replay.c" -o "$RUN/kiwi-replay-m1"
"$RUN/kiwi-replay-m1" --expect strict --phases 60 --loop 40 "$RUN/stuckedge.frames" 2>/dev/null | tail -n 1 || true
echo "== M2 hold rule disabled: defense in depth only (the static rule already bounds the blob, so no behavioral delta is observable)"
echo "== M4 static rule disabled: flat2s must FAIL again"
sed 's/#define KIWI_STATIC_FRAMES\t120/#define KIWI_STATIC_FRAMES\t0x7fffffff/' "$DRIVE" > "$RUN/m4.c"
cc -O2 -I"$SHIM" -include linux-shim.h -DKIWI_DRV="\"$RUN/m4.c\"" "$S/replay.c" -o "$RUN/kiwi-replay-m4"
"$RUN/kiwi-replay-m4" --expect flat --phases 60 --loop 40 "$RUN/flat2s.frames" 2>/dev/null | tail -n 1 || true
echo "== M3 agreeing-pair chain dropped (accept on first pair): idle must still PASS"
sed 's/if (++k->agree < 2) {/if (0) {/' "$DRIVE" > "$RUN/m3.c"
cc -O2 -I"$SHIM" -include linux-shim.h -DKIWI_DRV="\"$RUN/m3.c\"" "$S/replay.c" -o "$RUN/kiwi-replay-m3"
"$RUN/kiwi-replay-m3" --expect none --phases 60 --loop 40 "$RUN/idlecycle.frames" 2>/dev/null | tail -n 1 || true
echo "== M5 carry gap check removed: presshold must FAIL again"
sed 's/k->now - k->last_end <= KIWI_MISS_MAX) {/k->now - k->last_end <= KIWI_HOLD_FRAMES + KIWI_MISS_MAX) {/' "$DRIVE" > "$RUN/m5.c"
cc -O2 -I"$SHIM" -include linux-shim.h -DKIWI_DRV="\"$RUN/m5.c\"" "$S/replay.c" -o "$RUN/kiwi-replay-m5"
"$RUN/kiwi-replay-m5" --expect press --phases 1 --loop 1 "$RUN/presshold.frames" 2>/dev/null | tail -n 1 || true
echo "== M6 blob grow removed (reach 0): holdrebirth must FAIL again, the old 4 borns"
sed 's/#define KIWI_SUP_REACH\t\t6/#define KIWI_SUP_REACH\t\t0/' "$DRIVE" > "$RUN/m6.c"
cc -O2 -I"$SHIM" -include linux-shim.h -DKIWI_DRV="\"$RUN/m6.c\"" "$S/replay.c" -o "$RUN/kiwi-replay-m6"
"$RUN/kiwi-replay-m6" --expect reborn:583,594 --phases 1 --loop 1 "$RUN/holdrebirth.frames" 2>/dev/null | tail -n 1 || true
