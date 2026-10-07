#!/bin/bash
# Install the aurora custom/sep kernel and Touch ID on an Omarchy Mac.
#
#   curl -fsSL https://github.com/iconidentify/aurora-linux/releases/latest/download/install-aurora-sep.sh | bash
#   ... | bash -s -- --read-only      install, but never let the driver write to the enclave
#   ... | bash -s -- --uninstall      go back to the kernel this Mac had before
#   ... | bash -s -- --reset-touchid  start Touch ID over: new keybag, enrol again
#   ... | bash -s -- --m3-gpu-experiment   M3 MacBook Air only: install the GPU start
#                                         experiment's scripts with the kernel (arms nothing)
#   ... | bash -s -- --no-m3-mesa     M3 Pro: leave out the M3 Pro's Mesa (installed by default)
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
# 11.109-test (not a release): the M3 Pro display handoff on the 14" J514S.
# Kernel 11.109 takes each Mac's PMP values from the boot loader, and m1n1
# aurora7 passes them (/chosen/asahi,t6030-pmp). For --m3-handoff on a J514S.
# 11.36.1 changes only this script and adds an M3 m1n1: an M3 Pro model whose
# m1n1 display and GPU handoff has been booted (the 16" J516S for now) gets
# m1n1-aurora with it switched on, and the built-in display and GPU work.
# Every other M3 stays kernel-only, and --m3-handoff lets an M3 Pro owner try
# the handoff on a model not yet on the list (see --agent-prompt). M1, M2 and
# the Neo are unchanged.
# 11.37: the unified M1/M2/M3 kernel, now 11.110 (aurora-linux air/t8122). Its
# only device-tree change from 11.36 is the five M3 (T8122) trees; the T8122
# GPU and display nodes stay disabled and the driver fails closed, so without
# an opt-in M3 Air m1n1 (shipped only in the -test channel) an Air is kernel-
# only, exactly as in 11.36. The 16" J516S keeps its display + GPU handoff
# (now m1n1 aurora7, which also passes each Mac's PMP values). --m3-handoff is
# available on the 14" J514S (opt-in, not yet on by default). This release adds
# --m3-report and refuses chips it does not support (SUPPORTED_SOCS; #32).
# 11.38: the same device trees and boot loaders as 11.37. The display driver
# checks each display's modes and attributes before using them and refreshes a
# connector's state before reporting a plug or unplug; the M3 Pro PMP's boot
# settings and startup tables are checked before it starts; the M3 GPU
# settings are per chip; each PCI USB controller reserves only the interrupt it
# uses, leaving interrupts for a dock's other devices. An M3 Air stays kernel-only.
# 12.0: one release and one kernel for every Mac this script supports; the
# 11.1xx test builds end here. On the 16" M3 Pro (J516S) the kernel drives
# external displays on the USB-C and HDMI ports, and its m1n1 also hands over
# the second external display processor. The M3 Type-C PHY changes behind this
# apply to every M3. Other M3 Pros keep the handoff opt-in (--m3-handoff). An
# M3 MacBook Air stays kernel-only unless its owner asks with --m3-handoff for
# m1n1's display handoff with GPU diagnostics (the desktop stays on the boot
# framebuffer). An Air that has an earlier test build's boot loader must ask
# again with --m3-handoff.
# Every Mac that gets an m1n1 from this script now gets the same one,
# m1n1-aurora aurora12: M1 and M2 move to it from aurora3, and the M3s on the
# handoff path from aurora7 and 8.x. Only the switches in /etc/m1n1.conf
# differ between Macs. Each Mac's boot.bin is kept on the EFI partition before
# it is rebuilt, with the steps to put it back.
# The kernel also describes the Air's internal display and GPU configuration,
# both inert for now, and the M2 Max Neural Engine cleans up after a failed
# probe (iconidentify/aurora-linux#37, Joshua Warren). M1, M2 and the Neo
# keep their boot loaders.
# 12.1: on M1 and M2, a direct USB-C display's route follows the display
# pipeline the desktop gives it (iconidentify/aurora-linux#39;
# appledrm.typec_follow_crtc=0 turns it off). Its boot loader and device
# trees are 12.0's.
# 12.2 is 12.1 plus a GPU start experiment for the M3 MacBook Air, off unless
# the Air's owner asks for it. The kernel starts the Air's GPU firmware only
# with asahi.t8122_start=1 on the kernel command line. --m3-gpu-experiment
# (Air only) installs the scripts that arm that for one boot and collect the
# result, with a separate G15G Mesa build in /opt/mesa-m3-g15g; it arms
# nothing itself. Every Mac moves to m1n1-aurora aurora12.1: aurora12 plus a
# stand-in power model for the Air's GPU handoff, which stays off unless
# /etc/m1n1.conf has both chosen.asahi,t8122-gpu=1 and
# chosen.asahi,t8122-gpu-power-standin=1. This script writes neither line.
# Without them every Mac boots as it did with aurora12 (12.0 and 12.1).
# 12.3: every M3 Pro (T6030) gets the M3 Pro's Mesa by default: mesa-m3, a
# pacman package with Mesa and the M3 Pro GPU driver in /opt/mesa-m3, never
# over the system Mesa, and a login hook that uses it only on an M3 Pro whose
# GPU is up. It is installed in a pacman transaction of its own after the
# kernel. If it can't be (a package it needs is missing or too old, or pacman
# fails), the kernel install still completes, the summary says so and the
# exit status is 3. --no-m3-mesa leaves it out; --uninstall removes it. M1,
# M2, the M3 Max and the M3 MacBook Air download and install nothing of it.
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
# m1n1: one m1n1-aurora (M1N1_PACKAGE) for every Mac. It builds
# AsahiLinux/m1n1 main (3e354a24), which knows the macOS 26.5 to 27.0 firmware
# the MacBook Neo ships with, plus Omarchy's patches: the SEP
# warm-registration guard and preboot-UUID forwarding from aurora-silicon/m1n1,
# the usb4-N-pcie-adapter alias fallback (aurora-silicon/m1n1#4), and the M3
# display and GPU handoff, which stays off unless /etc/m1n1.conf arms it.
# From aurora12.1 it also carries the M3 Air GPU handoff's stand-in power
# model, off unless /etc/m1n1.conf arms it too.
# Before boot.bin is rebuilt with it, the boot.bin the Mac booted with is kept
# on the EFI partition (keep_bootbin_on_esp). A MacBook Neo keeps its own m1n1
# unless NEO_AURORA_M1N1 is 1.
# M3: experimental. linux-aurora goes on. On an M3 Pro model in
# M3_HANDOFF_BOARDS (or with --m3-handoff), m1n1-aurora replaces m1n1 and
# three chosen.asahi,t6030-* lines in /etc/m1n1.conf switch its display and
# GPU handoff on. Every other M3 keeps m1n1's boot.bin exactly as it is: no
# m1n1-aurora, no update-m1n1 run, and a freeze on update-m1n1 unless one is
# in place already (see m3_plan). Every M3 Pro also gets mesa-m3 (see the
# M3 Pro's Mesa below, m3_pro_mesa_*).
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

