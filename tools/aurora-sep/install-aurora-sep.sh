#!/bin/bash
# Install the aurora custom/sep kernel and Touch ID on an Omarchy Mac.
#
#   curl -fsSL https://github.com/iconidentify/aurora-linux/releases/latest/download/install-aurora-sep.sh | bash
#   ... | bash -s -- --read-only      install, but never let the driver write to the enclave
#   ... | bash -s -- --uninstall      go back to the kernel this Mac had before
#   ... | bash -s -- --reset-touchid  start Touch ID over: new keybag, enrol again
#
# Kernel: iconidentify/aurora-linux custom/sep (dbbfb92908ba), aurora-silicon/linux aurora-wip plus the
# Secure Enclave (Touch ID) driver, Thunderbolt (#8), the Apple video
# decoder (#45), the M2 Max (t6021) profile and the consolidated Touch ID
# series (aurora-silicon/linux#69: matching after a reboot on every profile,
# M1 Pro J314s, j293 SPI mode, a scan in progress ended before sleep),
# Thunderbolt displays on M1 and M1 Pro/Max by default, and two displays
# through one dock on every M2 Pro/Max laptop (aurora-silicon/linux#8, with
# #46, #50 and #64), and the MacBook Neo (J700) work from #40, #42,
# #43, #54 and #55, including its Touch ID device tree.
# 11.20 adds Touch ID on the MacBook Air M2 15" (J415, aurora-silicon/linux#152),
# a retry from a fresh sensor registration when the first sensor patch fails at
# boot, battery time estimates that read "no data" instead of 0 (#53), the
# Omarchy boot logo in place of Tux, and aurora-wip through 3bb0a6104a11 (the
# MacBook Neo video decoder, PCIe bring-up and radio drivers, inert elsewhere).
# On a MacBook Neo the script keeps the Neo's own m1n1.
# 11.21 adds Bluetooth recovery for the Broadcom PCIe controllers: a radio
# wedged by an rfkill cycle under heavy traffic is reset on the next open, and
# its calibration and address restored (aurora-silicon/linux#7).
# 11.22 installs the VA-API bridge for the video decoder too, so players use
# the hardware decoder instead of falling back to software, and
# aurora-touchid-setup --help prints its usage instead of starting an enrolment.
# The display manager waits (up to 30 s) for the Apple display driver, so a
# late driver can't leave the built-in screen black, and reloading the Touch ID
# driver on M1 is refused with "reboot to re-attach" instead of hanging.
# 11.23 routes USB-C displays on the M2 Pro/Max MacBook Pros in the order the
# compositor pairs them, so two monitors attached at boot each keep their own
# modes, and describes the Touch ID sensor on every M2 Pro/Max MacBook Pro.
# 11.24 adds Touch ID on the MacBook Pro 14"/16" M1 Max and the MacBook Air 13"
# M2, untested on both. On those boards the script first keeps the boot.bin the
# Mac booted with on the EFI partition and prints how to restore it from macOS.
# 11.25 adds the in-tree Apple Neural Engine driver (aurora-silicon/linux#155),
# switched on for the M1 Max and M2 Max only, and the read-only SEP diagnostics
# under /sys/bus/platform/devices/*.sep/diag/. When omarchy-ane-dkms is
# installed the script says so: its modules take precedence over this kernel's.
# 11.25.1 (installer only, same packages): rebuilding m1n1's stage 2 now keeps
# the rest of /etc/default/update-m1n1, so a MacBook Neo keeps its own M1N1=
# and U_BOOT= instead of being rebuilt from an m1n1 that cannot boot it.
# 11.31 brings up PCIe over Thunderbolt on the M1 Pro/Max and M2 Pro/Max by
# default (iconidentify/aurora-linux#10, Wesley Grimes, with follow-up fixes;
# pcie_apple.tunnel_kernel_init=0 turns it off): USB, Ethernet and audio
# behind Thunderbolt docks and displays now work there as they do on M1. It
# builds the drivers for the Intel Ethernet in Thunderbolt 3 and 4 docks (igb
# and igc), and carries the MacBook Neo's Wi-Fi in the shared kernel
# (iconidentify/aurora-linux#11). On a Neo the radios need that Neo's own
# firmware, calibration and country files, which the script checks for; sleep
# is refused while they are active, and Bluetooth is off by default.
# The Neural Engine now powers off when idle (aurora-silicon/linux#155, Joshua
# Warren), and is switched on for the M1, M1 Pro and M2 Pro as well as the M1
# Max and M2 Max.
# 11.32 carries three Touch ID fixes from Justin Pfister's review
# (aurora-silicon/linux#69): a deleted fingerprint is saved as deleted and no
# longer returns after a reboot; an M2 Pro/Max on system firmware 26.2 or
# earlier uses the 13.5 key store, whose keybag that enclave accepts; and the
# kernel's random-number thread no longer asks the enclave for data while the
# Mac sleeps, which left Touch ID failing after resume on an M2 Pro.
# 11.33 adds --reset-touchid, which starts Touch ID over on a Mac whose
# keybag no longer loads (such an M2 Pro/Max after a macOS update): it moves
# the Touch ID state aside for a new keybag at the next boot. Enrolment
# failures are now logged with their cause, and aurora-touchid-setup verifies
# as root, so it also completes over SSH.
# 11.34 sets the brightness of an Apple Studio Display through DCP
# (iconidentify/aurora-linux#15, Wesley Grimes): brightnessctl -d 'dcp-DP-*',
# and nothing is sent until a level is chosen. On the M1/M2 Pro and Max it
# keeps the Thunderbolt root port's link out of ASPM L1, so unplugging an idle
# dock no longer leaves the port dead until reboot.
# 11.35 lets a Touch ID enrolment take up to 36 captures, enough for the 16 to
# 22 an M2 Pro needs (iconidentify/aurora-linux#17, Justin Pfister), and
# reports one stage per capture, so every accepted touch shows progress.
# 11.36 is one kernel for M1, M2 and M3: on M1 Pro/Max laptops a second
# USB-C or Thunderbolt display gets the HDMI port's pipeline when HDMI is idle,
# and a dock display left waiting lights once a pipeline frees. M3 support is
# experimental and kernel-only (see is_m3).
# 11.36.1 changes only this script and adds an M3 m1n1: an M3 Pro model whose
# m1n1 display and GPU handoff has been booted (the 16" J516S for now) gets
# m1n1-aurora with it switched on, and the built-in display and GPU work.
# Every other M3 stays kernel-only, and --m3-handoff lets an M3 Pro owner try
# the handoff on a model not yet on the list (see --agent-prompt). M1, M2 and
# the Neo are unchanged.
# It replaces linux-asahi (or linux-aurora) as a pacman package,
# so mkinitcpio and update-m1n1 run from their own hooks; on a GRUB Mac this
# script regenerates grub.cfg and keeps the previous kernel as a fallback entry.
#
# Touch ID: libfprint with the Apple SEP driver, fprintd, the apple-sep
# service, a sleep hook that stops fprintd before suspend (so a lock screen
# waiting for a finger at lid close can't leave the sensor claimed), and
# this Mac's own sensor calibration. After the reboot, run
# aurora-touchid-setup to enrol a finger and use it for sudo and the lock screen.
#
# m1n1: m1n1-aurora builds AsahiLinux/m1n1 main (3e354a24), which knows the
# macOS 26.5 to 27.0 firmware the MacBook Neo ships with, plus three patches:
# the SEP warm-registration guard and preboot-UUID forwarding from
# aurora-silicon/m1n1, and the usb4-N-pcie-adapter alias fallback
# (aurora-silicon/m1n1#4).
# M3: experimental. linux-aurora goes on. On an M3 Pro model in
# M3_HANDOFF_BOARDS (or with --m3-handoff), M3_M1N1_PACKAGE replaces m1n1 and
# three chosen.asahi,t6030-* lines in /etc/m1n1.conf switch its display and
# GPU handoff on. Every other M3 keeps m1n1's boot.bin exactly as it is: no
# m1n1-aurora, no update-m1n1 run, and a freeze on update-m1n1 unless one is
# in place already (see m3_plan).
# On M2 and later the platform hands Linux an already-running Secure
# Enclave, and the driver attaches to it with one registration that can only be
# sent once per boot. Stock m1n1 asks the enclave for randomness on the way up,
# spending that attempt before Linux sees it. aurora-silicon/m1n1 has a guard
# that skips the request when the enclave is already running. On M1, where the
# enclave is still in its boot ROM, the guard never fires.
#
# Testing this build? Run with --agent-prompt for the test plan and the format
# to report results in.
set -euo pipefail

# The kernel package version and the release tag move independently: a release
# that only changes m1n1 reuses the previous kernel packages unchanged.
VERSION=7.1.12.aurora2-11.36
TAG=sep-7.1.12.aurora2-11.36.1
# Packages are fetched from this script's own tag, never from "latest": the
# checksums below belong to this release and nothing else.
RELEASE_URL=https://github.com/iconidentify/aurora-linux/releases/download/$TAG
RELEASES_API=https://api.github.com/repos/iconidentify/aurora-linux/releases
# Where to always get the current script, whatever this copy turns out to be.
LATEST_URL=https://github.com/iconidentify/aurora-linux/releases/latest/download/install-aurora-sep.sh
PACKAGES=(
  "linux-aurora-$VERSION-aarch64.pkg.tar.zst cb5cb0800fafaaecde2dcb350044bc3afbfc7018ef2f54dd4422bc26c293c9ec"
  "linux-aurora-headers-$VERSION-aarch64.pkg.tar.zst e21ab2add5491d404980f87bce168cfd05ee298e1fc0f12815d9ff68d12e6398"
  "libfprint-1.94.100-1.1-aarch64.pkg.tar.zst bc7d9762db6644f2cfb58ddb209602c1d513845eb1498c098e01f12600fcbdf9"
  "aurora-touchid-20261003-1-any.pkg.tar.zst 29b0360fac8c257d754e64bd1b9c33c487eb2595dd3c31e9138d7a476afa3d64"
  "m1n1-aurora-1.6.1.aurora3-1-aarch64.pkg.tar.zst bc3451aaa88bc3f4912bc3613f9569aa8f3e05f376fa851fa837b5e2080e8c2f"
)
# Only for an M3 on the handoff path (see m3_plan), in place of the m1n1-aurora
# above: the same m1n1 plus the T6030 display and GPU handoff.
M3_M1N1_PACKAGE="m1n1-aurora-1.6.1.aurora5-1-aarch64.pkg.tar.zst e06dd92a75fb95cba822eb0a414a6f01f6d5bf9ca4f85749b0ef663922c38294"
PINNED="linux-aurora linux-aurora-headers libfprint m1n1-aurora"
PIN_BEGIN="# >>> aurora-sep pin (remove with: install-aurora-sep.sh --uninstall)"
PIN_END="# <<< aurora-sep pin"
STATE=/var/lib/aurora-sep
FALLBACK_ID=aurora-sep-previous-kernel
# Written only by --read-only; keeps the driver from writing to the enclave.
MODPROBE_CONF=/etc/modprobe.d/aurora-sep.conf
READ_ONLY=0
DT=/proc/device-tree

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

sudo=""
if (( EUID != 0 )); then
  command -v sudo >/dev/null || die "run as root or install sudo"
  sudo=sudo
