#!/bin/sh
# Fixtures for mesa-m3-session-confirm (the live-compositor success marker):
#   tests/test-confirm.sh WLCHECK      (WLCHECK: a built mesa-m3-wlcheck)
# A stand-in compositor (fixtures/fake-compositor.py) listens on the session's
# Wayland socket with the environment a compositor of the login would have.
#   1. success: alive after the settle and answering: a marker bound to the
#      login (boot_id, session_id, hook_run_id, the compositor's pid);
#   2. the target is reached but the compositor dies during the settle: no marker;
#   3. the socket is there but nothing answers (no round trip): no marker;
#   4. a socket with no listener left: no marker;
#   5. a compositor of another login (hook_run_id or session id): no marker;
#   6. the unit runs in another login: nothing written;
#   7. a real compositor (headless weston), when installed: a marker.
# The gate's side (a stale marker from an earlier login does not count) is in
# test-session-env.sh.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CONFIRM="$HERE/../mesa-m3-session-confirm"
FAKE="$HERE/fixtures/fake-compositor.py"
WLCHECK=$1
T=$(mktemp -d); chmod 700 "$T"
cpid=
trap 'kill $cpid 2>/dev/null; rm -rf "$T"' EXIT
fail=0
mkdir -p "$T/root/opt/mesa-m3/libexec" "$T/root/proc/sys/kernel/random" "$T/run" "$T/bin" "$T/state/mesa-m3"
cp "$WLCHECK" "$T/root/opt/mesa-m3/libexec/mesa-m3-wlcheck"
echo boot-a > "$T/root/proc/sys/kernel/random/boot_id"
printf '#!/bin/sh\n[ "$1" = -t ] && shift 2\nprintf "%%s\\n" "$*" >> "%s/log"\n' "$T" > "$T/bin/logger"; chmod +x "$T/bin/logger"
RUN=11111111-2222-3333-4444-555555555555
ATT="$T/state/mesa-m3/attempt"
OK="$T/state/mesa-m3/session-ok"
attempt() {
	printf 'schema=aurora.mesa-m3-attempt/2\nboot_id=boot-a\nsession_id=5\nhook_run_id=%s\nwritten_at=2026-10-07T00:00:00Z\n' "$RUN" > "$ATT"
}
# compositor MODE [VAR=value...]: start the stand-in on $T/run/wayland-9
compositor() {
	_m=$1; shift
	kill $cpid 2>/dev/null; rm -f "$T/run/wayland-9"
	env -i PATH=/usr/bin:/bin "$@" python3 "$FAKE" "$T/run/wayland-9" "$_m" &
	cpid=$!
	for _ in $(seq 1 50); do [ -S "$T/run/wayland-9" ] && break; sleep 0.1; done
}
# confirm [VAR=value...]: run the unit's script as systemd would, settle 2 s
confirm() {
	: > "$T/log"
	env -i PATH="$T/bin:/usr/bin:/bin" XDG_RUNTIME_DIR="$T/run" XDG_STATE_HOME="$T/state" MESA_M3_ROOT="$T/root" WAYLAND_DISPLAY=wayland-9 \
		MESA_M3_HOOK_RUN_ID="$RUN" "$@" "$CONFIRM" --settle 2
}
result() { # NAME EXPECTED_RC RC MARKER(yes|no) LOG_PATTERN
	if [ "$3" = "$2" ] && { { [ "$4" = yes ] && [ -f "$OK" ]; } || { [ "$4" = no ] && [ ! -e "$OK" ]; }; } &&
		grep -q -- "$5" "$T/log"; then
		echo "ok    $1"
	else
		echo "FAIL  $1: rc=$3 marker=$([ -f "$OK" ] && echo yes || echo no) log: $(cat "$T/log")"; fail=1
	fi
}

