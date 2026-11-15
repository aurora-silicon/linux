#!/bin/sh
# The hook through uwsm 0.27.0's own environment preloader
# (fixtures/uwsm-0.27.0-prepare-env.sh, sha256 24caf3e6…, the m3pro's file),
# against a fake T6030 root. Checks what uwsm would export.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REC=$HERE/..
PE=$HERE/fixtures/uwsm-0.27.0-prepare-env.sh
fail=0
UID_=$(id -u)

# case NAME "USER_ENV" "EXPECT_LINES": run the preloader; compare exports + decision.
# UW_DESK (default Hyprland) are the session's desktop names; UW_FILE and
# UW_TEXT add one more file under the home (relative path, its text).
UW_DESK=Hyprland UW_FILE= UW_TEXT=
case_() {
	_name=$1 _uenv=$2 _want=$3
	T=$(mktemp -d)
	mkdir -p "$T/bin"
	printf '#!/bin/sh\necho "Booting Linux"\necho "mmc0: tuning execution failed: -110"\n' > "$T/bin/journalctl"
	printf '#!/bin/sh\n:\n' > "$T/bin/logger"
	printf '#!/bin/sh\necho 7\n' > "$T/bin/loginctl"
	printf '#!/bin/sh\necho 0123\n' > "$T/bin/systemctl"
	chmod +x "$T/bin/"*
	r="$T/root"; HD="$T/home"
	mkdir -p "$r/proc/device-tree" "$r/proc/sys/kernel/random" "$r/sys/class/drm" "$r/sys/bus/platform/drivers/asahi" \
		"$r/dev/dri" "$r/etc" "$r/usr/lib" "$r/run/user/$UID_" "$HD/.config"
	echo boot-a > "$r/proc/sys/kernel/random/boot_id"
	echo "root=UUID=x rw quiet" > "$r/proc/cmdline"
	printf '%s\0' apple,j516s apple,t6030 apple,arm-platform > "$r/proc/device-tree/compatible"
	d="$r/sys/devices/platform/gpu0"; mkdir -p "$d/of_node"
	printf '%s\0' apple,agx-t6030 apple,agx-g15x apple,agx-g15s > "$d/of_node/compatible"
	ln -s "$r/sys/bus/platform/drivers/asahi" "$d/driver"; mkdir -p "$r/sys/class/drm/renderD128"
	ln -s "$d" "$r/sys/class/drm/renderD128/device"; : > "$r/dev/dri/renderD128"; chmod 600 "$r/dev/dri/renderD128"
	d="$r/sys/devices/platform/display"; mkdir -p "$d/of_node"; printf '%s\0' apple,t6030-display-subsystem > "$d/of_node/compatible"
	mkdir -p "$r/sys/class/drm/card2"; ln -s "$d" "$r/sys/class/drm/card2/device"
	p="$r/opt/mesa-m3"; mkdir -p "$p/share/vulkan/icd.d" "$p/share/glvnd/egl_vendor.d" "$p/share/drirc.d" "$p/lib/gbm" \
		"$p/lib/dri" "$p/share/mesa-m3" "$p/libexec"
	for f in share/vulkan/icd.d/asahi_icd.aarch64.json share/glvnd/egl_vendor.d/50_mesa.json share/drirc.d/10-m3-asahi-zink.conf \
		lib/libvulkan_asahi.so lib/libEGL_mesa.so.0 lib/libgbm.so.1 lib/gbm/dri_gbm.so lib/dri/libdril_dri.so \
		lib/libgallium-26.1.4.so; do : > "$p/$f"; done
	ln -s libdril_dri.so "$p/lib/dri/zink_dri.so"
	printf '#!/bin/sh\n[ "${FAKE_ABI:-ok}" = ok ]\n' > "$p/libexec/mesa-m3-abi-check"
	chmod +x "$p/libexec/mesa-m3-abi-check"
	printf 'libdrm.so.2\n' > "$p/share/mesa-m3/needed-sonames"; : > "$r/usr/lib/libdrm.so.2"
	echo "26.1.4.m3.1-3" > "$p/share/mesa-m3/package-version"
	printf '#!/bin/sh\nexit 0\n' > "$p/libexec/mesa-m3-loadcheck"; chmod +x "$p/libexec/mesa-m3-loadcheck"
	printf '#!/bin/sh\n[ "${FAKE_ABI:-ok}" = ok ]\n' > "$p/libexec/mesa-m3-abi-check"
	chmod +x "$p/libexec/mesa-m3-abi-check"
	cp "$REC/mesa-m3-user-setup" "$p/libexec/"; cp "$REC/user-setup.list" "$p/share/mesa-m3/"
	cp "$REC/mesa-m3-session-confirm" "$p/libexec/"; : > "$p/libexec/mesa-m3-wlcheck"
	# The installed hook, pointed at the fake root (the real 50-mesa-m3 with
	# its sh -n check, sourcing a copy of the gate that runs against $r).
	{ echo 'MESA_M3_SESSION_ENV_NO_RUN=1 . '"$REC"'/mesa-m3-session-env'; echo 'unset MESA_M3_SESSION_ENV_NO_RUN'
	  echo "mesa_m3_session_env $r"; echo 'unset -f mesa_m3_gate mesa_m3_session_env _mm3_init _mm3_state _mm3_node_open _mm3_attempt'
	} > "$T/gate-in-root"
	mkdir -p "$T/share/uwsm/env.d"
	sed "s|/opt/mesa-m3/libexec/mesa-m3-session-env|$T/gate-in-root|g" "$REC/50-mesa-m3" > "$T/share/uwsm/env.d/50-mesa-m3"
	if [ -n "$_uenv" ]; then mkdir -p "$HD/.config/uwsm" "$r$HD/.config/uwsm"; printf '%s\n' "$_uenv" > "$HD/.config/uwsm/env"
		cp "$HD/.config/uwsm/env" "$r$HD/.config/uwsm/env"; fi
	if [ -n "$UW_FILE" ]; then
		_t=$(printf '%s\n' "$UW_TEXT" | sed "s|@HOME@|$HD|g")
		mkdir -p "$(dirname "$HD/$UW_FILE")" "$(dirname "$r$HD/$UW_FILE")"
		printf '%s\n' "$_t" > "$HD/$UW_FILE"; printf '%s\n' "$_t" > "$r$HD/$UW_FILE"
		mkdir -p "$r$HD/src/m3-gpu-work/mesa-eryk-prefix-20261001/lib"; : > "$r$HD/src/m3-gpu-work/mesa-eryk-prefix-20261001/lib/libgallium-26.1.4.so"
	fi
	MARK=feedfacefeedface
	printf '__SELF_NAME__=uwsm\n__WM_ID__=x\n__WM_ID_UNIT_STRING__=x\n__WM_BIN_ID__=x\n__WM_DESKTOP_NAMES__=%s\n__WM_FIRST_DESKTOP_NAME__=%s\n__WM_DESKTOP_NAMES_EXCLUSIVE__=false\n__LOAD_PROFILE__=false\n__OIFS__=" \t\n"\n__RANDOM_MARK__=%s\nIN_UWSM_ENV_PRELOADER=true\n' "$UW_DESK" "${UW_DESK%%:*}" $MARK > "$T/vars"
	env -i PATH="$T/bin:/usr/bin:/bin" HOME="$HD" XDG_CONFIG_HOME="$HD/.config" XDG_CONFIG_DIRS="$T/etcxdg" \
		XDG_DATA_DIRS="$T/share" XDG_RUNTIME_DIR="$r/run/user/$UID_" sh "$PE" "$T/vars" > "$T/out" 2> "$T/err"
	_rc=$?
	_got=$(printf 'rc=%s\n' "$_rc"; sed -n "s/.*$MARK//p" "$T/out" | tr '\0' '\n' |
		grep -E '^(LD_LIBRARY_PATH|GBM_BACKENDS_PATH|LIBGL_ALWAYS_SOFTWARE|MESA_M3_SESSION|MESA_M3_FALLBACK)=' | sed "s|$HD|~|g" | sort
		sed -n 's/^\(decision\|reason\|fallback\)=/\1=/p' "$r/run/user/$UID_/mesa-m3-session.state" 2>/dev/null)
	if [ "$_got" = "$_want" ]; then echo "ok    $_name"; else
		echo "FAIL  $_name"; printf '%s\n' "$_got" | sed 's/^/      got  /'; printf '%s\n' "$_want" | sed 's/^/      want /'; fail=1; fi
	rm -rf "$T"
}