fi

# A saved copy of this script keeps installing its own build forever. Tell the
# operator - human or agent - when a newer one exists. Never fatal: no network,
# rate limit or API change should stop an install that was going to work.
newer_release() {
  # Explicitly non-fatal. The pipeline returns non-zero whenever there is no
  # network, GitHub rate-limits, or the response is not what we expect, and
  # whether set -e acts on that inside a command substitution is subtle enough
  # that it should not be left to chance in a script that runs as root.
  local seen=""
  # Ask for the release marked Latest. Listing all releases is not ordered by
  # version: they share a commit, so GitHub falls back to comparing tag names
  # as text, and 11.9 sorts above 11.10.
  seen=$(curl -fsSL --max-time 8 "$RELEASES_API/latest" 2>/dev/null |
    grep -o '"tag_name"[[:space:]]*:[[:space:]]*"sep-[^"]*"' |
    head -1 | sed 's/.*"\(sep-[^"]*\)"$/\1/') || true
  [[ -n $seen && $seen != "$TAG" ]] && echo "$seen"
  return 0
}

version_notice() {
  local seen
  seen=$(newer_release)
  if [[ -n $seen ]]; then
    warn "this script installs $TAG, but $seen is published.
    You are probably running a saved copy. The current one:
      curl -fsSL $LATEST_URL | bash"
  else
    say "$TAG is the current release"
  fi
}

boot_chain() {
  # /boot/efi can be readable only by root, so test through sudo. Limine is
  # active where omarchy-mac-boot is installed, or where its activation marker
  # and defaults exist without it (the test omarchy-mac-limine-active and
  # limine-mkinitcpio-hook's Apple gate make), as on Macs switched by hand.
  if { pacman -Q omarchy-mac-boot >/dev/null 2>&1 ||
    [[ -f /var/lib/omarchy/limine.enabled && -f /etc/default/limine ]]; } &&
    { $sudo test -e /boot/EFI/BOOT/BOOTAA64.EFI || $sudo test -e /boot/efi/EFI/BOOT/BOOTAA64.EFI; } &&
    command -v limine >/dev/null; then
    echo limine
  elif [[ -f /boot/grub/grub.cfg ]] && command -v grub-mkconfig >/dev/null; then
    echo grub
  else
    echo unknown
  fi
}

# The MacBook Neo (T8140) boots an m1n1 built from aurora-silicon's J700
# branch. m1n1-aurora has no T8140 support, so on a Neo this script never
# installs it or the stock m1n1 over the one the Mac already boots.
is_neo() {
  tr '\0' '\n' <"$DT/compatible" 2>/dev/null | grep -qx 'apple,t8140'
}

# Every M3 chip: M3 (t8122), M3 Pro (t6030), M3 Max (t6031, t6034).
is_m3() {
  tr '\0' '\n' <"$DT/compatible" 2>/dev/null | grep -Eqx 'apple,(t8122|t6030|t6031|t6034)'
}

is_m3_pro() {
  tr '\0' '\n' <"$DT/compatible" 2>/dev/null | grep -qx 'apple,t6030'
}

# M3 support is experimental. Every M3 gets linux-aurora; what happens to m1n1's
# stage 2 (boot.bin: m1n1, device trees and U-Boot) depends on the model, and
# m3_plan decides it before anything is downloaded (M3_MODE):
#   kernel   boot.bin stays exactly as it is. No m1n1-aurora, no /etc/m1n1.conf
#            change, no update-m1n1 run, and a freeze on update-m1n1 unless one
#            is in place already (the M3 bring-up's install-m3gpu.sh has one),
#            because pacman's hook would otherwise rebuild boot.bin when the
#            kernel's device trees arrive. --uninstall lifts it.
#   handoff  An M3 Pro model in M3_HANDOFF_BOARDS on the tested stub, or any M3
#            Pro whose owner asked for it with --m3-handoff. It gets
#            M3_M1N1_PACKAGE, whose T6030 display and GPU handoff (from
#            iconidentify/m3-m1n1) stays off unless boot.bin ends with the
#            three M3_SWITCHES lines; update-m1n1 copies chosen.* lines from
#            /etc/m1n1.conf into every rebuild, so they survive updates.
# M1, M2 and the Neo keep the m1n1-aurora in PACKAGES: only an M3 on the
# handoff path gets the newer m1n1.
UPDATE_M1N1_CONF=/etc/default/update-m1n1
M3_FREEZE_BEGIN="# >>> aurora-sep: keep this M3's boot.bin as it is (remove with: install-aurora-sep.sh --uninstall)"
M3_FREEZE_END="# <<< aurora-sep: keep this M3's boot.bin as it is"
M3_FROZEN_BY=""
M3_BOOTBIN_SHA=""
# Models the handoff has been booted on, by us or by a tester's report.
M3_HANDOFF_BOARDS="j516s"
M3_SWITCHES="chosen.asahi,t6030-gpu=1 chosen.asahi,t6030-dcp=1 chosen.asahi,t6030-dcpext=1"
# The handoff is tested with one macOS system-firmware stub only, 14.8.3 (GPU
# firmware 14.8.3, DCP 14.7), which the Omarchy installer gives every M3. m1n1
# reports the stub's iBoot as asahi,iboot2-version.
M3_STUB_VERSION=14.8.3
M3_STUB_IBOOT=iBoot-10151.140.19
M1N1_CONF=/etc/m1n1.conf
M1N1_CONF_BEGIN="# >>> aurora-sep: M3 Pro display and GPU handoff (remove with: install-aurora-sep.sh --uninstall)"
M1N1_CONF_END="# <<< aurora-sep: M3 Pro display and GPU handoff"
# The M3 bring-up's install-m3gpu.sh froze boot.bin with exactly these lines,
# and left this marker when it created the file.
M3GPU_FREEZE=(
  "# Added by m3-gpu-work/scripts/install-m3gpu.sh: keep the M3 GPU candidate boot.bin."
  "# Undo with rollback-m3gpu.sh (then run: sudo update-m1n1)."
  "M1N1_UPDATE_DISABLED=1"
)
M3GPU_MARKER=/etc/default/.update-m1n1.created-by-m3gpu
# none (not an M3), kernel or handoff; set by m3_plan, kept in $STATE/m3-mode
# for --uninstall.
M3_MODE=none
# Set by --m3-handoff: the owner asks for the handoff on an M3 Pro model that
# isn't in M3_HANDOFF_BOARDS yet.
M3_TRY=0

# Whether update-m1n1 would exit without building, judged the way it judges:
# it sources this file and stops when M1N1_UPDATE_DISABLED is non-empty.
update_m1n1_frozen() {
  [[ -f $UPDATE_M1N1_CONF ]] || return 1
  # shellcheck disable=SC2016 # expanded by that sh, not here
  env -i PATH="$PATH" sh -c 'set -e; . "$1"; [ -n "${M1N1_UPDATE_DISABLED:-}" ]' _ \
    "$UPDATE_M1N1_CONF" >/dev/null 2>&1
}

m3_freeze() {
  local tmp
  if update_m1n1_frozen; then
    M3_FROZEN_BY=already
    return 0
  fi
  tmp=$(mktemp)
  if [[ -f $UPDATE_M1N1_CONF ]]; then cat "$UPDATE_M1N1_CONF" >"$tmp"; fi
  printf '%s\nM1N1_UPDATE_DISABLED=1\n%s\n' "$M3_FREEZE_BEGIN" "$M3_FREEZE_END" >>"$tmp"
  $sudo install -m 644 "$tmp" "$UPDATE_M1N1_CONF"
  rm -f "$tmp"
  update_m1n1_frozen || die "could not freeze update-m1n1 in $UPDATE_M1N1_CONF; nothing was installed"
  M3_FROZEN_BY=aurora-sep
}

# update-m1n1's configuration without this script's freeze, on stdout.
m3_unfrozen() {
  awk -v b="$M3_FREEZE_BEGIN" -v e="$M3_FREEZE_END" '
    $0 == b { skip = 1; next }
    skip && $0 == e { skip = 0; next }
    !skip' "$UPDATE_M1N1_CONF"
}

# Lift only this script's freeze; a file that held nothing else goes.
m3_unfreeze() {
  local tmp
  [[ -f $UPDATE_M1N1_CONF ]] && grep -qxF "$M3_FREEZE_BEGIN" "$UPDATE_M1N1_CONF" || return 0
  tmp=$(mktemp)
  m3_unfrozen >"$tmp"
  if grep -q '[^[:space:]]' "$tmp"; then
    $sudo install -m 644 "$tmp" "$UPDATE_M1N1_CONF"
  else
    $sudo rm -f "$UPDATE_M1N1_CONF"
  fi
  rm -f "$tmp"
}

# m1n1's boot.bin on the EFI partition, where macOS can reach it. Omarchy
# mounts that partition readable by root only, so look through sudo.
esp_bootbin() {
  local target
  for target in /boot/efi/m1n1/boot.bin /boot/m1n1/boot.bin; do
    $sudo test -f "$target" || continue
    [[ $(findmnt -no FSTYPE --target "${target%/m1n1/boot.bin}") == vfat ]] || continue
    echo "$target"
    return 0
  done
  return 1
}

m3_bootbin_sha() {
  local target
  target=$(esp_bootbin) || return 0
  $sudo sha256sum "$target" | cut -d' ' -f1
}

# After a kernel-only install: say plainly what happened to boot.bin, and prove it.
m3_bootbin_report() {
  local now
  now=$(m3_bootbin_sha)
  if [[ -n $M3_BOOTBIN_SHA && $now != "$M3_BOOTBIN_SHA" ]]; then
    die "m1n1's boot.bin changed during the install, which it must not on an M3.
    Please report it at https://github.com/iconidentify/aurora-linux/issues before rebooting."
  fi
  if [[ $M3_FROZEN_BY == aurora-sep ]]; then
    say "M3: m1n1's boot.bin is unchanged. pacman's \"Updating m1n1 image\" step did nothing:
    this script froze update-m1n1 in $UPDATE_M1N1_CONF, so kernel and m1n1
    updates keep boot.bin as it is. --uninstall lifts the freeze."
  else
    say "M3: m1n1's boot.bin is unchanged. pacman's \"Updating m1n1 image\" step did nothing:
    update-m1n1 was already frozen in $UPDATE_M1N1_CONF, and stays that way."
  fi
}

is_m3_handoff_board() {
  local board
  board=$(this_board)
  is_m3_pro && [[ -n $board && " $M3_HANDOFF_BOARDS " == *" $board "* ]]
}

# Why this Mac's system-firmware stub isn't the one the handoff is tested
# with; nothing when it is. The installer's stub_info.json names the stub's
# macOS version, and m1n1 reports the stub's iBoot.
m3_stub_problem() {
  local iboot bootbin info="" version=""
  iboot=$(tr -d '\0' <"$DT/chosen/asahi,iboot2-version" 2>/dev/null) || iboot=""
  if bootbin=$(esp_bootbin); then
    info=${bootbin%/m1n1/boot.bin}/asahi/stub_info.json
    version=$($sudo cat "$info" 2>/dev/null | grep -o '"ProductVersion": *"[^"]*"' | head -1 |
      sed 's/.*"\([^"]*\)"$/\1/') || version=""
  fi
  if [[ -n $version && $version != "$M3_STUB_VERSION" ]]; then
    echo "its macOS system-firmware stub is $version ($info)"
  elif [[ $iboot != "$M3_STUB_IBOOT"* ]]; then
    echo "its system-firmware stub's iBoot is ${iboot:-not reported by m1n1}"
  fi
  return 0
}

