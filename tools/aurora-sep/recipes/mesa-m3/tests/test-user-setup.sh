#!/bin/sh
# Fixtures for the user-setup detector: exact output and exit status per case.
#   tests/test-user-setup.sh [DETECTOR [LIST]]
# Defaults to the recipe's mesa-m3-user-setup and user-setup.list. Any other
# copy of the detector (for example the installer's built-in one) must pass the
# same fixtures with the same output. Needs a non-root user (unreadable files).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
DET=${1:-$HERE/../mesa-m3-user-setup}
LIST=${2:-$HERE/../user-setup.list}
T=$(mktemp -d)
trap 'chmod -R u+rwX "$T" 2>/dev/null; rm -rf "$T"' EXIT
fail=0
H=/home/u
if [ "$(id -u)" = 0 ]; then echo "run as a normal user"; exit 2; fi

# fresh NAME: an empty root with a home; prints the root.
fresh() { r="$T/$1"; mkdir -p "$r$H/.config"; echo "$r"; }
# put ROOT PATH TEXT: write a file under ROOT.
put() { mkdir -p "$(dirname "$1$2")"; printf '%s' "$3" > "$1$2"; }
mesa() { mkdir -p "$1$2"; : > "$1$2/libgallium-26.1.4.so"; }

# check NAME RC EXPECTED-LINES ROOT [detector options]: the verdict lines
# (schema, user_setup, paths, details) exactly, for a session with desktop
# names $SESS (default Hyprland). The per-finding scope lines are checked
# exactly by checkx below.
SESS=Hyprland
check() {
	_n=$1 _rc=$2 _want=$3 _r=$4
	shift 4
	_got=$(env -i PATH=/usr/bin:/bin "$DET" --home "$H" --list "$LIST" --root "$_r" --session-desktops "$SESS" "$@" 2>&1 |
		grep -v -e '^user_setup_finding=' -e '^session_desktops=')
	_grc=$?
	_grc=$(env -i PATH=/usr/bin:/bin "$DET" --home "$H" --list "$LIST" --root "$_r" --session-desktops "$SESS" "$@" >/dev/null 2>&1; echo $?)
	if [ "$_grc" = "$_rc" ] && [ "$_got" = "$_want" ]; then
		printf 'ok    %-34s rc=%s %s\n' "$_n" "$_grc" "$(printf '%s\n' "$_got" | sed -n 's/^user_setup=//p')"
	else
		printf 'FAIL  %s: rc=%s (want %s)\n--- got\n%s\n--- want\n%s\n' "$_n" "$_grc" "$_rc" "$_got" "$_want"
		fail=1
	fi
}
S=schema=aurora.mesa-m3-user-setup/3
NONE="$S
user_setup=none"

R=$(fresh empty)
check "nothing at all" 0 "$NONE" "$R"

# Dave's three: a comment naming /opt/mesa-m3 hides nothing; a list with one
# /opt/mesa-m3 entry hides nothing; an unreadable file is never "none".
R=$(fresh d1); put "$R" "$H/.config/uwsm/env" 'export LIBGL_ALWAYS_SOFTWARE=1 # /opt/mesa-m3/
'
check "d1 inline comment hides a flag" 1 "$S
user_setup=present
user_setup_path=$H/.config/uwsm/env
user_setup_detail=$H/.config/uwsm/env:1 sets LIBGL_ALWAYS_SOFTWARE" "$R"
R=$(fresh d2); put "$R" "$H/.config/uwsm/env" 'VK_DRIVER_FILES=/opt/mesa-m3/share/vulkan/icd.d/asahi_icd.aarch64.json:/opt/private/icd.json
'
check "d2 list with a private entry" 1 "$S
user_setup=present
user_setup_path=$H/.config/uwsm/env
user_setup_detail=$H/.config/uwsm/env:1 sets VK_DRIVER_FILES (/opt/private/icd.json)" "$R"
printf 'VK_DRIVER_FILES=/opt/mesa-m3/share/vulkan/icd.d/asahi_icd.aarch64.json:/opt/private/icd.json\0' > "$T/environ-d2"
check "d2 same, in the environment" 1 "$S
user_setup=present
user_setup_path=environment:VK_DRIVER_FILES
user_setup_detail=the environment sets VK_DRIVER_FILES (/opt/private/icd.json)" "$(fresh d2e)" --environ "$T/environ-d2"
R=$(fresh d3); put "$R" "$H/.config/uwsm/env" 'export VK_ICD_FILENAMES=/home/u/icd.json
'; chmod 000 "$R$H/.config/uwsm/env"
check "d3 unreadable env file" 3 "$S
user_setup=unknown
user_setup_path=$H/.config/uwsm/env
user_setup_detail=$H/.config/uwsm/env cannot be read (Permission denied)" "$R"