case_ "active through uwsm" "" "rc=0
GBM_BACKENDS_PATH=/opt/mesa-m3/lib/gbm
LD_LIBRARY_PATH=/opt/mesa-m3/lib
MESA_M3_SESSION=/opt/mesa-m3
decision=active
reason=active
fallback=none"
case_ "a later env file replacing LD_LIBRARY_PATH (review F2)" "export LD_LIBRARY_PATH=/usr/local/lib" "rc=0
LD_LIBRARY_PATH=/usr/local/lib
LIBGL_ALWAYS_SOFTWARE=1
MESA_M3_FALLBACK=software
decision=inactive
reason=user-setup-ldpath
fallback=software"
case_ "a later env file adding to LD_LIBRARY_PATH" 'export LD_LIBRARY_PATH="/usr/local/lib:$LD_LIBRARY_PATH"' "rc=0
GBM_BACKENDS_PATH=/opt/mesa-m3/lib/gbm
LD_LIBRARY_PATH=/usr/local/lib:/opt/mesa-m3/lib
MESA_M3_SESSION=/opt/mesa-m3
decision=active
reason=active
fallback=none"
# Round 4: only a setting the session uses counts.
UW_FILE=.config/chonkstep/m3gpu-session.env UW_TEXT='CHONKSTEP_M3_MESA_PREFIX=@HOME@/src/m3-gpu-work/mesa-eryk-prefix-20261001'
case_ "Hyprland, only chonkstep's file names a private prefix: active" "" "rc=0
GBM_BACKENDS_PATH=/opt/mesa-m3/lib/gbm
LD_LIBRARY_PATH=/opt/mesa-m3/lib
MESA_M3_SESSION=/opt/mesa-m3
decision=active
reason=active
fallback=none"
UW_DESK=chonkstep
case_ "chonkstep, the same file: the hook yields (chonkstep's launcher sets its prefix)" "" "rc=0
decision=inactive
reason=user-setup
fallback=none"
UW_DESK=Hyprland UW_FILE=.config/uwsm/env-hyprland UW_TEXT='export LD_LIBRARY_PATH="@HOME@/src/m3-gpu-work/mesa-eryk-prefix-20261001/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GBM_BACKENDS_PATH=@HOME@/src/m3-gpu-work/mesa-eryk-prefix-20261001/lib/gbm'
case_ "Hyprland, uwsm env-hyprland chooses a private Mesa: the hook yields, Hyprland gets that Mesa" "" "rc=0
GBM_BACKENDS_PATH=~/src/m3-gpu-work/mesa-eryk-prefix-20261001/lib/gbm
LD_LIBRARY_PATH=~/src/m3-gpu-work/mesa-eryk-prefix-20261001/lib
decision=inactive
reason=user-setup
fallback=none"
UW_DESK=sway
case_ "an unknown desktop (sway), the same env-hyprland: not read by uwsm, active" "" "rc=0
GBM_BACKENDS_PATH=/opt/mesa-m3/lib/gbm
LD_LIBRARY_PATH=/opt/mesa-m3/lib
MESA_M3_SESSION=/opt/mesa-m3
decision=active
reason=active
fallback=none"
UW_DESK=Hyprland UW_FILE= UW_TEXT=
[ "$fail" = 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit "$fail"