# The script this run reads, for the M3 Pro's Mesa record (m3_pro_mesa_record): its sha256 when
# bash reads it from a regular file (bash /path/install-aurora-sep.sh), hashed here, before
# anything else runs. Through a pipe (curl ... | bash) it can't be read again: then
# "unavailable" and source "stdin", never a guess.
SELF_SOURCE=stdin SELF_SHA256=unavailable
case ${BASH_SOURCE[0]:-} in
  "" | /dev/* | /proc/*) ;;
  *)
    if [[ -f ${BASH_SOURCE[0]} ]]; then
      SELF_SOURCE="file"
      SELF_SHA256=$(sha256sum <"${BASH_SOURCE[0]}" 2>/dev/null | cut -d' ' -f1) || SELF_SHA256=""
      [[ $SELF_SHA256 =~ ^[0-9a-f]{64}$ ]] || SELF_SHA256=unavailable
    fi
    ;;
esac
# This run's id, for the M3 Pro's Mesa record and the summary line that names it, so a check can
# match the record to the output of the run that wrote it.
RUN_ID=$(cat /proc/sys/kernel/random/uuid 2>/dev/null) || RUN_ID=""
[[ $RUN_ID =~ ^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$ ]] || RUN_ID=unavailable

# The kernel package version and the release tag move independently: a release
# that only changes m1n1 reuses the previous kernel packages unchanged.
VERSION=7.1.12.aurora2-12.2
TAG=sep-7.1.12.aurora2-12.2
# Packages are fetched from this script's own tag, never from "latest": the
# checksums below belong to this release and nothing else.
PUBLIC_RELEASE_URL=https://github.com/iconidentify/aurora-linux/releases/download/$TAG
PUBLIC_RELEASES_API=https://api.github.com/repos/iconidentify/aurora-linux/releases
# AURORA_RELEASE_URL and AURORA_RELEASES_API are a staging or mirror override
# for these two (a file://, http:// or https:// URL; see release_source); the
# checksums below still decide what is installed. The commands this script
# prints name the public release (PUBLIC_RELEASE_URL or LATEST_URL), never the
# override: a staged copy can go away, and the checksums don't cover the
# script itself.
RELEASE_URL=${AURORA_RELEASE_URL:-$PUBLIC_RELEASE_URL}
RELEASES_API=${AURORA_RELEASES_API:-$PUBLIC_RELEASES_API}
# Where to always get the current script, whatever this copy turns out to be.
LATEST_URL=https://github.com/iconidentify/aurora-linux/releases/latest/download/install-aurora-sep.sh
PACKAGES=(
  "linux-aurora-$VERSION-aarch64.pkg.tar.zst c1139905a60237066d7c322e1cc9f95884b1b0c727e9ec7288d90b416922086d"
  "linux-aurora-headers-$VERSION-aarch64.pkg.tar.zst 3f1c3f0c32b65cf1520dd4ece9146e34d9535c57ecb14247f78d5fba31be8a34"
  "libfprint-1.94.100-1.1-aarch64.pkg.tar.zst bc7d9762db6644f2cfb58ddb209602c1d513845eb1498c098e01f12600fcbdf9"
  "aurora-touchid-20261003-1-any.pkg.tar.zst 29b0360fac8c257d754e64bd1b9c33c487eb2595dd3c31e9138d7a476afa3d64"
)
# The one m1n1 for every Mac, as "file sha256": M1 and M2, an M3 on the handoff
# path (see m3_plan), and the MacBook Neo once NEO_AURORA_M1N1 is 1. Macs differ
# only in the switches /etc/m1n1.conf arms (m3_switches), never in the binary.
M1N1_PACKAGE="m1n1-aurora-1.6.1.aurora12.1-2-aarch64.pkg.tar.zst 2c223ef9a36410039f823c171c982296eb1dbd5b0de48f2cf6fc87b2160d12c8"
# The sha256 of the m1n1.bin in M1N1_PACKAGE: the bytes update-m1n1 puts at
# the start of boot.bin. The script tells m1n1 builds apart by these bytes,
# never by the version string they report: aurora8.5-1 and 8.5-2 both reported
# v1.6.1-omarchy.aurora8.5.
M1N1_BIN_SHA=557d4825ac2e0f0a2c87d75098e5ca04f5932532b9da7845f7c0f70d0cd07aaa
# 0: a MacBook Neo keeps its own m1n1 (its M1N1= and U_BOOT= in
# /etc/default/update-m1n1), as before 12.0. 1: it gets M1N1_PACKAGE like every
# other Mac, and update-m1n1 builds its boot.bin from that m1n1 and the Neo's
# own U-Boot. Set it to 1 only once M1N1_PACKAGE carries every patch of the
# Neo's own m1n1 (aurora-silicon/m1n1, J700) and has booted on a Neo.
NEO_AURORA_M1N1=0
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

# AURORA_RELEASE_URL and AURORA_RELEASES_API go to curl and into printed
# text, so only a plain file://, http:// or https:// URL is taken: nothing curl
# could read as an option, and no spaces, control characters or other bytes
# outside printable ASCII (percent-encode those).
override_ok() {
  local LC_ALL=C
  [[ $1 != -* && $1 =~ ^(file|https?)://. && $1 != *[![:graph:]]* ]]
}

# True when either override points away from the public release.
release_overridden() {
  [[ $RELEASE_URL != "$PUBLIC_RELEASE_URL" || $RELEASES_API != "$PUBLIC_RELEASES_API" ]]
}

# Checked before the first download or release lookup that uses them. A run
# from a staging or mirror copy says so once, at the start.
release_source() {
  local name shown
  for name in AURORA_RELEASE_URL AURORA_RELEASES_API; do
    [[ -z ${!name:-} ]] || override_ok "${!name}" ||
      die "$name must be a file://, http:// or https:// URL without spaces or
    control characters (percent-encode anything outside printable ASCII).
    Unset it to use the public release. Nothing was installed."
  done
  if ! release_overridden; then
    return 0
  elif [[ $RELEASE_URL != "$PUBLIC_RELEASE_URL" && $RELEASES_API != "$PUBLIC_RELEASES_API" ]]; then
    shown="$RELEASE_URL (release list $RELEASES_API)"
  elif [[ $RELEASE_URL != "$PUBLIC_RELEASE_URL" ]]; then
    shown=$RELEASE_URL
  else
    shown="release list $RELEASES_API"
  fi
  say "Using a staging/mirror copy: $shown; checksums are still verified"
}

# A saved copy of this script keeps installing its own build forever. Tell the
# operator - human or agent - when a newer one exists. Never fatal: no network,
# rate limit or API change should stop an install that was going to work.
newer_release() {
  # Explicitly non-fatal. The pipeline returns non-zero whenever there is no
  # network, GitHub rate-limits, or the response is not what we expect, and
  # whether set -e acts on that inside a command substitution is subtle enough
  # that it should not be left to chance in a script that runs as root.
  local seen="" mine theirs
  # Ask for the release marked Latest. Listing all releases is not ordered by
  # version: they share a commit, so GitHub falls back to comparing tag names
  # as text, and 11.9 sorts above 11.10. Only letters, digits, '.', '_' and
  # '-' are taken as a tag, so a mirror's answer can't put terminal escapes
  # into the notice.
  seen=$(curl -fsSL --max-time 8 "$RELEASES_API/latest" 2>/dev/null |
    LC_ALL=C grep -o '"tag_name"[[:space:]]*:[[:space:]]*"sep-[A-Za-z0-9._-]*"' |
    head -1 | sed 's/.*"\(sep-[A-Za-z0-9._-]*\)"$/\1/') || true
  # Only a higher release number counts, the part after the kernel version
  # (sep-7.1.12.aurora2-12.0 is 12.0): a release staged before it is marked
  # Latest must not be pointed at the older one.
  mine=${TAG#sep-*-} theirs=${seen#sep-*-}
  [[ $theirs == [0-9]* && $theirs != "$mine" &&
    $(printf '%s\n' "$mine" "$theirs" | sort -V | tail -1) == "$theirs" ]] && echo "$seen"
  return 0
}

# A staging or mirror run names a newer release but points at nothing: the
# public script is not what it is testing.
version_notice() {
  local seen
  seen=$(newer_release)
  if [[ -n $seen ]] && release_overridden; then
    warn "$seen is newer than $TAG, which this script installs"
  elif [[ -n $seen ]]; then
    warn "this script installs $TAG, but $seen is published.
    You are probably running a saved copy. The current one:
      curl -fsSL $LATEST_URL | bash"
  elif ! release_overridden; then
    say "$TAG is the current release"
  fi
}

# The same check for --agent-prompt, on stderr only.
prompt_notice() {
  local seen
  seen=$(newer_release)
  if [[ -n $seen ]] && release_overridden; then
    warn "$seen is newer than $TAG, which this plan is for"
  elif [[ -n $seen ]]; then
    warn "the current release is $seen; this plan is for $TAG"
  fi
  return 0
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

# The chips this release installs on: M1 (t8103, t6000, t6001, t6002), M2
# (t8112, t6020, t6021, t6022), the MacBook Neo (t8140) and the M3s that is_m3
# lists. The kernel also carries device trees for the M3 Ultra (t6032) and
# M4 and later chips (t8132, t8142, t8152), but nobody has booted this kernel
# on them, and the M1/M2 path would replace their boot loader with an m1n1
# that can't start them. Keep this list in step with is_m3 and is_neo.
SUPPORTED_SOCS="t8103 t6000 t6001 t6002 t8112 t6020 t6021 t6022 t8140 t8122 t6030 t6031 t6034"

# This Mac's chip from the device tree, as tNNNN; empty when there is none.
this_soc() {
  tr '\0' '\n' <"$DT/compatible" 2>/dev/null | sed -n 's/^apple,\(t[0-9][0-9][0-9][0-9]\)$/\1/p' | head -1
}

# Stops before anything is downloaded or changed on a chip this release
# doesn't support. --agent-prompt and --reset-touchid don't come here.
require_supported_soc() {
  local soc
  soc=$(this_soc)
  [[ -n $soc && " $SUPPORTED_SOCS " == *" $soc "* ]] && return 0
  die "this Mac (${soc:+apple,$soc, }$(this_board)) is not one $VERSION supports: M1, M2, the MacBook
    Neo, and M3, M3 Pro and M3 Max. ${1:-Installing} would replace its boot loader with an m1n1 that
    can't start it. Nothing was changed. If you are bringing this Mac up, please open an issue at
    https://github.com/iconidentify/aurora-linux/issues"
}

# Every M3 chip: M3 (t8122), M3 Pro (t6030), M3 Max (t6031, t6034).
is_m3() {
  tr '\0' '\n' <"$DT/compatible" 2>/dev/null | grep -Eqx 'apple,(t8122|t6030|t6031|t6034)'
}

is_m3_pro() {
  tr '\0' '\n' <"$DT/compatible" 2>/dev/null | grep -qx 'apple,t6030'
}

# The M3 MacBook Airs. The other T8122 Macs (the 14" MacBook Pro M3, J504, and
# the iMacs, J433 and J434) have no handoff path and stay kernel-only.
M3_AIR_BOARDS="j613 j615"
is_m3_air() {
  local board
  tr '\0' '\n' <"$DT/compatible" 2>/dev/null | grep -qx 'apple,t8122' || return 1
  board=$(this_board)
  [[ -n $board && " $M3_AIR_BOARDS " == *" $board "* ]]
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
#            M1N1_PACKAGE, whose T6030 display and GPU handoff (from
#            iconidentify/m3-m1n1) stays off unless boot.bin ends with the
#            three M3_SWITCHES lines; update-m1n1 copies chosen.* lines from
#            /etc/m1n1.conf into every rebuild, so they survive updates.
#            An M3 MacBook Air takes the same path with the same m1n1 and its
#            own switches (m3_switches): the display handoff with GPU
#            diagnostics, and only with --m3-handoff until its board is in
#            M3_HANDOFF_BOARDS.
# M1 and M2 get the same M1N1_PACKAGE with no switches; the Neo keeps its own
# m1n1 unless NEO_AURORA_M1N1 is 1.
UPDATE_M1N1_CONF=/etc/default/update-m1n1
M3_FREEZE_BEGIN="# >>> aurora-sep: keep this M3's boot.bin as it is (remove with: install-aurora-sep.sh --uninstall)"
M3_FREEZE_END="# <<< aurora-sep: keep this M3's boot.bin as it is"
M3_FROZEN_BY=""
M3_BOOTBIN_SHA=""
# Models the handoff has been booted on, by us or by a tester's report. Only
# an M3 Pro (t6030) or an M3 MacBook Air (M3_AIR_BOARDS) listed here gets it
# by default; no Air is listed yet.
M3_HANDOFF_BOARDS="j516s"
M3_SWITCHES="chosen.asahi,t6030-gpu=1 chosen.asahi,t6030-dcp=1 chosen.asahi,t6030-dcpext=1"
# The M3 Pro handoff's name in $STATE/m3-mode. A plain run keeps the handoff on
# an M3 Pro that is not in M3_HANDOFF_BOARDS only while this matches, so it
# changes with each m1n1 that changes what a Pro boots: a J514S that opted in
# on an earlier release (t6030: aurora7 or 8.x) moves to this release's m1n1
# only with a new --m3-handoff. A listed Pro moves with the release.
# aurora12.1 boots an M3 Pro exactly as aurora12 does (its additions are for
# the T8122 only), so the name stays t6030-12: a Pro that opted in on 12.0
# or 12.1 moves to aurora12.1 on a plain run, as it would have on 12.0.
M3_PRO_VARIANT="t6030-12"
# The M3 MacBook Air's handoff switches, for a later build that starts the
# Air's GPU (M3_AIR_DRY_RUN=0): the GPU alone, or with M3_AIR_DCP=1 the
# display too, once m1n1's T8122 DCP handoff has been booted on an Air.
M3_AIR_GPU_SWITCH="chosen.asahi,t8122-gpu=1"
M3_AIR_DCP_SWITCH="chosen.asahi,t8122-dcp=1"
M3_AIR_DCP=0
# A dry run in place of the handoff (M3_AIR_DRY_RUN=1): m1n1 reads the Air's
# GPU and display details from the boot firmware and reports them on the
# serial console and under /chosen, powers the GPU only for a bounded
# identity read, and starts nothing. The desktop stays as it was.
# chosen.asahi,t8122-dcp=1 is a dry run only while m1n1 pins no T8122 DCP
# firmware image (aurora8 pins none); a release whose m1n1 pins one must give
# the dry run another variant name in m3_variant, so no Air gets it unasked.
# The display handoff (M3_AIR_DISPLAY_HANDOFF=1) is that case: the same
# switches with an m1n1 that pins the image (aurora8.3 and later), so m1n1
# hands the internal display over and publishes its checked state for Linux.
# The kernel keeps the boot framebuffer until it has a T8122 PMP description,
# and the GPU switches stay diagnostics: nothing starts the GPU.
# M3_AIR_DISPLAY_VARIANT names it in $STATE/m3-mode. A plain run keeps an
# Air's boot loader only while that name matches, so it changes with each m1n1
# that changes what an Air boots: an Air on an earlier test build's boot loader
# (air-display-handoff, aurora8.3 and 8.4) moves to this one only with a new
# --m3-handoff.
# aurora12.1 boots an Air with these switches exactly as aurora12 does: its
# GPU stand-in needs chosen.asahi,t8122-gpu=1 and
# chosen.asahi,t8122-gpu-power-standin=1, which no variant here sets. So the
# name stays air-display-handoff-12, and an Air that opted in on 12.0 or 12.1
# moves to aurora12.1 on a plain run with the same switches.
M3_AIR_DISPLAY_HANDOFF=1
M3_AIR_DISPLAY_VARIANT="air-display-handoff-12"
M3_AIR_DRY_RUN=1
M3_AIR_DRY_RUN_SWITCHES="chosen.asahi,t8122-gpu-diag=1 chosen.asahi,t8122-gpu-handoff-diag=1 chosen.asahi,t8122-gpu-power-diag=1 chosen.asahi,t8122-dcp=1"
# --m3-gpu-experiment (M3 MacBook Air only) installs the GPU start experiment's tools with the
# kernel: three scripts in M3_GPU_BIN, from this release (M3_GPU_SCRIPTS, checked like the
# packages), and the G15G Mesa prefix package once there is one. It arms nothing and changes
# no boot setting: air-gpu-oneshot.sh arms one boot at a time, only when its owner runs it.
# Their sources are tools/aurora-sep/air-gpu/ in this repository.
M3_GPU_EXPERIMENT=0
M3_GPU_BIN=/usr/local/bin
M3_GPU_SCRIPTS=(
  "air-gpu-oneshot.sh d0259869b8519439a4dbcdae08ef1b0e17fdcaf84ac746fca61febf6a325f8aa"
  "air-gpu-collect.sh face8fac811b59c31a8e98e6ca704a2cb33eceada8bc37986397cb393b050b30"
  "air-gpu-job.sh f76e57154e96165daf8834508975a98f745519ab1b51ac2edae4f5710edae349"
)
# The G15G Mesa build: its prefix package as "file sha256" (a pacman package that installs Mesa
# into a prefix of its own, never over the system Mesa), and that prefix, for air-gpu-job.sh.
# mesa-m3-g15g installs only /opt/mesa-m3-g15g; its G15G support is opt-in there too
# (ASAHI_M3_G15G=1, which air-gpu-job.sh and the prefix's bin/g15g-run set).
M3_GPU_MESA_PACKAGE="mesa-m3-g15g-26.1.4.g15g1-5-aarch64.pkg.tar.zst 1dd4c3876a26edf0cc3ad1b23eba79a66d99fd4c3cb9d2683f346e7bca1fa904"
M3_GPU_MESA_PREFIX="/opt/mesa-m3-g15g"
# What the Mesa package depends on with a minimum version, as "name>=version". Each must be
# installed already, at that version or newer, or m3_gpu_mesa_install leaves the package out:
# its pacman -U must never pull in an upgrade of the C library or the compiler runtime alone.
M3_GPU_MESA_NEEDS="glibc>=2.43 gcc-libs>=11 spirv-tools>=1:1.4.357.0"
# 1 once m3_gpu_mesa_install has installed the Mesa package on this run.
M3_GPU_MESA_DONE=0
# The M3 Pro's Mesa (t6030 only, on by default; --no-m3-mesa leaves it out): a pacman package,
# "file sha256", that installs Mesa with the M3 Pro (G15S) GPU driver into a prefix of its own,
# never over the system Mesa, plus the files that make a login session use that prefix on an
# M3 Pro. It is a release asset of its own, never one of the PACKAGES every Mac gets, and is
# installed in its own pacman transaction once the kernel install is done (m3_pro_mesa_install).
# PLACEHOLDER: the release guard test fails, and an M3 Pro install stops before any download.
M3_PRO_MESA_PACKAGE="mesa-m3-PENDING-aarch64.pkg.tar.zst PENDING-U1-PACKAGE-BUILD"
M3_PRO_MESA_PREFIX="/opt/mesa-m3"
# The package's name, as its .PKGINFO gives it; --uninstall removes it by this exact name.
M3_PRO_MESA_NAME="mesa-m3"
# What the package depends on, as "name>=version" or a bare "name": each must be installed
# already (at that version or newer), or the package is left out, so its pacman -U never pulls
# in an upgrade of the C library or the compiler runtime alone, or a new package.
# PLACEHOLDER: the release guard test fails, and an M3 Pro install stops before any download.
M3_PRO_MESA_NEEDS="PENDING-U1-INVENTORY"
# The package's own switch-offs, read only to say so in the summary: this file, the user's
# ~/.config/mesa-m3/disable, or mesa_m3=off on the kernel command line.
M3_PRO_MESA_DISABLE=/etc/mesa-m3/disable
# mesa-m3's own user-setup detector and the list file it reads: what counts as a user's own Mesa
# setup, for the session hook and for this script alike. Once mesa-m3 is installed the record's
# user_setup comes from that detector (m3_pro_mesa_detector).
# PLACEHOLDER: the release guard test fails, and an M3 Pro install stops before any download.
M3_PRO_MESA_DETECTOR="PENDING-U1-DETECTOR"
M3_PRO_MESA_SETUP_LIST="PENDING-U1-SETUP-LIST"
# The same list built in, "kind item" per line in the list file's order (fill-m3-pro-mesa.sh
# refuses a package whose list file says anything else), for runs without mesa-m3 installed.
# m3_pro_mesa_builtin_setup says what each kind means.
M3_PRO_MESA_SETUP_RULES="variable CHONKSTEP_M3_MESA_PREFIX
variable VK_ICD_FILENAMES
variable VK_DRIVER_FILES
variable VK_ADD_DRIVER_FILES
variable LIBGL_DRIVERS_PATH
variable MESA_LOADER_DRIVER_OVERRIDE
variable GALLIUM_DRIVER
variable LIBGL_ALWAYS_SOFTWARE
variable __EGL_VENDOR_LIBRARY_FILENAMES
variable __EGL_VENDOR_LIBRARY_DIRS
variable GBM_BACKENDS_PATH
variable GBM_ALWAYS_SOFTWARE
variable DRIRC_CONFIGDIR
ldpath LD_LIBRARY_PATH
envfile {config}/environment.d/*.conf
envfile {config}/uwsm/env
envfile {config}/uwsm/env-*
envfile {config}/uwsm/env.d/*
envfile {config}/uwsm/env-*.d/*
envfile {config}/uwsm/default
envfile {config}/hypr/*.conf
envfile {config}/hypr/*.lua
envfile /etc/xdg/uwsm/env
envfile /etc/xdg/uwsm/env-*
envfile /etc/xdg/uwsm/env.d/*
envfile /etc/environment
drirc ~/.drirc
drirc {config}/drirc
drirc /etc/drirc
chonkstep {config}/chonkstep/m3gpu-session.env"
# Where the built-in check finds the system's files (/etc/...); tests point it elsewhere.
M3_PRO_MESA_SETUP_ROOT=""
# 0 with --no-m3-mesa.
M3_PRO_MESA=1
# What happened on this run (m3_pro_mesa_plan/_install): "" (not an M3 Pro, or no package in
# this release), installed, current, newer-kept, skipped-flag, skipped-deps or failed.
M3_PRO_MESA_RESULT=""
# The record --uninstall and the lab check read, on an M3 Pro only, and its first line
# (m3_pro_mesa_record says what is in it).
M3_PRO_MESA_RECORD_NAME="m3-pro-mesa"
M3_PRO_MESA_SCHEMA="aurora.m3-pro-mesa-state/1"
# How this run's record write went (m3_pro_mesa_record), and the result it holds.
M3_PRO_MESA_RECORD_WRITE=""
M3_PRO_MESA_RECORD_RESULT=""
# The handoff is tested with one macOS system-firmware stub only, 14.8.3 (GPU
# firmware 14.8.3, DCP 14.7), which the Omarchy installer gives every M3. m1n1
# reports the stub's iBoot as asahi,iboot2-version.
M3_STUB_VERSION=14.8.3
M3_STUB_IBOOT=iBoot-10151.140.19
# The M3 m1n1 (stage 2) must fit below the display processors' log buffers,
# which every M3 boot so far placed 0x120000 bytes above where stage 1 loaded
# m1n1. That has only been seen with these stage 1 versions, as m1n1 reports
# them in /chosen/asahi,m1n1-stage1-version; the handoff is refused with any
# other, and on a Mac that reports none.
# The check is for M3s only. That log buffer placement is the M3's; on M1 and
# M2 nothing has been seen at that offset, and they have booted aurora3, whose
# file part reached about 0x110000, from every stage 1 in the field since 11.x.
# M1N1_PACKAGE is linked to end its file part below 0x120000 (0xe0000 for the
# aurora8.5 line it comes from), so a stage 1 allowlist there would only
# refuse Macs that boot today.
M3_STAGE1_VERSIONS="v1.6.1-dirty"
# m1n1 (aurora8.5-2 and later) adds this node when one of those log buffers
# overlaps where stage 1 loaded it. It keeps the buffer reserved and boots on,
# but it was never checked in that layout.
M3_OSLOG_OVERLAP=chosen/asahi,m1n1-oslog-overlap
M1N1_CONF=/etc/m1n1.conf
# What update-m1n1 puts at the start of boot.bin.
M1N1_BIN=/usr/lib/asahi-boot/m1n1.bin
# After a rebuild with M1N1_PACKAGE, $STATE/m1n1-installed records the m1n1
# that boot.bin was checked to start with: "sha256 bytes package".
M1N1_CONF_BEGIN="# >>> aurora-sep: M3 Pro display and GPU handoff (remove with: install-aurora-sep.sh --uninstall)"
M1N1_CONF_END="# <<< aurora-sep: M3 Pro display and GPU handoff"
M1N1_CONF_AIR_BEGIN="# >>> aurora-sep: M3 Air handoff (remove with: install-aurora-sep.sh --uninstall)"
M1N1_CONF_AIR_END="# <<< aurora-sep: M3 Air handoff"
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
# Set by --m3-handoff: the owner asks for the handoff on an M3 Pro or M3
# MacBook Air model that isn't in M3_HANDOFF_BOARDS yet.
M3_TRY=0
# 1 when this run keeps the boot.bin this Mac has because an m1n1 from this
# script failed on it before ($STATE/m1n1-failed): set by m3_plan and
# m1n1_keep_plan.
M1N1_KEEP=0
# Where issue reports for a boot loader that failed go.
ISSUE_URL=https://github.com/iconidentify/aurora-linux/issues/6

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
  { is_m3_pro || is_m3_air; } && [[ -n $board && " $M3_HANDOFF_BOARDS " == *" $board "* ]]
}

# The switch lines this Mac's handoff needs, separated by spaces: the T6030
# ones, or the M3 Air's.
m3_switches() {
  if ! is_m3_air; then
    echo "$M3_SWITCHES"
  elif [[ $M3_AIR_DRY_RUN == 1 ]]; then
    echo "$M3_AIR_DRY_RUN_SWITCHES"
  elif [[ $M3_AIR_DCP == 1 ]]; then
    echo "$M3_AIR_GPU_SWITCH $M3_AIR_DCP_SWITCH"
  else
    echo "$M3_AIR_GPU_SWITCH"
  fi
}

# Whether this run puts M1N1_PACKAGE on this Mac: M1 and M2, an M3 on the
# handoff path, and a MacBook Neo only with NEO_AURORA_M1N1=1. Needs m3_plan
# first.
m1n1_for_this_mac() {
  if ((M1N1_KEEP)); then
    return 1
  elif is_neo; then
    [[ $NEO_AURORA_M1N1 == 1 ]]
  elif [[ $M3_MODE != none ]]; then
    [[ $M3_MODE == handoff ]]
  fi
}

# M1N1_PACKAGE's version, as pacman prints it: 1.6.1.aurora12.1-2.
m1n1_version() {
  local file=${M1N1_PACKAGE%% *}
  file=${file#m1n1-aurora-}
  echo "${file%-aarch64.pkg.tar.zst}"
}

# What the handoff does on this Mac, for messages.
m3_handoff_name() {
  if ! is_m3_air; then
    echo "M3 Pro display and GPU handoff"
  elif [[ $M3_AIR_DISPLAY_HANDOFF == 1 ]]; then
    echo "M3 Air display handoff with GPU diagnostics"
  elif [[ $M3_AIR_DRY_RUN == 1 ]]; then
    echo "M3 Air GPU and display dry run"
  elif [[ $M3_AIR_DCP == 1 ]]; then
    echo "M3 Air display and GPU handoff"
  else
    echo "M3 Air GPU handoff"
  fi
}

# Why this Mac's system-firmware stub isn't the one the handoff is tested
# with; nothing when it is. The installer's stub_info.json names the stub's
# macOS version, and m1n1 reports the stub's iBoot.
m3_stub_problem() {
  local iboot bootbin info="" version=""
  iboot=$({ tr -d '\0' <"$DT/chosen/asahi,iboot2-version"; } 2>/dev/null) || iboot=""
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

# Why this Mac's m1n1 stage 1 isn't one the M3 m1n1 is checked with (see
# M3_STAGE1_VERSIONS); nothing when it is.
m3_stage1_problem() {
  local stage1
  stage1=$({ tr -d '\0' <"$DT/chosen/asahi,m1n1-stage1-version"; } 2>/dev/null) || stage1=""
  if [[ -z $stage1 ]]; then
    echo "its m1n1 reports no stage 1 version"
  elif [[ " $M3_STAGE1_VERSIONS " != *" $stage1 "* ]]; then
    echo "its m1n1 stage 1 is $stage1"
  fi
  return 0
}

# After a boot: warn when this boot's m1n1 found a display log buffer over
# the place stage 1 loaded it (M3_OSLOG_OVERLAP). Only an M3 m1n1 sets it.
m3_oslog_overlap_check() {
  [[ -e $DT/$M3_OSLOG_OVERLAP ]] || return 0
  warn "this boot's m1n1 reports that a display log buffer overlaps where its stage 1 loaded
    it ($DT/$M3_OSLOG_OVERLAP). This m1n1 was only checked with those buffers clear of it.
    Please report it at https://github.com/iconidentify/aurora-linux/issues with the file that
    --m3-report writes. The boot loader this Mac had before the handoff is kept on the EFI
    partition as m1n1/boot.bin.before-<version>."
}

# The sha256 of a downloaded m1n1-aurora package's m1n1.bin.
m1n1_pkg_sha() {
  { bsdtar -xOf "$1" usr/lib/asahi-boot/m1n1.bin | sha256sum | cut -d' ' -f1; } 2>/dev/null
}

# The sha256 of the first $2 bytes of boot.bin $1: the m1n1 at its start, when
# $2 is that m1n1's size.
bootbin_m1n1_sha() {
  $sudo head -c "$2" "$1" | sha256sum | cut -d' ' -f1
}

# True when update-m1n1's configuration names its own m1n1 or target: boot.bin
# then never starts with M1N1_PACKAGE's m1n1, which is not checked there.
update_m1n1_own_m1n1() {
  [[ -f $UPDATE_M1N1_CONF ]] || return 1
  # shellcheck disable=SC2016 # expanded by that sh, not here
  env -i PATH="$PATH" sh -c '. "$1" >/dev/null 2>&1; [ -n "${M1N1:-}${SOURCE:-}${TARGET:-}" ]' _ \
    "$UPDATE_M1N1_CONF" >/dev/null 2>&1
}

# The m1n1 builds that failed on this Mac: the sha256 that starts each line of
# $STATE/m1n1-failed. The restore steps (keep_bootbin_on_esp) have the owner
# add a line; m1n1_rollback_check adds one when it finds the boot.bin put back.
m1n1_failed_shas() {
  [[ -f $STATE/m1n1-failed ]] || return 0
  grep -oE '^[0-9a-f]{64}' "$STATE/m1n1-failed" || true
}

m1n1_failed_add() {
  grep -qxF "$1" <<<"$(m1n1_failed_shas)" && return 0
  $sudo install -d "$STATE"
  echo "$1 $2" | $sudo tee -a "$STATE/m1n1-failed" >/dev/null
}

# A Mac whose boot.bin was put back by hand after this script rebuilt it, as
# the restore steps say: boot.bin starts neither with the m1n1 recorded in
# $STATE/m1n1-installed nor with the m1n1 that is installed now. That m1n1 is
# recorded as failed on this Mac. A rebuild that only changed the device trees
# or the switches keeps the same m1n1, and is not taken for one.
m1n1_rollback_check() {
  local sha size pkg target
  [[ -f $STATE/m1n1-installed ]] || return 0
  read -r sha size pkg _ <"$STATE/m1n1-installed" || return 0
  [[ $sha =~ ^[0-9a-f]{64}$ && $size =~ ^[0-9]+$ ]] || return 0
  target=$(esp_bootbin) || return 0
  [[ $(bootbin_m1n1_sha "$target" "$size") == "$sha" ]] && return 0
  if [[ -f $M1N1_BIN ]] && $sudo cmp -s -n "$(stat -c %s "$M1N1_BIN")" "$M1N1_BIN" "$target"; then
    return 0
  fi
  m1n1_failed_add "$sha" "${pkg:-m1n1}: boot.bin was put back by hand, found $(date +%F)"
}

# On a Mac that is not an M3 (m3_plan decides for those): when this release's
# m1n1 failed on it before, keep the boot.bin it has (M1N1_KEEP). update-m1n1
# must stay frozen, or pacman's hook would put that m1n1 back.
m1n1_keep_plan() {
  if is_m3 || ! m1n1_for_this_mac; then return 0; fi
  grep -qxF "$M1N1_BIN_SHA" <<<"$(m1n1_failed_shas)" || return 0
  update_m1n1_frozen ||
    die "this release's m1n1 (sha256 $M1N1_BIN_SHA) failed on this Mac before (recorded in
    $STATE/m1n1-failed), so this script does not put it back, and update-m1n1 must not either.
    Keep updates from rebuilding boot.bin, then run this again:
      echo M1N1_UPDATE_DISABLED=1 | sudo tee -a $UPDATE_M1N1_CONF
    Nothing was installed. If you have not yet, please report what happened at $ISSUE_URL"
  M1N1_KEEP=1
  say "This release's m1n1 (sha256 $M1N1_BIN_SHA) failed on this Mac before (recorded in
    $STATE/m1n1-failed), so it is not put back: boot.bin stays as it is, and update-m1n1 stays
    frozen in $UPDATE_M1N1_CONF. If you have not yet, please report what happened at $ISSUE_URL"
}

# After an install that kept boot.bin (M1N1_KEEP) on a Mac that is not an M3.
m1n1_keep_report() {
  [[ $(m3_bootbin_sha) == "$M3_BOOTBIN_SHA" ]] ||
    die "m1n1's boot.bin changed during the install, which it must not here. Please report it
    at $ISSUE_URL before rebooting."
  say "m1n1's boot.bin is unchanged: update-m1n1 stays frozen in $UPDATE_M1N1_CONF."
}

# After update-m1n1 rebuilt boot.bin with M1N1_PACKAGE: check the m1n1 at its
# start by its bytes, and record it in $STATE/m1n1-installed.
m1n1_check_and_record() {
  local target size sha
  target=$(esp_bootbin) || die "could not find m1n1's boot.bin to check its m1n1 in"
  size=$(stat -c %s "$M1N1_BIN")
  sha=$(bootbin_m1n1_sha "$target" "$size")
  [[ $sha == "$M1N1_BIN_SHA" ]] ||
    die "the m1n1 at the start of $target is sha256 $sha, not this release's
    ($M1N1_BIN_SHA), so boot.bin was not rebuilt as it should be. The boot loader this Mac booted
    with is kept as m1n1/boot.bin.before-$VERSION on the EFI partition. Please report it at
    https://github.com/iconidentify/aurora-linux/issues before rebooting."
  printf '%s %s %s\n' "$sha" "$size" "${M1N1_PACKAGE%% *}" | $sudo tee "$STATE/m1n1-installed" >/dev/null
  say "m1n1's boot.bin starts with this release's m1n1 (sha256 $sha)"
}

# True when a downloaded m1n1-aurora package knows every switch this Mac's
# handoff needs (m3_switches): asahi,t6030-gpu for chosen.asahi,t6030-gpu=1.
m1n1_pkg_has_handoff() {
  local bin s rc=0
  bin=$(mktemp)
  # One string per line, in a file: grep -q on a pipe would stop reading early
  # and fail the 3.8 MB tr with SIGPIPE under pipefail.
  { bsdtar -xOf "$1" usr/lib/asahi-boot/m1n1.bin | tr '\0' '\n' >"$bin"; } 2>/dev/null || rc=1
  for s in $(m3_switches); do
    s=${s#chosen.}
    s=${s%%=*}
    ((rc == 0)) && ! grep -qaxF "$s" "$bin" && rc=1
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
  # shellcheck disable=SC2016 # expanded by that sh, not here
  env -i PATH="$PATH" sh -c 'set -e; . "$1"; [ -n "${M1N1_UPDATE_DISABLED:-}" ]' _ "$tmp" \
    >/dev/null 2>&1 && rc=0
  rm -f "$tmp"
  return "$rc"
}

# True when update-m1n1's configuration points it at another m1n1, U-Boot or
# config than the packaged ones: the handoff check could then pass on a boot.bin
# this release did not build.
update_m1n1_customised() {
  [[ -f $UPDATE_M1N1_CONF ]] || return 1
  # shellcheck disable=SC2016 # expanded by that sh, not here
  env -i PATH="$PATH" sh -c '. "$1" >/dev/null 2>&1; [ -n "${M1N1:-}${SOURCE:-}${U_BOOT:-}${CONFIG:-}${TARGET:-}" ]' _ \
    "$UPDATE_M1N1_CONF" >/dev/null 2>&1
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
    [[ -f $STATE/m3gpu-marker.saved ]] || $sudo touch "$STATE/m3gpu-marker.saved"
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

# /etc/m1n1.conf without this script's switch block (M3 Pro or M3 Air), on
# stdout.
m1n1_conf_without_switches() {
  [[ -f $M1N1_CONF ]] || return 0
  awk -v b="$M1N1_CONF_BEGIN" -v e="$M1N1_CONF_END" \
    -v ab="$M1N1_CONF_AIR_BEGIN" -v ae="$M1N1_CONF_AIR_END" '
    $0 == b || $0 == ab { skip = 1; next }
    skip && ($0 == e || $0 == ae) { skip = 0; next }
    !skip' "$M1N1_CONF"
}

m3_switches_write() {
  local tmp s begin=$M1N1_CONF_BEGIN end=$M1N1_CONF_END
  if is_m3_air; then
    begin=$M1N1_CONF_AIR_BEGIN
    end=$M1N1_CONF_AIR_END
  fi
  tmp=$(mktemp)
  m1n1_conf_without_switches >"$tmp"
  {
    echo "$begin"
    for s in $(m3_switches); do echo "$s"; done
    echo "$end"
  } >>"$tmp"
  $sudo install -m 644 "$tmp" "$M1N1_CONF"
  rm -f "$tmp"
}

m3_switches_remove() {
  local tmp
  [[ -f $M1N1_CONF ]] && grep -qxF -e "$M1N1_CONF_BEGIN" -e "$M1N1_CONF_AIR_BEGIN" "$M1N1_CONF" || return 0
  tmp=$(mktemp)
  m1n1_conf_without_switches >"$tmp"
  if grep -q '[^[:space:]]' "$tmp"; then
    $sudo install -m 644 "$tmp" "$M1N1_CONF"
  else
    $sudo rm -f "$M1N1_CONF"
  fi
  rm -f "$tmp"
}

# The M3 path an earlier install took, or nothing.
m3_recorded_mode() {
  local mode=""
  if [[ -f $STATE/m3-mode ]]; then read -r mode _ <"$STATE/m3-mode" || true; fi
  echo "$mode"
}

# What this release's handoff does on this Mac. It is recorded with the mode,
# so a later release never turns one kind of Air test into another (a dry run
# into a GPU start) without a new --m3-handoff.
m3_variant() {
  if ! is_m3_air; then echo "$M3_PRO_VARIANT"
  elif [[ $M3_AIR_DISPLAY_HANDOFF == 1 ]]; then echo "$M3_AIR_DISPLAY_VARIANT"
  elif [[ $M3_AIR_DRY_RUN == 1 ]]; then echo air-dry-run
  elif [[ $M3_AIR_DCP == 1 ]]; then echo air-gpu-dcp
  else echo air-gpu
  fi
}

m3_recorded_variant() {
  local mode="" variant=""
  if [[ -f $STATE/m3-mode ]]; then read -r mode variant _ <"$STATE/m3-mode" || true; fi
  echo "$variant"
}

# A Mac that has the handoff from an earlier install, on which this release's
# m1n1 is not checked ($1, and how this Mac differs, $2): it keeps what it has.
m3_kept_refusal() {
  die "M3 ($(this_board)): this Mac has m1n1's $(m3_handoff_name) from an earlier install, and
    this release's m1n1 is only checked with $1, and this Mac differs: $2.
    Nothing was installed; this Mac keeps the boot loader and kernel it has. Please report it
    at $ISSUE_URL with the file that this writes:
      curl -fsSL $LATEST_URL | bash -s -- --m3-report"
}

# Decide the M3 path before anything is downloaded, so a Mac this release can't
# set up as asked stops with nothing changed.
m3_plan() {
  local board problem failed variant again="run this again" air=0 kept=0
  M3_MODE=none
  if ! is_m3; then
    ((M3_TRY == 0)) || die "--m3-handoff is for an M3 Pro or an M3 MacBook Air, and this Mac isn't an M3. Nothing was installed."
    return 0
  fi
  board=$(this_board)
  if is_m3_air; then air=1; fi
  m3_oslog_overlap_check
  # An m1n1 from this script failed on this Mac before: a plain run keeps the
  # boot loader it has now, whatever this release's m1n1 is, and --m3-handoff
  # never puts back the one that failed.
  failed=$(m1n1_failed_shas)
  if [[ -n $failed ]] && ((M3_TRY == 0)); then
    M3_MODE=kernel M1N1_KEEP=1
    say "M3 ($board): an m1n1 from this script failed on this Mac before (recorded in
    $STATE/m1n1-failed). Installing the kernel only: the boot loader this Mac has now stays as
    it is. If you have not yet, please report what happened at $ISSUE_URL"
    return 0
  fi
  if [[ -n $failed ]] && grep -qxF "$M1N1_BIN_SHA" <<<"$failed"; then
    die "--m3-handoff: this release's m1n1 (sha256 $M1N1_BIN_SHA) is the one that failed on this
    Mac (recorded in $STATE/m1n1-failed), so this script does not put it back.
    Nothing was installed. Keep the kernel-only install: run this again without --m3-handoff.
    If you have not yet, please report what happened at $ISSUE_URL"
  fi
  # An update never takes the handoff away again: once a Mac has it (listed, or
  # tried with --m3-handoff), a plain run keeps it, and its checks still apply.
  if ((M3_TRY == 0)) && [[ $(m3_recorded_mode) == handoff ]]; then
    # A freeze that is neither this script's nor the bring-up's: the line the
    # restore steps give after a boot loader failed. Keep that boot.bin.
    if update_m1n1_frozen_by_others; then
      M3_MODE=kernel M1N1_KEEP=1
      say "M3 ($board): this Mac has m1n1's handoff from an earlier install, and update-m1n1 is
    frozen in $UPDATE_M1N1_CONF, not by this script (the restore steps add that line).
    Installing the kernel only: the boot loader this Mac has now stays as it is. If a boot
    loader from this script failed on this Mac, please report it at $ISSUE_URL"
      return 0
    fi
    if ((air)) && [[ $(m3_recorded_variant) != "$(m3_variant)" ]]; then
      die "M3 MacBook Air ($board): this Mac has an earlier test build's boot loader
    ($(m3_recorded_variant)), and this release's Air boot loader is a different one: the
    $(m3_handoff_name) ($(m3_variant)). Run this again with --m3-handoff to switch to it.
    Nothing was installed."
    fi
    # The same for an M3 Pro that is not on the list: it opted in to an
    # earlier m1n1, not to this one.
    variant=$(m3_recorded_variant)
    if is_m3_pro && ! is_m3_handoff_board && [[ $variant != "$(m3_variant)" ]]; then
      die "M3 Pro ($board): this Mac has m1n1's display and GPU handoff from an earlier release
    (${variant:-no variant recorded}), and this release's m1n1 is a newer one ($(m3_variant)) that
    nobody has booted on this model yet. Nothing was installed; this Mac keeps the boot loader
    and kernel it has. To switch to the new m1n1 (it replaces this Mac's boot loader, and the
    steps to put the old one back follow), run this again with --m3-handoff:
      curl -fsSL $LATEST_URL | bash -s -- --m3-handoff"
    fi
    # Kept from the record, not asked for: say so only once every check
    # below has passed, and refuse in words that name no flag.
    M3_TRY=1 kept=1
  fi
  M3_MODE=kernel
  if ((air)); then
    # Never by default: a plain run gives an Air the handoff only once its
    # board is in M3_HANDOFF_BOARDS.
    if ! is_m3_handoff_board && ((M3_TRY == 0)); then
      say "M3 MacBook Air ($board): installing the kernel only, and boot.bin stays as it is. m1n1's
    $(m3_handoff_name) for the Air is being tested and is not on by default. To help test it
    (it replaces this Mac's boot loader), see case D in the M3 section of: bash -s -- --agent-prompt"
      return 0
    fi
  elif ! is_m3_pro; then
    ((M3_TRY == 0)) || die "--m3-handoff is for an M3 Pro (t6030) or an M3 MacBook Air (j613, j615);
    this M3 ($board) has no display and GPU handoff in m1n1 yet. Nothing was installed."
    say "M3 ($board): installing the kernel only. m1n1 has no display and GPU handoff for this chip
    yet, so boot.bin stays as it is and the display runs on the boot framebuffer."
    return 0
  elif ! is_m3_handoff_board && ((M3_TRY == 0)); then
    say "M3 Pro ($board): installing the kernel only, and boot.bin stays as it is. The display and
    GPU handoff in m1n1 hasn't been booted on this model yet. To try it (it replaces this
    Mac's boot loader), see the M3 section of: bash -s -- --agent-prompt"
    return 0
  fi
  problem=$(m3_stub_problem)
  if [[ -n $problem ]]; then
    if ((kept)); then m3_kept_refusal "the macOS $M3_STUB_VERSION system-firmware stub" "$problem"; fi
    ((M3_TRY == 0)) || die "--m3-handoff: the handoff is only tested with the macOS $M3_STUB_VERSION
    system-firmware stub, and this Mac differs: $problem. Nothing was installed."
    warn "the $(m3_handoff_name) is only tested with the macOS $M3_STUB_VERSION
    system-firmware stub, and this Mac differs: $problem. Installing the kernel only;
    boot.bin stays as it is."
    return 0
  fi
  problem=$(m3_stage1_problem)
  if [[ -n $problem ]]; then
    if ((kept)); then m3_kept_refusal "m1n1 stage 1 $M3_STAGE1_VERSIONS" "$problem"; fi
    ((M3_TRY == 0)) || die "--m3-handoff: this release's M3 m1n1 is only checked with m1n1 stage 1
    $M3_STAGE1_VERSIONS, and this Mac differs: $problem. Nothing was installed."
    warn "the $(m3_handoff_name) is only checked with m1n1 stage 1 $M3_STAGE1_VERSIONS, and
    this Mac differs: $problem. Installing the kernel only; boot.bin stays as it is."
    return 0
  fi
  if ((M3_TRY)); then again+=" with --m3-handoff"; fi
  if update_m1n1_frozen_by_others; then
    die "$UPDATE_M1N1_CONF sets M1N1_UPDATE_DISABLED, and not from this script or the M3
    bring-up's install-m3gpu.sh. Switching the $(m3_handoff_name) on needs m1n1's boot.bin
    rebuilt with this release's m1n1. Remove that line and $again. Nothing was installed."
  fi
  if update_m1n1_customised; then
    die "$UPDATE_M1N1_CONF points update-m1n1 at its own m1n1, U-Boot, config or target
    (M1N1=, SOURCE=, U_BOOT=, CONFIG= or TARGET=). The $(m3_handoff_name) needs boot.bin
    built from this release's m1n1. Remove those lines and $again. Nothing was installed."
  fi
  M3_MODE=handoff
  if ((kept && air)); then
    say "M3 MacBook Air ($board): this Mac has m1n1's $(m3_handoff_name) from an earlier install; keeping it"
  elif ((kept)); then
    say "M3 ($board): this Mac has m1n1's display and GPU handoff from an earlier install; keeping it"
  fi
  if ((air)); then
    if is_m3_handoff_board; then
      say "M3 MacBook Air ($board, macOS $M3_STUB_VERSION stub): installing m1n1 with the $(m3_handoff_name)"
    elif ((kept)); then
      # Its owner asked with --m3-handoff when it was installed.
      :
    elif [[ $M3_AIR_DISPLAY_HANDOFF == 1 ]]; then
      warn "--m3-handoff: trying m1n1's display handoff on this M3 MacBook Air ($board).
    It publishes the checked display state for Linux and collects GPU diagnostics;
    it does not start the GPU. Native display still needs T8122 PMP support, so the
    desktop stays on the boot framebuffer and renders in software.
    This replaces the Mac's boot loader; the steps to put the old one back from macOS follow."
    elif [[ $M3_AIR_DRY_RUN == 1 ]]; then
      warn "--m3-handoff: installing m1n1's GPU and display dry run on this M3 MacBook Air ($board).
    It reads the Air's GPU and display details and reports them for the bring-up, powering the
    GPU only for a short identity read; it switches nothing on, so the desktop stays as it is,
    on the boot framebuffer and rendered in software.
    This replaces the Mac's boot loader; the steps to put the old one back from macOS follow."
    else
      warn "--m3-handoff: trying m1n1's GPU handoff on an M3 MacBook Air ($board). It is in testing
    and not on by default. This replaces the Mac's boot loader; the steps to put the old one
    back from macOS follow. The display stays on the boot framebuffer, and the desktop keeps
    rendering in software: this is for testing the GPU handoff, not a faster desktop."
    fi
  elif is_m3_handoff_board; then
    say "M3 Pro ($board, macOS $M3_STUB_VERSION stub): installing m1n1 with the display and GPU handoff"
  elif ((!kept)); then
    warn "--m3-handoff: trying m1n1's display and GPU handoff on an M3 Pro model ($board) nobody
    has booted it on yet. This replaces the Mac's boot loader; the steps to put the old one
    back from macOS follow."
  fi
}

# After update-m1n1, boot.bin must end with the switch lines. update-m1n1
# appends /etc/m1n1.conf's lines straight after the gzipped U-Boot, so the
# first one has no newline in front: look for the block, not whole lines.
m3_verify_bootbin() {
  local target tail block size kind="M3 Pro"
  if is_m3_air; then kind="M3 Air"; fi
  target=$(esp_bootbin) || die "could not find m1n1's boot.bin to check the $kind switches in"
  size=$(stat -c %s "$M1N1_BIN")
  $sudo cmp -s -n "$size" "$M1N1_BIN" "$target" ||
    die "$target does not start with this release's m1n1 ($M1N1_BIN), so it was not rebuilt.
    The boot loader this Mac booted with is kept as m1n1/boot.bin.before-$VERSION on the EFI
    partition. Please report it before rebooting."
  block=$(m3_switches | tr ' ' '\n')
  tail=$($sudo tail -c 1024 "$target" | tr -d '\0')
  [[ $tail == *"$block"* ]] ||
    die "the rebuilt $target does not carry the $kind switch lines. The boot loader this Mac
    booted with is kept as m1n1/boot.bin.before-$VERSION on the EFI partition. Please report
    it before rebooting."
  say "m1n1's boot.bin is this release's m1n1 with the $kind handoff switches"
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
    replace_on_esp "$STATE/boot.bin.saved" "$target"
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

# --m3-gpu-experiment: stops before anything is downloaded on a Mac that is not an M3 Air, or
# when the Mesa entry is malformed.
m3_gpu_plan() {
  ((M3_GPU_EXPERIMENT)) || return 0
  is_m3_air || die "--m3-gpu-experiment is for the M3 MacBook Air (j613, j615) only, and this Mac is
    $(this_board) ($(this_soc)). Nothing was installed."
  [[ -z $M3_GPU_MESA_PACKAGE || $M3_GPU_MESA_PACKAGE =~ ^[A-Za-z0-9._+-]+\.pkg\.tar\.zst\ [0-9a-f]{64}$ ]] ||
    die "M3_GPU_MESA_PACKAGE is not \"file sha256\" (a packaging mistake). Nothing was installed."
  warn "--m3-gpu-experiment: installing the M3 Air GPU experiment's scripts with the kernel$([[ -n $M3_GPU_MESA_PACKAGE ]] && echo ", and its Mesa prefix once the kernel is in").
    They arm nothing: every boot stays as it is until air-gpu-oneshot.sh arms one."
}

# The experiment's files for the download loop, "file sha256" per line (none without the flag).
m3_gpu_files() {
  ((M3_GPU_EXPERIMENT)) || return 0
  printf '%s\n' "${M3_GPU_SCRIPTS[@]}"
  if [[ -n $M3_GPU_MESA_PACKAGE ]]; then echo "$M3_GPU_MESA_PACKAGE"; fi
  return 0
}

# After the download loop: keep the Mesa package out of the kernel's pacman -U
# ("$work"/*.pkg.tar.zst). m3_gpu_mesa_install installs it on its own, after the kernel.
m3_gpu_set_aside() {
  if ((M3_GPU_EXPERIMENT)) && [[ -n $M3_GPU_MESA_PACKAGE ]]; then
    mkdir -p "$work/m3-gpu"
    mv "$work/${M3_GPU_MESA_PACKAGE%% *}" "$work/m3-gpu/"
  fi
  return 0
}

# After the kernel's pacman -U: the scripts, what they need, and $STATE/m3-gpu-experiment,
# which --uninstall reads. The Mesa package's line is added only once it is installed.
m3_gpu_install() {
  local entry file record=""
  ((M3_GPU_EXPERIMENT)) || return 0
  for entry in "${M3_GPU_SCRIPTS[@]}"; do
    file=${entry%% *}
    $sudo install -D -m 0755 "$work/$file" "$M3_GPU_BIN/$file"
    record+="script $file ${entry#* }"$'\n'
  done
  # air-gpu-job.sh runs its job through Python and the Vulkan loader.
  $sudo pacman -S --needed --noconfirm python vulkan-icd-loader ||
    warn "could not install python and vulkan-icd-loader; air-gpu-job.sh needs them"
  printf '%s' "$record" | $sudo tee "$STATE/m3-gpu-experiment" >/dev/null
}

# The M3_GPU_MESA_NEEDS packages that are missing or older than their minimum, one
# "name version (needs min or newer)" per line; nothing when all are new enough.
m3_gpu_mesa_too_old() { mesa_needs_too_old "$M3_GPU_MESA_NEEDS"; }

# The packages of NEEDS ("name>=version" words) that are missing or older than their minimum,
# one "name version (needs min or newer)" per line; nothing when all are new enough.
mesa_needs_too_old() {
  local need name min have order
  for need in $1; do
    # name>=version, or a bare name: installed at any version.
    name=${need%%>=*} min="" order=""
    if [[ $need == *">="* ]]; then min=${need#*>=}; fi
    have=$(pacman -Q "$name" 2>/dev/null | cut -d' ' -f2) || have=""
    if [[ -n $have && -z $min ]]; then continue; fi
    if [[ -n $have ]]; then order=$(vercmp "$have" "$min" 2>/dev/null) || order=""; fi
    # vercmp prints -1, 0 or 1.
    [[ $order =~ ^[0-9]+$ ]] || echo "$name ${have:-not installed}${min:+ (needs $min or newer)}"
  done
  return 0
}

# Once the kernel install is done, on an Air with --m3-gpu-experiment: the G15G Mesa package
# in a pacman transaction of its own, so a problem with it can't stop the kernel install
# halfway. It is left out when a package it needs is missing or too old (M3_GPU_MESA_NEEDS),
# rather than letting it pull an upgrade in. Any failure is reported, and the kernel install
# stays as it is.
m3_gpu_mesa_install() {
  local file name old
  ((M3_GPU_EXPERIMENT)) && [[ -n $M3_GPU_MESA_PACKAGE ]] && is_m3_air || return 0
  file=$work/m3-gpu/${M3_GPU_MESA_PACKAGE%% *}
  old=$(m3_gpu_mesa_too_old)
  if [[ -n $old ]]; then
    warn "--m3-gpu-experiment: left out the G15G Mesa prefix, because it needs newer packages than
    this Mac has: $(paste -sd ';' <<<"$old" | sed 's/;/; /g'). Nothing of it was installed, and the
    kernel install is complete. Update the system and install them
    (sudo pacman -Syu glibc gcc-libs spirv-tools), then run this again with --m3-gpu-experiment."
    return 0
  fi
  name=$(bsdtar -xOf "$file" .PKGINFO 2>/dev/null | sed -n 's/^pkgname = //p' | head -1) || name=""
  if [[ ! $name =~ ^[A-Za-z0-9._+-]+$ ]]; then
    warn "--m3-gpu-experiment: left out the G15G Mesa prefix: ${file##*/} names no package. The
    kernel install is complete. Please report it at https://github.com/iconidentify/aurora-linux/issues"
    return 0
  fi
  say "Installing the G15G Mesa prefix ($name) on its own, now that the kernel is in"
  if ! $sudo pacman -U --noconfirm "$file"; then
    warn "--m3-gpu-experiment: could not install the G15G Mesa prefix ($name); pacman says why
    above. The kernel install is complete and stays as it is. To try again, run this again with
    --m3-gpu-experiment."
    # An earlier run's copy stays installed: keep it in the record, so --uninstall removes it.
    if pacman -Q "$name" >/dev/null 2>&1; then
      echo "mesa $name" | $sudo tee -a "$STATE/m3-gpu-experiment" >/dev/null
    fi
    return 0
  fi
  echo "mesa $name" | $sudo tee -a "$STATE/m3-gpu-experiment" >/dev/null
  M3_GPU_MESA_DONE=1
}