# Variants: comments, quoting, lists.
R=$(fresh v1); put "$R" "$H/.config/uwsm/env" 'export VK_ICD_FILENAMES=/opt/mesa-m3/share/vulkan/icd.d/asahi_icd.aarch64.json # VK_ICD_FILENAMES=/private
# LIBGL_DRIVERS_PATH=/usr/lib/dri
'
check "v1 ours, with a comment naming another" 0 "$NONE" "$R"
R=$(fresh v2); put "$R" "$H/.config/uwsm/env" 'export VK_ICD_FILENAMES="/opt/mesa-m3/a.json:/home/u/b.json"
'
check "v2 double-quoted list" 1 "$S
user_setup=present
user_setup_path=$H/.config/uwsm/env
user_setup_detail=$H/.config/uwsm/env:1 sets VK_ICD_FILENAMES (/home/u/b.json)" "$R"
R=$(fresh v3); put "$R" "$H/.config/environment.d/10-mesa.conf" "LIBGL_DRIVERS_PATH='/opt/mesa-m3/lib/dri:/usr/lib/dri'
"
check "v3 single-quoted list (environment.d)" 1 "$S
user_setup=present
user_setup_path=$H/.config/environment.d/10-mesa.conf
user_setup_detail=$H/.config/environment.d/10-mesa.conf:1 sets LIBGL_DRIVERS_PATH (/usr/lib/dri)" "$R"
R=$(fresh v4); put "$R" "$H/.config/uwsm/env" 'A=1; export B=2 GALLIUM_DRIVER=zink # x
'
check "v4 several assignments on one line" 1 "$S
user_setup=present
user_setup_path=$H/.config/uwsm/env
user_setup_detail=$H/.config/uwsm/env:1 sets GALLIUM_DRIVER" "$R"
R=$(fresh v5); put "$R" "$H/.config/uwsm/env" 'export LIBGL_ALWAYS_SOFTWARE=#1
'
check "v5 a # inside a word is literal" 1 "$S
user_setup=present
user_setup_path=$H/.config/uwsm/env
user_setup_detail=$H/.config/uwsm/env:1 sets LIBGL_ALWAYS_SOFTWARE" "$R"
R=$(fresh v6); put "$R" "$H/.config/uwsm/env" 'export VK_ICD_FILENAMES=$VK_ICD_FILENAMES:/opt/mesa-m3/x.json
export CHONKSTEP_M3_MESA_PREFIX=/opt/mesa-m3/
'
check "v6 self reference and trailing slash" 0 "$NONE" "$R"
R=$(fresh v7); put "$R" "$H/.config/uwsm/env" 'export VK_DRIVER_FILES=
'
check "v7 set empty" 1 "$S
user_setup=present
user_setup_path=$H/.config/uwsm/env
user_setup_detail=$H/.config/uwsm/env:1 sets VK_DRIVER_FILES empty" "$R"
R=$(fresh v8); put "$R" "$H/.config/uwsm/env" 'export GBM_BACKENDS_PATH="$XDG_DATA_HOME/gbm"
'
check "v8 unresolved value" 3 "$S
user_setup=unknown
user_setup_path=$H/.config/uwsm/env
user_setup_detail=$H/.config/uwsm/env:1 sets GBM_BACKENDS_PATH to an unresolved entry (\$XDG_DATA_HOME/gbm)" "$R"
R=$(fresh v9); put "$R" "$H/.config/uwsm/env" 'export VK_ICD_FILENAMES="/home/u/x
'
check "v9 unbalanced quote" 3 "$S
user_setup=unknown
user_setup_path=$H/.config/uwsm/env
user_setup_detail=$H/.config/uwsm/env:1 cannot be split (unbalanced quotes)" "$R"