# True when a downloaded m1n1-aurora package carries all three handoff switches.
m1n1_pkg_has_handoff() {
  local bin s rc=0
  bin=$(mktemp)
  bsdtar -xOf "$1" usr/lib/asahi-boot/m1n1.bin >"$bin" 2>/dev/null || rc=1
  for s in asahi,t6030-gpu asahi,t6030-dcp asahi,t6030-dcpext; do
    ((rc == 0)) && ! grep -qaF "$s" "$bin" && rc=1
  done
  rm -f "$bin"
  return "$rc"
}

# update-m1n1's configuration without the M3 bring-up's freeze, on stdout.
m3gpu_unfrozen() {
  awk -v a="${M3GPU_FREEZE[0]}" -v b="${M3GPU_FREEZE[1]}" -v c="${M3GPU_FREEZE[2]}" '
    { line[NR] = $0 }
    END {
      for (i = 1; i <= NR; i++) {
        if (line[i] == a && line[i + 1] == b && line[i + 2] == c) { i += 2; continue }
        print line[i]
      }
    }' "$UPDATE_M1N1_CONF"
}

# True when something other than the M3 bring-up or this script stops update-m1n1.
update_m1n1_frozen_by_others() {
  local tmp rc=1
  [[ -f $UPDATE_M1N1_CONF ]] || return 1
  tmp=$(mktemp)
  m3gpu_unfrozen | awk -v b="$M3_FREEZE_BEGIN" -v e="$M3_FREEZE_END" '
    $0 == b { skip = 1; next }
    skip && $0 == e { skip = 0; next }
    !skip' >"$tmp"
  grep -Eq '^[^#]*M1N1_UPDATE_DISABLED=' "$tmp" && rc=0
  rm -f "$tmp"
  return "$rc"
}

# Lift the M3 bring-up's freeze: only its own three lines, and its marker.
# The original is kept so --uninstall can put the freeze back.
m3gpu_unfreeze() {
  local tmp
  [[ -f $UPDATE_M1N1_CONF ]] && grep -qxF "${M3GPU_FREEZE[0]}" "$UPDATE_M1N1_CONF" || return 0
  [[ -f $STATE/update-m1n1.m3gpu.saved ]] || $sudo cp -p "$UPDATE_M1N1_CONF" "$STATE/update-m1n1.m3gpu.saved"
  tmp=$(mktemp)
  m3gpu_unfrozen >"$tmp"
  if [[ -e $M3GPU_MARKER ]]; then
    $sudo touch "$STATE/m3gpu-marker.saved"
    $sudo rm -f "$M3GPU_MARKER"
  fi
  if grep -q '[^[:space:]]' "$tmp"; then
    $sudo install -m 644 "$tmp" "$UPDATE_M1N1_CONF"
  else
    $sudo rm -f "$UPDATE_M1N1_CONF"
  fi
  rm -f "$tmp"
  say "Lifted the M3 bring-up's freeze on update-m1n1 (from install-m3gpu.sh)"
}

# /etc/m1n1.conf without this script's switch block, on stdout.
m1n1_conf_without_switches() {
  [[ -f $M1N1_CONF ]] || return 0
  awk -v b="$M1N1_CONF_BEGIN" -v e="$M1N1_CONF_END" '
    $0 == b { skip = 1; next }
    skip && $0 == e { skip = 0; next }
    !skip' "$M1N1_CONF"
}

m3_switches_write() {
  local tmp s
  tmp=$(mktemp)
  m1n1_conf_without_switches >"$tmp"
  {
    echo "$M1N1_CONF_BEGIN"
    for s in $M3_SWITCHES; do echo "$s"; done
    echo "$M1N1_CONF_END"
  } >>"$tmp"
  $sudo install -m 644 "$tmp" "$M1N1_CONF"
  rm -f "$tmp"
}

m3_switches_remove() {
  local tmp
  [[ -f $M1N1_CONF ]] && grep -qxF "$M1N1_CONF_BEGIN" "$M1N1_CONF" || return 0
  tmp=$(mktemp)
  m1n1_conf_without_switches >"$tmp"
  if grep -q '[^[:space:]]' "$tmp"; then
    $sudo install -m 644 "$tmp" "$M1N1_CONF"
  else
    $sudo rm -f "$M1N1_CONF"
  fi
  rm -f "$tmp"
}

# Decide the M3 path before anything is downloaded, so a Mac this release can't
# set up as asked stops with nothing changed.
m3_plan() {
  local board problem
  M3_MODE=none
  if ! is_m3; then
    ((M3_TRY == 0)) || die "--m3-handoff is for an M3 Pro, and this Mac isn't an M3. Nothing was installed."
    return 0
  fi
  board=$(this_board)
  M3_MODE=kernel
  if ! is_m3_pro; then
    ((M3_TRY == 0)) || die "--m3-handoff is for an M3 Pro (t6030); this M3 ($board) has no
    display and GPU handoff in m1n1 yet. Nothing was installed."
    say "M3 ($board): installing the kernel only. m1n1 has no display and GPU handoff for this chip
    yet, so boot.bin stays as it is and the display runs on the boot framebuffer."
    return 0
  fi
  if ! is_m3_handoff_board && ((M3_TRY == 0)); then
    say "M3 Pro ($board): installing the kernel only, and boot.bin stays as it is. The display and
    GPU handoff in m1n1 hasn't been booted on this model yet. To try it (it replaces this
    Mac's boot loader), see the M3 section of: bash -s -- --agent-prompt"
    return 0
  fi
  problem=$(m3_stub_problem)
  if [[ -n $problem ]]; then
    ((M3_TRY == 0)) || die "--m3-handoff: the handoff is only tested with the macOS $M3_STUB_VERSION
    system-firmware stub, and this Mac differs: $problem. Nothing was installed."
    warn "the M3 Pro display and GPU handoff is only tested with the macOS $M3_STUB_VERSION
    system-firmware stub, and this Mac differs: $problem. Installing the kernel only;
    boot.bin stays as it is."
    return 0
  fi
  if update_m1n1_frozen_by_others; then
    die "$UPDATE_M1N1_CONF sets M1N1_UPDATE_DISABLED, and not from this script or the M3
    bring-up's install-m3gpu.sh. Switching the M3 Pro handoff on needs m1n1's boot.bin rebuilt.
    Remove that line and run this again. Nothing was installed."
  fi
  M3_MODE=handoff
  if is_m3_handoff_board; then
    say "M3 Pro ($board, macOS $M3_STUB_VERSION stub): installing m1n1 with the display and GPU handoff"
  else
    warn "--m3-handoff: trying m1n1's display and GPU handoff on an M3 Pro model ($board) nobody
    has booted it on yet. This replaces the Mac's boot loader; the steps to put the old one
    back from macOS follow."
  fi
}

# After update-m1n1, boot.bin must end with the switch lines. update-m1n1
# appends /etc/m1n1.conf's lines straight after the gzipped U-Boot, so the
# first one has no newline in front: look for the block, not whole lines.
m3_verify_bootbin() {
  local target tail block
  target=$(esp_bootbin) || die "could not find m1n1's boot.bin to check the M3 Pro switches in"
  block=$(tr ' ' '\n' <<<"$M3_SWITCHES")
  tail=$($sudo tail -c 1024 "$target" | tr -d '\0')
  [[ $tail == *"$block"* ]] ||
    die "the rebuilt $target does not carry the M3 Pro switch lines; the previous one is saved in $STATE/boot.bin.saved"
  say "m1n1's boot.bin carries the M3 Pro handoff switches"
}

# --uninstall on a bring-up Mac: update-m1n1 has just rebuilt boot.bin from the
# stock m1n1, which has no handoff. Put back the boot.bin the bring-up froze, and
# the freeze.
m3_restore_bringup() {
  local target
  [[ -f $STATE/update-m1n1.m3gpu.saved ]] || return 0
  $sudo cp -p "$STATE/update-m1n1.m3gpu.saved" "$UPDATE_M1N1_CONF"
  if [[ -f $STATE/m3gpu-marker.saved ]]; then $sudo touch "$M3GPU_MARKER"; fi
  if [[ -f $STATE/boot.bin.saved ]] && target=$(esp_bootbin); then
    $sudo cp "$STATE/boot.bin.saved" "$target"
    say "Restored the M3 bring-up's boot.bin and its freeze on update-m1n1"
  else
    warn "restored the M3 bring-up's freeze on update-m1n1, but not its boot.bin (no saved copy)"
  fi
}

# Boards whose Touch ID support nobody has booted yet. Once a board's device
# tree names the enclave, m1n1 needs two boot manifests from the platform for
# it, and a board without them stops in m1n1 before any boot entry. Every
# board checked so far has them; these have not been checked.
UNPROVEN_SEP_BOARDS="j314c j316c j413"

this_board() {
  tr '\0' '\n' <"$DT/compatible" 2>/dev/null | sed -n '1s/^apple,//p'
}

# Before anything rebuilds boot.bin on such a board, keep the one this boot
# came up on next to it on the EFI partition, where macOS can reach it, and
# say how to put it back. A later run keeps the first copy.
unproven_board_backup() {
  local board
  board=$(this_board)
  [[ -n $board && " $UNPROVEN_SEP_BOARDS " == *" $board "* ]] || return 0
  keep_bootbin_on_esp "Touch ID on this Mac model ($board) is new in $VERSION, and nobody has
    booted it on this model yet."
}

keep_bootbin_on_esp() {
  local why=$1 target keep uuid
  if target=$(esp_bootbin); then
    keep=$target.before-$VERSION
    $sudo test -f "$keep" || $sudo cp "$target" "$keep" ||
      die "could not keep a copy of $target; nothing was installed"
    uuid=$(findmnt -no PARTUUID --target "${target%/m1n1/boot.bin}")
    warn "$why If the Mac stops in m1n1 after this install (m1n1
    text on screen, often \"No valid payload found\", and no boot menu), put the
    boot loader it booted with back from macOS:
      1. Hold the power button until the Mac turns off, then press and hold it
         again for the startup options, and start macOS (or Options, then
         Utilities > Terminal).
      2. In Terminal, run: diskutil list
         Find the partition whose UUID is $uuid (any case) with:
           diskutil info diskNsM | grep -i 'partition uuid'
      3. sudo diskutil mount diskNsM   (it prints the /Volumes path)
      4. cp -X '<that path>/m1n1/boot.bin.before-$VERSION' '<that path>/m1n1/boot.bin'
    The copy is kept at $keep. Either way, please report it at
    https://github.com/iconidentify/aurora-linux/issues"
    return 0
  fi
  die "could not find m1n1's boot.bin on the EFI partition to keep a copy of; nothing was installed"
}

current_kernel() {
  if pacman -Q linux-aurora >/dev/null 2>&1; then echo linux-aurora
  elif pacman -Q linux-asahi >/dev/null 2>&1; then echo linux-asahi
  fi
}