# What the owner reads at the end of an install with the flag, or of a plain run that keeps
# the tools of an earlier one.
m3_gpu_notice() {
  if ((M3_GPU_EXPERIMENT)); then
    say "The M3 Air GPU experiment's scripts are in $M3_GPU_BIN: air-gpu-oneshot.sh,
    air-gpu-collect.sh and air-gpu-job.sh. Nothing is armed; every boot stays normal until
      sudo air-gpu-oneshot.sh start
    arms the next boot only (sudo air-gpu-oneshot.sh --check first says whether it can)."
    if ((M3_GPU_MESA_DONE)); then
      echo "   The G15G Mesa prefix for air-gpu-job.sh: $M3_GPU_MESA_PREFIX"
    elif [[ -n $M3_GPU_MESA_PACKAGE ]]; then
      echo "   The G15G Mesa prefix was not installed (see the warning above), so air-gpu-job.sh has"
      echo "   nothing to run with yet."
    else
      echo "   This release has no G15G Mesa prefix yet, so air-gpu-job.sh has nothing to run with."
    fi
    if [[ " $(m3_switches 2>/dev/null) " != *" $M3_AIR_GPU_SWITCH "* ]]; then
      echo "   This Mac's boot loader does not hand the GPU over ($M3_AIR_GPU_SWITCH is not armed), so an"
      echo "   armed boot ends ARMED-NOT-STARTED until the boot loader hands the GPU over (see"
      echo "   https://github.com/iconidentify/aurora-linux/issues/35)."
    fi
  elif [[ -f $STATE/m3-gpu-experiment ]]; then
    echo "   Keeping the M3 Air GPU experiment's scripts from an earlier install in $M3_GPU_BIN;"
    echo "   run this again with --m3-gpu-experiment to update them to this release's."
  fi
  return 0
}