# LD_LIBRARY_PATH: only entries holding a Mesa count.
R=$(fresh l1); mesa "$R" "$H/mesa/lib"; put "$R" "$H/.config/uwsm/env-hyprland" 'export LD_LIBRARY_PATH=/usr/local/lib
export LD_LIBRARY_PATH="${HOME}/mesa/lib:$LD_LIBRARY_PATH"
'
check "l1 LD_LIBRARY_PATH replaced, and a Mesa" 1 "$S
user_setup=present
user_setup_path=$H/.config/uwsm/env-hyprland
user_setup_detail=$H/.config/uwsm/env-hyprland:1 replaces LD_LIBRARY_PATH (without \$LD_LIBRARY_PATH)
user_setup_detail=$H/.config/uwsm/env-hyprland:2 puts another Mesa in LD_LIBRARY_PATH ($H/mesa/lib)" "$R"
R=$(fresh l2); put "$R" "$H/.config/environment.d/20-path.conf" 'LD_LIBRARY_PATH=/usr/local/lib:~/lib
'
check "l2 unrelated LD_LIBRARY_PATH" 0 "$NONE" "$R"
R=$(fresh l3); put "$R" "$H/.config/environment.d/20-path.conf" 'LD_LIBRARY_PATH=$XDG_DATA_HOME/lib
'
check "l3 unresolved LD_LIBRARY_PATH entry" 3 "$S
user_setup=unknown
user_setup_path=$H/.config/environment.d/20-path.conf
user_setup_detail=$H/.config/environment.d/20-path.conf:1 puts an unresolved entry in LD_LIBRARY_PATH (\$XDG_DATA_HOME/lib)" "$R"

# A file read after the hook that replaces LD_LIBRARY_PATH (review F2).
R=$(fresh f2a); put "$R" "$H/.config/uwsm/env" 'export LD_LIBRARY_PATH=/usr/local/lib
'
check "f2a replaces LD_LIBRARY_PATH after the hook" 1 "$S
user_setup=present
user_setup_path=$H/.config/uwsm/env
user_setup_detail=$H/.config/uwsm/env:1 replaces LD_LIBRARY_PATH (without \$LD_LIBRARY_PATH)" "$R"
R=$(fresh f2b); put "$R" "$H/.config/uwsm/env" 'export LD_LIBRARY_PATH="/usr/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
'
check "f2b keeps LD_LIBRARY_PATH" 0 "$NONE" "$R"
R=$(fresh f2c); put "$R" "/usr/share/uwsm/env.d/60-later" 'export LD_LIBRARY_PATH=/x
'; put "$R" "/usr/share/uwsm/env.d/20-earlier" 'export LD_LIBRARY_PATH=/y
'; put "$R" "/usr/share/uwsm/env.d/50-mesa-m3" '[ -r /opt/mesa-m3/libexec/mesa-m3-session-env ] && . /opt/mesa-m3/libexec/mesa-m3-session-env
'
check "f2c hook directory: only later files" 1 "$S
user_setup=present
user_setup_path=/usr/share/uwsm/env.d/60-later
user_setup_detail=/usr/share/uwsm/env.d/60-later:1 replaces LD_LIBRARY_PATH (without \$LD_LIBRARY_PATH)" "$R"
R=$(fresh f2d); put "$R" "$H/.config/uwsm/default" 'export LD_LIBRARY_PATH=/usr/local/lib
'
check "f2d uwsm/default is read before the hook" 0 "$NONE" "$R"