# 1. success
attempt; rm -f "$OK"
compositor serve MESA_M3_HOOK_RUN_ID="$RUN" XDG_SESSION_ID=5
confirm; rc=$?
result "success: the compositor is alive after the settle and answers" 0 "$rc" yes "^confirmed: compositor .*\[$cpid\] alive after 2 s"
want=$(printf 'schema=aurora.mesa-m3-session-ok/1\nboot_id=boot-a\nsession_id=5\nhook_run_id=%s\ncompositor_pid=%s\n' "$RUN" "$cpid")
[ "$(head -n 5 "$OK" 2>/dev/null)" = "$want" ] && grep -qx "wayland_socket=$T/run/wayland-9" "$OK" &&
	grep -qx 'settle_s=2' "$OK" && grep -qE '^compositor_starttime=[0-9]+$' "$OK" &&
	[ "$(stat -c %a "$OK")" = 644 ] && [ -z "$(find "$T/state" -name '.session-ok.*')" ] &&
	echo "ok    success: the marker is bound to boot_id, session_id, hook_run_id and the compositor pid; no temporary file left" ||
	{ echo "FAIL  marker content: $(cat "$OK" 2>/dev/null)"; fail=1; }

# 2. the target is reached, then the compositor dies during the settle
attempt; rm -f "$OK"
compositor die-after=1 MESA_M3_HOOK_RUN_ID="$RUN" XDG_SESSION_ID=5
confirm; rc=$?
result "the compositor answers once, then dies within the settle: no marker" 1 "$rc" no "exited within 2 s"

# 3. the socket is there, nothing answers
attempt; rm -f "$OK"
compositor silent MESA_M3_HOOK_RUN_ID="$RUN" XDG_SESSION_ID=5
confirm; rc=$?
result "the socket is there but no round trip completes: no marker" 1 "$rc" no "no Wayland round trip on .*(fail timeout"

# 4. a socket left behind by a compositor that is gone
attempt; rm -f "$OK"
compositor serve MESA_M3_HOOK_RUN_ID="$RUN" XDG_SESSION_ID=5
kill $cpid; wait $cpid 2>/dev/null; cpid=
[ -S "$T/run/wayland-9" ] || { echo "FAIL  fixture: the socket file is gone"; fail=1; }
confirm; rc=$?
result "a socket with no listener: no marker" 1 "$rc" no "connect: Connection refused"

# 5. a compositor that is not this login's
attempt; rm -f "$OK"
compositor serve MESA_M3_HOOK_RUN_ID=99999999-2222-3333-4444-555555555555 XDG_SESSION_ID=5
confirm; rc=$?
result "a compositor with another hook_run_id: no marker" 1 "$rc" no "is not this login's (its MESA_M3_HOOK_RUN_ID differs)"
compositor serve MESA_M3_HOOK_RUN_ID="$RUN" XDG_SESSION_ID=6
confirm; rc=$?
result "a compositor of another logind session: no marker" 1 "$rc" no "its XDG_SESSION_ID is not 5"

# 6. the unit runs in another login (the user manager has another run id)
attempt; rm -f "$OK"
compositor serve MESA_M3_HOOK_RUN_ID="$RUN" XDG_SESSION_ID=5
confirm MESA_M3_HOOK_RUN_ID=99999999-2222-3333-4444-555555555555; rc=$?
result "the unit runs in another login: nothing written" 0 "$rc" no "is not the attempt's"
# An attempt of another boot: nothing to do.
sed -i 's/^boot_id=.*/boot_id=boot-z/' "$ATT"
confirm; rc=$?
[ "$rc" = 0 ] && [ ! -e "$OK" ] && echo "ok    an attempt of another boot: nothing written" || { echo "FAIL  other boot: rc=$rc"; fail=1; }
kill $cpid 2>/dev/null; cpid=

# 7. a real compositor
if command -v weston >/dev/null; then
	attempt; rm -f "$OK" "$T/run/wayland-9"
	env -i PATH=/usr/bin:/bin XDG_RUNTIME_DIR="$T/run" MESA_M3_HOOK_RUN_ID="$RUN" XDG_SESSION_ID=5 \
		weston --backend=headless --renderer=pixman --socket=wayland-9 --idle-time=0 > "$T/weston.log" 2>&1 &
	cpid=$!
	for _ in $(seq 1 50); do [ -S "$T/run/wayland-9" ] && break; sleep 0.1; done
	confirm; rc=$?
	result "headless weston: alive after the settle and answering: a marker" 0 "$rc" yes "^confirmed: compositor weston\[$cpid\]"
else
	echo "skip  headless weston (weston not installed)"
fi

[ "$fail" = 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit "$fail"