# The installed one-shot script.
m3_gpu_oneshot() { $sudo "$M3_GPU_BIN/air-gpu-oneshot.sh" "$@"; }

# Clear any armed experiment boot and its boot entry. Called before anything replaces the kernel:
# an install (a new release, or a rerun) rebuilds the UKI the armed entry pins, and --uninstall
# replaces the kernel, so an entry left armed would fail Limine's hash check at the next boot.
# Returns 0 when the experiment was never installed, or nothing was armed; non-zero when the
# disarm failed (the caller stops before changing anything).
m3_gpu_disarm() {
  [[ -f $STATE/m3-gpu-experiment && -x $M3_GPU_BIN/air-gpu-oneshot.sh ]] || return 0
  say "Clearing any armed M3 Air GPU experiment boot before the kernel is replaced (arm it again after the reboot)"
  m3_gpu_oneshot --disarm
}

m3_gpu_remove() {
  local kind name leftover=0
  [[ -f $STATE/m3-gpu-experiment ]] || return 0
  # uninstall_all already cleared any armed boot, before the kernel was replaced.
  while read -r kind name _; do
    case $kind in
      script) [[ $name =~ ^air-gpu-[a-z]+\.sh$ ]] && $sudo rm -f "$M3_GPU_BIN/$name" ;;
      mesa) [[ $name =~ ^[A-Za-z0-9._+-]+$ ]] && { $sudo pacman -Rns --noconfirm "$name" ||
        warn "could not remove $name"; } ;;
    esac
  done <"$STATE/m3-gpu-experiment"
  # Any Mesa package still installed under a name a later release renamed (the record is
  # overwritten by a plain rerun), plus the state dir's own leftovers.
  for name in $(pacman -Qq 2>/dev/null | grep -E '^mesa-m3-g15g' || true); do
    $sudo pacman -Rns --noconfirm "$name" 2>/dev/null && leftover=1 || true
  done
  ((leftover == 0)) || warn "removed a leftover G15G Mesa package"
  $sudo rm -rf /var/lib/air-gpu
  say "Removed the M3 Air GPU experiment's scripts"
}

# ---- the M3 Pro's Mesa (t6030, on by default) --------------------------------------------------
# On an M3 Pro, the release's mesa-m3 package is downloaded and checked with the other release
# assets, then installed in a pacman transaction of its own once the kernel install is done.
# Everything that makes a login session use its prefix belongs to the package itself, so
# pacman -R mesa-m3 undoes it all; this script writes no file of its own for it. It writes one
# record, $STATE/m3-pro-mesa, which --uninstall and the lab check read. Every other Mac (M1, M2,
# the M3 Max, the M3 MacBook Air) never downloads, installs or records anything of it.