preflight() {
  [[ $(uname -m) == aarch64 ]] || die "this is not an aarch64 machine"
  grep -qa 'apple,' /proc/device-tree/compatible 2>/dev/null || die "this is not an Apple Silicon Mac"
  command -v pacman >/dev/null || die "pacman not found; this is for Omarchy on Arch Linux ARM"
  [[ -n $(current_kernel) ]] || die "neither linux-asahi nor linux-aurora is installed; this Mac's kernel is not one this script replaces"
  [[ $(boot_chain) != unknown ]] || die "could not find GRUB or Limine on this Mac; not touching its boot setup"
}

remove_pin() {
  $sudo sed -i "/^$(sed 's/[][\/.*^$]/\\&/g' <<<"$PIN_BEGIN")\$/,/^$PIN_END\$/d" /etc/pacman.conf
}

add_pin() {
  remove_pin
  # IgnorePkg lines accumulate, so any existing IgnorePkg stays in force.
  $sudo sed -i "/^\[options\]/a $PIN_BEGIN\nIgnorePkg = $PINNED\n$PIN_END" /etc/pacman.conf
}

snapshot() {
  if command -v snapper >/dev/null && $sudo snapper list-configs 2>/dev/null | grep -q '^root'; then
    say "Taking a snapper snapshot"
    $sudo snapper -c root create -c important -d "before $1" || warn "snapshot failed; continuing"
  fi
}

# /boot has to hold the fallback copy of the old kernel and the new kernel's
# image and initramfs; checked before anything changes.
boot_space() {
  local kernel=$1 need free
  need=$(du -cm "/boot/vmlinuz-$kernel" "/boot/initramfs-$kernel.img" 2>/dev/null | tail -1 | cut -f1)
  [[ -f $STATE/previous ]] && need=0
  # Room for linux-aurora's image and initramfs, unless it is already installed.
  if [[ $kernel == linux-aurora ]]; then need=$(( need + 20 )); else need=$(( need + 90 )); fi
  free=$(df -m --output=avail /boot | tail -1 | tr -d ' ')
  (( free >= need )) || die "/boot has ${free} MB free and this needs about ${need} MB (the old kernel kept as a fallback plus the new one).
Free space in /boot first (old test kernels, for example), then run this again. Nothing was changed."
}

# GRUB: keep the running kernel bootable from the menu, whatever pacman removes.
keep_grub_fallback() {
  local kernel=$1 release="" dir root_uuid subvol cmdline linux initrd
  # The modules that belong to the package being replaced (not necessarily
  # the running kernel, which may be a hand-installed test kernel).
  for dir in /usr/lib/modules/*/; do
    [[ $(cat "${dir}pkgbase" 2>/dev/null) == "$kernel" ]] && release=$(basename "$dir")
  done
  [[ -n $release ]] || { warn "could not find the modules of $kernel; no fallback entry"; return 0; }
  [[ -f /boot/vmlinuz-$kernel && -f /boot/initramfs-$kernel.img ]] || { warn "no /boot/vmlinuz-$kernel to keep as a fallback"; return 0; }
  $sudo install -d "$STATE"
  if ! $sudo cp /boot/vmlinuz-$kernel /boot/vmlinuz-aurora-sep-previous ||
    ! $sudo cp /boot/initramfs-$kernel.img /boot/initramfs-aurora-sep-previous.img; then
    $sudo rm -f /boot/vmlinuz-aurora-sep-previous /boot/initramfs-aurora-sep-previous.img
    die "could not copy the previous kernel to /boot (out of space?). Nothing else was changed."
  fi
  # pacman removes the old kernel's modules; keep the running kernel's for the fallback.
  $sudo cp -a "/usr/lib/modules/$release" "$STATE/modules-$release"
  # The fallback's device trees must never be what update-m1n1 picks.
  $sudo rm -rf "$STATE/modules-$release/dtbs"
  echo "$kernel $release" | $sudo tee "$STATE/previous" >/dev/null
  # kernel-modules-hook's linux-modules-cleanup.service deletes every
  # /usr/lib/modules tree that no package owns and that is not the running
  # kernel, so the fallback's modules live in $STATE and are put back only when
  # the fallback kernel itself boots, before anything loads a module.
  $sudo tee /etc/systemd/system/aurora-sep-fallback-modules.service >/dev/null <<EOF
[Unit]
Description=Restore the modules of the pre-aurora-sep fallback kernel
DefaultDependencies=no
ConditionKernelVersion=$release
ConditionPathExists=!/usr/lib/modules/$release/modules.dep
After=systemd-remount-fs.service
Before=systemd-modules-load.service systemd-udevd.service systemd-udev-trigger.service linux-modules-cleanup.service

[Service]
Type=oneshot
ExecStart=/usr/bin/cp -a $STATE/modules-$release /usr/lib/modules/$release

[Install]
WantedBy=sysinit.target
EOF
  $sudo systemctl daemon-reload
  $sudo systemctl enable aurora-sep-fallback-modules.service

  root_uuid=$(findmnt -no UUID /)
  subvol=$(findmnt -no FSROOT /)
  cmdline=$(sed -e 's/BOOT_IMAGE=[^ ]*//' /proc/cmdline)
  if [[ $(findmnt -no FSTYPE /boot) == vfat ]]; then
    linux=/vmlinuz-aurora-sep-previous initrd=/initramfs-aurora-sep-previous.img
    search="search --no-floppy --fs-uuid --set=root $(findmnt -no UUID /boot)"
  else
    linux="${subvol%/}/boot/vmlinuz-aurora-sep-previous" initrd="${subvol%/}/boot/initramfs-aurora-sep-previous.img"
    search="search --no-floppy --fs-uuid --set=root $root_uuid"
  fi
  $sudo tee /etc/grub.d/42_aurora_sep_previous >/dev/null <<EOF
#!/bin/sh
cat <<'MENU'
menuentry 'Previous kernel ($kernel $release, before aurora-sep)' --class omarchy --class gnu-linux --id $FALLBACK_ID {
    load_video
    set gfxpayload=keep
    insmod part_gpt
    insmod fat
    insmod btrfs
    $search
    linux $linux $cmdline
    initrd $initrd
}
MENU
EOF
  $sudo chmod 755 /etc/grub.d/42_aurora_sep_previous
}

# update-m1n1 takes the device trees of the highest-versioned *-ARCH kernel
# directory, which after this install can be the old linux-asahi's (a newer
# version number, kept for the fallback entry) without the Touch ID sensor node.
# Point it at whatever directory linux-aurora owns, now and after its updates.
m1n1_update() {
  local target conf=$UPDATE_M1N1_CONF tmp
  [[ -f $conf && ! -f $STATE/update-m1n1.default.saved ]] &&
    $sudo cp "$conf" "$STATE/update-m1n1.default.saved"
  # Replace only the DTBS setting and keep the rest of the file: a MacBook
  # Neo's M1N1= and U_BOOT= point at its own J700 builds, and dropping them
  # would rebuild boot.bin from an m1n1 that cannot boot it.
  tmp=$(mktemp)
  if [[ -f $conf ]]; then
    $sudo sed -e '/^# aurora-sep: build m1n1/,/^DTBS=/d' -e '/^DTBS=/d' "$conf" >"$tmp"
  fi
  cat >>"$tmp" <<'EOF'
# aurora-sep: build m1n1's stage 2 from the device trees the installed
# linux-aurora package owns, which carry the Touch ID sensor node.
#
# Not from a module directory whose pkgbase says linux-aurora: right after an
# upgrade the running kernel's modules are restored unowned but still carry that
# pkgbase, so both kernels' DTBs would be bundled. m1n1 keeps the last matching
# DTB, and the glob sorts 11.10 before 11.9, so the stale one can win.
# (update-m1n1 runs under set -e, so this assignment must always succeed.)
DTBS=$(pacman -Qlq linux-aurora 2>/dev/null | grep '/dtbs/[^/]*\.dtb$'; true)
EOF
  $sudo install -m 644 "$tmp" "$conf"
  rm -f "$tmp"
  if [[ ! -f $STATE/boot.bin.saved ]] && target=$(esp_bootbin); then
    $sudo cp "$target" "$STATE/boot.bin.saved"
  fi
  # update-m1n1 exits without a word while this is set; say so rather than
  # claim a rebuild.
  if update_m1n1_frozen; then
    warn "$conf sets M1N1_UPDATE_DISABLED, so m1n1's boot.bin was not rebuilt;
    it keeps the m1n1 and device trees it already had"
    return 0
  fi
  say "Rebuilding m1n1 with the aurora device trees"
  $sudo update-m1n1 || die "update-m1n1 failed; the previous boot.bin is saved in $STATE/boot.bin.saved"
}

grub_update() {
  # mkinitcpio's hook installs /boot/vmlinuz-linux-aurora; a preset whose
  # kernel is gone would make later mkinitcpio -P runs fail.
  if [[ -f /etc/mkinitcpio.d/linux-asahi.preset ]] && ! pacman -Q linux-asahi >/dev/null 2>&1; then
    $sudo mv /etc/mkinitcpio.d/linux-asahi.preset "$STATE/linux-asahi.preset.saved"
  fi
  say "Regenerating the GRUB menu"
  $sudo grub-mkconfig -o /boot/grub/grub.cfg

  # Select the linux-aurora entry by its id, never by position. Entry 0 is
  # whichever kernel grub-mkconfig considers newest, and a Mac that has been
  # used for testing often has hand-installed vmlinuz-* files in /boot with no
  # matching initramfs. grub-mkconfig happily writes those as entries with no
  # initrd line at all, and booting one hangs before it can mount root.
  # grub.cfg can be root-only, so read it through sudo like the ESP check above.
  # grub-mkconfig puts every kernel but its idea of the newest inside the
  # "Advanced options" submenu, and a bare id only resolves against top-level
  # entries: setting one that lives in the submenu silently falls back to entry
  # 0. The documented way to reach it is the submenu>entry path, and it has to
  # be quoted in /etc/default/grub or the > is a shell redirect.
  local entry submenu target
  entry=$($sudo grep -oE 'gnulinux-linux-aurora-advanced-[0-9a-f-]+' /boot/grub/grub.cfg | head -1)
  [[ -n $entry ]] || die "grub.cfg has no linux-aurora entry; the previous kernel is still the fallback entry"
  submenu=$($sudo grep -oE 'gnulinux-advanced-[0-9a-f-]+' /boot/grub/grub.cfg | head -1)
  if [[ -n $submenu ]]; then target="$submenu>$entry"; else target="$entry"; fi

  if ! grep -qxF "GRUB_DEFAULT=\"$target\"" /etc/default/grub; then
    [[ -f $STATE/grub.default.saved ]] || $sudo cp /etc/default/grub "$STATE/grub.default.saved"
    if grep -qE '^GRUB_DEFAULT=' /etc/default/grub; then
      $sudo sed -i "s|^GRUB_DEFAULT=.*|GRUB_DEFAULT=\"$target\"|" /etc/default/grub
    else
      echo "GRUB_DEFAULT=\"$target\"" | $sudo tee -a /etc/default/grub >/dev/null
    fi
    $sudo grub-mkconfig -o /boot/grub/grub.cfg
  fi

  # Prove the default resolves to an entry that can actually boot: the path
  # must be written out, and that entry must carry both a kernel and an initrd.
  # A vmlinuz-* in /boot with no matching initramfs generates an entry with no
  # initrd line at all, which hangs before it can mount root.
  $sudo grep -qF "set default=\"$target\"" /boot/grub/grub.cfg ||
    die "GRUB's default entry is not linux-aurora. Not leaving this Mac pointing at an unbootable default: pick 'Omarchy Linux, with Linux linux-aurora' from the menu, or run --uninstall."
  $sudo awk -v id="$entry" '
    /^[\t ]*menuentry/ && index($0, id) { found = 1 }
    found && /vmlinuz-linux-aurora/ { k = 1 }
    found && /initramfs-linux-aurora\.img/ { i = 1 }
    found && /^[\t ]*}/ { exit }
    END { exit !(k && i) }' /boot/grub/grub.cfg ||
    die "the linux-aurora GRUB entry is missing its kernel or initramfs line; refusing to make it the default"

  # Stray hand-installed kernels poison the auto-generated top entry.
  local stray="" k n
  for k in /boot/vmlinuz-*; do
    n=${k#/boot/vmlinuz-}
    [[ -e /boot/initramfs-$n.img ]] || stray+=" $n"
  done
  [[ -z $stray ]] ||
    warn "these kernels in /boot have no initramfs and boot to a black screen if picked:$stray"
}

