#!/bin/sh
# Fixtures for mesa-m3-probe's platform reporting (any GPU, no M3 needed):
#   tests/test-probe.sh PROBE
# 1. no WAYLAND_DISPLAY, 2. an unreachable one: gl.platform=surfaceless, the
#    reason, and a failed result naming the surfaceless fallback;
# 3. a headless weston: gl.platform=wayland with a window surface, a checked
#    render and a frame handed to the compositor, and no surfaceless reason.
set -u
PROBE=$1
T=$(mktemp -d); chmod 700 "$T"
trap 'kill "$wpid" 2>/dev/null; rm -rf "$T"' EXIT
wpid=
fail=0
expect() { # NAME TEXT PATTERN...
	_n=$1 _t=$2; shift 2
	for _p in "$@"; do
		case "$_p" in
		!*) if printf '%s\n' "$_t" | grep -q -- "${_p#!}"; then echo "FAIL  $_n: has $_p"; fail=1; return; fi ;;
		*) if ! printf '%s\n' "$_t" | grep -q -- "$_p"; then echo "FAIL  $_n: lacks $_p"; printf '%s\n' "$_t" | sed 's/^/      /'; fail=1; return; fi ;;
		esac
	done
	echo "ok    $_n"
}
out=$(env -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$T" "$PROBE" 2>&1)
expect "no WAYLAND_DISPLAY" "$out" '^gl.platform=surfaceless$' '^gl.wayland=fail WAYLAND_DISPLAY is not set$' \
	'^gl.surface=offscreen' '^result=fail.*not a Wayland client (surfaceless fallback, diagnostic only)' '^exit=1$'
out=$(XDG_RUNTIME_DIR="$T" WAYLAND_DISPLAY=no-such-display "$PROBE" 2>&1)
expect "unreachable compositor" "$out" '^gl.platform=surfaceless$' '^gl.wayland=fail cannot connect to the compositor$' \
	'^result=fail.*not a Wayland client'
if command -v weston >/dev/null; then
	XDG_RUNTIME_DIR="$T" weston --backend=headless --renderer=gl --socket=probe-test --idle-time=0 \
		> "$T/weston.log" 2>&1 &
	wpid=$!
	for _ in $(seq 1 50); do [ -S "$T/probe-test" ] && break; sleep 0.1; done
	out=$(XDG_RUNTIME_DIR="$T" WAYLAND_DISPLAY=probe-test "$PROBE" --out "$T/report.txt" 2>&1)
	expect "headless weston" "$out" '^gl.platform=wayland$' '^gl.surface=wayland-window 64x64$' \
		'^gl.render=pass' '^gl.swap=pass$' '!not a Wayland client' '!^gl.wayland=fail'
	[ "$(tail -n 1 "$T/report.txt" 2>/dev/null)" = "$(printf '%s\n' "$out" | tail -n 1)" ] &&
		echo "ok    --out report complete" || { echo "FAIL  --out report"; fail=1; }
else
	echo "skip  headless weston (weston not installed)"
fi
[ "$fail" = 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit "$fail"