m3_pro_mesa_file() { echo "${M3_PRO_MESA_PACKAGE%% *}"; }
# The package's version (pkgver-pkgrel) from its file name, <name>-<version>-<arch>.pkg.tar.zst.
m3_pro_mesa_version() {
  local f
  f=$(m3_pro_mesa_file)
  f=${f#"$M3_PRO_MESA_NAME"-}
  echo "${f%-*.pkg.tar.zst}"
}
# The installed mesa-m3's version, or nothing.
m3_pro_mesa_installed() { pacman -Q "$M3_PRO_MESA_NAME" 2>/dev/null | cut -d' ' -f2 || true; }

# The user whose session the package serves: the one who ran this (through sudo, or not).
m3_pro_mesa_user() { echo "${SUDO_USER:-$(id -un)}"; }
m3_pro_mesa_user_home() { getent passwd "$1" 2>/dev/null | cut -d: -f6; }

# DIR holds Mesa libraries (the list's ldpath rule).
m3_pro_mesa_has_mesa() {
  local f
  for f in "$1"/libgallium* "$1"/libEGL_mesa* "$1"/libvulkan_asahi* "$1"/dri/*_dri.so; do
    if [[ -e $f ]]; then return 0; fi
  done
  return 1
}

# VALUE points into the prefix (after spaces and quotes are trimmed).
m3_pro_mesa_ours() {
  local v=$1
  while [[ $v == [[:space:]\"\']* ]]; do v=${v:1}; done
  while [[ $v == *[[:space:]\"\'] ]]; do v=${v%?}; done
  [[ $v == "$M3_PRO_MESA_PREFIX" || $v == "$M3_PRO_MESA_PREFIX"/* ]]
}

# This script's own copy of mesa-m3's user-setup check (mesa-m3-user-setup, files only), from
# M3_PRO_MESA_SETUP_RULES, for the home HOME. It prints NUL-terminated pairs, a path then what it
# holds, one per file in the list's order, so any path comes through whole. This script's own
# environment is not the session's, so only files count. "~" is HOME, "{config}" is HOME/.config,
# and other paths are under M3_PRO_MESA_SETUP_ROOT (tests).
#   variable V         an envfile line (not a comment, # or --) naming V, without the prefix
#                      ("PREFIX/" in it, or the line ending in PREFIX)
#   ldpath V           an envfile line naming V with a directory, other than PREFIX/lib, that
#                      holds libgallium*, libEGL_mesa*, libvulkan_asahi* or dri/*_dri.so
#   envfile GLOB       a file searched for those two
#   drirc FILE         a drirc file with a dri_driver option
#   chonkstep FILE     chonkstep's M3 settings: the last CHONKSTEP_M3_MESA_PREFIX names another
#                      prefix, or there is none (chonkstep then uses its own default prefix)
# It changes nothing. fill-m3-pro-mesa.sh keeps the list the package's.
m3_pro_mesa_builtin_setup() {
  local LC_ALL=C home=$1 kind item path fixed rest f line v t d hit vre lre
  local vars=() ldvars=() items=()
  local -A seen=()
  [[ -n $home && -d $home ]] || return 0
  while read -r kind item; do
    case $kind in
      variable) vars+=("$item") ;;
      ldpath) ldvars+=("$item") ;;
      *) items+=("$kind $item") ;;
    esac
  done <<<"$M3_PRO_MESA_SETUP_RULES"
  vre="(^|[^A-Za-z0-9_])($(IFS='|'; echo "${vars[*]}"))([^A-Za-z0-9_]|\$)"
  lre="(^|[^A-Za-z0-9_])($(IFS='|'; echo "${ldvars[*]}"))([^A-Za-z0-9_]|\$)"
  for item in "${items[@]}"; do
    kind=${item%% *} item=${item#* }
    case $item in
      "~"/*) path=$home/${item#"~/"} ;;
      "{config}"/*) path=$home/.config/${item#"{config}/"} ;;
      *) path=$M3_PRO_MESA_SETUP_ROOT$item ;;
    esac
    # Before the list's first glob the path may hold spaces; the glob part is the list's own.
    fixed=$path rest=""
    while [[ $fixed == *[*?[]* ]]; do rest=${fixed##*/}${rest:+/$rest} fixed=${fixed%/*}; done
    # shellcheck disable=SC2086 # the list's glob, unquoted on purpose
    for f in "$fixed"${rest:+/}$rest; do
      [[ -f $f && -r $f && -z ${seen[$f]:-} ]] || continue
      seen[$f]=1 hit=""
      case $kind in
        envfile)
          while IFS= read -r line || [[ -n $line ]]; do
            [[ $line =~ ^[[:space:]]*(#|--) ]] && continue
            if [[ $line =~ $vre ]]; then
              v=${BASH_REMATCH[2]} t=$line
              while [[ $t == *[[:space:]\"\'] ]]; do t=${t%?}; done
              if [[ $line != *"$M3_PRO_MESA_PREFIX/"* && $t != *"$M3_PRO_MESA_PREFIX" ]]; then
                hit="sets $v"
                break
              fi
            fi
            if [[ $line =~ $lre ]]; then
              line=${line//\$\{HOME\}/$home} line=${line//\$HOME/$home} line=${line//\~\//$home/}
              while [[ $line =~ (/[^:\"\'\ ,\;\)]+)(.*) ]]; do
                d=${BASH_REMATCH[1]} line=${BASH_REMATCH[2]}
                while [[ $d == */ && $d != / ]]; do d=${d%/}; done
                [[ $d != "$M3_PRO_MESA_PREFIX/lib" ]] || continue
                [[ $d == "$home"/* ]] || d=$M3_PRO_MESA_SETUP_ROOT$d
                if m3_pro_mesa_has_mesa "$d"; then
                  hit="puts another Mesa in LD_LIBRARY_PATH (${d#"$M3_PRO_MESA_SETUP_ROOT"})"
                  break 2
                fi
              done
            fi
          done <"$f"
          ;;
        drirc)
          if grep -Eq 'name[[:space:]]*=[[:space:]]*"dri_driver"' "$f" 2>/dev/null; then
            hit="chooses a driver (dri_driver)"
          fi
          ;;
        chonkstep)
          v=$(sed -n -E 's/^[[:space:]]*(export[[:space:]]+)?CHONKSTEP_M3_MESA_PREFIX=//p' "$f" 2>/dev/null | tail -n 1)
          v=${v%% #*}
          while [[ $v == [[:space:]\"\']* ]]; do v=${v:1}; done
          while [[ $v == *[[:space:]\"\'] ]]; do v=${v%?}; done
          if [[ -z $v ]]; then
            hit="sets no CHONKSTEP_M3_MESA_PREFIX: chonkstep's M3 launcher uses its own default prefix"
          elif ! m3_pro_mesa_ours "$v"; then
            hit="CHONKSTEP_M3_MESA_PREFIX=$v"
          fi
          ;;
      esac
      if [[ -n $hit ]]; then printf '%s\0%s\0' "$f" "$hit"; fi
    done
  done
  return 0
}

# mesa-m3's own detector for HOME (mesa-m3-user-setup --home HOME: files only), its answer as
# the same pairs as m3_pro_mesa_builtin_setup's. Returns non-zero when it fails (its exit 2, or
# anything but 0 for none and 1 for present) or answers in another form.
m3_pro_mesa_detector() {
  local out rc=0 line want p d w paths=() details=()
  out=$("$M3_PRO_MESA_DETECTOR" --home "$1" 2>/dev/null) || rc=$?
  ((rc == 0 || rc == 1)) || return "$rc"
  [[ ${out%%$'\n'*} == "schema=aurora.mesa-m3-user-setup/1" ]] || return 3
  want=none
  while IFS= read -r line; do
    case $line in
      user_setup=*) want=${line#*=} ;;
      user_setup_path=*) paths+=("${line#*=}") ;;
      user_setup_detail=*) details+=("${line#*=}") ;;
    esac
  done <<<"$out"
  if ((${#paths[@]})); then
    [[ $want == present && $rc == 1 ]] || return 3
  else
    [[ $want == none && $rc == 0 ]] || return 3
  fi
  for p in "${paths[@]}"; do
    w=""
    for d in "${details[@]}"; do
      if [[ $d == "$p"* ]]; then
        w=${d#"$p"} w=${w#:} w=${w# }
        break
      fi
    done
    printf '%s\0%s\0' "$p" "${w:-a Mesa setup}"
  done
}

# The invoking user's own Mesa setup, as pairs in M3_PRO_MESA_SETUP, and where it came from in
# M3_PRO_MESA_SETUP_SOURCE: package-detector (mesa-m3's detector, once mesa-m3 is installed) or
# installer-builtin (before that, without the detector, or when the detector failed: one
# warning). M3_PRO_MESA_SETUP_OK is 0 when no check gave an answer (one warning too).
M3_PRO_MESA_SETUP=() M3_PRO_MESA_SETUP_SOURCE="" M3_PRO_MESA_SETUP_OK=1
m3_pro_mesa_setup_collect() {
  local home rc=0
  home=$(m3_pro_mesa_user_home "$(m3_pro_mesa_user)")
  M3_PRO_MESA_SETUP=() M3_PRO_MESA_SETUP_SOURCE=installer-builtin M3_PRO_MESA_SETUP_OK=1
  if [[ -n $home && -n $(m3_pro_mesa_installed) && -x $M3_PRO_MESA_DETECTOR ]]; then
    mapfile -d '' -t M3_PRO_MESA_SETUP < <(m3_pro_mesa_detector "$home")
    wait $! || rc=$?
    if ((rc == 0 && ${#M3_PRO_MESA_SETUP[@]} % 2 == 0)); then
      M3_PRO_MESA_SETUP_SOURCE=package-detector
      return 0
    fi
    warn "$M3_PRO_MESA_DETECTOR failed (exit $rc); the record's user setup comes from this script's own check"
  fi
  rc=0
  mapfile -d '' -t M3_PRO_MESA_SETUP < <(m3_pro_mesa_builtin_setup "$home")
  wait $! || rc=$?
  if ((rc != 0 || ${#M3_PRO_MESA_SETUP[@]} % 2 != 0)); then
    M3_PRO_MESA_SETUP=() M3_PRO_MESA_SETUP_OK=0
    warn "this script's own check of $(m3_pro_mesa_user)'s Mesa setup failed (exit $rc); the record says user_setup=unknown"
  fi
}

# The package's switch-offs that are set, as NUL-terminated pairs like m3_pro_mesa_builtin_setup's
# (an empty path for mesa_m3=off on the kernel command line). Left as they are.
m3_pro_mesa_optout() {
  local home
  home=$(m3_pro_mesa_user_home "$(m3_pro_mesa_user)")
  if [[ -e $M3_PRO_MESA_DISABLE ]]; then printf '%s\0%s\0' "$M3_PRO_MESA_DISABLE" "off for every user"; fi
  if [[ -n $home && -e $home/.config/mesa-m3/disable ]]; then
    printf '%s\0%s\0' "$home/.config/mesa-m3/disable" "off for $(m3_pro_mesa_user)"
  fi
  if [[ " $(cat /proc/cmdline 2>/dev/null) " == *" mesa_m3=off "* ]]; then
    printf '%s\0%s\0' "" "mesa_m3=off on the kernel command line"
  fi
  return 0
}

# "path: what; path: what" for the summary, from such pairs on stdin. A path with a newline or a
# carriage return in it is shown quoted ($'...').
m3_pro_mesa_describe() {
  local p w out=""
  while IFS= read -r -d '' p && IFS= read -r -d '' w; do
    if [[ $p == *[$'\n\r']* ]]; then p=$(printf '%q' "$p"); fi
    out+="${out:+; }${p:+$p: }$w"
  done
  echo "$out"
}

# A scalar value of this Mac's record, read as data, never sourced. Nothing when there is no
# record, or its first line is not this script's schema.
m3_pro_mesa_record_ok() {
  local first="" rec=$STATE/$M3_PRO_MESA_RECORD_NAME
  [[ -f $rec ]] || return 1
  IFS= read -r first <"$rec" || true
  [[ $first == "schema=$M3_PRO_MESA_SCHEMA" ]]
}
m3_pro_mesa_record_get() {
  m3_pro_mesa_record_ok || return 0
  sed -n "/^$1=/{s/^$1=//p;q}" "$STATE/$M3_PRO_MESA_RECORD_NAME"
}

# Before anything is downloaded: decides whether this run installs the package, and stops on a
# malformed entry. Says one line when the package is left out by --no-m3-mesa or is current.
M3_PRO_MESA_PREEXISTING=""
m3_pro_mesa_plan() {
  local have want order
  is_m3_pro && [[ -n $M3_PRO_MESA_PACKAGE ]] || return 0
  have=$(m3_pro_mesa_installed)
  # Before the first record (or with one this script can't read): what was installed before.
  if ! m3_pro_mesa_record_ok; then M3_PRO_MESA_PREEXISTING=${have:-none}; fi
  if ((!M3_PRO_MESA)); then
    M3_PRO_MESA_RESULT=skipped-flag
    say "--no-m3-mesa: leaving out the M3 Pro's Mesa ($M3_PRO_MESA_NAME)${have:+; the $have installed earlier stays as it is}"
    return 0
  fi
  [[ $M3_PRO_MESA_PACKAGE =~ ^${M3_PRO_MESA_NAME}-[A-Za-z0-9._+:]+-[0-9.]+-(aarch64|any)\.pkg\.tar\.zst\ [0-9a-f]{64}$ &&
    $M3_PRO_MESA_PACKAGE != *PENDING* ]] ||
    die "M3_PRO_MESA_PACKAGE is not \"file sha256\" for $M3_PRO_MESA_NAME (a packaging mistake). Nothing was installed."
  [[ $M3_PRO_MESA_NEEDS =~ ^([A-Za-z0-9._+-]+(\>=[A-Za-z0-9._+:-]+)?[[:space:]]*)+$ && $M3_PRO_MESA_NEEDS != *PENDING* ]] ||
    die "M3_PRO_MESA_NEEDS is not a list of name>=version or name (a packaging mistake). Nothing was installed."
  [[ $M3_PRO_MESA_DETECTOR =~ ^/[A-Za-z0-9._/+-]+$ && $M3_PRO_MESA_SETUP_LIST =~ ^/[A-Za-z0-9._/+-]+$ &&
    $M3_PRO_MESA_DETECTOR$M3_PRO_MESA_SETUP_LIST != *PENDING* ]] ||
    die "M3_PRO_MESA_DETECTOR and M3_PRO_MESA_SETUP_LIST are not mesa-m3's paths (a packaging mistake). Nothing was installed."
  [[ -n $have ]] || return 0
  want=$(m3_pro_mesa_version)
  order=$(vercmp "$have" "$want" 2>/dev/null) || order=""
  if [[ $order == 0 ]]; then
    M3_PRO_MESA_RESULT=current
    say "The M3 Pro's Mesa ($M3_PRO_MESA_NAME $have) is installed already; nothing new to install"
  elif [[ $order == 1 ]]; then
    M3_PRO_MESA_RESULT=newer-kept
    say "A newer $M3_PRO_MESA_NAME ($have) than this release's ($want) is installed; keeping it"
  fi
  return 0
}

# The package for the download loop, "file sha256" (only when this run installs it).
m3_pro_mesa_files() {
  is_m3_pro && [[ -n $M3_PRO_MESA_PACKAGE ]] && ((M3_PRO_MESA)) && [[ -z $M3_PRO_MESA_RESULT ]] || return 0
  echo "$M3_PRO_MESA_PACKAGE"
}

# After the download loop: keep the package out of the kernel's pacman -U ("$work"/*.pkg.tar.zst).
m3_pro_mesa_set_aside() {
  local file
  file=$(m3_pro_mesa_file)
  [[ -n $M3_PRO_MESA_PACKAGE && -f $work/$file ]] || return 0
  mkdir -p "$work/m3-pro"
  mv "$work/$file" "$work/m3-pro/"
}

# Once the kernel install is done, on an M3 Pro: the package in a pacman transaction of its own,
# so a problem with it can't stop the kernel install halfway. It is left out when a package it
# needs is missing or too old, rather than letting it pull an upgrade in. Any failure is a
# warning, the kernel install stays as it is, and the summary and exit status say so.
m3_pro_mesa_install() {
  local file old name
  file=$work/m3-pro/$(m3_pro_mesa_file)
  [[ -n $M3_PRO_MESA_PACKAGE && -f $file ]] || return 0
  old=$(mesa_needs_too_old "$M3_PRO_MESA_NEEDS")
  if [[ -n $old ]]; then
    M3_PRO_MESA_RESULT=skipped-deps
    warn "left out the M3 Pro's Mesa ($M3_PRO_MESA_NAME), because it needs newer packages than this
    Mac has: $(paste -sd ';' <<<"$old" | sed 's/;/; /g'). Nothing of it was installed, and the
    kernel install is complete. Update them (sudo pacman -Syu $(cut -d' ' -f1 <<<"$old" | paste -sd' ')),
    then run this again."
    return 0
  fi
  name=$(bsdtar -xOf "$file" .PKGINFO 2>/dev/null | sed -n 's/^pkgname = //p' | head -1) || name=""
  if [[ $name != "$M3_PRO_MESA_NAME" ]]; then
    M3_PRO_MESA_RESULT=failed
    warn "left out the M3 Pro's Mesa: ${file##*/} names the package \"$name\", not $M3_PRO_MESA_NAME.
    The kernel install is complete. Please report it at https://github.com/iconidentify/aurora-linux/issues"
    return 0
  fi
  say "Installing the M3 Pro's Mesa ($name $(m3_pro_mesa_version)) on its own, now that the kernel is in"
  if ! $sudo pacman -U --noconfirm "$file"; then
    M3_PRO_MESA_RESULT=failed
    warn "could not install the M3 Pro's Mesa ($name); pacman says why above. Nothing of it was
    installed by this run, and the kernel install is complete and stays as it is. Run this again
    to try again."
    return 0
  fi
  M3_PRO_MESA_RESULT=installed
}