calibration() {
  local node name="" out board dtb
  # The driver reads firmware-name from the device tree it boots, which is the
  # aurora one installed above. When this runs from linux-asahi, the running
  # tree has no sensor node at all, so ask the installed aurora DTB first.
  board=$(tr '\0' '\n' </proc/device-tree/compatible 2>/dev/null | sed -n '1s/^apple,//p')
  if [[ -n $board ]]; then
    dtb=$(pacman -Qlq linux-aurora 2>/dev/null | grep -m1 "/dtbs/t[0-9]*-$board\.dtb$" || true)
    [[ -n $dtb && -f $dtb ]] &&
      name=$(grep -aoE -m1 'apple/mesacal-[A-Za-z0-9_-]+\.bin' "$dtb" | head -1 || true)
  fi
  if [[ -z $name ]]; then
    for node in /proc/device-tree/soc*/spi*/*/ /proc/device-tree/*/spi*/*/; do
      [[ -f $node/compatible ]] && grep -qa mesa "$node/compatible" || continue
      [[ -f $node/firmware-name ]] && name=$(tr -d '\0' <"$node/firmware-name")
      break
    done
  fi
  name=${name:-apple/mesa_calibration.bin}
  out=/usr/lib/firmware/$name
  if [[ -s $out ]]; then
    say "Touch ID calibration already present: $out"
    return 0
  fi
  say "Extracting this Mac's Touch ID calibration (read-only) to $out"
  $sudo install -d -m 755 "$(dirname "$out")"
  $sudo /usr/lib/aurora-touchid/extract-mesa-calibration -o "$out" ||
    warn "could not extract the calibration; Touch ID will not work until it is present at $out"
}

# The first line in modprobe.d that blacklists or redirects apple_sep, other
# than our own file. Either spelling is the owner saying "do not load this".
sep_block_line() {
  local f
  for f in /etc/modprobe.d/*.conf /run/modprobe.d/*.conf /usr/lib/modprobe.d/*.conf; do
    [[ -f $f && $f != "$MODPROBE_CONF" ]] || continue
    grep -m1 -HnE '^[[:space:]]*(blacklist|install)[[:space:]]+apple[-_]sep([[:space:]]|$)' "$f" 2>/dev/null &&
      return 0
  done
  return 0
}

# With its defaults the driver provisions an identity keybag and writes to the
# enclave's anti-replay store (xART) on the first boot of a Mac that has a
# Touch ID profile -- no enrolment needed. That is the intended behaviour, but
# the owner has to be able to opt out, and a block they already set must win.
sep_policy() {
  local block
  block=$(sep_block_line)
  if [[ -n $block ]]; then
    warn "apple_sep is blocked on this Mac ($block).
    Leaving it that way: the SEP service is not enabled. Note that a plain
    'blacklist' line would NOT have stopped the service, which loads the driver
    by name; that is why this script checks for it."
    $sudo systemctl disable apple-sep.path apple-sep.service 2>/dev/null || true
    return 0
  fi
  if (( READ_ONLY )); then
    printf '%s\n' \
      "# aurora-sep --read-only: the driver attaches and reports, but never writes" \
      "# to the enclave. Delete this file to allow enrolment." \
      "options apple_sep xart_writes=0 provision_keybag=0" |
      $sudo tee "$MODPROBE_CONF" >/dev/null
    say "Read-only: the SEP driver will attach but not write to the enclave ($MODPROBE_CONF)"
  elif [[ -f $MODPROBE_CONF ]]; then
    # Re-running to update must never quietly turn writes back on.
    say "Still read-only from an earlier --read-only install; delete $MODPROBE_CONF to allow enrolment"
  fi
  $sudo systemctl enable apple-sep.path apple-sep.service
}

sep_write_notice() {
  (( READ_ONLY )) && return 0
  [[ -f $MODPROBE_CONF || -n $(sep_block_line) ]] && return 0
  warn "on the first boot of a Mac with a Touch ID profile, the SEP driver creates
    an identity keybag and writes to the enclave's anti-replay store. Use a Mac
    you can DFU-restore. If you cannot, press Ctrl-C now and run with --read-only:
      curl -fsSL $LATEST_URL | bash -s -- --read-only"
  return 0
}

# omarchy-ane-dkms builds its own Neural Engine modules into updates/dkms,
# which modprobe prefers over the ones this kernel ships. Say so rather than
# remove it: it also carries the M2 firmware fetch its owner may rely on.
ane_dkms_notice() {
  pacman -Q omarchy-ane-dkms >/dev/null 2>&1 || return 0
  warn "omarchy-ane-dkms is installed. Its Neural Engine modules take precedence
    over the driver built into this kernel, so a test would exercise that
    package's driver instead. To test this kernel's driver, remove it first:
      sudo pacman -R omarchy-ane-dkms
    Removing it also removes the Neural Engine firmware it fetched on an M2
    Pro/Max."
}

# The MacBook Neo's Wi-Fi and Bluetooth need this Neo's own firmware,
# calibration and country files from its macOS; none ship with Linux, and
# another unit's files can't be substituted. Report what is there without
# failing the install: the kernel runs without them, the radios stay off.
# File names follow Documentation/networking/device_drivers/wifi/mt7932-neo.rst.
neo_radio_notice() {
  local fw=/usr/lib/firmware/mediatek f missing="" mac cc=""
  for f in IZUBA_WIFI_MT7932_patch_mcu_1_2_hdr.bin IZUBA_W7932_2.bin ppr.bin \
    config-original.bin wcal.bin oca2.bin; do
    [[ -s $fw/mt7932/$f ]] || missing+=" mt7932/$f"
  done
  # The policy is per country with no fallback to the world file once a
  # country is set; world-XZ.bin only covers the unset ("00") case.
  if command -v iw >/dev/null; then
    cc=$(iw reg get 2>/dev/null | awk '$1=="global"{g=1; next} g && $1=="country"{sub(":","",$2); print $2; exit}')
  fi
  if [[ -z $cc || $cc == 00 ]]; then
    [[ -s $fw/mt7932/policy/world-XZ.bin ]] || missing+=" mt7932/policy/world-XZ.bin"
  else
    [[ -s $fw/mt7932/policy/$cc.bin ]] || missing+=" mt7932/policy/$cc.bin (country $cc)"
  fi
  mac=$(find /proc/device-tree -path '*wifi*' -name local-mac-address 2>/dev/null | head -1)
  if [[ -n $mac ]] && od -An -tx1 "$mac" | grep -q '[1-9a-f]'; then
    say "Wi-Fi MAC address: provided by this Neo's m1n1"
  else
    warn "m1n1 did not provide the Wi-Fi MAC address (wifi0 local-mac-address); Wi-Fi will refuse to start"
  fi
  if [[ -z $missing ]]; then
    say "Wi-Fi firmware, calibration and country files: all present under $fw/mt7932"
  else
    warn "Wi-Fi needs this Neo's own files under $fw, and these are missing:$missing
    They come from this Neo's own macOS and can't be shared between machines.
    See Documentation/networking/device_drivers/wifi/mt7932-neo.rst in
    iconidentify/aurora-linux. Wi-Fi stays off until they are all there."
  fi
  say "MacBook Neo, read before you reboot:
    - Sleep isn't supported on the Neo yet. While Wi-Fi/Bluetooth support is
      active (the default) the kernel refuses suspend, so closing the lid does
      nothing: shut down instead of putting it in a bag.
    - Wi-Fi works on 2.4 GHz and on 5 GHz channels 36-48 only, with WPA2 (AES)
      or open networks. 5 GHz networks on channels 149-165 won't be listed.
    - Bluetooth is off by default in this release."
}

# The packages this Mac gets, one "file sha256" per line: PACKAGES, except
# that a Neo keeps its own m1n1, and an M3 gets M3_M1N1_PACKAGE on the handoff
# path and no m1n1 otherwise. Needs m3_plan first.
packages_for_this_mac() {
  local entry
  for entry in "${PACKAGES[@]}"; do
    if [[ $entry == m1n1-aurora-* ]] && { is_neo || [[ $M3_MODE != none ]]; }; then continue; fi
    echo "$entry"
  done
  if [[ $M3_MODE == handoff ]]; then echo "$M3_M1N1_PACKAGE"; fi
  return 0
}

install_all() {
  local entry file sha kernel chain
  local -a entries
  version_notice
  sep_write_notice
  ane_dkms_notice
  kernel=$(current_kernel)
  chain=$(boot_chain)
  if [[ $chain == grub ]]; then boot_space "$kernel"; fi
  m3_plan
  work=$(mktemp -d)
  trap 'rm -rf "${work:-}"' EXIT
  if is_neo; then say "Keeping this MacBook Neo's own m1n1 (m1n1-aurora has no T8140 support)"; fi
  if [[ $M3_MODE == kernel ]]; then say "Keeping this M3's own m1n1 and boot.bin"; fi
  mapfile -t entries < <(packages_for_this_mac)
  for entry in "${entries[@]}"; do
    read -r file sha <<<"$entry"
    say "Downloading $file"
    curl -fL --retry 3 --progress-bar -o "$work/$file" "$RELEASE_URL/$file" ||
      die "could not download $file from $TAG.
    The release is missing a file this script expects, which is a packaging
    mistake rather than anything wrong with this Mac. Nothing was installed.
    Please report it with the file name above."
    [[ $(sha256sum "$work/$file" | cut -d' ' -f1) == "$sha" ]] || die "$file does not match its published checksum"
  done
  if [[ $M3_MODE == handoff ]] && ! m1n1_pkg_has_handoff "$work/${M3_M1N1_PACKAGE%% *}"; then
    die "this release's M3 m1n1 has no M3 Pro display and GPU handoff, so it can't switch it
    on. Nothing was installed. Please report it at https://github.com/iconidentify/aurora-linux/issues"
  fi

  snapshot "aurora-sep $VERSION"
  # Before pacman's update-m1n1 hook rebuilds boot.bin below.
  is_neo || unproven_board_backup
  if [[ $M3_MODE == handoff ]]; then
    keep_bootbin_on_esp "This replaces the boot loader of this M3 Pro ($(this_board)) with
    m1n1-aurora and its display and GPU handoff."
  fi
  $sudo install -d "$STATE"
  if [[ ! -f $STATE/previous && $chain == grub ]]; then
    keep_grub_fallback "$kernel"
  fi
  pacman -Q "$kernel" | $sudo tee "$STATE/previous-package" >/dev/null
  if [[ $M3_MODE != none ]]; then echo "$M3_MODE" | $sudo tee "$STATE/m3-mode" >/dev/null; fi
  # Before pacman's update-m1n1 hook runs on the kernel's device trees.
  case $M3_MODE in
    kernel)
      M3_BOOTBIN_SHA=$(m3_bootbin_sha)
      m3_freeze
      ;;
    # So the hook's rebuild, where it runs, already carries the switches.
    handoff) m3_switches_write ;;
  esac

  # linux-aurora-headers pulls in pahole, and fprintd below comes from the
  # repositories. On a Mac whose package database has gone stale, pacman
  # resolves those to versions the mirror no longer carries and the whole
  # transaction dies with a 404 after the packages are already downloaded.
  say "Refreshing the package database"
  $sudo pacman -Sy --noconfirm || warn "could not refresh the package database; continuing"

  say "Installing the aurora-sep kernel, libfprint with the Apple SEP driver, fprintd and aurora-touchid"
  # --ask 4 accepts replacing linux-asahi (and its headers), which linux-aurora conflicts with.
  $sudo pacman -U --noconfirm --ask 4 "$work"/*.pkg.tar.zst
  $sudo pacman -S --needed --noconfirm fprintd
  # linux-aurora carries the Apple video decoder, whose firmware linux-asahi
  # installs never needed; without it the decoder fails to load at boot.
  $sudo pacman -S --needed --noconfirm avd-fw ||
    warn "could not install avd-fw; hardware video decode will not work until it is installed"
  # The VA-API bridge to that decoder. Without it, players fall back to
  # software decode with no error (reported on a 16" M1 Pro installed from the
  # Omarchy Mac ISO). Leave any other build of the bridge alone: the AUR
  # libva-v4l2_request packages conflict with it.
  if ! pacman -Qq libva-v4l2_request-avd libva-v4l2_request >/dev/null 2>&1 &&
    [[ ! -e /usr/lib/dri/v4l2_request_drv_video.so ]]; then
    $sudo pacman -S --needed --noconfirm libva-v4l2_request-avd ||
      warn "could not install libva-v4l2_request-avd; video players will decode in software until it is installed"
  fi
  add_pin
  # Both boot chains boot through m1n1, and both need the aurora device trees
  # in boot.bin: update-m1n1 otherwise takes the DTBs of the highest-versioned
  # kernel directory, which on a Mac with an older hand-installed test kernel
  # is not this one -- and those DTBs lack the Touch ID sensor node. pacman's
  # own hook already ran update-m1n1 during the install above, before this
  # configuration existed, so run it again now that it is in place.
  # A kernel-only M3 keeps the boot.bin it has. On the handoff path, the
  # freezes kept the hook from rebuilding boot.bin before the new m1n1 was in
  # place; lift them only now (see m3_plan).
  case $M3_MODE in
    kernel) m3_bootbin_report ;;
    handoff)
      m3gpu_unfreeze
      m3_unfreeze
      m1n1_update
      m3_verify_bootbin
      ;;
    *) m1n1_update ;;
  esac
  if [[ $chain == grub ]]; then
    grub_update
  fi

  calibration
  $sudo systemctl daemon-reload
  sep_policy
  if is_neo; then neo_radio_notice; fi
  pacman -Q linux-aurora libfprint aurora-touchid
  echo
  if [[ -f $MODPROBE_CONF ]]; then
    say "Done, read-only. Reboot; the SEP driver will attach and report without writing."
    echo "   Enrolling a finger needs writes: delete $MODPROBE_CONF, reboot, then run aurora-touchid-setup."
  else
    say "Done. Reboot, then run:  aurora-touchid-setup"
  fi
  echo "   Testing this build? The plan and reporting format:"
  echo "      curl -fsSL $RELEASE_URL/install-aurora-sep.sh | bash -s -- --agent-prompt"
  if [[ $chain == grub ]]; then
    echo "   The previous kernel stays in the GRUB menu as 'Previous kernel … before aurora-sep'."
  fi
  echo "   To undo everything:    curl -fsSL $RELEASE_URL/install-aurora-sep.sh | bash -s -- --uninstall"
}

uninstall_all() {
  local previous=linux-asahi m3_mode=none
  [[ -f $STATE/previous-package ]] && read -r previous _ <"$STATE/previous-package"
  # 11.36 kept no record: every M3 it installed was kernel-only.
  if [[ -f $STATE/m3-mode ]]; then
    read -r m3_mode _ <"$STATE/m3-mode"
  elif is_m3; then
    m3_mode=kernel
  fi
  command -v aurora-touchid-setup >/dev/null && aurora-touchid-setup --remove || true
  $sudo systemctl disable apple-sep.path apple-sep.service 2>/dev/null || true
  remove_pin
  snapshot "removing aurora-sep"
  local m1n1=m1n1
  if is_neo; then
    m1n1=
    say "Reinstalling $previous and the stock libfprint; this MacBook Neo keeps its own m1n1"
  elif [[ $m3_mode == kernel ]]; then
    m1n1=
    say "Reinstalling $previous and the stock libfprint; this M3 keeps its m1n1 and boot.bin as they are"
  else
    say "Reinstalling $previous, the stock m1n1 and the stock libfprint"
  fi
  $sudo pacman -Rdd --noconfirm aurora-touchid 2>/dev/null || true
  # The stock m1n1 has no M3 handoff; drop the switches before its rebuild.
  m3_switches_remove
  $sudo pacman -Sy --noconfirm --ask 4 "$previous" "$previous-headers" libfprint $m1n1
  # Restore the stock update-m1n1 configuration on either chain before the
  # rebuild below, so boot.bin goes back to the packaged m1n1 and DTBs.
  # A kernel-only M3's boot.bin and update-m1n1 configuration were never
  # changed; only this script's freeze comes off, at the end.
  if [[ $(boot_chain) != grub && $m3_mode != kernel ]]; then
    if [[ -f $STATE/update-m1n1.default.saved ]]; then
      $sudo cp "$STATE/update-m1n1.default.saved" "$UPDATE_M1N1_CONF"
    else
      $sudo rm -f "$UPDATE_M1N1_CONF"
    fi
    $sudo update-m1n1 || warn "update-m1n1 failed; boot.bin still has the aurora m1n1"
  fi

  if [[ $(boot_chain) == grub ]]; then
    [[ -f $STATE/linux-asahi.preset.saved && ! -f /etc/mkinitcpio.d/linux-asahi.preset ]] &&
      $sudo mv "$STATE/linux-asahi.preset.saved" /etc/mkinitcpio.d/linux-asahi.preset
    [[ -f $STATE/grub.default.saved ]] && $sudo cp "$STATE/grub.default.saved" /etc/default/grub
    if [[ $m3_mode != kernel ]]; then
      if [[ -f $STATE/update-m1n1.default.saved ]]; then
        $sudo cp "$STATE/update-m1n1.default.saved" "$UPDATE_M1N1_CONF"
      else
        $sudo rm -f "$UPDATE_M1N1_CONF"
      fi
      $sudo update-m1n1
    fi
    $sudo systemctl disable aurora-sep-fallback-modules.service 2>/dev/null || true
    $sudo rm -f /etc/systemd/system/aurora-sep-fallback-modules.service
    $sudo rm -f /etc/grub.d/42_aurora_sep_previous /boot/vmlinuz-aurora-sep-previous /boot/initramfs-aurora-sep-previous.img
    $sudo grub-mkconfig -o /boot/grub/grub.cfg
  fi
  case $m3_mode in
    kernel) m3_unfreeze ;;
    handoff) m3_restore_bringup ;;
  esac
  $sudo rm -f "$MODPROBE_CONF"
  $sudo rm -rf "$STATE"
  say "Done. Reboot to run $previous."
}

# --reset-touchid: start Touch ID over on this Mac. For a Mac whose stored
# identity keybag no longer loads, such as an M2 Pro/Max enrolled on system
# firmware 26.2 or earlier and then updated. Moves the SEP driver's state and
# fprintd's prints for the Apple sensor aside; the next boot creates a new
# keybag, and every finger is enrolled again. The old keybag stays in the
# enclave unused: there is no way to delete it.
#
# The keybag is last, so a reset that stops partway leaves it in place and
# the next boot does not create a second one next to the old Catacombs.
TOUCHID_STATE_FILES=(
  /var/lib/aurora-sep-host-state.bin
  /var/lib/apple-sep-catacomb-master.bin
  /var/lib/apple-sep-catacomb-owner.bin
  /var/lib/apple-sep-catacomb-user.bin
  /var/lib/aurora-sep-refkey.bin
  /var/lib/aurora-sep-refkey-v2.bin
  /var/lib/aurora-sep-keybag.bin
)

sep_diag() {
  local f
  for f in /sys/bus/platform/devices/*.sep/diag/"$1"; do
    [[ -r $f ]] && { cat "$f"; return; }
  done
  echo none
}

reset_touchid() {
  local yes=0 force=0 arg f d keybag found=0
  for arg in "$@"; do
    case $arg in
      --yes) yes=1 ;;
      --force) force=1 ;;
      *) die "unknown option $arg for --reset-touchid (--yes, --force)" ;;
    esac
  done
  grep -qa 'apple,' /proc/device-tree/compatible 2>/dev/null || die "this is not an Apple Silicon Mac"
  [[ -f $MODPROBE_CONF ]] &&
    die "this Mac was installed read-only ($MODPROBE_CONF): a new keybag needs enclave writes. Delete that file first if you mean to allow them."
  for f in "${TOUCHID_STATE_FILES[@]}"; do [[ -e $f ]] && found=1; done
  ((found)) || die "no Touch ID state on this Mac; nothing to reset"

  # Ask for sudo now, in the foreground: a password prompt under timeout
  # below could not read the terminal. Not sudo -v, which wants a password
  # unless every rule for the user is NOPASSWD (a wheel member's is not).
  if [[ -n $sudo ]]; then $sudo true || die "--reset-touchid needs sudo"; fi

  # Only a keybag the driver could not load is worth replacing. At boot the
  # driver reports keybag=present for any keybag file it finds; it loads the
  # keybag, and reports failed if that does not work, only when the sensor is
  # first opened. Until then touchid=unknown, so open it through fprintd once.
  keybag=$(sep_diag keybag)
  [[ $keybag == none ]] &&
    die "the Touch ID driver is not running (no SEP diagnostics). Boot the aurora kernel first."
  if [[ $(sep_diag touchid) == unknown ]]; then
    timeout --foreground 20 $sudo fprintd-list root >/dev/null 2>&1 || true
    keybag=$(sep_diag keybag)
  fi
  if [[ $keybag != failed ]]; then
    ((force)) || die "the Touch ID keybag is not broken (driver reports keybag=$keybag, touchid=$(sep_diag touchid)).
    A reset would throw away a working keybag. If Touch ID still fails and you mean it, run again with --reset-touchid --force."
    warn "resetting although the driver reports keybag=$keybag (--force)"
  fi

  warn "this removes every enrolled fingerprint on this Mac and creates a new Touch ID keybag at the next boot.
    Anything sealed with the Secure Enclave (kernel trusted keys) can no longer be unsealed.
    The old keybag stays in the Secure Enclave, unused; it cannot be deleted, and this cannot be undone once the Mac has rebooted.
    If Touch ID has failed only this once, reboot and try it again first: a passing error looks the same."
  if ((!yes)); then
    # Piped from curl, stdin is the script; ask on the terminal, if there is one.
    { : </dev/tty; } 2>/dev/null || die "no terminal to confirm on; run again with --reset-touchid --yes"
    local answer=""
    printf 'Type RESET to continue: ' >/dev/tty
    read -r answer </dev/tty || true
    [[ $answer == RESET ]] || die "not reset"
  fi

  # Until the reboot nothing may touch the sensor: a match or enrolment would
  # write new state behind the move. The runtime mask goes away at boot. A
  # verify already running finishes its save within about a second.
  $sudo systemctl mask --runtime --now fprintd.service
  sleep 3

  # Outside /var/lib/aurora-sep, which --uninstall removes. Each move is
  # recorded so that a failure puts everything back as it was.
  local dest src to moved=() sources=()
  dest=/var/lib/aurora-sep-touchid-reset-$(date +%Y%m%d-%H%M%S)
  # fprintd's directories are root-only, so look for them as root.
  while IFS= read -r d; do sources+=("$d"); done < <(
    $sudo find /var/lib/fprint -mindepth 2 -maxdepth 2 -type d -name apple-sep 2>/dev/null)
  for f in "${TOUCHID_STATE_FILES[@]}"; do [[ -e $f ]] && sources+=("$f"); done
  for src in "${sources[@]}"; do
    case $src in
      /var/lib/fprint/*) to=$dest/fprint/$(basename "$(dirname "$src")")/apple-sep ;;
      *) to=$dest/$(basename "$src") ;;
    esac
    if ! { $sudo mkdir -p "$(dirname "$to")" && $sudo mv "$src" "$to"; }; then
      for d in "${moved[@]}"; do $sudo mv "${d#*|}" "${d%%|*}" || warn "could not put back ${d%%|*} from ${d#*|}"; done
      $sudo systemctl unmask --runtime fprintd.service || true
      die "could not move $src; put back everything already moved, so Touch ID is as it was"
    fi
    moved+=("$src|$to")
  done
  say "Moved the Touch ID state to $dest"
  say "Reboot now. Touch ID creates its new keybag during boot; then enrol your fingers again."
}

# Printed by --agent-prompt, and pointed at from the end of a successful
# install. This is written for an agent driving the test on a real Mac: it says
# what to establish, what counts as a pass, and how to write it up.
agent_prompt() {
  cat <<'PROMPT'
You are testing an experimental Secure Enclave / Touch ID kernel on an Apple
Silicon Mac. Work through this in order and report at the end. Do not skip the
identification step: nearly every past report was unusable because the machine
was not pinned down.

SAFETY, NON-NEGOTIABLE
  - This touches the Secure Enclave. Confirm the owner can DFU-restore this Mac
    before you start.
  - A default install is NOT read-only. On the first boot of a Mac with a
    Touch ID profile, the SEP driver loads automatically, creates an identity
    keybag and writes to the enclave's anti-replay store (xART) -- before
    anyone enrols a finger. If the owner cannot DFU-restore, install with
    --read-only instead, before the first reboot:
      curl -fsSL https://github.com/iconidentify/aurora-linux/releases/latest/download/install-aurora-sep.sh | bash -s -- --read-only
    A 'blacklist apple_sep' line does not prevent this on its own; the
    installer detects one and leaves the driver off.
  - Do not run any enrol/delete/re-provision loop unless the owner asks for
    it in writing: repeated cycles drift a device-wide counter.
  - Do not paste key material, serial numbers, or the contents of
    mesa_calibration.bin into a report.

0. CONFIRM YOU HAVE THE CURRENT BUILD
   Results against a superseded build waste everyone's time, and a saved copy
   of this script installs its own packages forever. This script checks on
   every install and prints either "<tag> is the current release" or a warning
   naming the newer one. To check without installing:

     curl -fsSL https://api.github.com/repos/iconidentify/aurora-linux/releases/latest \
       | grep -m1 '"tag_name"' 

   Always fetch the script from the "latest" URL rather than a tag you were
   handed, so you get the current one automatically:

     curl -fsSL https://github.com/iconidentify/aurora-linux/releases/latest/download/install-aurora-sep.sh | bash

   State the tag you installed in your report. If you were given a specific
   older tag on purpose, say so and say why.

1. IDENTIFY THE MACHINE
     tr -d '\0' < /proc/device-tree/compatible; echo
     for p in os-fw-version system-fw-version iboot2-version; do
       printf '%s = ' "$p"
       tr -d '\0' < /proc/device-tree/chosen/asahi,$p 2>/dev/null; echo
     done
     uname -r; pacman -Q linux-aurora m1n1-aurora libfprint aurora-touchid
   asahi,system-fw-version is the sepOS the enclave actually runs and is the
   single most useful line in the whole report. asahi,os-fw-version is only the
   stub. Report both; they are often different.

2. CONFIRM THE BOOTLOADER GUARD (M2 and later)
   In the m1n1 stage 2 boot log, expect:
     SEP: Preserving iBoot warm registration; seeding RNG from ADT
   and expect NO line reading "SEP: couldn't get enough random bytes".
   You should also see the OS identity forwarded:
     FDT: apfs-preboot-uuid = '<uuid>'
   On M1 neither line is expected: the enclave is cold and m1n1 talks to it
   normally. If you see the "couldn't get enough random bytes" line on any
   machine, the stock m1n1 is still in boot.bin - say so and stop, because every
   later result is then measuring the wrong thing.
   On an M3 installed kernel-only (step 11), the boot loader is the one the Mac
   already had, so this step does not apply: say "M3, kernel-only" instead.

3. DRIVER ATTACH
   The driver is already loaded at boot by apple-sep.service; do not reload
   it. Its write mode was fixed at install time. Read what it did:
     sudo dmesg | grep -iE 'apple_sep|apple-mesa'
   The xART line ends "writes ENABLED" or "writes disabled" -- quote it, and
   say which you expected.
   PASS:  "attach: N endpoints advertised in M messages" with N >= 7, and
          /dev/sep-bio exists.
   FAIL:  "attach: 0 messages received but no endpoint advertised", or no
          apple_sep lines at all. Capture the whole block either way.
   If the profile line says a SoC you did not expect, report that verbatim.
   On every MacBook Pro M2 Pro/Max (J414s, J414c, J416s, J416c) it reads
   "T6020/J414s", or "T6020/J414s (13.5 key store)" on system firmware 26.2
   or earlier; on the MacBook Pro 14"/16" M1 Max (J314c, J316c)
   "T6000/J316s", and on the MacBook Air 13" M2 (J413) "T8112/J415". Those
   are expected. On an M2 Pro/Max, also quote the line that starts
   "M2 Pro/Max on system firmware".
   If "CREATE_KEYBAG" fails with status -13, quote that line too. Firmware
   up to 26.2 uses the 13.5 key store and later firmware variant 5. Variant
   5 works on 26.6.x and 27.0; 26.3 to 26.5 have not been tried, so a -13
   there is the report we need.
   Updating macOS from 26.2 or earlier to a later release after enrolling
   loses the enrolments: Touch ID then needs a new keybag. Say so if that
   is what happened. With the owner's agreement, start Touch ID over:
     curl -fsSL https://github.com/iconidentify/aurora-linux/releases/latest/download/install-aurora-sep.sh | bash -s -- --reset-touchid
   then reboot and enrol again.
   The sensor itself: "sudo dmesg | grep apple-mesa" should say the power line
   came "from the device node". Quote it if it says anything else.

4. IF ATTACH FAILED, GET THE MAILBOX STATE
     sudo busybox devmem 0x396408110 32   # A2I control (t600x/t602x)
     sudo busybox devmem 0x396408114 32   # I2A control
     grep -i mbox /proc/interrupts
   Those addresses are for t600x and t602x. The mailbox base differs per SoC,
   so take it from the device tree rather than a table:

     sudo cat /proc/device-tree/soc/mbox@*/reg 2>/dev/null | xxd | head -2
     # or read the sep_mbox node address from your board's dtsi:
     #   t8103            0x242408000  -> 0x242408110 / 0x242408114
     #   t8112            0x25e408000  -> 0x25e408110 / 0x25e408114
     #   t600x, t602x     0x396408000  -> 0x396408110 / 0x396408114
     #   t8140 (Neo)      0x282608000  (a v4 mailbox: report the base and
     #                    the /proc/interrupts lines, not these offsets)

   Bit 0 is the enable; a non-zero FIFO count with an unchanged read pointer
   means the enclave never took the message. Report the raw values, not your
   reading of them.

5. TOUCH ID END TO END (only if step 3 passed)
     aurora-touchid-setup
   Enrol one finger, then:
     fprintd-verify
   Then REBOOT and run fprintd-verify again without re-enrolling. Matching
   right after enrolment and matching after a reboot are different paths (the
   second restores the enrolment from disk), so report them separately.
   If a verify fails, the driver logs the step that ended it:
     sudo dmesg | grep -E 'verify:|scrd:|Touch ID|catacomb|matching unavailable'
   fprintd's "verify-unknown-error" is not a result on its own; quote those
   lines with it.
   Then check the consumers the owner actually cares about: sudo, polkit, and
   the lock screen. Report each as pass/fail separately - partial success here
   is the normal outcome and is worth knowing precisely.

6. THUNDERBOLT AND DISPLAYPORT - TEST THIS EVEN IF YOU CAME FOR TOUCH ID
   This build carries the Thunderbolt/USB4 display and PCIe work, and it needs
   hardware reports as much as the enclave does. Do not skip it because the
   machine has no dock: "no dock available" is itself a useful answer, and
   step 6a still applies.

   6a. The controllers (no dock needed)
         ls /sys/bus/platform/devices/ | grep cio
         sudo dmesg | grep -iE 'thunderbolt|usb4|acio|tunnel' | head -30
       Expect one *.cio device per controller, bound to thunderbolt-apple-acio.
       An EMPTY /sys/bus/thunderbolt/devices/ with nothing plugged in is
       normal: routers only appear when a device is attached. Do not report
       that as a failure.
       Anything behind a Thunderbolt 3 dock's PCIe controller -- Ethernet,
       storage, and on docks such as the CalDigit TS3 Plus the USB ports too --
       now comes up on M1, M1 Pro/Max and M2 Pro/Max. On the Pro/Max chips the
       kernel starts the tunnel itself when a dock is plugged in, and logs
         port ... cold init done, status 0x3 ...
       (pcie_apple.tunnel_kernel_init=0 turns that off). Displays, and USB on
       USB4 docks, do not need the PCIe tunnel. On the M2 MacBook Air/Pro 13"
       and on M3 the tunnel isn't supported yet, so this line is normal there:
         PCIe-C tunnel disabled: not initialized by m1n1 or the kernel
       Report it if you see it on an M1, M1 Pro/Max or M2 Pro/Max.

   6b. With a dock or a DisplayPort monitor, in this order:
         - attached at boot: does the display come up, and at what resolution
           and refresh rate
         - hot unplug: does the tunnel tear down cleanly, no hang, no stuck
           compositor
         - hot replug: does the display come back, same mode as before
         - repeat both of the above on the SECOND USB-C port, not just the
           first: the ports are separate controllers and have behaved
           differently
         - two displays or a dual-output dock, if you have one
         - M2 Pro/Max: two monitors plugged straight into two USB-C ports
           (no dock), attached at boot. Does each come up at its own native
           resolution? Quote "hyprctl monitors" (name, mode, and the port each
           is on). Then log out and back in, and repeat.
         - PCIe behind the dock: Ethernet, USB storage, card readers, and a
           keyboard and mouse on the dock's USB ports. Do they all work, and
           still work after a replug? Quote:
             lspci -nn
             sudo dmesg | grep -E 'cold init done|link up after|translation fault|HC died'
           Any "translation fault" or "HC died" line is a failure to report.
           For a full report, after plugging the dock in:
             curl -fsSLO https://raw.githubusercontent.com/iconidentify/aurora-linux/refs/tags/sep-7.1.12.aurora2-11.36.1/tools/aurora-tb/tb-pcie-report
             sudo sh tb-pcie-report --no-wait

   6c. Across suspend:
         systemctl suspend
       Then wake and re-check: display back, dock devices back, and
       /dev/sep-bio still present. Report anything that needed a replug to
       recover.

7. HARDWARE VIDEO DECODE
   Play an H.264 file and an HEVC file and confirm it is not silently falling
   back to software:
     sudo dmesg | grep -iE 'avd|apple-vpu' | tail -20
   Report which codecs you exercised and at what resolution.

8. ONE SUSPEND/RESUME CYCLE AT THE END
   Ask the owner to lock the screen and, while it waits for a fingerprint,
   close the lid. Then open it and unlock with the finger. Report whether
   the Mac woke and whether the finger unlocked it, and quote:
     sudo dmesg | grep -E 'Touch ID: ending|still running|PM: suspend'
   Re-check /dev/sep-bio, the display and the dock after resume.

9. NEURAL ENGINE
   This build carries the in-tree Apple Neural Engine driver
   (aurora-silicon/linux#155), switched on for the M1, M1 Pro, M1 Max, M2 Pro
   and M2 Max. On every other Mac no Neural Engine lines are expected, and that
   is a pass. The ANE now powers off about 1.5 s after its last use:
     cat /sys/bus/platform/drivers/ane*/*.ane/power/runtime_status
   should read "suspended" while nothing uses it (M1 family).
     pacman -Q omarchy-ane-dkms 2>/dev/null   # if installed, say so: its modules replace this kernel's
     modinfo -F filename ane ane_t6021
     ls -l /dev/accel/ 2>/dev/null
     sudo dmesg | grep -iE '\bane\b|ane_t6021|neural' | head -40
   M1, M1 Pro, M1 Max PASS: the ane module is bound, /dev/accel/accel0 exists,
   and the module path is under kernel/drivers/accel/ane (not updates/dkms).
   M2 Pro, M2 Max: without the Neural Engine firmware, expect a firmware load
   error and nothing else broken. With it (fetched by Joshua Warren's
   omarchy-ane-firmware-fetch), expect ane_t6021 bound and /dev/accel/accel0.
   The M2 Pro has not run it before this build: report it either way.
   If omarchy-ane's tools are installed, also run omarchy-ane-check, and on an
   M2 Pro/Max omarchy-ane-check --smoke; quote their result lines.
   Report any kernel log line at emergency or alert level, and whether idle
   battery drain changed against the previous build. If the Mac does not finish
   booting, add module_blacklist=ane,ane_t6021 to the kernel command line from
   the boot menu, and report that.

10. MACBOOK NEO (J700) ONLY
   The Neo's Wi-Fi is in the regular kernel now. It needs this Neo's own
   firmware, calibration and country files; the installer listed any that
   were missing. Never install another Neo's files.
     sudo dmesg | grep -iE 'mt7932|piodma|REGULATORY|CALIBRATION|admission'
     iw reg get | head -3
     nmcli device wifi list | head
   - Without the country file, expect
       REGULATORY_BLOCKED: <CC> generation=N error=-2 recovery-required=0
     and nothing else broken; quote it. After installing that file, retry a
     scan (nmcli device wifi rescan): it should come up without a reboot.
   - With every file present: does it scan, connect to a WPA2 network and
     pass traffic? Try a 2.4 GHz network and a 5 GHz one on channels 36-48.
     Networks on channels 149-165 are not listed, by design.
   - systemctl suspend is refused by design while the radios are active.
     Expect "sleep refused: Neo radio bootstrap retains DMA memory until full
     hardware reset" and quote it.
   - Bluetooth is off by default. Only test it if the owner asks (see
     Documentation/networking/device_drivers/wifi/mt7932-neo.rst).

   Report "not tested" honestly rather than guessing, for any of the above.

11. M3 (M3, M3 PRO, M3 MAX) ONLY
   Every M3 is experimental. The installer took one of two paths and said
   which near the start; the file below records it too:
     cat /var/lib/aurora-sep/m3-mode
   - kernel: the kernel only. m1n1's boot.bin was left exactly as it was,
     and the install ended with "M3: m1n1's boot.bin is unchanged". Expect
     the desktop on the boot framebuffer (one fixed resolution, software
     rendering).
   - handoff: an M3 Pro model whose m1n1 display and GPU handoff has been
     booted before. The installer replaced boot.bin, kept the old one as
     m1n1/boot.bin.before-<version> on the EFI partition, and printed the
     macOS steps to put it back. Expect the built-in display at its native
     resolution and the GPU in use.
   Check and quote:
     tr -d '\0' < /proc/device-tree/compatible; echo
     ls /sys/class/drm/
     sudo dmesg | grep -iE 'asahi.*(gpu|firmware)|apple-dcp|t6030' | head -20
     ls /proc/device-tree/soc/usb4-pcie-tunnel-0/pcie@730000000/pci@0,0/apple,tunable 2>&1
   Then: does it reach the desktop, and do Wi-Fi, Bluetooth, keyboard,
   trackpad, audio and one suspend/resume work? With a Thunderbolt dock, do
   its display and its PCIe devices (Ethernet, storage) come up? Report each
   one, including "not tested".

   ADDING AN M3 PRO MODEL TO THE HANDOFF
   Only an M3 Pro (apple,t6030: J514S 14", J516S 16") can try the handoff;
   m1n1 has none for the M3 or M3 Max yet, and the installer refuses there.
   The installer also refuses unless the Mac has the macOS 14.8.3
   system-firmware stub that the Omarchy installer gives every M3.
   This REPLACES THE MAC'S BOOT LOADER. If the new one does not start, the
   Mac stops before any boot menu, and the only way back is from macOS on
   the same Mac. So:
     - Do it only with the owner's explicit agreement, and only if the owner
       can start macOS on this Mac (hold the power button at startup).
     - Keep the restore steps the installer prints: they name this Mac's EFI
       partition and the saved boot.bin.before-<version>.
     - Do not try it unattended or over SSH alone: someone must be at the Mac
       for the first boot.
   Then, with the owner watching:
     curl -fsSL https://github.com/iconidentify/aurora-linux/releases/latest/download/install-aurora-sep.sh | bash -s -- --m3-handoff
   and reboot. Report one of:
     - It boots to the desktop on the built-in display at native resolution:
       give the checks above, and again after one suspend/resume. That
       report is what adds the model to the installer's list.
     - It stops in m1n1 (text on screen, no boot menu) or stays black: the
       owner puts the old boot.bin back from macOS with the printed steps.
       Report exactly what the screen showed and when.
     - It boots, but the display or GPU is wrong: collect
         sudo journalctl -b -k | grep -iE 'dcp|asahi|t6030|m1n1'
       over SSH if you can, then run the installer with --uninstall, which
       puts the stock m1n1 back, and reboot.
   Title such a report "<board> (<model>): M3 handoff works" or "... fails".

HOW TO REPORT
  Open one issue per Mac at https://github.com/iconidentify/aurora-linux/issues
  (not on omacom/linux#7 any more), with the first line of the report as its
  title. Post later results for the same Mac as comments on that issue.
  Structure it exactly like this:

    # <board> (<marketing name>): <one-line outcome>
    **Machine:** apple,jXXX / apple,tXXXX, <model>
    **Firmware:** asahi,os-fw-version = X, asahi,system-fw-version = Y
    **Build:** <release tag>, linux-aurora <ver>, m1n1-aurora <ver>
    **m1n1 guard:** fired / did not fire / stock m1n1 still installed
    **Touch ID:** enrol <pass/fail>, verify <pass/fail>, verify after reboot <pass/fail>
    **Thunderbolt:** <dock model, or "no dock"> on port <1 / 2 / both>
    **M3 path:** not an M3 / kernel / handoff / handoff with --m3-handoff

    ## What worked
    ## What did not
    ## Logs
    (fenced blocks, trimmed to the relevant lines - never a whole dmesg)

  Rules for the write-up:
    - Lead with the outcome, not the narrative.
    - Quote log lines exactly; do not paraphrase an error.
    - Say explicitly what you did NOT test. "No dock available" and "only
      tested port 1" are useful answers; silence is not.
    - Name the dock and the display mode. "CalDigit TS3 Plus, 3840x2160 @ 60"
      is actionable; "external display worked" is not.
    - If something failed, give the last known-good state and the first bad one.
    - Do not claim a fix works because it compiled or because the module
      loaded. Only step 3's endpoint line and step 5's verify-match count.
PROMPT
}

# A reset needs none of the kernel and boot checks; it checks for itself.
preflight_needed() { case ${1:-} in --agent-prompt | --reset-touchid) return 1 ;; *) return 0 ;; esac; }

# Tests source this file for its functions only.
if [[ ${AURORA_SEP_SOURCE_ONLY:-} == 1 ]]; then return 0; fi

# --m3-handoff goes with an install, alone or with --read-only.
args=()
for a in "$@"; do
  if [[ $a == --m3-handoff ]]; then M3_TRY=1; else args+=("$a"); fi
done
set -- "${args[@]}"
if ((M3_TRY)) && [[ -n ${1:-} && $1 != --read-only ]]; then
  die "--m3-handoff goes with an install (alone or with --read-only), not with $1"
fi

if preflight_needed "${1:-}"; then preflight; fi
case ${1:-} in
  "") install_all ;;
  --read-only) READ_ONLY=1; install_all ;;
  --uninstall) uninstall_all ;;
  --reset-touchid) shift; reset_touchid "$@" ;;
  --agent-prompt) t=$(newer_release); [[ -n $t ]] && warn "the current release is $t; this plan is for $TAG"; agent_prompt ;;
  *) die "unknown option $1 (--read-only, --uninstall, --reset-touchid, --agent-prompt or --m3-handoff)" ;;
esac