# Review F6: more lines that must not read as none.
R=$(fresh f6a); put "$R" "$H/.config/uwsm/env" 'export LIBGL_ALWAYS_SOFTWARE=1 LIBGL_DRIVERS_PATH=/opt/mesa-m3/lib/dri
export GALLIUM_DRIVER=llvmpipe  # was /opt/mesa-m3/lib before
'
check "f6a two settings, ours beside" 1 "$S
user_setup=present
user_setup_path=$H/.config/uwsm/env
user_setup_detail=$H/.config/uwsm/env:1 sets LIBGL_ALWAYS_SOFTWARE
user_setup_detail=$H/.config/uwsm/env:2 sets GALLIUM_DRIVER" "$R"
printf 'VK_DRIVER_FILES=/opt/mesa-m3/share/vulkan/icd.d/asahi_icd.aarch64.json:/usr/share/vulkan/icd.d/lvp_icd.aarch64.json\0' > "$T/environ-f6"
check "f6b lavapipe after ours (environment)" 1 "$S
user_setup=present
user_setup_path=environment:VK_DRIVER_FILES
user_setup_detail=the environment sets VK_DRIVER_FILES (/usr/share/vulkan/icd.d/lvp_icd.aarch64.json)" "$(fresh f6b)" --environ "$T/environ-f6"
R=$(fresh f6c); put "$R" "$H/.config/environment.d/30-egl.conf" '__EGL_VENDOR_LIBRARY_FILENAMES=/opt/mesa-m3/../../usr/share/glvnd/egl_vendor.d/50_mesa.json
'
check "f6c a path that leaves the prefix" 1 "$S
user_setup=present
user_setup_path=$H/.config/environment.d/30-egl.conf
user_setup_detail=$H/.config/environment.d/30-egl.conf:1 sets __EGL_VENDOR_LIBRARY_FILENAMES (/opt/mesa-m3/../../usr/share/glvnd/egl_vendor.d/50_mesa.json)" "$R"
R=$(fresh f6d); put "$R" "$H/.config/hypr/conf.d/deep/gfx.conf" 'env = LIBGL_ALWAYS_SOFTWARE,1
'
check "f6d hypr subdirectory" 1 "$S
user_setup=present
user_setup_path=$H/.config/hypr/conf.d/deep/gfx.conf
user_setup_detail=$H/.config/hypr/conf.d/deep/gfx.conf:1 sets LIBGL_ALWAYS_SOFTWARE" "$R"
R=$(fresh f6e); mkdir -p "$R$H/.config/hypr/conf.d"; put "$R" "$H/.config/hypr/conf.d/x.conf" 'env = GALLIUM_DRIVER,zink
'; chmod 000 "$R$H/.config/hypr/conf.d"
check "f6e unreadable hypr subdirectory" 3 "$S
user_setup=unknown
user_setup_path=$H/.config/hypr/conf.d
user_setup_detail=$H/.config/hypr/conf.d cannot be listed" "$R"
R=$(fresh f6f); put "$R" "$H/.config/uwsm/env.d/a" 'X=1
'
out=$(env -i PATH=/usr/bin:/bin python3 -c '
import glob, sys
def boom(*a, **k): raise RuntimeError("simulated")
glob.glob = boom
det = sys.argv[1]
sys.argv = [det, "--home", "/home/u", "--list", sys.argv[2], "--root", sys.argv[3]]
exec(compile(open(det).read(), det, "exec"), {"__name__": "__main__", "__file__": det})
' "$DET" "$LIST" "$R" 2>&1); rc=$?
[ "$rc" = 2 ] && printf '%s\n' "$out" | grep -q 'internal error: RuntimeError: simulated' &&
	echo "ok    f6f a crash exits 2, never 1        rc=2" || { echo "FAIL  f6f crash: rc=$rc $out"; fail=1; }

# Hyprland files.
R=$(fresh h1); put "$R" "$H/.config/hypr/envs.conf" 'env = GBM_BACKENDS_PATH,/usr/lib/gbm # /opt/mesa-m3/lib/gbm
# env = VK_ICD_FILENAMES,/x
'
check "h1 hypr conf, comment names ours" 1 "$S
user_setup=present
user_setup_path=$H/.config/hypr/envs.conf
user_setup_detail=$H/.config/hypr/envs.conf:1 sets GBM_BACKENDS_PATH (/usr/lib/gbm)" "$R"
R=$(fresh h2); put "$R" "$H/.config/hypr/envs.lua" '-- hl.env("LIBGL_DRIVERS_PATH", "/x")
hl.env("LIBGL_DRIVERS_PATH", "/opt/mesa-m3/lib/dri") -- hl.env("X", "/y")
hl.env("VK_ICD_FILENAMES", os.getenv("HOME") .. "/icd.json")
'
check "h2 hypr lua, a computed value" 3 "$S
user_setup=unknown
user_setup_path=$H/.config/hypr/envs.lua
user_setup_detail=$H/.config/hypr/envs.lua:3 sets VK_ICD_FILENAMES to a value that is not a plain string" "$R"
R=$(fresh h3); mkdir -p "$R$H/.config/hypr"; put "$R" "$H/.config/hypr/a.conf" 'env = VK_ICD_FILENAMES,/x
'; chmod 000 "$R$H/.config/hypr"
check "h3 unreadable hypr directory" 3 "$S
user_setup=unknown
user_setup_path=$H/.config/hypr
user_setup_detail=$H/.config/hypr cannot be listed" "$R"

# drirc.
R=$(fresh r1); put "$R" "$H/.drirc" '<driconf><!-- <option name="dri_driver" value="zink"/> --><device><application name="x"><option name="vblank_mode" value="0"/></application></device></driconf>
'
check "r1 dri_driver only in a comment" 0 "$NONE" "$R"
R=$(fresh r2); put "$R" "$H/.config/drirc" "<driconf><device><application name='all'><option name='dri_driver' value='zink'/></application></device></driconf>
"
check "r2 dri_driver (single quotes)" 1 "$S
user_setup=present
user_setup_path=$H/.config/drirc
user_setup_detail=$H/.config/drirc chooses a driver (dri_driver)" "$R"
R=$(fresh r3); put "$R" "$H/.drirc" '<driconf/>
'; chmod 000 "$R$H/.drirc"
check "r3 unreadable drirc" 3 "$S
user_setup=unknown
user_setup_path=$H/.drirc
user_setup_detail=$H/.drirc cannot be read (Permission denied)" "$R"
R=$(fresh r4); ln -s /nonexistent "$R$H/.drirc"
check "r4 dangling drirc link" 0 "$NONE" "$R"

# chonkstep (in a chonkstep session; scopes below).
SESS=chonkstep
R=$(fresh c1); put "$R" "$H/.config/chonkstep/m3gpu-session.env" 'CHONKSTEP_M3_CLIENTS=gpu

# Qualified private Mesa and Xwayland GLX; see docs/m3-xwayland-glx.md.
CHONKSTEP_M3_MESA_PREFIX=/home/u/src/m3-gpu-work/mesa-eryk-prefix-20261001
CHONKSTEP_M3_XWAYLAND_GLAMOR=1
'
check "c1 chonkstep private prefix" 1 "$S
user_setup=present
user_setup_path=$H/.config/chonkstep/m3gpu-session.env
user_setup_detail=$H/.config/chonkstep/m3gpu-session.env sets CHONKSTEP_M3_MESA_PREFIX to /home/u/src/m3-gpu-work/mesa-eryk-prefix-20261001" "$R"
R=$(fresh c2); put "$R" "$H/.config/chonkstep/m3gpu-session.env" 'CHONKSTEP_M3_MESA_PREFIX="/opt/mesa-m3" # the package
'
check "c2 chonkstep on the package" 0 "$NONE" "$R"
R=$(fresh c3); put "$R" "$H/.config/chonkstep/m3gpu-session.env" 'CHONKSTEP_M3_CLIENTS=gpu
'
check "c3 chonkstep with no prefix line" 1 "$S
user_setup=present
user_setup_path=$H/.config/chonkstep/m3gpu-session.env
user_setup_detail=$H/.config/chonkstep/m3gpu-session.env sets no CHONKSTEP_M3_MESA_PREFIX: chonkstep's M3 launcher uses its own default prefix" "$R"
printf 'CHONKSTEP_M3_MESA_PREFIX=/opt/mesa-m3\0' > "$T/environ-c3"
check "c3 same, environment names ours" 0 "$NONE" "$R" --environ "$T/environ-c3"
SESS=Hyprland

# Round 4: only a setting the session uses counts. checkx compares the whole
# output, including session_desktops and every user_setup_finding line.
# checkx NAME RC EXPECTED-LINES ROOT [detector options]
checkx() {
	_n=$1 _rc=$2 _want=$3 _r=$4
	shift 4
	_got=$(env -i PATH=/usr/bin:/bin "$DET" --home "$H" --list "$LIST" --root "$_r" "$@" 2>&1)
	_grc=$?
	if [ "$_grc" = "$_rc" ] && [ "$_got" = "$_want" ]; then
		printf 'ok    %-34s rc=%s %s\n' "$_n" "$_grc" "$(printf '%s\n' "$_got" | sed -n 's/^user_setup=//p')"
	else
		printf 'FAIL  %s: rc=%s (want %s)\n--- got\n%s\n--- want\n%s\n' "$_n" "$_grc" "$_rc" "$_got" "$_want"
		fail=1
	fi
}
CHONK="$H/.config/chonkstep/m3gpu-session.env"
CF="$CHONK sets CHONKSTEP_M3_MESA_PREFIX to /home/u/src/m3-gpu-work/mesa-eryk-prefix-20261001"
R=$(fresh s1); put "$R" "$CHONK" 'CHONKSTEP_M3_MESA_PREFIX=/home/u/src/m3-gpu-work/mesa-eryk-prefix-20261001
'
checkx "s1 Hyprland, only chonkstep's file" 0 "$S
session_desktops=hyprland
user_setup=none
user_setup_finding=ignored desktop:chonkstep sure $CF" "$R" --session-desktops Hyprland
checkx "s2 chonkstep, the same file" 1 "$S
session_desktops=chonkstep
user_setup=present
user_setup_path=$CHONK
user_setup_detail=$CF
user_setup_finding=applied desktop:chonkstep sure $CF" "$R" --session-desktops chonkstep
checkx "s3 unknown session, the same file" 0 "$S
session_desktops=unknown
user_setup=none
user_setup_finding=ignored desktop:chonkstep sure $CF" "$R"
R=$(fresh s4); P=/home/u/src/m3-gpu-work/mesa-eryk-prefix-20261001; mesa "$R" "$P/lib"
put "$R" "$CHONK" "CHONKSTEP_M3_MESA_PREFIX=$P
"
put "$R" "$H/.config/uwsm/env-hyprland" "export LD_LIBRARY_PATH=\"$P/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\"
export __EGL_VENDOR_LIBRARY_FILENAMES=$P/share/glvnd/egl_vendor.d/50_mesa.json
"
E="$H/.config/uwsm/env-hyprland"
checkx "s4 Hyprland, uwsm env-hyprland: used" 1 "$S
session_desktops=hyprland
user_setup=present
user_setup_path=$E
user_setup_detail=$E:1 puts another Mesa in LD_LIBRARY_PATH ($P/lib)
user_setup_detail=$E:2 sets __EGL_VENDOR_LIBRARY_FILENAMES ($P/share/glvnd/egl_vendor.d/50_mesa.json)
user_setup_finding=applied desktop:hyprland sure $E:1 puts another Mesa in LD_LIBRARY_PATH ($P/lib)
user_setup_finding=applied desktop:hyprland sure $E:2 sets __EGL_VENDOR_LIBRARY_FILENAMES ($P/share/glvnd/egl_vendor.d/50_mesa.json)
user_setup_finding=ignored desktop:chonkstep sure $CHONK sets CHONKSTEP_M3_MESA_PREFIX to $P" "$R" --session-desktops Hyprland
checkx "s5 chonkstep, env-hyprland not read" 1 "$S
session_desktops=chonkstep
user_setup=present
user_setup_path=$CHONK
user_setup_detail=$CHONK sets CHONKSTEP_M3_MESA_PREFIX to $P
user_setup_finding=ignored desktop:hyprland sure $E:1 puts another Mesa in LD_LIBRARY_PATH ($P/lib)
user_setup_finding=ignored desktop:hyprland sure $E:2 sets __EGL_VENDOR_LIBRARY_FILENAMES ($P/share/glvnd/egl_vendor.d/50_mesa.json)
user_setup_finding=applied desktop:chonkstep sure $CHONK sets CHONKSTEP_M3_MESA_PREFIX to $P" "$R" --session-desktops chonkstep
checkx "s6 unknown session: no desktop file applies" 0 "$S
session_desktops=unknown
user_setup=none
user_setup_finding=ignored desktop:hyprland sure $E:1 puts another Mesa in LD_LIBRARY_PATH ($P/lib)
user_setup_finding=ignored desktop:hyprland sure $E:2 sets __EGL_VENDOR_LIBRARY_FILENAMES ($P/share/glvnd/egl_vendor.d/50_mesa.json)
user_setup_finding=ignored desktop:chonkstep sure $CHONK sets CHONKSTEP_M3_MESA_PREFIX to $P" "$R"
R=$(fresh s7); put "$R" "$H/.config/uwsm/env" 'export CHONKSTEP_M3_MESA_PREFIX=/srv/m3
'; put "$R" "$H/.config/uwsm/env-chonkstep" 'export GALLIUM_DRIVER=zink
'; put "$R" "$H/.config/uwsm/env-Hyprland" 'export GALLIUM_DRIVER=zink
'; put "$R" "$H/.config/hypr/hyprland.conf" 'env = LIBGL_ALWAYS_SOFTWARE,1
'
checkx "s7 Hyprland: its config applies; chonkstep's variable, env-chonkstep, env-Hyprland do not" 1 "$S
session_desktops=hyprland:wlroots
user_setup=present
user_setup_path=$H/.config/hypr/hyprland.conf
user_setup_detail=$H/.config/hypr/hyprland.conf:1 sets LIBGL_ALWAYS_SOFTWARE
user_setup_finding=ignored desktop:chonkstep sure $H/.config/uwsm/env:1 sets CHONKSTEP_M3_MESA_PREFIX to /srv/m3
user_setup_finding=ignored desktop:Hyprland sure $H/.config/uwsm/env-Hyprland:1 sets GALLIUM_DRIVER
user_setup_finding=ignored desktop:chonkstep sure $H/.config/uwsm/env-chonkstep:1 sets GALLIUM_DRIVER
user_setup_finding=applied desktop:hyprland sure $H/.config/hypr/hyprland.conf:1 sets LIBGL_ALWAYS_SOFTWARE" "$R" --session-desktops Hyprland:wlroots
checkx "s8 chonkstep: Hyprland's config is not read" 1 "$S
session_desktops=chonkstep
user_setup=present
user_setup_path=$H/.config/uwsm/env
user_setup_path=$H/.config/uwsm/env-chonkstep
user_setup_detail=$H/.config/uwsm/env:1 sets CHONKSTEP_M3_MESA_PREFIX to /srv/m3
user_setup_detail=$H/.config/uwsm/env-chonkstep:1 sets GALLIUM_DRIVER
user_setup_finding=applied desktop:chonkstep sure $H/.config/uwsm/env:1 sets CHONKSTEP_M3_MESA_PREFIX to /srv/m3
user_setup_finding=ignored desktop:Hyprland sure $H/.config/uwsm/env-Hyprland:1 sets GALLIUM_DRIVER
user_setup_finding=applied desktop:chonkstep sure $H/.config/uwsm/env-chonkstep:1 sets GALLIUM_DRIVER
user_setup_finding=ignored desktop:hyprland sure $H/.config/hypr/hyprland.conf:1 sets LIBGL_ALWAYS_SOFTWARE" "$R" --session-desktops chonkstep
R=$(fresh s9); put "$R" "$CHONK" 'X=1
'; chmod 000 "$R$CHONK"; put "$R" "$H/.config/uwsm/env" 'export VK_DRIVER_FILES=/srv/icd.json
'
checkx "s9 Hyprland: unreadable chonkstep file ignored, all-scope found" 1 "$S
session_desktops=hyprland
user_setup=present
user_setup_path=$H/.config/uwsm/env
user_setup_detail=$H/.config/uwsm/env:1 sets VK_DRIVER_FILES (/srv/icd.json)
user_setup_finding=applied all sure $H/.config/uwsm/env:1 sets VK_DRIVER_FILES (/srv/icd.json)
user_setup_finding=ignored desktop:chonkstep unsure $CHONK cannot be read (Permission denied)" "$R" --session-desktops Hyprland

# The environment: the hook's own software fallback is not a user setup.
printf 'LIBGL_ALWAYS_SOFTWARE=1\0MESA_M3_FALLBACK=software\0' > "$T/environ-fb"
check "e1 the hook's own fallback" 0 "$NONE" "$(fresh e1)" --environ "$T/environ-fb"
printf 'LIBGL_ALWAYS_SOFTWARE=1\0' > "$T/environ-sw"
check "e2 a user's LIBGL_ALWAYS_SOFTWARE" 1 "$S
user_setup=present
user_setup_path=environment:LIBGL_ALWAYS_SOFTWARE
user_setup_detail=the environment sets LIBGL_ALWAYS_SOFTWARE" "$(fresh e2)" --environ "$T/environ-sw"

# present wins over unknown; both paths listed.
R=$(fresh m1); put "$R" "$H/.config/uwsm/env" 'export GALLIUM_DRIVER=llvmpipe
'; put "$R" "$H/.drirc" '<driconf/>
'; chmod 000 "$R$H/.drirc"
check "m1 present and unreadable" 1 "$S
user_setup=present
user_setup_path=$H/.config/uwsm/env
user_setup_path=$H/.drirc
user_setup_detail=$H/.config/uwsm/env:1 sets GALLIUM_DRIVER
user_setup_detail=$H/.drirc cannot be read (Permission denied)" "$R"

# Usage errors.
out=$("$DET" --home relative 2>&1); rc=$?
[ "$rc" = 2 ] && echo "ok    usage error                        rc=2" || { echo "FAIL usage error rc=$rc"; fail=1; }

[ "$fail" = 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit "$fail"