# The record of this run, on an M3 Pro: $STATE/m3-pro-mesa, one "key=value" per line, for
# --uninstall and the lab check. Read it as data, never source it. The first line is
# schema=aurora.m3-pro-mesa-state/1; a record with another first line is not read. Every key
# appears exactly once, except user_setup_path, opt_out_path and record_error_key (one line per
# item, each path whole). The keys, in order:
#   schema; run_id (this run's, also in its summary line), release (this script's tag),
#   written_at (UTC, ISO 8601), boot_id, kernel (uname -r) and board (the first device-tree
#   compatible string), all of this run;
#   installer_sha256 and installer_source: the sha256 of the script file this run reads and
#     "file", or "unavailable" and "stdin" when it came through a pipe;
#   package, version, file, sha256 (this release's mesa-m3 entry) and prefix;
#   result (below), installed_version (or none), installed_by (below), preexisting (the mesa-m3
#     version installed before this script first wrote a record on this Mac, or none);
#   user (the invoking user); user_setup_source (package-detector or installer-builtin),
#     user_setup (present, none, or unknown when the check failed) and a user_setup_path line per
#     file of that user's own Mesa setup; opt_out (present or none), an opt_out_path line per switch-off file and
#     opt_out_cmdline (yes or no); all left as they are;
#   created_files: 0, as the package owns every file.
# result, what this run did:
#   installed     this run installed the package (none was installed, or an older one)
#   current       the same version was installed already: nothing new was installed
#   newer-kept    a newer mesa-m3 was installed, and was kept
#   skipped-flag  --no-m3-mesa: left out; one installed earlier stays
#   skipped-deps  a package it needs is missing or too old: left out (exit status 3)
#   failed        its pacman -U failed, or its file holds another package (exit status 3)
#   record-error  this run could not write its record (an error record, below)
# installed_by, who put the installed mesa-m3 there:
#   installer     this script's pacman -U. Set by a run that installs it, and kept by later runs
#                 while the same version stays installed.
#   owner         anyone else: installed before this script first ran, or replaced or
#                 installed outside it since (another version than the record's).
#   none          no mesa-m3 is installed.
# When a value holds a newline or a carriage return, or the write fails before its rename, the
# previous run's record must not look current: an error record replaces it (schema, the run's
# keys from run_id to installer_source, package, result=record-error, record_error (value or
# write), a record_error_key line per refused key, never its value, and installed_version,
# installed_by and preexisting, which keep the ownership history). If that write fails too, the
# previous record is moved aside to m3-pro-mesa.stale-<its written_at> (never over another file),
# so no record is current. Every failure is in the one warning, and the exit status is 4.
# --uninstall removes mesa-m3 only when installed_by is installer, the installed version is the
# record's, preexisting is none, and result is not record-error. The lab's positive check:
# result installed or current, installed_by installer, installed_version the pinned version,
# sha256 the pinned package's sha256, and run_id the one in the summary line of the installer
# run under test. The record says what this script did, not what renders.
m3_pro_mesa_record() {
  local have by pre k v p w rc errs="" cause bad="" keys=() optout=() lines=() head=() elines=()
  local rec=$STATE/$M3_PRO_MESA_RECORD_NAME
  is_m3_pro && [[ -n $M3_PRO_MESA_PACKAGE ]] || return 0
  have=$(m3_pro_mesa_installed)
  pre=$M3_PRO_MESA_PREEXISTING
  if [[ -z $pre ]]; then pre=$(m3_pro_mesa_record_get preexisting); fi
  if [[ -z $have ]]; then
    by=none
  elif [[ $M3_PRO_MESA_RESULT == installed ]] ||
    [[ $(m3_pro_mesa_record_get installed_by) == installer && $(m3_pro_mesa_record_get installed_version) == "$have" ]]; then
    by=installer
  else
    by=owner
  fi
  m3_pro_mesa_setup_collect
  mapfile -d '' -t optout < <(m3_pro_mesa_optout)
  head=("schema=$M3_PRO_MESA_SCHEMA" "run_id=$RUN_ID" "release=$TAG" "written_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    "boot_id=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo unavailable)"
    "kernel=$(uname -r)" "board=$(tr '\0' '\n' <"$DT/compatible" 2>/dev/null | sed -n 1p)"
    "installer_sha256=$SELF_SHA256" "installer_source=$SELF_SOURCE")
  lines=("${head[@]}" "package=$M3_PRO_MESA_NAME" "version=$(m3_pro_mesa_version)" "file=$(m3_pro_mesa_file)"
    "sha256=${M3_PRO_MESA_PACKAGE#* }" "prefix=$M3_PRO_MESA_PREFIX" "result=$M3_PRO_MESA_RESULT"
    "installed_version=${have:-none}" "installed_by=$by" "preexisting=${pre:-none}"
    "user=$(m3_pro_mesa_user)" "user_setup_source=$M3_PRO_MESA_SETUP_SOURCE")
  if ((!M3_PRO_MESA_SETUP_OK)); then
    lines+=(user_setup=unknown)
  elif ((${#M3_PRO_MESA_SETUP[@]})); then
    lines+=(user_setup=present)
  else
    lines+=(user_setup=none)
  fi
  for ((k = 0; k < ${#M3_PRO_MESA_SETUP[@]}; k += 2)); do
    if [[ -n ${M3_PRO_MESA_SETUP[k]} ]]; then lines+=("user_setup_path=${M3_PRO_MESA_SETUP[k]}"); fi
  done
  if ((${#optout[@]})); then lines+=(opt_out=present); else lines+=(opt_out=none); fi
  v=no
  for ((k = 0; k < ${#optout[@]}; k += 2)); do
    if [[ -n ${optout[k]} ]]; then lines+=("opt_out_path=${optout[k]}"); else v=yes; fi
  done
  lines+=("opt_out_cmdline=$v" "created_files=0")
  for p in "${lines[@]}"; do
    w=${p#*=}
    if [[ $w == *[$'\n\r']* ]]; then
      bad+="${bad:+, }${p%%=*}=$(printf '%q' "$w")"
      [[ " ${keys[*]} " == *" ${p%%=*} "* ]] || keys+=("${p%%=*}")
    fi
  done

  M3_PRO_MESA_RECORD_RESULT=$M3_PRO_MESA_RESULT
  if [[ -z $bad ]]; then
    rc=0
    m3_pro_mesa_record_write "${lines[@]}" || rc=$?
    if ((rc == 0)); then
      M3_PRO_MESA_RECORD_WRITE=ok
    elif ((rc == 2)); then
      M3_PRO_MESA_RECORD_WRITE=unsynced
      warn "wrote $rec, but its durability is not confirmed: $M3_PRO_MESA_WRITE_ERR"
    fi
    if ((rc != 1)); then m3_pro_mesa_record_leftovers; return 0; fi
    cause=write errs="this run's record: $M3_PRO_MESA_WRITE_ERR"
  else
    cause=value errs="a value holds a newline or a carriage return ($bad)"
  fi

  # The error record: the run's keys (a refused one as "unavailable"), never a refused value.
  for p in "${head[@]}"; do
    if [[ ${p#*=} == *[$'\n\r']* ]]; then elines+=("${p%%=*}=unavailable"); else elines+=("$p"); fi
  done
  elines+=("package=$M3_PRO_MESA_NAME" "result=record-error" "record_error=$cause")
  for k in "${keys[@]}"; do elines+=("record_error_key=$k"); done
  elines+=("installed_version=${have:-none}" "installed_by=$by" "preexisting=${pre:-none}")
  M3_PRO_MESA_RECORD_RESULT="record-error"
  rc=0
  m3_pro_mesa_record_write "${elines[@]}" || rc=$?
  if ((rc == 0)); then
    M3_PRO_MESA_RECORD_WRITE=error-record
    warn "did not write this run's record ($errs). An error record (result=record-error) replaces
    $rec, so no earlier run's record looks current."
  elif ((rc == 2)); then
    M3_PRO_MESA_RECORD_WRITE=error-record-unsynced
    warn "did not write this run's record ($errs). An error record (result=record-error) replaces
    $rec, but its durability is not confirmed: $M3_PRO_MESA_WRITE_ERR"
  else
    errs+="; the error record: $M3_PRO_MESA_WRITE_ERR"
    M3_PRO_MESA_RECORD_RESULT=""
    if [[ ! -e $rec ]]; then
      M3_PRO_MESA_RECORD_WRITE=none
      warn "could not write this run's record or an error record ($errs). There is no record."
    elif m3_pro_mesa_record_aside; then
      M3_PRO_MESA_RECORD_WRITE=none
      warn "could not write this run's record or an error record ($errs). The previous record is
      now $M3_PRO_MESA_ASIDE, so no record is current.${M3_PRO_MESA_WRITE_ERR:+ But $M3_PRO_MESA_WRITE_ERR.}"
    else
      M3_PRO_MESA_RECORD_WRITE=stale
      warn "could not write this run's record or an error record ($errs), and could not move the
      previous record aside ($M3_PRO_MESA_WRITE_ERR): $rec is an earlier run's record, not this
      run's."
    fi
  fi
  m3_pro_mesa_record_leftovers
}

# Writes LINES as the record: an exclusive temporary file in $STATE made through $sudo (so
# root's), written in full, mode 0644, fsynced, renamed over the record, then $STATE fsynced.
# Returns 0 when all of that worked; 1 when it failed before the rename (the previous record, if
# any, is untouched, and only this call's own temporary file is removed); 2 when the new record
# is in place but the fsync of $STATE failed (written, durability not confirmed).
# M3_PRO_MESA_WRITE_ERR says what failed.
M3_PRO_MESA_WRITE_ERR=""
m3_pro_mesa_record_write() {
  local rec=$STATE/$M3_PRO_MESA_RECORD_NAME tmp
  M3_PRO_MESA_WRITE_ERR=""
  if ! tmp=$($sudo mktemp "$STATE/.$M3_PRO_MESA_RECORD_NAME.XXXXXX"); then
    M3_PRO_MESA_WRITE_ERR="no temporary file could be made in $STATE"
    return 1
  fi
  if ! printf '%s\n' "$@" | $sudo tee "$tmp" >/dev/null; then
    M3_PRO_MESA_WRITE_ERR="writing $tmp failed"
  elif ! $sudo chmod 0644 "$tmp"; then
    M3_PRO_MESA_WRITE_ERR="chmod 0644 $tmp failed"
  elif ! $sudo sync "$tmp"; then
    M3_PRO_MESA_WRITE_ERR="fsync of $tmp failed"
  elif ! $sudo mv -f "$tmp" "$rec"; then
    M3_PRO_MESA_WRITE_ERR="renaming $tmp to $rec failed"
  elif ! $sudo sync "$STATE"; then
    M3_PRO_MESA_WRITE_ERR="fsync of $STATE failed after the rename"
    return 2
  else
    return 0
  fi
  $sudo rm -f "$tmp" || M3_PRO_MESA_WRITE_ERR+="; $tmp could not be removed"
  return 1
}

# Moves the previous record aside to m3-pro-mesa.stale-<its written_at> (or .stale-unknown), with
# .1, .2, ... when that name is taken; never over an existing file. The new name is in
# M3_PRO_MESA_ASIDE. Returns 1, with M3_PRO_MESA_WRITE_ERR, when it could not; a failed fsync of
# $STATE afterwards goes in M3_PRO_MESA_WRITE_ERR too.
M3_PRO_MESA_ASIDE=""
m3_pro_mesa_record_aside() {
  local rec=$STATE/$M3_PRO_MESA_RECORD_NAME ts name n=0
  M3_PRO_MESA_WRITE_ERR="" M3_PRO_MESA_ASIDE=""
  ts=$(sed -n '/^written_at=/{s/^written_at=//p;q}' "$rec" 2>/dev/null) || ts=""
  [[ $ts =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$ ]] || ts=unknown
  name=$rec.stale-$ts
  while ((n < 100)); do
    if [[ ! -e $name && ! -L $name ]]; then
      $sudo mv -n "$rec" "$name" || true
      if [[ ! -e $rec && -f $name ]]; then
        M3_PRO_MESA_ASIDE=$name
        $sudo sync "$STATE" || M3_PRO_MESA_WRITE_ERR="the fsync of $STATE failed after the move"
        return 0
      fi
      if [[ ! -e $name ]]; then
        M3_PRO_MESA_WRITE_ERR="moving $rec to $name failed"
        return 1
      fi
    fi
    n=$((n + 1)) name=$rec.stale-$ts.$n
  done
  M3_PRO_MESA_WRITE_ERR="no free name for $rec.stale-$ts"
  return 1
}

# Says, in one line, when temporary record files of another or an interrupted run are in
# $STATE. They are left alone: another run may still be writing one.
m3_pro_mesa_record_leftovers() {
  local left=() f
  for f in "$STATE/.$M3_PRO_MESA_RECORD_NAME".??????; do
    if [[ -e $f ]]; then left+=("$f"); fi
  done
  if ((${#left[@]})); then
    say "Left alone: ${#left[@]} temporary record file(s) of another or an interrupted run ($STATE/.$M3_PRO_MESA_RECORD_NAME.*)"
  fi
  return 0
}

# What the owner reads at the end of an install on an M3 Pro.
m3_pro_mesa_notice() {
  local want have setup="" optout
  is_m3_pro && [[ -n $M3_PRO_MESA_PACKAGE ]] || return 0
  want=$(m3_pro_mesa_version) have=$(m3_pro_mesa_installed)
  case $M3_PRO_MESA_RESULT in
    installed)
      echo "   The M3 Pro's Mesa ($M3_PRO_MESA_NAME $want, in $M3_PRO_MESA_PREFIX) is installed. The new"
      echo "   graphics take effect at the next login: the reboot does that."
      if [[ $M3_MODE != handoff ]]; then
        echo "   This M3 Pro boots kernel-only (no GPU handoff), so at login the package finds no GPU,"
        echo "   leaves itself off and the desktop keeps rendering in software."
      fi ;;
    current)
      echo "   The M3 Pro's Mesa ($M3_PRO_MESA_NAME $have) was installed already; nothing new was installed." ;;
    newer-kept)
      echo "   A newer $M3_PRO_MESA_NAME ($have) than this release's ($want) is installed and was kept." ;;
    skipped-flag)
      if [[ -n $have ]]; then
        echo "   --no-m3-mesa: this run left the M3 Pro's Mesa out; the $M3_PRO_MESA_NAME $have installed"
        echo "   earlier stays installed (sudo pacman -R $M3_PRO_MESA_NAME removes it)."
      else
        echo "   --no-m3-mesa: the M3 Pro's Mesa was not installed, so the desktop renders in software."
      fi ;;
    skipped-deps)
      echo "   The M3 Pro's Mesa was NOT installed: it needs newer packages (see the warning above)."
      echo "   The kernel install is complete; until it is installed the desktop renders in software."
      echo "   Update the packages the warning names, then run this again. The exit status is 3." ;;
    failed)
      echo "   The M3 Pro's Mesa was NOT installed: pacman could not install it (see above). The"
      echo "   kernel install is complete; until it is installed the desktop renders in software."
      echo "   Run this again to try again. The exit status is 3." ;;
  esac
  if ((${#M3_PRO_MESA_SETUP[@]})); then setup=$(printf '%s\0' "${M3_PRO_MESA_SETUP[@]}" | m3_pro_mesa_describe); fi
  if [[ -n $setup ]]; then
    echo "   Left as it is: $(m3_pro_mesa_user)'s own M3 Mesa setup ($setup)."
  fi
  optout=$(m3_pro_mesa_optout | m3_pro_mesa_describe)
  if [[ -n $optout && -n $have ]]; then
    echo "   Left as it is: the M3 Pro's Mesa is switched off at login by $optout."
  fi
  case $M3_PRO_MESA_RECORD_WRITE in
    ok) ;;
    unsynced | error-record-unsynced)
      echo "   This run's record ($STATE/$M3_PRO_MESA_RECORD_NAME) was written, but its durability is not"
      echo "   confirmed (see the warning above). The exit status is 4." ;;
    *)
      echo "   This run could not write its record ($STATE/$M3_PRO_MESA_RECORD_NAME; see the warning above)."
      echo "   The exit status is 4." ;;
  esac
  echo "   m3-pro-mesa record: run_id=$RUN_ID result=${M3_PRO_MESA_RECORD_RESULT:-none} write=${M3_PRO_MESA_RECORD_WRITE:-none}"
  return 0
}

# After a complete kernel install, on an M3 Pro: exit status 4 when this run's record is not
# written and durable (whatever happened to the Mesa: the summary line says), else 3 when the
# M3 Pro's Mesa was not installed.
m3_pro_mesa_status() {
  is_m3_pro && [[ -n $M3_PRO_MESA_PACKAGE ]] || return 0
  [[ $M3_PRO_MESA_RECORD_WRITE == ok ]] || return 4
  case $M3_PRO_MESA_RESULT in skipped-deps | failed) return 3 ;; esac
  return 0
}

# --uninstall, on an M3 Pro (or a Mac with a record): removes mesa-m3 by its exact name when the
# record says this script installed the version that is installed and no mesa-m3 was there
# before this script first ran. The owner's own (installed by them, there before, or changed
# since) stays, and so does one this script has no readable record of. This script created no
# other file for it.
m3_pro_mesa_remove() {
  local rec=$STATE/$M3_PRO_MESA_RECORD_NAME have
  is_m3_pro || [[ -f $rec ]] || return 0
  have=$(m3_pro_mesa_installed)
  [[ -n $have ]] || return 0
  if [[ -f $rec ]] && ! m3_pro_mesa_record_ok; then
    warn "keeping $M3_PRO_MESA_NAME $have: $rec is not a record this script reads (its first line is not schema=$M3_PRO_MESA_SCHEMA)"
    return 0
  fi
  if [[ $(m3_pro_mesa_record_get result) == record-error ]]; then
    say "Keeping $M3_PRO_MESA_NAME $have: the last install could not write its record (result=record-error), so this script can't tell it installed it"
    return 0
  fi
  if [[ $(m3_pro_mesa_record_get installed_by) != installer || $(m3_pro_mesa_record_get installed_version) != "$have" ]]; then
    say "Keeping $M3_PRO_MESA_NAME $have: this script did not install it"
    return 0
  fi
  local pre
  pre=$(m3_pro_mesa_record_get preexisting)
  if [[ $pre != none ]]; then
    say "Keeping $M3_PRO_MESA_NAME $have: a $M3_PRO_MESA_NAME (${pre:-of unknown version}) was installed before this script first ran"
    return 0
  fi
  if $sudo pacman -Rn --noconfirm "$M3_PRO_MESA_NAME"; then
    say "Removed the M3 Pro's Mesa ($M3_PRO_MESA_NAME $have)"
  else
    warn "could not remove $M3_PRO_MESA_NAME; remove it with: sudo pacman -R $M3_PRO_MESA_NAME"
  fi
}

this_board() {
  tr '\0' '\n' <"$DT/compatible" 2>/dev/null | sed -n '1s/^apple,//p'
}

# Before anything rebuilds boot.bin with this release's m1n1 (every Mac that
# m1n1_for_this_mac names), keep the one this boot came up on next to it on the
# EFI partition, where macOS can reach it, and say how to put it back. A later
# run of the same release keeps the first copy.
bootbin_backup() {
  local board
  m1n1_for_this_mac || return 0
  board=$(this_board)
  if [[ $M3_MODE == handoff ]] && is_m3_air; then
    keep_bootbin_on_esp "This replaces the boot loader of this M3 MacBook Air ($board)
    with m1n1-aurora and its $(m3_handoff_name)."
  elif [[ $M3_MODE == handoff ]]; then
    keep_bootbin_on_esp "This replaces the boot loader of this M3 Pro ($board) with
    m1n1-aurora and its display and GPU handoff."
  elif [[ -n $board && " $UNPROVEN_SEP_BOARDS " == *" $board "* ]]; then
    keep_bootbin_on_esp "Touch ID on this Mac model ($board) is new in $VERSION, and nobody has
    booted it on this model yet. This also replaces its boot loader with m1n1-aurora
    $(m1n1_version), the one m1n1 every Mac gets from $VERSION on."
  else
    keep_bootbin_on_esp "This replaces this Mac's boot loader with m1n1-aurora $(m1n1_version),
    the one m1n1 every Mac gets from $VERSION on."
  fi
}

# Replace a file on the EFI partition without ever leaving a partial one in
# its place: copy next to it, compare, then rename.
replace_on_esp() {
  local src=$1 dst=$2
  if ! { $sudo cp "$src" "$dst.new" && $sudo cmp -s "$src" "$dst.new" && sync &&
    $sudo mv "$dst.new" "$dst" && sync; }; then
    die "could not write $dst; the previous one is still there"
  fi
}

keep_bootbin_on_esp() {
  local why=$1 target keep uuid
  if target=$(esp_bootbin); then
    keep=$target.before-$VERSION
    # Only a complete, compared copy gets the final name, so a later run can
    # trust one it finds.
    if ! $sudo test -f "$keep"; then
      if ! { $sudo cp "$target" "$keep.new" && $sudo cmp -s "$target" "$keep.new" && sync &&
        $sudo mv "$keep.new" "$keep"; }; then
        die "could not keep a copy of $target; nothing was installed"
      fi
    fi
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
    If it boots but something is wrong, the same copy goes back from Linux with:
      sudo cp '$keep' '$target.new' && sync && sudo mv '$target.new' '$target' && sync
    After either, start Linux and run these two lines. The first keeps updates from
    rebuilding the new boot loader; the second records that its m1n1 failed on this
    Mac, so no later run of this script puts it back:
      echo M1N1_UPDATE_DISABLED=1 | sudo tee -a $UPDATE_M1N1_CONF
      echo $M1N1_BIN_SHA | sudo tee -a $STATE/m1n1-failed
    The copy is kept at $keep. Either way, please report it at
    $ISSUE_URL"
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
  local -a drop=(-e '/^# aurora-sep: build m1n1/,/^DTBS=/d' -e '/^DTBS=/d')
  [[ -f $conf && ! -f $STATE/update-m1n1.default.saved ]] &&
    $sudo cp "$conf" "$STATE/update-m1n1.default.saved"
  # Replace only the DTBS setting and keep the rest of the file: a MacBook
  # Neo's M1N1= and U_BOOT= point at its own J700 builds, and dropping them
  # would rebuild boot.bin from an m1n1 that cannot boot it. A Neo on
  # M1N1_PACKAGE (NEO_AURORA_M1N1=1) drops only its M1N1=, so update-m1n1
  # takes the packaged m1n1 with the Neo's own U-Boot. --uninstall puts the
  # saved file back.
  if is_neo && [[ $NEO_AURORA_M1N1 == 1 ]]; then
    drop+=(-e '/^[[:space:]]*\(export[[:space:]]\{1,\}\)\{0,1\}M1N1=/d')
  fi
  tmp=$(mktemp)
  if [[ -f $conf ]]; then
    $sudo sed "${drop[@]}" "$conf" >"$tmp"
  fi
  cat >>"$tmp" <<'EOF'
# aurora-sep: build m1n1's stage 2 from the device trees the installed
# linux-aurora package owns, which carry the Touch ID sensor node.
#
# Not from a module directory whose pkgbase says linux-aurora: right after an
# upgrade the running kernel's modules are restored unowned but still carry that
# pkgbase, so both kernels' DTBs would be bundled. m1n1 keeps the last matching
# DTB, and the glob sorts 11.10 before 11.9, so the stale one can win.
# (update-m1n1 runs under sh's set -e, which on Arch also applies inside
# $(...), so the pipeline must succeed even when grep matches nothing.)
DTBS=$(pacman -Qlq linux-aurora 2>/dev/null | grep '/dtbs/[^/]*\.dtb$' || true)
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

# The first install records what it found, before anything changes, so that
# --uninstall can put it back even after a run that stopped partway:
# update-m1n1's configuration (without this script's own M3 freeze from 11.36),
# or that there was none; the bring-up's freeze; and boot.bin itself.
snapshot_boot_state() {
  local tmp target
  if [[ ! -e $STATE/update-m1n1.default.saved && ! -e $STATE/update-m1n1.absent ]]; then
    if [[ -f $UPDATE_M1N1_CONF ]]; then
      tmp=$(mktemp)
      m3_unfrozen >"$tmp"
      $sudo install -m 644 "$tmp" "$STATE/update-m1n1.default.saved"
      rm -f "$tmp"
    else
      $sudo touch "$STATE/update-m1n1.absent"
    fi
  fi
  if [[ -f $UPDATE_M1N1_CONF && ! -f $STATE/update-m1n1.m3gpu.saved ]] &&
    grep -qxF "${M3GPU_FREEZE[0]}" "$UPDATE_M1N1_CONF"; then
    $sudo cp -p "$UPDATE_M1N1_CONF" "$STATE/update-m1n1.m3gpu.saved"
    if [[ -e $M3GPU_MARKER ]]; then $sudo touch "$STATE/m3gpu-marker.saved"; fi
  fi
  if [[ ! -f $STATE/boot.bin.saved ]] && target=$(esp_bootbin); then
    $sudo cp "$target" "$STATE/boot.bin.saved"
  fi
  return 0
}

# The packages this Mac gets, one "file sha256" per line: PACKAGES, and
# M1N1_PACKAGE where m1n1_for_this_mac says so. Needs m3_plan first.
packages_for_this_mac() {
  printf '%s\n' "${PACKAGES[@]}"
  if m1n1_for_this_mac; then echo "$M1N1_PACKAGE"; fi
  return 0
}

install_all() {
  local entry file sha kernel chain
  local -a entries
  release_source
  require_supported_soc
  version_notice
  sep_write_notice
  ane_dkms_notice
  kernel=$(current_kernel)
  chain=$(boot_chain)
  if [[ $chain == grub ]]; then boot_space "$kernel"; fi
  m1n1_rollback_check
  m3_plan
  m3_gpu_plan
  m3_pro_mesa_plan
  m1n1_keep_plan
  work=$(mktemp -d)
  trap 'rm -rf "${work:-}"' EXIT
  if is_neo && ! m1n1_for_this_mac; then say "Keeping this MacBook Neo's own m1n1 (m1n1-aurora has no T8140 support)"; fi
  if [[ $M3_MODE == kernel ]]; then say "Keeping this M3's own m1n1 and boot.bin"; fi
  mapfile -t entries < <(packages_for_this_mac; m3_gpu_files; m3_pro_mesa_files)
  for entry in "${entries[@]}"; do
    read -r file sha <<<"$entry"
    say "Downloading $file"
    if ! curl -fL --retry 3 --progress-bar -o "$work/$file" "$RELEASE_URL/$file"; then
      [[ $RELEASE_URL == "$PUBLIC_RELEASE_URL" ]] ||
        die "could not download $file from the staging/mirror copy that
    AURORA_RELEASE_URL names (shown at the start). That copy is missing the file
    or can't be reached: check it, or unset AURORA_RELEASE_URL to install from
    the public release. Nothing was installed."
      die "could not download $file from $TAG.
    The release is missing a file this script expects, which is a packaging
    mistake rather than anything wrong with this Mac. Nothing was installed.
    Please report it with the file name above."
    fi
    [[ $(sha256sum "$work/$file" | cut -d' ' -f1) == "$sha" ]] || die "$file does not match its published checksum"
  done
  m3_gpu_set_aside
  m3_pro_mesa_set_aside
  if m1n1_for_this_mac; then
    sha=$(m1n1_pkg_sha "$work/${M1N1_PACKAGE%% *}")
    [[ $sha == "$M1N1_BIN_SHA" ]] ||
      die "${M1N1_PACKAGE%% *} holds an m1n1.bin with sha256 ${sha:-(none)}, not the
    $M1N1_BIN_SHA this release names. Nothing was installed. Please report it at
    https://github.com/iconidentify/aurora-linux/issues"
  fi
  if [[ $M3_MODE == handoff ]]; then
    m1n1_pkg_has_handoff "$work/${M1N1_PACKAGE%% *}" ||
      die "this release's m1n1 has no $(m3_handoff_name), so it can't switch it
    on. Nothing was installed. Please report it at https://github.com/iconidentify/aurora-linux/issues"
  fi

  m3_gpu_disarm || die "air-gpu-oneshot.sh --disarm failed, so a boot may still be armed for the
    kernel this install replaces. Nothing was installed. Run: sudo air-gpu-oneshot.sh --disarm"
  snapshot "aurora-sep $VERSION"
  # Before pacman's update-m1n1 hook rebuilds boot.bin below.
  bootbin_backup
  $sudo install -d "$STATE"
  snapshot_boot_state
  if [[ ! -f $STATE/previous && $chain == grub ]]; then
    keep_grub_fallback "$kernel"
  fi
  # The kernel this Mac had before the first install; an update keeps it.
  if [[ ! -f $STATE/previous-package ]]; then
    pacman -Q "$kernel" | $sudo tee "$STATE/previous-package" >/dev/null
  fi
  if [[ $M3_MODE == handoff ]]; then
    echo "$M3_MODE $(m3_variant)" | $sudo tee "$STATE/m3-mode" >/dev/null
  elif [[ $M3_MODE != none ]]; then
    echo "$M3_MODE" | $sudo tee "$STATE/m3-mode" >/dev/null
  fi
  # Before pacman's update-m1n1 hook runs on the kernel's device trees.
  case $M3_MODE in
    kernel)
      M3_BOOTBIN_SHA=$(m3_bootbin_sha)
      m3_freeze
      ;;
    # So the hook's rebuild, where it runs, already carries the switches.
    handoff) m3_switches_write ;;
    # update-m1n1 is frozen (m1n1_keep_plan): prove boot.bin stays as it is.
    *) if ((M1N1_KEEP)); then M3_BOOTBIN_SHA=$(m3_bootbin_sha); fi ;;
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
  m3_gpu_install
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
      # grub.cfg first, so a failed check below never leaves it pointing at
      # the replaced kernel.
      if [[ $chain == grub ]]; then grub_update; fi
      m3gpu_unfreeze
      m3_unfreeze
      if update_m1n1_frozen; then
        die "update-m1n1 is still frozen in $UPDATE_M1N1_CONF, so boot.bin can't be rebuilt with
    the handoff. boot.bin was not changed. Please report it with that file."
      fi
      m1n1_update
      m3_verify_bootbin
      m1n1_check_and_record
      # The test reboot may be a hard reset (m3-serial.py reboot): get the new
      # boot.bin onto the EFI partition first.
      sync
      ;;
    *)
      if ((M1N1_KEEP)); then
        m1n1_keep_report
      else
        m1n1_update
        # Not where update-m1n1 is frozen (m1n1_update said so) or builds from
        # an m1n1 of its own, as a Neo without NEO_AURORA_M1N1 does.
        if m1n1_for_this_mac && ! update_m1n1_frozen && ! update_m1n1_own_m1n1; then
          m1n1_check_and_record
        fi
      fi
      ;;
  esac
  if [[ $chain == grub && $M3_MODE != handoff ]]; then
    grub_update
  fi

  calibration
  $sudo systemctl daemon-reload
  sep_policy
  if is_neo; then neo_radio_notice; fi
  m3_gpu_mesa_install
  m3_pro_mesa_install
  m3_pro_mesa_record
  pacman -Q linux-aurora libfprint aurora-touchid
  echo
  if [[ $M3_MODE == handoff ]] && is_m3_air && [[ $M3_AIR_DISPLAY_HANDOFF == 1 ]]; then
    say "Done. Reboot with the serial recorder running and someone watching: expect the Omarchy
    logo, the boot menu, then the same desktop on the boot framebuffer. Check the serial log
    for the T8122 handoff result and the \"PMP: T8122:\" lines.
    Native display and GPU acceleration are not enabled. Touch ID is not supported on M3 yet.
    What to send back: case D of step 11 of the test plan that the --agent-prompt command below
    prints."
  elif [[ $M3_MODE == handoff ]] && is_m3_air && [[ $M3_AIR_DRY_RUN == 1 ]]; then
    say "Done. Reboot with the serial recorder running and someone watching: expect the Omarchy
    logo, the boot menu, then the desktop on the boot framebuffer, as before. This build only
    reads and reports, so nothing on the desktop changes. Touch ID is not supported on M3 yet.
    What to send back: case D of step 11 of the test plan that the --agent-prompt command below
    prints."
  elif [[ $M3_MODE == handoff ]] && is_m3_air && [[ $M3_AIR_DCP != 1 ]]; then
    say "Done. Reboot with someone watching: expect the Omarchy logo, the boot menu, then the
    desktop on the boot framebuffer, as before. The GPU handoff is for testing: the desktop
    still renders in software. Touch ID is not supported on M3 yet. What to check and
    report: case D of step 11 of the test plan that the --agent-prompt command below prints."
  elif [[ $M3_MODE == handoff ]]; then
    say "Done. Reboot with someone watching: expect the Omarchy logo, the boot menu, then the
    desktop on the built-in display at its native resolution. Touch ID is not supported
    on M3 yet."
  elif [[ $M3_MODE == kernel ]] && ((M1N1_KEEP)); then
    say "Done. Reboot: this Mac keeps the boot loader it has now, with this release's kernel.
    Touch ID is not supported on M3 yet."
  elif [[ $M3_MODE == kernel ]]; then
    say "Done. Reboot: expect the desktop on the boot framebuffer. Touch ID is not supported
    on M3 yet. How to help bring this M3 further: step 11 of the test plan that the
    --agent-prompt command below prints."
  elif [[ -f $MODPROBE_CONF ]]; then
    say "Done, read-only. Reboot; the SEP driver will attach and report without writing."
    echo "   Enrolling a finger needs writes: delete $MODPROBE_CONF, reboot, then run aurora-touchid-setup."
  else
    say "Done. Reboot, then run:  aurora-touchid-setup"
  fi
  if ((M1N1_KEEP)) && ! is_m3; then
    echo "   This Mac keeps the boot loader it has: the m1n1 that failed on it is not put back."
  fi
  m3_gpu_notice
  m3_pro_mesa_notice
  echo "   Testing this build? The plan and reporting format:"
  echo "      curl -fsSL $PUBLIC_RELEASE_URL/install-aurora-sep.sh | bash -s -- --agent-prompt"
  if [[ $chain == grub ]]; then
    echo "   The previous kernel stays in the GRUB menu as 'Previous kernel … before aurora-sep'."
  fi
  echo "   To undo everything:    curl -fsSL $PUBLIC_RELEASE_URL/install-aurora-sep.sh | bash -s -- --uninstall"
  # The kernel install is complete either way; 3 or 4: the M3 Pro's Mesa or its record.
  m3_pro_mesa_status || exit $?
}

uninstall_all() {
  local previous=linux-asahi m3_mode=none
  require_supported_soc "Uninstalling, which rebuilds boot.bin with the stock m1n1,"
  # Before anything changes: a boot left armed for the kernel being removed would fail Limine's
  # hash check at the next boot.
  m3_gpu_disarm || die "air-gpu-oneshot.sh --disarm failed, so a boot may still be armed for the
    kernel --uninstall would remove. Nothing was uninstalled. Run: sudo air-gpu-oneshot.sh --disarm"
  if [[ -f $STATE/previous-package ]]; then read -r previous _ <"$STATE/previous-package" || true; fi
  # 11.36 rewrote this on every run, so an updated Mac may name linux-aurora
  # itself, which the repositories don't carry for these Macs.
  if [[ -z $previous || $previous == linux-aurora ]]; then previous="linux-asahi"; fi
  # 11.36 kept no record: every M3 it installed was kernel-only. Whatever the
  # record says, an M3 with m1n1-aurora installed had its boot loader replaced.
  m3_mode=$(m3_recorded_mode)
  if is_m3; then
    if pacman -Q m1n1-aurora >/dev/null 2>&1; then
      m3_mode=handoff
    elif [[ $m3_mode != handoff ]]; then
      m3_mode=kernel
    fi
  else
    m3_mode=none
  fi
  command -v aurora-touchid-setup >/dev/null && aurora-touchid-setup --remove || true
  $sudo systemctl disable apple-sep.path apple-sep.service 2>/dev/null || true
  remove_pin
  snapshot "removing aurora-sep"
  local m1n1=m1n1
  # A Neo keeps its own m1n1, unless it got m1n1-aurora (NEO_AURORA_M1N1):
  # then the stock m1n1 package goes back, and its own M1N1= with the saved
  # update-m1n1 configuration below.
  if is_neo && { [[ $NEO_AURORA_M1N1 != 1 ]] || ! pacman -Q m1n1-aurora >/dev/null 2>&1; }; then
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
  m3_gpu_remove
  m3_pro_mesa_remove
  # The m1n1 builds that failed on this Mac stay recorded, so a later install
  # never puts one of them back.
  local failed=""
  if [[ -f $STATE/m1n1-failed ]]; then failed=$(cat "$STATE/m1n1-failed"); fi
  $sudo rm -rf "$STATE"
  if [[ -n $failed ]]; then
    $sudo install -d "$STATE"
    printf '%s\n' "$failed" | $sudo tee "$STATE/m1n1-failed" >/dev/null
    say "Kept $STATE/m1n1-failed: a later install never puts back an m1n1 that failed on this Mac"
  fi
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

# The m1n1 in boot.bin by its bytes, for --m3-report: two builds can report the
# same stage 2 version.
m3_report_m1n1() {
  local target size="" installed=""
  if [[ -f $M1N1_BIN ]]; then
    size=$(stat -c %s "$M1N1_BIN")
    installed=$(sha256sum "$M1N1_BIN" | cut -d' ' -f1)
  fi
  echo "installed m1n1.bin: sha256 ${installed:--} (${size:--} bytes)"
  if [[ -n $size ]] && target=$(esp_bootbin); then
    echo "boot.bin: sha256 $($sudo sha256sum "$target" | cut -d' ' -f1)"
    echo "boot.bin's first $size bytes: sha256 $(bootbin_m1n1_sha "$target" "$size")"
  else
    echo "boot.bin: -"
  fi
  printf 'm1n1-installed: '; cat "$STATE/m1n1-installed" 2>/dev/null || echo -
}

# Printed by --agent-prompt, and pointed at from the end of a successful
# install. This is written for an agent driving the test on a real Mac: it says
# what to establish, what counts as a pass, and how to write it up.
# --m3-report: one file with what an M3 test report needs, in the current
# directory. It reads only: the boot loader's /chosen entries, a kernel log
# filtered to the M3 bring-up, USB-C and display state. Lines naming a USB
# serial number are dropped and MAC addresses are masked.
M3_REPORT_DMESG='asahi|agx|gpu|g15|dcp|dart|t8122|t6030|reserved|iommu|mailbox|pmp|simpledrm|m1n1|tipd|typec|sn201202|atc|usb|xhci|dwc3|thermal|macsmc'
m3_report() {
  local dir out board soc f
  board=$(this_board) soc=$(this_soc)
  out=$PWD/aurora-m3-report-${board:-mac}-$(date +%Y%m%d-%H%M%S).tgz
  dir=$(mktemp -d)
  {
    echo "board: ${board:-?} soc: ${soc:-?}"
    echo "kernel: $(uname -r)"
    for f in os-fw-version system-fw-version iboot2-version m1n1-stage1-version m1n1-stage2-version; do
      printf '%s: ' "$f"; { tr -d '\0' <"$DT/chosen/asahi,$f"; } 2>/dev/null || printf '-'; echo
    done
    pacman -Q linux-aurora m1n1-aurora m1n1 2>/dev/null || true
    printf 'm3-mode: '; cat "$STATE/m3-mode" 2>/dev/null || echo -
    m3_report_m1n1
    echo "m1n1.conf switches:"; grep '^chosen\.' "$M1N1_CONF" 2>/dev/null || echo -
    printf 'm1n1-oslog-overlap: '; if [[ -e $DT/$M3_OSLOG_OVERLAP ]]; then echo present; else echo absent; fi
    echo "reserved display logs:"; ls -d "$DT"/reserved-memory/dcp-oslog@* 2>/dev/null || echo -
  } >"$dir/system.txt"
  { dmesg 2>/dev/null || $sudo dmesg; } | grep -iE "$M3_REPORT_DMESG" | grep -viE 'serialnumber|serial number' |
    sed -E 's/([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}/xx:xx:xx:xx:xx:xx/g' >"$dir/dmesg-m3.txt" || true
  mkdir -p "$dir/chosen"
  for f in "$DT"/chosen/asahi,t8122-* "$DT"/chosen/asahi,t6030-* "$DT/$M3_OSLOG_OVERLAP"; do
    [[ -e $f ]] && cp -r "$f" "$dir/chosen/"
  done
  {
    echo "== lsusb -t"; lsusb -t 2>/dev/null || echo "(lsusb not installed)"
    echo "== /sys/class/typec"
    for f in /sys/class/typec/port*; do
      [[ -d $f ]] || continue
      echo "$(basename "$f"): data_role=$(cat "$f/data_role" 2>/dev/null) power_role=$(cat "$f/power_role" 2>/dev/null)"
    done
    echo "== /dev/dri"; ls -l /dev/dri 2>/dev/null || true
    echo "== drm connectors"
    for f in /sys/class/drm/card*-*/status; do [[ -e $f ]] && echo "$f: $(cat "$f")"; done
  } >"$dir/usb-display.txt" 2>&1
  tar czf "$out" -C "$dir" . && rm -rf "$dir"
  m3_oslog_overlap_check
  say "Report written to $out
    Attach it to your issue at https://github.com/iconidentify/aurora-linux/issues, together
    with the serial log if you recorded one. It has no full kernel log, no USB serial numbers
    and no MAC addresses."
}

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
  - The install replaces the Mac's boot loader (m1n1), on every Mac but an
    M3 that stays kernel-only and a MacBook Neo. Before it does, it keeps
    the old one on the EFI partition and prints the steps to put it back:
    keep them, they name this Mac's EFI partition.

IF THE MAC STOPS IN m1n1 AFTER AN INSTALL (any Mac)
  m1n1 text on screen and no boot menu, or a black screen for more than two
  minutes: the owner puts the old boot.bin back from macOS with the printed
  steps, starts Linux, and runs the two printed lines. The first keeps
  updates from rebuilding the new boot loader; the second records that its
  m1n1 failed on this Mac. Later plain runs of the one-liner then keep the
  boot loader the Mac has and install the rest, and never put that m1n1
  back. If it boots but something is wrong, the same copy goes back from
  Linux with the printed line, followed by the same two lines. Do not use
  --uninstall until the maintainer says so. Report what the screen showed
  and when, with the serial log if there is one, at
  https://github.com/iconidentify/aurora-linux/issues/6

ON AN M3 (M3, M3 Pro, M3 Max): Touch ID is not supported there yet. Do steps
0 and 1, then go to step 11, which says what should happen on your model and
how to get it added. Steps 6, 7 and 9 apply as well, and step 8 without the
fingerprint.

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
             curl -fsSLO https://raw.githubusercontent.com/iconidentify/aurora-linux/refs/tags/sep-7.1.12.aurora2-11.110-test/tools/aurora-tb/tb-pcie-report
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

11. M3 MACS (M3, M3 PRO, M3 MAX): WHAT SHOULD HAPPEN, AND HOW YOURS GETS ADDED
   Every M3 is experimental, and every M3 report moves M3 support forward.
   Touch ID is not supported on M3 yet: skip steps 2 to 5 and the fingerprint
   part of step 8, and expect
   "apple_sep: no platform profile for this SoC; the SEP stays disabled".
   First find the model and chip, then follow the matching case below:
     tr '\0' '\n' < /proc/device-tree/compatible | head -2
     cat /var/lib/aurora-sep/m3-mode     # after an install: kernel or handoff
     cat /var/lib/aurora-sep/m3-pro-mesa # M3 Pro: what happened to its Mesa
   On an M3 MacBook Air, follow case D.

   A. apple,j516s + apple,t6030 (MacBook Pro 16" M3 Pro): SUPPORTED, handoff on.
      The plain one-liner installs the kernel and replaces m1n1 with one
      that hands the built-in display and the GPU over to Linux. While it
      runs it prints:
        M3 Pro (j516s, macOS 14.8.3 stub): installing m1n1 with the display and GPU handoff
      then the macOS steps to put the old boot loader back (it is kept as
      m1n1/boot.bin.before-<version> on the EFI partition), and at the end:
        m1n1's boot.bin is this release's m1n1 with the M3 Pro handoff switches
      Every M3 Pro also gets the M3 Pro's Mesa (mesa-m3, in /opt/mesa-m3),
      installed on its own after the kernel. The summary says:
        The M3 Pro's Mesa (mesa-m3 <version>, in /opt/mesa-m3) is installed.
      If it says "The M3 Pro's Mesa was NOT installed" instead (exit status
      3), quote the warning above it: the kernel install is complete, and the
      desktop renders in software until a rerun installs it. (--no-m3-mesa
      leaves the Mesa out; only use it if the owner asks.)
      Reboot with the owner watching. Expected within about a minute: the
      Omarchy logo, the boot menu, then the desktop on the built-in display
      at 3456x2234, GPU accelerated. After login, quote
        cat "$XDG_RUNTIME_DIR/mesa-m3/session-status"
      (the Mesa's login check: decision=on, or why it stayed off). Then run
      the checks below and report.
      External displays: try each USB-C port and the HDMI port, one at a
      time and then HDMI with one USB-C display, and say which port, which
      display and which mode lit. Sleep has known limits on M3; if you try
      it with an external display attached, quote what happened.
      If it STOPS (m1n1 text and no boot menu, or a black screen for more
      than two minutes), follow "IF THE MAC STOPS IN m1n1" above. If it
      boots but the display or the GPU is WRONG, collect
        sudo journalctl -b -k | grep -iE 'dcp|asahi|t6030|m1n1'
      first, then follow the same section from Linux.

   B. any other apple,t6030 model (MacBook Pro 14" M3 Pro, J514S): NOT ON
      THE LIST YET. A REPORT FROM YOU IS HOW IT GETS ADDED.
      The plain one-liner installs the kernel only (boot.bin untouched, boot
      framebuffer) and says so:
        M3 Pro (j514s): installing the kernel only, and boot.bin stays as it is.
      It installs the M3 Pro's Mesa as in case A, and the summary says that
      without the handoff it stays off at login (software rendering).
      Run the checks below on that first; it is a useful report on its own.
      Then, to try the display and GPU handoff on this model:
        - Only with the owner's explicit agreement, with the owner at the
          Mac for the first boot, and only if the owner can start macOS on
          this Mac (hold the power button at startup). If the new boot
          loader does not start, macOS on the same Mac is the only way back.
        - The installer itself refuses unless the Mac has the macOS 14.8.3
          system-firmware stub that the Omarchy installer gives every M3.
        - Run:
            curl -fsSL https://github.com/iconidentify/aurora-linux/releases/latest/download/install-aurora-sep.sh | bash -s -- --m3-handoff
          Keep the restore steps it prints (they name this Mac's EFI
          partition). It must end with "m1n1's boot.bin is this release's
          m1n1 with the M3 Pro handoff switches"; if it stops with an error
          instead, nothing was changed: report the error.
        - Reboot, with the owner watching. Then report exactly one of:
          * WORKS: the desktop on the built-in display at native resolution.
            Give the checks below, and again after one suspend/resume. Title
            it "<board> (<model>): M3 handoff works". This report is what
            adds the model to the list in the next release; until then a
            plain re-run of the one-liner keeps the handoff on this Mac.
            A release with a newer m1n1 asks again: its plain run stops
            with "run this again with --m3-handoff" and changes nothing.
          * STOPS: m1n1 text and no boot menu, or a black screen for more than
            two minutes. Follow "IF THE MAC STOPS IN m1n1" above, and report
            what the screen showed and when.
          * WRONG: it boots, but the display or the GPU is wrong. Collect
              sudo journalctl -b -k | grep -iE 'dcp|asahi|t6030|m1n1'
            over SSH if you can, then follow the same section from Linux,
            reboot, and report.

   C. apple,t8122 other than the MacBook Air (MacBook Pro 14" M3, J504;
      iMac M3, J433/J434) or apple,t6031 / apple,t6034 (M3 Max): KERNEL
      ONLY for now.
      m1n1 has no display and GPU handoff for these Macs yet, and
      --m3-handoff refuses here. The one-liner installs the kernel only and
      leaves boot.bin as it is. Expected: the desktop on the boot framebuffer
      (one fixed resolution, software rendering). Report what works and
      what does not: Wi-Fi, Bluetooth, keyboard, trackpad, audio, USB and
      Thunderbolt, suspend/resume, battery. These reports decide which chip
      gets the handoff next.

   D. apple,j613 or apple,j615 + apple,t8122 (MacBook Air 13" or 15" M3):
      KERNEL ONLY BY DEFAULT. THE BOOT LOADER TEST IS OPT-IN.
      The plain one-liner installs the kernel only, as in case C, and says so:
        M3 MacBook Air (j613): installing the kernel only ...
      Run the checks below on that first and report it: it is the baseline
      every later Air test is compared with.
      Then, to try m1n1's display handoff with GPU diagnostics on this Air:
        - What it does: it replaces this Mac's boot loader (m1n1) with one
          that hands the built-in display over and publishes its checked
          state for Linux, and reads the Air's GPU, PMP and display clock
          details and reports them: on the serial console, and under
          /proc/device-tree/chosen in Linux. It powers the GPU only for a
          short identity read and does not start it. Native display still
          needs T8122 PMP support in the kernel, so the desktop stays on the
          boot framebuffer and renders in software, as in the baseline.
        - Only with the owner's explicit agreement, with the owner at the
          Mac for the first boot, with the Aurora maintainer told first and
          reachable while it runs, and only if the owner can start macOS on
          this Mac (hold the power button at startup). If the new boot
          loader does not start, macOS on the same Mac is the only way back.
        - Record the boot on the serial console if the owner has a second
          Apple Silicon Mac and a USB 3 USB-C cable between the two Macs'
          DFU ports: tools/aurora-m3/m3-serial.py in this repository
          ("sudo python3 m3-serial.py reboot" on the second Mac). Most of
          the display details are only printed there.
        - Run the one-liner with --m3-handoff, also on an Air that has an
          earlier test build's boot loader (a plain run there stops with
          "Run this again with --m3-handoff" and changes nothing):
            curl -fsSL https://github.com/iconidentify/aurora-linux/releases/latest/download/install-aurora-sep.sh | bash -s -- --m3-handoff
          It warns "--m3-handoff: trying m1n1's display handoff", then
          prints the restore steps, from macOS and from Linux: keep them,
          they name this Mac's EFI partition and boot.bin. Near the end it
          must print "m1n1's boot.bin is this release's m1n1 with the M3 Air
          handoff switches". An error ending "Nothing was installed" changed
          nothing: report it. For any other error, do not reboot: report it
          first.
        - Reboot: run sync on the Air, then, if the second Mac records the
          serial console, start "sudo python3 m3-serial.py reboot" there; it
          reboots the Air itself. Without one, run "sudo reboot" on the Air.
          The owner watches the screen. Expected within about a minute: the
          Omarchy logo, the boot menu, then the same desktop as before. Then
          report exactly one of:
          * WORKS: the desktop comes up as before. Run the one-liner with
            --m3-report ("bash -s -- --m3-report"): it writes one file,
            aurora-m3-report-<board>-<date>.tgz, in the current directory.
            Attach that and the serial log, with the line that shows the
            Mac's serial number removed. A handoff that went through prints
            "DCP: T8122: handoff published" on the serial console, and Linux
            keeps the display disabled with "no T8122 PMP description is
            built in"; if a handoff check refused instead, quote the refusal
            line (the boot framebuffer is then expected too). The serial log
            also has "PMP: T8122:" lines, and the report carries
            /chosen/asahi,t8122-pmp, -pmp-facts and -clock-facts. Give the
            baseline checks below again too. Title it
            "<board> (<model>): M3 Air display handoff".
          * STOPS: m1n1 text and no boot menu, or a black screen for more
            than two minutes. Follow "IF THE MAC STOPS IN m1n1" above.
            Report what the screen showed and when, with the serial log if
            there is one: it says where m1n1 stopped.
          * WRONG: it boots, but something from the baseline no longer works
            (keyboard, Wi-Fi, suspend). Collect
              sudo journalctl -b -k | grep -iE 'asahi|gpu|g15|t8122|dcp|m1n1'
            then follow the same section from Linux, reboot, and report.
        - Later plain runs keep the display handoff on this Air. To go back,
          use the printed restore lines; ask the maintainer before using
          --uninstall on an Air.
        - The GPU start experiment, only when the maintainer asks for it:
          --m3-gpu-experiment ("bash -s -- --m3-gpu-experiment", with
          --m3-handoff if the Air needs it) installs air-gpu-oneshot.sh,
          air-gpu-collect.sh and air-gpu-job.sh in /usr/local/bin and arms
          nothing. "sudo air-gpu-oneshot.sh start" (or knob=value ...) arms
          the next boot only. Limine clears the one-shot before that boot
          starts, so the boots after it, a power cycle included, should be
          the normal entry; this has been tested on an M3 Pro only. So the
          first arming on each Air is the harmless one,
            sudo air-gpu-oneshot.sh t8122_pstate_cap=1
          and in that armed boot "sudo air-gpu-oneshot.sh --status" must say
          "ubootefi.var: does not name air-gpu-oneshot" before any other
          knob is tried. If the experiment ever boots twice, pick the normal
          entry in the boot menu and report it. After the armed boot (or the
          boot after it, if it hung) run "sudo air-gpu-collect.sh" and
          attach its tgz with the last line it prints, "AIR-GPU VERDICT:
          ...". Stop and report on any crash or panic.

   Checks for every M3 (quote the output; on a kernel-only M3 the handoff
   lines are expected to be missing, so say so):
     tr -d '\0' < /proc/device-tree/chosen/asahi,m1n1-stage2-version; echo
     ls /proc/device-tree/soc/dcp@*/apple,t6030-handoff 2>&1
     for c in /sys/class/drm/card*-*; do [ -e "$c/status" ] && echo "$c $(cat "$c/status") $(head -1 "$c/modes")"; done
     sudo dmesg | grep -E 't6030-display|\[drm\] Initialized|GPU firmware|aop.*crash|apple_sep' | head -12
     nproc
     ls /proc/device-tree/soc/usb4-pcie-tunnel-0/pcie@730000000/pci@0,0/apple,tunable 2>&1
     ls /proc/device-tree/chosen/asahi,m1n1-oslog-overlap 2>&1
     ls -d /proc/device-tree/reserved-memory/dcp-oslog@* 2>&1
   With the handoff, the first of those two must say "No such file or
   directory": if the node exists, report that before anything else. The
   second lists three dcp-oslog nodes on an M3 Pro and one on an Air.
   In the report header, set **M3 path:** to kernel, handoff, or handoff
   with --m3-handoff.

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
preflight_needed() { case ${1:-} in --agent-prompt | --reset-touchid | --m3-report) return 1 ;; *) return 0 ;; esac; }

# Tests source this file for its functions only.
if [[ ${AURORA_SEP_SOURCE_ONLY:-} == 1 ]]; then return 0; fi

# --m3-handoff, --m3-gpu-experiment and --no-m3-mesa go with an install, alone, together or
# with --read-only.
args=()
for a in "$@"; do
  case $a in
    --m3-handoff) M3_TRY=1 ;;
    --m3-gpu-experiment) M3_GPU_EXPERIMENT=1 ;;
    --no-m3-mesa) M3_PRO_MESA=0 ;;
    *) args+=("$a") ;;
  esac
done
set -- "${args[@]}"
if ((M3_TRY)) && [[ -n ${1:-} && $1 != --read-only ]]; then
  die "--m3-handoff goes with an install (alone or with --read-only), not with $1"
fi
if ((M3_GPU_EXPERIMENT)) && [[ -n ${1:-} && $1 != --read-only ]]; then
  die "--m3-gpu-experiment goes with an install (alone, with --m3-handoff or with --read-only), not with $1"
fi
if ((!M3_PRO_MESA)) && [[ -n ${1:-} && $1 != --read-only ]]; then
  die "--no-m3-mesa goes with an install (alone, with another install option or with --read-only), not with $1"
fi
# One option at a time; only --reset-touchid takes arguments of its own.
if (($# > 1)) && [[ $1 != --reset-touchid ]]; then
  die "unexpected arguments after $1: ${*:2}"
fi

if preflight_needed "${1:-}"; then preflight; fi
case ${1:-} in
  "") install_all ;;
  --read-only) READ_ONLY=1; install_all ;;
  --uninstall) uninstall_all ;;
  --reset-touchid) shift; reset_touchid "$@" ;;
  --m3-report) m3_report ;;
  --agent-prompt) release_source >&2; prompt_notice; agent_prompt ;;
  *) die "unknown option $1 (--read-only, --uninstall, --reset-touchid, --agent-prompt, --m3-report, --m3-handoff, --m3-gpu-experiment or --no-m3-mesa)" ;;
esac
