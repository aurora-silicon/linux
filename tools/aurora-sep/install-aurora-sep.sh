#!/bin/bash
# Install the aurora custom/sep kernel and Touch ID on an Omarchy Mac.
#
#   curl -fsSL https://github.com/iconidentify/aurora-linux/releases/latest/download/install-aurora-sep.sh | bash
#   ... | bash -s -- --read-only      install, but never let the driver write to the enclave
#   ... | bash -s -- --uninstall      go back to the kernel this Mac had before
#   ... | bash -s -- --reset-touchid  start Touch ID over: new keybag, enrol again
#   ... | bash -s -- --m3-report      M3: write a bring-up report to attach to an issue (read-only)
#   ... | bash -s -- --m3-power-survey   M3: sample the SMC's temperature and power keys through
#                                         short CPU and backlight loads (opt-in, about 5 minutes)
#   ... | bash -s -- --m3-gpu-experiment   M3 MacBook Air only: install the GPU start
#                                         experiment's scripts with the kernel (arms nothing)
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
m3_gpu_mesa_too_old() {
  local need name min have order
  for need in $M3_GPU_MESA_NEEDS; do
    name=${need%%>=*} min=${need#*>=} order=""
    have=$(pacman -Q "$name" 2>/dev/null | cut -d' ' -f2) || have=""
    if [[ -n $have ]]; then order=$(vercmp "$have" "$min" 2>/dev/null) || order=""; fi
    # vercmp prints -1, 0 or 1.
    [[ $order =~ ^[0-9]+$ ]] || echo "$name ${have:-not installed} (needs $min or newer)"
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
  m1n1_keep_plan
  work=$(mktemp -d)
  trap 'rm -rf "${work:-}"' EXIT
  if is_neo && ! m1n1_for_this_mac; then say "Keeping this MacBook Neo's own m1n1 (m1n1-aurora has no T8140 support)"; fi
  if [[ $M3_MODE == kernel ]]; then say "Keeping this M3's own m1n1 and boot.bin"; fi
  mapfile -t entries < <(packages_for_this_mac; m3_gpu_files)
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
  echo "   Testing this build? The plan and reporting format:"
  echo "      curl -fsSL $PUBLIC_RELEASE_URL/install-aurora-sep.sh | bash -s -- --agent-prompt"
  if [[ $chain == grub ]]; then
    echo "   The previous kernel stays in the GRUB menu as 'Previous kernel … before aurora-sep'."
  fi
  echo "   To undo everything:    curl -fsSL $PUBLIC_RELEASE_URL/install-aurora-sep.sh | bash -s -- --uninstall"
  m3_next_steps
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

# ---- the M3 bring-up report (--m3-report) ------------------------------------------------------
# --m3-report: one file with what bringing up an M3 needs, in the current directory, to attach to
# an issue. It works on every M3 (t8122, t6030, t6031, t6034) and only reads: who this Mac is, the
# boot loader's /chosen entries, every device-tree node, which devices got a driver, the whole
# kernel log of this boot, the SMC's temperature and power keys, the CPUs, and the state of each
# kind of device a bring-up checks. The host name, user names, serial numbers and MAC addresses
# are masked (m3_privacy_mask), and the finished file is checked for them (m3_privacy_check):
# a file that still has any is not kept.
M3_REPORT_DMESG='asahi|agx|gpu|g15|dcp|dart|t8122|t6030|t6031|t6034|reserved|iommu|mailbox|pmp|simpledrm|m1n1|tipd|typec|sn201202|atc|usb|xhci|dwc3|thermal|macsmc'
# The M3 chips whose /chosen/asahi,<chip>-* entries (the boot loader's facts and switches) the
# report copies whole.
M3_CHOSEN_SOCS="t8122 t6030 t6031 t6034"
# Where the report and the power survey read the running system; tests point them at a fake Mac.
M3_SYSFS=/sys
M3_PROCFS=/proc
M3_DEBUGFS=/sys/kernel/debug
M3_ETC=/etc
M3_MODULES=/usr/lib/modules
M3_DEVFS=/dev
# The report's (or survey's) work directory, removed on exit.
M3_WORK=""
# The ADT reader (tools/aurora-sep/aurora-adt-extract.py), as "file sha256": a release asset that
# --m3-report downloads and checks like M3_GPU_SCRIPTS, unless the script runs from a directory
# that has its own copy (a checkout). It reads the boot loader's copy of the ADT through the
# read-only node of the phram MTD device named adt and prints an allowlist of it;
# "--check <node>" checks the node and what it is bound to, and reads nothing. Empty: this
# release has none, and the report says so.
M3_ADT_READER=""

# A file's first line, or "-" when it can't be read. No fork: the report reads a few thousand.
m3_attr() {
  local v=""
  { IFS= read -r v || [[ -n $v ]]; } 2>/dev/null <"$1" || { printf -- '-'; return 0; }
  printf '%s' "$v"
}

# A device-tree property's strings, separated by spaces, or "-".
m3_dt_words() {
  local -a w=()
  { mapfile -d '' -t w; } 2>/dev/null <"$1" || true
  if ((${#w[@]})); then printf '%s' "${w[*]}"; else printf -- '-'; fi
}

# A device-tree cell (big-endian 32 bits) as a number, or "-".
m3_dt_u32() {
  local v
  v=$(od -An -tu4 --endian=big -N4 "$1" 2>/dev/null | tr -d ' ') || v=""
  printf '%s' "${v:--}"
}

# Who this Mac is and what it runs: appended to system.txt after the lines earlier releases wrote.
m3_report_identity() {
  local f
  echo "model: $(m3_dt_words "$DT/model")"
  echo "compatible: $(m3_dt_words "$DT/compatible")"
  echo "memory: $(awk '/^MemTotal:/ { printf "%.1f GiB (MemTotal %s kB)", $2 / 1048576, $2 }' "$M3_PROCFS/meminfo" 2>/dev/null)"
  echo "uname: $(uname -srvm)"
  echo "cmdline: $(m3_attr "$M3_PROCFS/cmdline")"
  for f in "$DT"/chosen/framebuffer*; do
    [[ -d $f ]] || continue
    echo "boot framebuffer ${f##*/}: $(m3_dt_u32 "$f/width")x$(m3_dt_u32 "$f/height"), stride $(m3_dt_u32 "$f/stride"), format $(m3_dt_words "$f/format")"
  done
  echo "packages:"
  pacman -Q 2>/dev/null | grep -E '^(linux-(aurora|asahi)|m1n1|uboot-asahi|asahi-|mesa|vulkan-|libfprint|fprintd|aurora-|limine|omarchy-mac|alsa-ucm|speakersafetyd|tiny-dfr|libva|avd-fw|linux-firmware)' ||
    echo -
}

# A device-tree reg property as "address+size" pairs in hex, with the parent's cell counts.
m3_dt_reg() { # FILE ADDRESS-CELLS SIZE-CELLS
  local ac=$2 sc=$3
  [[ $ac =~ ^[0-9]+$ ]] || ac=2
  [[ $sc =~ ^[0-9]+$ ]] || sc=2
  [[ -f $1 ]] || { printf -- '-'; return 0; }
  od -An -tx4 --endian=big -v "$1" 2>/dev/null |
    awk -v ac="$ac" -v sc="$sc" '
      { for (i = 1; i <= NF; i++) w[n++] = $i }
      END {
        for (i = 0; i + ac + sc <= n; i += ac + sc) {
          a = ""; s = ""
          for (j = 0; j < ac; j++) a = a w[i + j]
          for (j = 0; j < sc; j++) s = s w[i + ac + j]
          sub(/^0+/, "", a); sub(/^0+/, "", s)
          printf "%s0x%s+0x%s", (i ? " " : ""), (a == "" ? "0" : a), (s == "" ? "0" : s)
        }
      }'
}

# The reserved-memory nodes: each one's name, compatible, label, reg and status (the addresses,
# never what is in them), and whether the boot loader's ADT and log nodes are there.
m3_report_reserved() {
  local rm=$DT/reserved-memory d ac sc label adt="" log=""
  ac=$(m3_dt_u32 "$rm/#address-cells") sc=$(m3_dt_u32 "$rm/#size-cells")
  echo "# $rm: node, compatible, label, reg (address+size), status, no-map"
  for d in "$rm"/*/; do
    d=${d%/}
    [[ -d $d ]] || continue
    label=$(m3_dt_words "$d/label")
    printf '%s\tcompatible=%s\tlabel=%s\treg=%s\tstatus=%s\tno-map=%s\n' "${d##*/}" "$(m3_dt_words "$d/compatible")" \
      "$label" "$(m3_dt_reg "$d/reg" "$ac" "$sc")" "$(m3_dt_words "$d/status")" "$([[ -e $d/no-map ]] && echo yes || echo no)"
    [[ $label == adt ]] && adt=${d##*/}
    [[ $label == m1n1_stage2.log ]] && log=${d##*/}
  done
  echo "adt node (the boot loader's copy of the ADT): ${adt:-absent}"
  echo "m1n1_stage2.log node (m1n1's log of this boot): ${log:-absent}"
}

# A device-tree property as text (strings joined by " | "), or its size when it isn't text.
m3_dt_text() {
  local v
  v=$(tr '\0' '\n' <"$1" 2>/dev/null | sed '/^$/d' | paste -sd'|' | sed 's/|/ | /g')
  if [[ -n $v ]] && ! LC_ALL=C grep -q '[^[:print:]]' <<<"$v"; then
    printf '%s' "$v"
  else
    printf '<%s bytes>' "$(stat -c %s "$1" 2>/dev/null || echo ?)"
  fi
}

# The macOS system-firmware stub's version, from the installer's stub_info.json.
m3_report_stub_version() {
  local bootbin v=""
  if bootbin=$(esp_bootbin); then
    v=$($sudo cat "${bootbin%/m1n1/boot.bin}/asahi/stub_info.json" 2>/dev/null |
      grep -o '"ProductVersion": *"[^"]*"' | head -1 | sed 's/.*"\([^"]*\)"$/\1/') || v=""
  fi
  echo "${v:--}"
}

# Which device tree this Mac boots: the running tree's model and compatible, the boot loader's
# asahi,* entries in /chosen, and the device trees in m1n1's boot.bin, each by its first board
# and chip compatible; this board's is matched by its bytes against the installed kernels'.
m3_report_boot_dt() {
  local board f target bin=$M3_WORK/boot.bin off size ver j t n=0 sha match="" dtbs
  board=$(this_board)
  echo "running device tree: model $(m3_dt_words "$DT/model"); compatible $(m3_dt_words "$DT/compatible")"
  echo "system-firmware stub (stub_info.json ProductVersion): $(m3_report_stub_version)"
  echo "== $DT/chosen asahi,* entries"
  for f in "$DT"/chosen/asahi,*; do
    [[ -e $f ]] || continue
    if [[ -d $f ]]; then echo "${f##*/}/ (a node)"; else echo "${f##*/}: $(m3_dt_text "$f")"; fi
  done
  echo "== the device trees in m1n1's boot.bin"
  if ! target=$(esp_bootbin) || ! $sudo cat "$target" >"$bin" 2>/dev/null; then
    echo "boot.bin: not found or not readable"
    return 0
  fi
  while IFS=: read -r off _; do
    # A device tree's header: the magic, its total size, ..., and version 17 at byte 20.
    size=$(od -An -tu4 --endian=big -j "$((off + 4))" -N4 "$bin" 2>/dev/null | tr -d ' ')
    ver=$(od -An -tu4 --endian=big -j "$((off + 20))" -N4 "$bin" 2>/dev/null | tr -d ' ')
    if ! [[ $size =~ ^[0-9]+$ && $ver == 17 ]] || ((size < 64 || size > 4194304)); then continue; fi
    tail -c +"$((off + 1))" "$bin" | head -c "$size" >"$M3_WORK/dtb.$n"
    j=$(LC_ALL=C grep -aoE 'apple,j[0-9a-z]+' "$M3_WORK/dtb.$n" | head -1)
    t=$(LC_ALL=C grep -aoE 'apple,t[0-9]{4}' "$M3_WORK/dtb.$n" | head -1)
    echo "dtb $n at byte $off, $size bytes: ${j:--} ${t:--}"
    if [[ -n $board && ${j#apple,} == "$board" ]]; then
      sha=$(sha256sum <"$M3_WORK/dtb.$n" | cut -d' ' -f1)
      dtbs=$(for f in "$M3_MODULES"/*/dtbs/*-"$board".dtb "$M3_MODULES"/*/dtbs/*/*-"$board".dtb; do
        [[ -f $f && $(sha256sum <"$f" | cut -d' ' -f1) == "$sha" ]] && echo "${f#"$M3_MODULES"/}"
      done | paste -sd' ')
      match+="dtb $n (sha256 $sha) is ${dtbs:-none of the device trees of the installed kernels}"$'\n'
    fi
    n=$((n + 1))
  done < <(LC_ALL=C grep -obUaP '\xd0\x0d\xfe\xed' "$bin" 2>/dev/null)
  echo "device trees in boot.bin: $n"
  if [[ -n $match ]]; then printf '%s' "$match"; else echo "no device tree in boot.bin names this board (${board:-?})"; fi
}

# The ADT reader to run. With M3_ADT_READER set, only bytes with its sha256: the copy next to
# this script when it has them, else this release's, downloaded and checked. Without it (a
# checkout before a release names one), the copy next to this script. Prints its path; or,
# returning 1, why there is none.
m3_adt_reader() {
  local self=${BASH_SOURCE[0]:-} local_copy="" file sha got
  if [[ -n $self && -f $self && -f $(dirname "$self")/aurora-adt-extract.py ]]; then
    local_copy=$(dirname "$self")/aurora-adt-extract.py
  fi
  if [[ -z $M3_ADT_READER ]]; then
    if [[ -n $local_copy ]]; then echo "$local_copy"; return 0; fi
    echo "this release ($TAG) has no ADT reader, so the ADT was not read"
    return 1
  fi
  read -r file sha <<<"$M3_ADT_READER"
  if [[ -n $local_copy && $(sha256sum "$local_copy" | cut -d' ' -f1) == "$sha" ]]; then
    echo "$local_copy"
    return 0
  fi
  if ! (release_source >/dev/null 2>&1); then
    echo "AURORA_RELEASE_URL or AURORA_RELEASES_API is not a URL this takes, so the ADT was not read"
    return 1
  fi
  if ! curl -fsSL --retry 3 -o "$M3_WORK/$file" "$RELEASE_URL/$file" 2>/dev/null; then
    echo "could not download $file from $TAG, so the ADT was not read"
    return 1
  fi
  got=$(sha256sum "$M3_WORK/$file" | cut -d' ' -f1)
  if [[ $got != "$sha" ]]; then
    echo "$file does not match its published checksum, so the ADT was not read"
    return 1
  fi
  echo "$M3_WORK/$file"
}

# The largest adt region the report reads (an ADT is well under 1 MiB).
M3_ADT_MAX_BYTES=$((16 * 1024 * 1024))
# The on-demand ADT read's state, for the traps: whether this run loaded phram, and the reader.
M3_PHRAM_LOADED=0
M3_ADT_PID=""

# Whether phram is loaded (or built in).
m3_phram_loaded() {
  [[ -d $M3_SYSFS/module/phram ]] || grep -q '^phram ' "$M3_PROCFS/modules" 2>/dev/null
}

# The MTD inventory and phram's state, as compared before and after the ADT read: one line per
# MTD device (not its read-only twin) with its number, name, type, size, erase size, device
# number and /dev/mtd/by-name link.
m3_mtd_inventory() {
  local m l by devfs
  devfs=$(readlink -f "$M3_DEVFS")
  if m3_phram_loaded; then echo "phram: loaded"; else echo "phram: not loaded"; fi
  for m in "$M3_SYSFS"/class/mtd/mtd*; do
    [[ ${m##*/} =~ ^mtd[0-9]+$ ]] || continue
    by=-
    for l in "$M3_DEVFS"/mtd/by-name/*; do
      [[ -L $l && $(readlink -f "$l") == "$devfs/${m##*/}" ]] && by=${l##*/}
    done
    printf '%s name=%s type=%s size=%s erasesize=%s dev=%s by-name=%s\n' "${m##*/}" "$(m3_attr "$m/name")" \
      "$(m3_attr "$m/type")" "$(m3_attr "$m/size")" "$(m3_attr "$m/erasesize")" "$(m3_attr "$m/dev")" "$by"
  done | sort -V
  return 0
}

# The device names in an m3_mtd_inventory, one per line, sorted.
m3_mtd_names() {
  sed -n 's/^mtd[0-9]* name=\([^ ]*\) .*/\1/p' | LC_ALL=C sort
}

# The reserved-memory node phram makes the adt MTD device from: label adt, compatible phram.
m3_adt_dt_node() {
  local d
  for d in "$DT"/reserved-memory/*/; do
    d=${d%/}
    [[ $(m3_dt_words "$d/label") == adt && " $(m3_dt_words "$d/compatible") " == *" phram "* ]] &&
      { echo "$d"; return 0; }
  done
  return 0
}

# The device-tree node an MTD device (mtdN) was made from: its own of_node, or its device's.
m3_mtd_of_node() {
  local m=$M3_SYSFS/class/mtd/$1
  if [[ -e $m/of_node ]]; then readlink -f "$m/of_node"; else readlink -f "$m/device/of_node"; fi
}

# The MTD device (mtdN) named NAME, or nothing.
m3_mtd_named() {
  local m
  for m in "$M3_SYSFS"/class/mtd/mtd*; do
    [[ ${m##*/} =~ ^mtd[0-9]+$ && $(m3_attr "$m/name") == "$1" ]] && { echo "${m##*/}"; return 0; }
  done
  return 0
}

# Waits (up to 10 s) until udev has handled every event, such as its own probe of a new MTD
# node, which holds the node open for a moment.
m3_udev_settle() {
  if command -v udevadm >/dev/null; then udevadm settle --timeout=10 2>/dev/null || true; fi
}

# Unloads phram if this run loaded it: after the reader has stopped and udev has settled, and
# once more after a short wait if phram is still in use. Never forced. Returns 1 when phram
# stays loaded; M3_PHRAM_LOADED is then 0 all the same, since the owner was told.
m3_adt_unload() {
  local try
  ((M3_PHRAM_LOADED)) || return 0
  if [[ -n $M3_ADT_PID ]]; then
    kill "$M3_ADT_PID" 2>/dev/null || true
    wait "$M3_ADT_PID" 2>/dev/null || true
    M3_ADT_PID=""
  fi
  for try in 1 2; do
    m3_udev_settle
    if $sudo modprobe -r phram 2>/dev/null; then
      M3_PHRAM_LOADED=0
      return 0
    fi
    ((try == 2)) || sleep 2
  done
  M3_PHRAM_LOADED=0
  warn "phram, which this report loaded, is still loaded (modprobe -r phram failed: in use). Unload it later with: sudo modprobe -r phram"
  return 1
}

# The report's traps: the reader stopped, phram unloaded if this run loaded it, the work
# directory removed.
m3_report_cleanup() {
  m3_adt_unload || true
  if [[ -n $M3_PARTIAL ]]; then rm -f "$M3_PARTIAL"; fi
  if [[ -n $M3_WORK ]]; then rm -rf "$M3_WORK"; fi
  M3_WORK=""
}

# The on-demand ADT read, on an M3. adt-allowlist.txt gets the reader's output, or one line
# saying why there is none; adt-check.txt records each step, and the MTD inventory and phram's
# state before and after, which must match. phram is loaded only when it isn't (an owner's
# phram is left alone, and the step stops), only when the device tree has the adt region, and
# only when no MTD device but nvram is present; it is unloaded on every path (the traps cover
# an interruption). The reader gets only the read-only node /dev/mtdNro of the device named adt,
# once its size and device-tree node match the region's and the reader's own --check agrees.
m3_report_adt() { # DIR
  local out=$1/adt-allowlist.txt log=$1/adt-check.txt reader node want reg mtd dev i rc=0 why="" before after
  local loaded new
  is_m3 || return 0
  before=$(m3_mtd_inventory)
  printf '== before\n%s\n' "$before" >"$log"
  if ! reader=$(m3_adt_reader); then
    why=$reader
  elif ! command -v python3 >/dev/null; then
    why="python3 is not installed, so the ADT was not read"
  elif m3_phram_loaded; then
    why="phram is already loaded on this Mac, not by this report, so the ADT step stopped and phram stays loaded"
  elif grep -v '^phram:' <<<"$before" | grep -vq ' name=nvram '; then
    why="an MTD device other than nvram is present, so the ADT step stopped (see adt-check.txt)"
  elif ! node=$(m3_adt_dt_node) || [[ -z $node ]]; then
    why="no adt region in the device tree: this boot's m1n1 reserved none, so the ADT was not read"
  fi
  if [[ -z $why ]]; then
    reg=$(m3_dt_reg "$node/reg" "$(m3_dt_u32 "$DT/reserved-memory/#address-cells")" "$(m3_dt_u32 "$DT/reserved-memory/#size-cells")")
    if [[ $reg =~ ^0x[0-9a-f]+\+0x([0-9a-f]{1,15})$ ]]; then
      want=$((16#${BASH_REMATCH[1]}))
      echo "region: ${node#"$DT"} reg $reg ($want bytes)" >>"$log"
      if ((want == 0 || want > M3_ADT_MAX_BYTES)); then
        why="the adt region is $want bytes, not 1 to $M3_ADT_MAX_BYTES, so the ADT was not read"
      fi
    else
      why="the adt region's reg ($reg) is not one address and size, so the ADT was not read"
    fi
  fi
  if [[ -z $why ]]; then
    say "Reading the boot loader's copy of the ADT: phram is loaded for it, and unloaded again"
    if ! $sudo modprobe phram 2>>"$log"; then
      why="no phram module on this kernel (it comes with 12.3), so the ADT was not read"
    else
      M3_PHRAM_LOADED=1
      m3_udev_settle
      loaded=$(m3_mtd_inventory)
      printf 'loaded: phram\n== with phram\n%s\n' "$loaded" >>"$log"
      # Exactly the two regions' devices are new: adt and m1n1_stage2.log.
      new=$(diff <(m3_mtd_names <<<"$before") <(m3_mtd_names <<<"$loaded") | sed -n 's/^> //p' | paste -sd' ') || true
      mtd=$(m3_mtd_named adt)
      if [[ -z $new ]]; then
        why="phram made no MTD device, so the ADT was not read"
      elif [[ $new != "adt m1n1_stage2.log" ]]; then
        why="phram made the MTD devices $new, not exactly adt and m1n1_stage2.log, so the ADT was not read"
      elif [[ -z $mtd ]]; then
        why="phram made no MTD device named adt, so the ADT was not read"
      elif [[ $(m3_attr "$M3_SYSFS/class/mtd/$mtd/size") != "$want" ||
        $(m3_mtd_of_node "$mtd") != "$(readlink -f "$node")" ]]; then
        why="the adt MTD device ($mtd, $(m3_attr "$M3_SYSFS/class/mtd/$mtd/size") bytes) does not match its reserved-memory region (${node##*/}, $want bytes), so the ADT was not read"
      else
        dev=$M3_DEVFS/${mtd}ro
        for ((i = 0; i < 10; i++)); do [[ -e $dev ]] && break; sleep 0.5; done
        if [[ ! -e $dev ]]; then
          why="no read-only device node $dev, so the ADT was not read"
        elif ! $sudo python3 "$reader" --check "$dev" >>"$log" 2>"$M3_WORK/adt.err"; then
          why="the ADT reader's check refused $dev, so the ADT was not read: $(tail -1 "$M3_WORK/adt.err" 2>/dev/null | cut -c1-200 || true)"
        else
          echo "read: $dev with ${reader##*/}" >>"$log"
          $sudo python3 "$reader" "$dev" >"$out" 2>"$M3_WORK/adt.err" &
          M3_ADT_PID=$!
          wait "$M3_ADT_PID" || rc=$?
          M3_ADT_PID=""
          if ((rc)); then
            why="the ADT reader failed (exit $rc), so its output was left out: $(tail -1 "$M3_WORK/adt.err" 2>/dev/null | cut -c1-200 || true)"
          fi
        fi
      fi
    fi
  fi
  if [[ -n $why ]]; then echo "$why" >"$out"; echo "stopped: $why" >>"$log"; else echo "read: done" >>"$log"; fi
  local unload=ok
  if ((M3_PHRAM_LOADED)); then
    if m3_adt_unload; then
      echo "unloaded: phram" >>"$log"
    else
      unload=failed
      echo "unload FAILED: phram stays loaded (in use after udev settled, twice; not forced)" >>"$log"
    fi
  fi
  after=$(m3_mtd_inventory)
  printf '== after\n%s\n' "$after" >>"$log"
  if [[ $after == "$before" ]]; then
    echo "restored: yes (the MTD inventory and phram's state match the before-state)" >>"$log"
  else
    echo "restored: NO (the MTD inventory or phram's state differs from the before-state)" >>"$log"
    # A failed unload has said so already, in one line.
    [[ $unload == failed ]] || warn "the ADT step left the MTD devices or phram not as it found them: see adt-check.txt in the report"
  fi
  return 0
}

# Every device-tree node, one per line: its path, compatible and status.
m3_report_dt_nodes() {
  local d rel
  echo "# every node of $DT: path, compatible, status (- when the node has none)"
  while IFS= read -r -d '' d; do
    rel=${d#"$DT"}
    printf '%s\tcompatible=%s\tstatus=%s\n' "/${rel#/}" "$(m3_dt_words "$d/compatible")" "$(m3_dt_words "$d/status")"
  done < <(find -H "$DT" -type d -print0 2>/dev/null | LC_ALL=C sort -z)
}

# Which devices got a driver and which didn't, the enabled SoC nodes no device was made for, the
# probes still deferred, the power domains and the loaded modules.
m3_report_drivers() {
  local dev bus drv node dt d c s tsv=$M3_WORK/devices.tsv
  dt=$(readlink -f "$DT")
  for dev in "$M3_SYSFS"/bus/*/devices/*; do
    [[ -e $dev/of_node ]] || continue
    bus=${dev#"$M3_SYSFS"/bus/}
    bus=${bus%%/*}
    drv=-
    if [[ -e $dev/driver ]]; then drv=$(basename "$(readlink -f "$dev/driver")"); fi
    node=$(readlink -f "$dev/of_node")
    node=${node#"$dt"}
    printf '%s\t%s\t%s\t/%s\n' "$bus" "${dev##*/}" "$drv" "${node#/}"
  done | LC_ALL=C sort >"$tsv"
  echo "== devices with a device-tree node and a driver: bus, device, driver, node"
  awk -F'\t' '$3 != "-"' "$tsv"
  echo "== devices with a device-tree node and no driver: bus, device, node"
  awk -F'\t' '$3 == "-" { print $1 "\t" $2 "\t" $4 }' "$tsv"
  echo "== enabled SoC nodes with a compatible and no device"
  for d in "$DT"/soc/*/; do
    d=${d%/}
    [[ -f $d/compatible ]] || continue
    s=$(m3_dt_words "$d/status")
    [[ $s == - || $s == okay || $s == ok ]] || continue
    c=/${d#"$DT"/}
    awk -F'\t' -v n="$c" '$4 == n { found = 1 } END { exit !found }' "$tsv" ||
      printf '%s\tcompatible=%s\n' "$c" "$(m3_dt_words "$d/compatible")"
  done
  echo "== deferred probes ($M3_DEBUGFS/devices_deferred)"
  $sudo cat "$M3_DEBUGFS/devices_deferred" 2>/dev/null || echo "(not readable)"
  echo "== power domains ($M3_DEBUGFS/pm_genpd/pm_genpd_summary)"
  $sudo cat "$M3_DEBUGFS/pm_genpd/pm_genpd_summary" 2>/dev/null || echo "(not readable)"
  echo "== loaded modules ($M3_PROCFS/modules)"
  cat "$M3_PROCFS/modules" 2>/dev/null || echo "(not readable)"
}

# Kernel log lines without the ones naming a serial number, and MAC addresses masked.
m3_report_scrub() {
  { LC_ALL=C grep -aviE 'serialnumber|serial number|serial-number|serial_number' || true; } |
    LC_ALL=C sed -E 's/([0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}/xx:xx:xx:xx:xx:xx/g'
}

# This boot's whole kernel log into OUT: from the journal when it has the start of the boot
# (as this user, then through sudo), else the kernel's buffer (DMESG, read already). Prints the
# source: journal or dmesg.
m3_report_klog() { # DMESG OUT
  local j=$M3_WORK/journal.raw
  journalctl -k -b 0 -o short-monotonic --no-pager >"$j" 2>/dev/null || true
  if ! LC_ALL=C grep -qaE 'Linux version|Kernel command line:' "$j" && [[ -n $sudo ]]; then
    $sudo journalctl -k -b 0 -o short-monotonic --no-pager >"$j" 2>/dev/null || true
  fi
  if LC_ALL=C grep -qaE 'Linux version|Kernel command line:' "$j"; then
    m3_report_scrub <"$j" >"$2"
    echo journal
  else
    m3_report_scrub <"$1" >"$2"
    echo dmesg
  fi
}

# The CPUs: each one's capacity, cluster and core ID, the cpufreq policies, cpuidle, cpuinfo.
m3_report_cpus() {
  local c p f
  echo "== CPUs: cpu, online, capacity, cluster, package, core, cluster CPUs, MIDR"
  for c in "$M3_SYSFS"/devices/system/cpu/cpu[0-9]*; do
    printf '%s online=%s capacity=%s cluster=%s package=%s core=%s cluster_cpus=%s midr=%s\n' "${c##*/}" \
      "$(m3_attr "$c/online")" "$(m3_attr "$c/cpu_capacity")" "$(m3_attr "$c/topology/cluster_id")" \
      "$(m3_attr "$c/topology/physical_package_id")" "$(m3_attr "$c/topology/core_id")" \
      "$(m3_attr "$c/topology/cluster_cpus_list")" "$(m3_attr "$c/regs/identification/midr_el1")"
  done | sort -V
  echo "== cpufreq policies"
  for p in "$M3_SYSFS"/devices/system/cpu/cpufreq/policy*; do
    [[ -d $p ]] || continue
    echo "${p##*/}:"
    for f in affected_cpus related_cpus scaling_driver scaling_governor cpuinfo_min_freq cpuinfo_max_freq \
      cpuinfo_transition_latency scaling_min_freq scaling_max_freq scaling_cur_freq \
      scaling_available_frequencies scaling_available_governors; do
      [[ -e $p/$f ]] && echo "  $f: $(m3_attr "$p/$f")"
    done
  done
  echo "== cpuidle: driver $(m3_attr "$M3_SYSFS/devices/system/cpu/cpuidle/current_driver"), governor $(m3_attr "$M3_SYSFS/devices/system/cpu/cpuidle/current_governor")"
  for f in "$M3_SYSFS"/devices/system/cpu/cpu0/cpuidle/state*; do
    [[ -d $f ]] && echo "cpu0 ${f##*/}: $(m3_attr "$f/name") latency $(m3_attr "$f/latency") us, residency $(m3_attr "$f/residency") us, disabled $(m3_attr "$f/disable")"
  done
  echo "== $M3_PROCFS/cpuinfo"
  cat "$M3_PROCFS/cpuinfo" 2>/dev/null || echo "(not readable)"
}

# Every readable attribute of a sysfs directory (one level), but its uevent, as "  name: value".
m3_report_attrs() {
  local f
  for f in "$1"/*; do
    [[ -f $f && ${f##*/} != uevent ]] && echo "  ${f##*/}: $(m3_attr "$f")"
  done
  return 0
}

# A sysfs device's driver, or "-".
m3_report_driver_of() {
  if [[ -e $1/driver ]]; then basename "$(readlink -f "$1/driver")"; else echo -; fi
}

# PCI and USB.
m3_report_buses() {
  echo "== lspci -nnk"
  lspci -nnk 2>&1 || echo "(lspci not installed)"
  echo "== lsusb"
  lsusb 2>&1 || echo "(lsusb not installed)"
  echo "== lsusb -t"
  lsusb -t 2>&1 || echo "(lsusb not installed)"
}

# USB-C ports, partners, cables and alternate modes (not a partner's identity).
m3_report_typec() {
  local d
  for d in "$M3_SYSFS"/class/typec/* "$M3_SYSFS"/class/typec/*/*.[0-9]*; do
    [[ -d $d ]] || continue
    echo "== ${d#"$M3_SYSFS"/class/typec/}"
    m3_report_attrs "$d"
  done
  for d in "$M3_SYSFS"/class/usb_power_delivery/*; do
    [[ -d $d ]] && echo "== usb_power_delivery ${d##*/}"
  done
  return 0
}

# DRM cards and connectors with their modes, and the backlight (never a display's EDID).
m3_report_display() {
  local c
  echo "== /dev/dri"
  ls -l "$M3_DEVFS/dri" 2>&1 || true
  for c in "$M3_SYSFS"/class/drm/card*; do
    [[ -d $c ]] || continue
    if [[ ${c##*/} == card+([0-9]) ]]; then
      echo "== ${c##*/}: driver $(m3_report_driver_of "$c/device")"
    else
      echo "== ${c##*/}: status $(m3_attr "$c/status"), enabled $(m3_attr "$c/enabled"), dpms $(m3_attr "$c/dpms")"
      [[ -r $c/modes ]] && echo "  modes: $(tr '\n' ' ' <"$c/modes" 2>/dev/null)"
    fi
  done
  for c in "$M3_SYSFS"/class/backlight/*; do
    [[ -d $c ]] || continue
    echo "== backlight ${c##*/}: driver $(m3_report_driver_of "$c/device")"
    m3_report_attrs "$c"
  done
  return 0
}

# Power supplies (without their serial numbers), thermal zones, cooling devices and hwmon.
m3_report_power() {
  local d f
  for d in "$M3_SYSFS"/class/power_supply/*; do
    [[ -d $d ]] || continue
    echo "== power_supply ${d##*/}"
    { grep -viE 'serial' "$d/uevent" 2>/dev/null || true; } | sed 's/^/  /'
  done
  for d in "$M3_SYSFS"/class/thermal/thermal_zone*; do
    [[ -d $d ]] || continue
    echo "== ${d##*/}: type $(m3_attr "$d/type"), temp $(m3_attr "$d/temp"), mode $(m3_attr "$d/mode"), policy $(m3_attr "$d/policy")"
    for f in "$d"/trip_point_*_type; do
      [[ -e $f ]] && echo "  ${f##*/}: $(m3_attr "$f") at $(m3_attr "${f%_type}_temp")"
    done
  done
  for d in "$M3_SYSFS"/class/thermal/cooling_device*; do
    [[ -d $d ]] && echo "== ${d##*/}: type $(m3_attr "$d/type"), state $(m3_attr "$d/cur_state") of $(m3_attr "$d/max_state")"
  done
  for d in "$M3_SYSFS"/class/hwmon/hwmon*; do
    [[ -d $d ]] || continue
    echo "== ${d##*/}: $(m3_attr "$d/name"), driver $(m3_report_driver_of "$d/device")"
    for f in "$d"/*_input; do
      [[ -e $f ]] || continue
      f=${f##*/}
      echo "  ${f%_input}: $(m3_attr "$d/${f%_input}_label") = $(m3_attr "$d/$f")"
    done
  done
  return 0
}

# Sound cards and input devices (a device's unique ID masked).
m3_report_sound_input() {
  echo "== $M3_PROCFS/asound/cards"
  cat "$M3_PROCFS/asound/cards" 2>/dev/null || echo "(none)"
  echo "== $M3_PROCFS/asound/pcm"
  cat "$M3_PROCFS/asound/pcm" 2>/dev/null || echo "(none)"
  echo "== $M3_PROCFS/bus/input/devices"
  sed -E 's/^(U: Uniq=).+/\1(masked)/' "$M3_PROCFS/bus/input/devices" 2>/dev/null || echo "(not readable)"
}

# Network interfaces and their drivers (never an address), Bluetooth controllers and rfkill.
m3_report_network() {
  local d
  for d in "$M3_SYSFS"/class/net/*; do
    [[ -e $d ]] || continue
    echo "${d##*/}: driver $(m3_report_driver_of "$d/device"), type $(m3_attr "$d/type"), operstate $(m3_attr "$d/operstate"), mtu $(m3_attr "$d/mtu")"
  done
  for d in "$M3_SYSFS"/class/bluetooth/*; do
    [[ -e $d ]] && echo "${d##*/}: driver $(m3_report_driver_of "$d/device")"
  done
  for d in "$M3_SYSFS"/class/rfkill/rfkill*; do
    [[ -e $d ]] && echo "${d##*/}: $(m3_attr "$d/name") $(m3_attr "$d/type") soft $(m3_attr "$d/soft") hard $(m3_attr "$d/hard")"
  done
  return 0
}

# The SMC's key list in debugfs (macsmc-hwmon), or nothing.
m3_smc_keys_file() {
  $sudo find "$M3_DEBUGFS" -maxdepth 3 -name keys -path '*smc*' 2>/dev/null | head -1 || true
}

# Why there is no SMC key list, in one line. This kernel's SMC driver makes the list on the M3s
# it knows, and t6034 is not one of them yet.
m3_smc_missing() {
  if [[ $(this_soc) == t6034 ]]; then
    echo "no SMC key list on this kernel (t6034 not yet supported)"
  else
    echo "no SMC key list on this kernel (no $M3_DEBUGFS/*smc*/keys: debugfs is not mounted, or the SMC driver did not start)"
  fi
}

# The SMC's key list: its header line and every T* (temperature, mC) and P* (power, mW) key.
m3_report_smc() {
  local kf
  kf=$(m3_smc_keys_file)
  if [[ -n $kf ]]; then
    # shellcheck disable=SC2016 # awk's fields
    $sudo awk 'NR == 1 || $2 ~ /^[TP]/' "$kf" 2>/dev/null || echo "(could not read $kf)"
  else
    m3_smc_missing
  fi
}

# The physical memory map: only the System RAM and reserved ranges. The addresses need root.
m3_report_iomem() {
  echo "# $M3_PROCFS/iomem, System RAM and reserved ranges only$([[ $EUID != 0 && -z $sudo ]] && echo " (not root: the kernel shows every address as 0)")"
  $sudo grep -E ': (System RAM|reserved)$' "$M3_PROCFS/iomem" 2>/dev/null || echo "(none, or not readable)"
}

# What the report holds, for whoever opens it.
m3_report_readme() {
  cat <<EOF
aurora-sep --m3-report ($TAG), $(date -u +%Y-%m-%dT%H:%M:%SZ)
Read-only. The host name is replaced by "host", user names by "user", serial numbers by
SERIAL and MAC addresses by xx:xx:xx:xx:xx:xx; kernel log lines naming a USB serial are left out.
  system.txt        who this Mac is, its boot loader, kernel, command line and packages
  dt-nodes.txt      every device-tree node: path, compatible, status
  drivers.txt       devices with and without a driver, deferred probes, power domains, modules
  kernel-log.txt    this boot's whole kernel log
  dmesg-m3.txt      the kernel log lines of the M3 bring-up
  smc-keys.txt      the SMC's temperature (T*, mC) and power (P*, mW) keys
  cpu.txt           CPU topology, cpufreq policies, cpuidle, cpuinfo
  buses.txt         lspci -nnk, lsusb, lsusb -t
  typec.txt         USB-C ports, partners and alternate modes
  display.txt       DRM connectors and modes, backlight
  power.txt         power supplies, thermal zones, cooling devices, hwmon
  sound-input.txt   sound cards and input devices
  network.txt       network interfaces and their drivers, Bluetooth, rfkill
  reserved-memory.txt  the reserved-memory nodes (name, compatible, label, reg, status), and
                    whether the boot loader's adt and m1n1_stage2.log nodes are there
  boot-dt.txt       which device tree this Mac boots: model, compatible, the /chosen asahi,*
                    entries, the stub's version, the device trees in boot.bin
  adt-allowlist.txt the ADT reader's allowlist of the boot loader's ADT (on an M3), or why
                    there is none
  adt-check.txt     the ADT read's steps, and the MTD devices and phram's state before and after
  interrupts.txt    $M3_PROCFS/interrupts
  iomem.txt         $M3_PROCFS/iomem, its System RAM and reserved ranges only
  usb-display.txt   USB tree, USB-C roles and DRM connector states (as in earlier reports)
  chosen/           the boot loader's /chosen entries for the M3 chips
EOF
}

m3_report() {
  local dir out board soc f s src
  board=$(this_board) soc=$(this_soc)
  [[ -w $PWD ]] || die "can't write to $PWD. Change to a directory you can write to (cd ~) and run this
    again. Nothing was written."
  out=$PWD/aurora-m3-report-${board:-mac}-$(date +%Y%m%d-%H%M%S).tgz
  M3_WORK=$(mktemp -d)
  trap 'm3_report_cleanup' EXIT
  trap 'm3_report_cleanup; warn "interrupted: nothing was written"; exit 130' INT TERM HUP
  dir=$M3_WORK/report
  mkdir "$dir"
  # What must not leave this Mac, gathered first (m3_privacy_mask, m3_privacy_check).
  m3_privacy_secrets >"$M3_WORK/secrets"
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
    ( set +e +o pipefail; m3_report_identity )
  } >"$dir/system.txt"
  { dmesg 2>/dev/null || $sudo dmesg; } >"$M3_WORK/dmesg.raw" 2>/dev/null || true
  grep -iE "$M3_REPORT_DMESG" "$M3_WORK/dmesg.raw" | grep -viE 'serialnumber|serial number' |
    sed -E 's/([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}/xx:xx:xx:xx:xx:xx/g' >"$dir/dmesg-m3.txt" || true
  src=$(m3_report_klog "$M3_WORK/dmesg.raw" "$dir/kernel-log.txt")
  mkdir -p "$dir/chosen"
  for s in $M3_CHOSEN_SOCS; do
    for f in "$DT"/chosen/asahi,"$s"-*; do
      [[ -e $f ]] && cp -r "$f" "$dir/chosen/"
    done
  done
  if [[ -e $DT/$M3_OSLOG_OVERLAP ]]; then cp -r "$DT/$M3_OSLOG_OVERLAP" "$dir/chosen/"; fi
  {
    echo "== lsusb -t"; lsusb -t 2>/dev/null || echo "(lsusb not installed)"
    echo "== /sys/class/typec"
    for f in "$M3_SYSFS"/class/typec/port*; do
      [[ -d $f ]] || continue
      echo "$(basename "$f"): data_role=$(cat "$f/data_role" 2>/dev/null) power_role=$(cat "$f/power_role" 2>/dev/null)"
    done
    echo "== /dev/dri"; ls -l "$M3_DEVFS/dri" 2>/dev/null || true
    echo "== drm connectors"
    for f in "$M3_SYSFS"/class/drm/card*-*/status; do [[ -e $f ]] && echo "$f: $(cat "$f")"; done
  } >"$dir/usb-display.txt" 2>&1
  # Each part reads on through anything it can't read; none of them writes.
  ( set +e +o pipefail; m3_report_dt_nodes ) >"$dir/dt-nodes.txt" 2>&1
  ( set +e +o pipefail; m3_report_drivers ) >"$dir/drivers.txt" 2>&1
  ( set +e +o pipefail; m3_report_smc ) >"$dir/smc-keys.txt" 2>&1
  ( set +e +o pipefail; m3_report_cpus ) >"$dir/cpu.txt" 2>&1
  ( set +e +o pipefail; m3_report_buses ) >"$dir/buses.txt" 2>&1
  ( set +e +o pipefail; m3_report_typec ) >"$dir/typec.txt" 2>&1
  ( set +e +o pipefail; m3_report_display ) >"$dir/display.txt" 2>&1
  ( set +e +o pipefail; m3_report_power ) >"$dir/power.txt" 2>&1
  ( set +e +o pipefail; m3_report_sound_input ) >"$dir/sound-input.txt" 2>&1
  ( set +e +o pipefail; m3_report_network ) >"$dir/network.txt" 2>&1
  ( set +e +o pipefail; m3_report_reserved ) >"$dir/reserved-memory.txt" 2>&1
  ( set +e +o pipefail; m3_report_boot_dt ) >"$dir/boot-dt.txt" 2>&1
  # In this shell, not a subshell: the traps must see whether it loaded phram.
  m3_report_adt "$dir"
  cat "$M3_PROCFS/interrupts" >"$dir/interrupts.txt" 2>&1 || true
  ( set +e +o pipefail; m3_report_iomem ) >"$dir/iomem.txt" 2>&1
  m3_report_readme >"$dir/README.txt"
  if [[ $src == journal ]]; then
    m3_privacy_pack "$dir" "$out" "$M3_WORK/secrets" "$dir/kernel-log.txt" || die "the report was not kept (see above). Nothing was written.
    Please tell us at https://github.com/iconidentify/aurora-linux/issues what this printed, without any file."
  else
    m3_privacy_pack "$dir" "$out" "$M3_WORK/secrets" || die "the report was not kept (see above). Nothing was written.
    Please tell us at https://github.com/iconidentify/aurora-linux/issues what this printed, without any file."
  fi
  m3_oslog_overlap_check
  [[ -n $(m3_smc_keys_file) ]] || warn "$(m3_smc_missing): the report has no SMC keys."
  say "Report written to $out
    It only read this Mac. The host name, user names, serial numbers and MAC addresses in it
    are masked, and the file was checked for them before it was kept.
    Attach it to an issue at https://github.com/iconidentify/aurora-linux/issues (drag the file
    into the comment box), together with the serial log if you recorded one."
}

# ---- privacy: what a report or survey file must not carry --------------------------------------
# This Mac's host names, as air-gpu-collect.sh finds them: this boot's, the static and transient
# ones and the FQDN, each with its short form. Generic names and names under 3 characters are
# left alone.
m3_privacy_hosts() {
  local n
  {
    uname -n 2>/dev/null
    cat "$M3_ETC/hostname" 2>/dev/null
    timeout 5 hostnamectl hostname 2>/dev/null
    timeout 5 hostnamectl --static 2>/dev/null
    timeout 5 hostname -f 2>/dev/null
  } | tr -d '[:blank:]\r' | while IFS= read -r n; do
    [[ -n $n ]] || continue
    echo "$n"
    echo "${n%%.*}"
  done | grep -E '^[A-Za-z0-9][A-Za-z0-9.-]{2,}$' |
    grep -vixE 'localhost|localhost\.localdomain|archlinux|omarchy|localdomain|host|user' || true
}

# The user names: whoever runs this, the sudo caller, and every regular account (uid 1000 to
# 59999). Root, one-character names and the mask words are left alone (a home directory is
# masked whatever its name).
m3_privacy_users() {
  {
    printf '%s\n' "${USER:-}" "${LOGNAME:-}" "${SUDO_USER:-}"
    id -un 2>/dev/null
    getent passwd 2>/dev/null | awk -F: '$3 >= 1000 && $3 < 60000 { print $1 }'
  } | grep -E '^[A-Za-z0-9._-]{2,}$' | grep -vixE 'root|nobody|host|user' || true
}

# This Mac's serial numbers, as the device tree (m1n1's root and SMBIOS entries), the firmware
# tables, USB devices, the battery and the SSD give them. Six characters or more.
m3_privacy_serials() {
  local f
  {
    while IFS= read -r -d '' f; do
      [[ ${f#"$DT"} == /aliases/* ]] && continue
      tr -d '\0' <"$f" 2>/dev/null
      echo
    done < <(find -H "$DT" -type f \( -name serial-number -o -name serial -o -name '*-serial-number' \
      -o -name '*serial_number' \) -print0 2>/dev/null)
    for f in "$M3_SYSFS"/class/dmi/id/product_serial "$M3_SYSFS"/class/dmi/id/board_serial \
      "$M3_SYSFS"/class/dmi/id/chassis_serial; do
      if [[ -e $f ]]; then $sudo cat "$f" 2>/dev/null; echo; fi
    done
    for f in "$M3_SYSFS"/bus/usb/devices/*/serial; do
      # A root hub's "serial" is its controller's name.
      [[ ${f%/serial} == */usb[0-9]* ]] || { cat "$f" 2>/dev/null; echo; }
    done
    for f in "$M3_SYSFS"/class/power_supply/*/serial_number "$M3_SYSFS"/class/nvme/*/serial; do
      [[ -e $f ]] && { cat "$f" 2>/dev/null; echo; }
    done
  } | tr -d '\r' | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' | grep -E '^[A-Za-z0-9._-]{6,}$' |
    grep -vE '^[0.]+$' || true
}

# This Mac's MAC and Bluetooth addresses as 12 lowercase hex digits: from the device tree (as
# the boot loader passes them, in both byte orders) and from every network interface.
m3_privacy_macs() {
  local f h
  {
    while IFS= read -r -d '' f; do
      [[ $(stat -c %s "$f" 2>/dev/null) == 6 ]] || continue
      h=$(od -An -tx1 -v "$f" 2>/dev/null | tr -d ' \n')
      echo "$h"
      echo "${h:10:2}${h:8:2}${h:6:2}${h:4:2}${h:2:2}${h:0:2}"
    done < <(find -H "$DT" -type f \( -name local-mac-address -o -name mac-address -o -name local-bd-address \) \
      -print0 2>/dev/null)
    for f in "$M3_SYSFS"/class/net/*/address "$M3_SYSFS"/class/bluetooth/*/address; do
      [[ -e $f ]] && tr -d ':\n' <"$f" 2>/dev/null && echo
    done
  } | tr 'A-F' 'a-f' | grep -xE '[0-9a-f]{12}' | grep -vxE '0{12}|f{12}' || true
}

# What m3_privacy_mask masks and m3_privacy_check looks for: "host NAME", "user NAME",
# "serial VALUE" and "mac HEX12" lines, longest first within each kind, each once.
m3_privacy_secrets() {
  local kind
  for kind in host user serial mac; do
    "m3_privacy_${kind}s" 2>/dev/null | awk -v k="$kind" 'NF { print length($0), k, $0 }' |
      sort -k1,1nr | awk '!seen[tolower($3)]++ { print $2, $3 }' || true
  done
}

# The sed script m3_privacy_mask runs over a text file (MODE text) or a binary one (MODE bin): a
# binary file keeps its length, with x in place of every masked byte. A name is masked where it
# stands alone (not inside a longer word); a MAC address wherever it has the shape of one.
m3_privacy_sed() { # SECRETS MODE
  local kind value re rep n=0
  while read -r kind value; do
    [[ -n $value ]] || continue
    n=$((n + 1))
    re=${value//./\\.}
    case $kind in
      host) rep=host ;;
      user) rep=user ;;
      serial) rep=SERIAL ;;
      mac) rep=xxxxxxxxxxxx ;;
      *) continue ;;
    esac
    [[ $2 == text ]] || rep=$(printf '%*s' "${#value}" '' | tr ' ' x)
    if [[ $kind == mac ]]; then
      printf ':m%d\ns/(^|[^[:xdigit:]])%s([^[:xdigit:]]|$)/\\1%s\\2/I\ntm%d\n' "$n" "$re" "$rep" "$n"
    else
      printf ':m%d\ns/(^|[^[:alnum:]])%s([^[:alnum:]]|$)/\\1%s\\2/I\ntm%d\n' "$n" "$re" "$rep" "$n"
    fi
  done <"$1"
  echo 's/([0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}/xx:xx:xx:xx:xx:xx/g'
  if [[ $2 == text ]]; then
    printf '%s\n' 's/(serial[ _-]?(number|num|no)\b[^[:alnum:]]{0,4})[[:alnum:]][[:alnum:]._-]{3,}/\1SERIAL/gI' \
      's#/home/[^/[:space:]]+/#/home/USER/#g' 's/Hostname set to <[^>]*>/Hostname set to <host>/g'
  fi
}

# Masks every file under DIR in place (see m3_privacy_sed). Each JOURNAL file is a kernel log in
# the journal's format, whose host column is masked on every line.
m3_privacy_mask() { # DIR SECRETS [JOURNAL...]
  local dir=$1 secrets=$2 f
  shift 2
  m3_privacy_sed "$secrets" text >"$secrets.text.sed" || return 1
  m3_privacy_sed "$secrets" bin >"$secrets.bin.sed" || return 1
  for f in "$@"; do
    [[ -f $f ]] || continue
    LC_ALL=C sed -i -E 's/^(\[ *[0-9.]+\]) [^ ]+ /\1 host /' -- "$f" || return 1
  done
  while IFS= read -r -d '' f; do
    if LC_ALL=C grep -qI . "$f"; then
      LC_ALL=C sed -i -E -f "$secrets.text.sed" -- "$f" || return 1
    else
      LC_ALL=C sed -i -E -f "$secrets.bin.sed" -- "$f" || return 1
    fi
  done < <(find "$dir" -type f -print0)
}

# Looks for what m3_privacy_mask masks in the finished tgz: in every file it holds, in the file
# list (only the files packed) and in the owner fields (numeric, 0). Prints one line per finding,
# naming the kind and the file, never the value. Returns 1 when anything is found.
m3_privacy_check() { # TGZ SECRETS LIST
  local tgz=$1 secrets=$2 list=$3 x kind value what f found=0
  x=$(mktemp -d)
  if ! tar -tzf "$tgz" >"$x/names" 2>/dev/null || ! tar --numeric-owner -tvzf "$tgz" >"$x/long" 2>/dev/null ||
    ! mkdir "$x/files" || ! tar -xzf "$tgz" -C "$x/files" 2>/dev/null; then
    echo "the file could not be read back"
    rm -rf "$x"
    return 1
  fi
  sed 's#^\./##' "$x/names" | grep -v '/$' | grep -vx '\.\?' | LC_ALL=C sort | cmp -s - "$list" ||
    { echo "the file list differs from what was collected"; found=1; }
  awk '$2 != "0/0" { bad = 1 } END { exit !bad }' "$x/long" && { echo "the file names an owner"; found=1; }
  for kind in host user serial mac; do
    while read -r what value; do
      [[ $what == "$kind" && -n $value ]] || continue
      if [[ $kind == mac ]]; then
        printf '(^|[^[:xdigit:]])%s([^[:xdigit:]]|$)\n' "$value"
      else
        printf '(^|[^[:alnum:]])%s([^[:alnum:]]|$)\n' "${value//./\\.}"
      fi
    done <"$secrets" >"$x/$kind.re"
  done
  while IFS= read -r -d '' f; do
    for kind in host user serial mac; do
      [[ -s $x/$kind.re ]] || continue
      if LC_ALL=C grep -qaiE -f "$x/$kind.re" "$f"; then
        case $kind in
          host) what="the host name" ;;
          user) what="a user name" ;;
          serial) what="a serial number" ;;
          mac) what="a MAC address" ;;
        esac
        echo "$what in ${f#"$x/files/"}"
        found=1
      fi
    done
    if LC_ALL=C grep -aoiE '([0-9a-f]{2}[:-]){5}[0-9a-f]{2}' "$f" | grep -q .; then
      echo "a MAC address in ${f#"$x/files/"}"
      found=1
    fi
    if LC_ALL=C grep -aoiE 'serial[ _-]?(number|num|no)\b[^[:alnum:]]{0,4}[[:alnum:]][[:alnum:]._-]{3,}' "$f" |
      grep -vq 'SERIAL$'; then
      echo "a serial number in ${f#"$x/files/"}"
      found=1
    fi
  done < <(find "$x/files" -type f -print0)
  rm -rf "$x"
  return $((found))
}

# Masks DIR's files, packs them into a tgz in the work directory and checks it
# (m3_privacy_check). Only a file that passed is copied to OUT (through OUT.partial, which the
# traps remove); one that didn't is removed, and it returns 1.
M3_PARTIAL=""
m3_privacy_pack() { # DIR OUT SECRETS [JOURNAL...]
  local dir=$1 out=$2 secrets=$3 list tgz problems
  shift 3
  list=$secrets.list tgz=$secrets.tgz
  m3_privacy_mask "$dir" "$secrets" "$@" || { warn "could not mask the collected files"; return 1; }
  (cd "$dir" && find . -type f -printf '%P\n') | LC_ALL=C sort >"$list"
  if ! tar --owner=0 --group=0 --numeric-owner -czf "$tgz" -C "$dir" .; then
    warn "could not pack the collected files"
    return 1
  fi
  if ! problems=$(m3_privacy_check "$tgz" "$secrets" "$list"); then
    rm -f "$tgz"
    warn "the privacy check found what should have been masked, so the file was removed:
    ${problems//$'\n'/$'\n'    }"
    return 1
  fi
  M3_PARTIAL=$out.partial
  if ! { cp "$tgz" "$M3_PARTIAL" && mv -f "$M3_PARTIAL" "$out"; }; then
    rm -f "$M3_PARTIAL"
    M3_PARTIAL=""
    warn "could not write $out (run this from a directory you can write to, such as your home)"
    return 1
  fi
  M3_PARTIAL=""
}

# ---- the M3 power survey (--m3-power-survey) ---------------------------------------------------
# Opt-in, on any M3: which SMC temperature (T*) and power (P*) keys follow the CPU clusters and
# the display. It samples every T* and P* key about once a second while it runs short, fixed
# loads, each after a rest: idle, all CPUs busy, P-cores only, E-cores only, and, when the Mac has
# a backlight, the backlight at maximum and at minimum. The load is plain busy loops, one pinned
# to each CPU. The traps stop them and put the backlight back on any exit, Ctrl-C included, and
# each loop ends on its own (timeout) shortly after its phase anyway. It stops early when a CPU or
# SoC die temperature key reads M3_SURVEY_LIMIT_MC or more. One masked tgz in the current
# directory. It is the M3 Air survey of issue #35 (air-smc-rails.sh), for every M3.
M3_SURVEY_PHASE_S=30   # seconds of each phase
M3_SURVEY_REST_S=10    # seconds at rest before each phase, so it starts from the floor
M3_SURVEY_WAIT_S=10    # seconds to press Ctrl-C after the plan is printed
M3_SURVEY_TICK=1       # the length of a second (tests shorten it)
M3_SURVEY_GAP=0.2      # pause between two reads of the key list; a read takes about a second
# The CPU and SoC die temperature keys: Tp* and Te*, the P- and E-cluster dies (as this kernel's
# T8140 hwmon node labels them); Tf*, the M3 family's CPU and GPU die keys (macsmc-hwmon's J516S
# list); Tg*, the GPU. A reading outside -40..150 C is not taken as a temperature, as
# macsmc-hwmon's SoC die zone does not.
M3_SURVEY_DIE_KEYS='^T[pefg]'
M3_SURVEY_LIMIT_MC=100000
M3_SURVEY_MIN_MC=-40000
M3_SURVEY_MAX_MC=150000
# The run's state, for the traps.
M3_SURVEY_PIDS=()
M3_SURVEY_P=()
M3_SURVEY_E=()
M3_SURVEY_SAMPLER=""
M3_SURVEY_BL=""
M3_SURVEY_BL_START=""
M3_SURVEY_SEEN=0
M3_SURVEY_HOT=""
M3_SURVEY_STOP=""

# The CPUs split by capacity, as the Air survey split them: those with the highest cpu_capacity
# are the P-cores (M3_SURVEY_P), the others the E-cores (M3_SURVEY_E). Offline CPUs are left out.
m3_survey_cpus() {
  local c n cap max=0
  local -a cpus=()
  M3_SURVEY_P=() M3_SURVEY_E=()
  for c in "$M3_SYSFS"/devices/system/cpu/cpu[0-9]*; do
    [[ -d $c && $(m3_attr "$c/online") != 0 ]] || continue
    cpus+=("${c##*/cpu}")
    cap=$(m3_attr "$c/cpu_capacity")
    if [[ $cap =~ ^[0-9]+$ ]] && ((cap > max)); then max=$cap; fi
  done
  while read -r n; do
    [[ -n $n ]] || continue
    cap=$(m3_attr "$M3_SYSFS/devices/system/cpu/cpu$n/cpu_capacity")
    # A CPU without a capacity counts as a P-core, as in the Air survey.
    if [[ ! $cap =~ ^[0-9]+$ ]] || ((cap == max)); then M3_SURVEY_P+=("$n"); else M3_SURVEY_E+=("$n"); fi
  done < <(printf '%s\n' ${cpus[@]+"${cpus[@]}"} | sort -n)
}

# The panel's backlight: of the backlight devices, the one with the largest range; or nothing.
m3_survey_backlight() {
  local b m best="" bm=-1
  for b in "$M3_SYSFS"/class/backlight/*; do
    m=$(m3_attr "$b/max_brightness")
    [[ $m =~ ^[0-9]+$ ]] || continue
    if ((m > bm)); then best=$b bm=$m; fi
  done
  echo "$best"
}

m3_survey_bl_set() { echo "$1" | $sudo tee "$M3_SURVEY_BL/brightness" >/dev/null; }

# One busy loop pinned to each CPU given, each ending on its own shortly after a phase.
m3_survey_load() {
  local n max
  max=$(awk -v s="$M3_SURVEY_PHASE_S" -v t="$M3_SURVEY_TICK" 'BEGIN { printf "%d", s * t + 15 }')
  for n in "$@"; do
    timeout "$max" taskset -c "$n" sh -c 'while :; do :; done' &
    M3_SURVEY_PIDS+=("$!")
  done
}

m3_survey_stop_load() {
  if ((${#M3_SURVEY_PIDS[@]})); then
    kill "${M3_SURVEY_PIDS[@]}" 2>/dev/null || true
    wait "${M3_SURVEY_PIDS[@]}" 2>/dev/null || true
  fi
  M3_SURVEY_PIDS=()
}

# The thermal zones to sample, as "path tz:<type>" lines.
m3_survey_zones() {
  local z
  for z in "$M3_SYSFS"/class/thermal/thermal_zone*; do
    [[ -r $z/temp ]] && echo "$z tz:$(m3_attr "$z/type" | tr -c 'A-Za-z0-9_.\n-' _)"
  done
  return 0
}

# The sampler, as root: while $M3_WORK/run exists it reads the SMC key list (KEYS, when there is
# one) and every thermal zone, back to back, and labels each read with the phase in
# $M3_WORK/phase. samples.txt gets "time phase key value" lines; a thermal zone's key is
# tz:<type>.
m3_survey_sampler_start() { # KEYS SECONDS
  : >"$M3_WORK/run"
  echo start >"$M3_WORK/phase"
  # shellcheck disable=SC2016 # expanded by that bash
  $sudo timeout "$2" bash -c '
    while [ -e "$2" ]; do
      ts=$(date +%s.%N)
      read -r ph <"$3" || ph=unknown
      if [ -n "$1" ]; then
        awk -v ts="$ts" -v ph="$ph" '\''$2 ~ /^[TP]/ && NF == 6 && $6 ~ /^-?[0-9]+$/ { print ts, ph, $2, $6 }'\'' "$1"
      fi
      while read -r z k; do
        read -r v <"$z/temp" 2>/dev/null && echo "$ts $ph $k $v"
      done <"$4"
      sleep "$5"
    done' _ "$1" "$M3_WORK/run" "$M3_WORK/phase" "$M3_WORK/zones" "$M3_SURVEY_GAP" >>"$M3_WORK/out/samples.txt" \
    2>>"$M3_WORK/sampler.err" &
  M3_SURVEY_SAMPLER=$!
}

m3_survey_sampler_stop() {
  [[ -n $M3_WORK ]] && rm -f "$M3_WORK/run"
  if [[ -n $M3_SURVEY_SAMPLER ]]; then
    kill "$M3_SURVEY_SAMPLER" 2>/dev/null || true
    wait "$M3_SURVEY_SAMPLER" 2>/dev/null || true
  fi
  M3_SURVEY_SAMPLER=""
}

# Everything a run started or changed, put back: on every exit, and before the tgz is made.
m3_survey_restore() {
  m3_survey_stop_load
  m3_survey_sampler_stop
  if [[ -n $M3_SURVEY_BL && -n $M3_SURVEY_BL_START ]]; then
    m3_survey_bl_set "$M3_SURVEY_BL_START" 2>/dev/null || true
    M3_SURVEY_BL_START=""
  fi
}

m3_survey_cleanup() {
  m3_survey_restore
  if [[ -n $M3_PARTIAL ]]; then rm -f "$M3_PARTIAL"; fi
  if [[ -n $M3_WORK ]]; then rm -rf "$M3_WORK"; fi
  M3_WORK=""
}

# Sets M3_SURVEY_HOT to "<key> read <C> C" when a die key or a thermal zone read
# M3_SURVEY_LIMIT_MC or more in a sample taken since the last call (complete lines only), else to
# nothing.
m3_survey_hot() {
  local raw=$M3_WORK/out/samples.txt n
  n=$(wc -l <"$raw")
  M3_SURVEY_HOT=$(awk -v from="$M3_SURVEY_SEEN" -v to="$n" -v re="$M3_SURVEY_DIE_KEYS" \
    -v lim="$M3_SURVEY_LIMIT_MC" -v hi="$M3_SURVEY_MAX_MC" '
    NR > to { exit }
    NR > from && ($3 ~ re || $3 ~ /^tz:/) && $4 ~ /^-?[0-9]+$/ && $4 + 0 >= lim && $4 + 0 <= hi {
      printf "%s read %.1f C", $3, $4 / 1000; exit
    }' "$raw")
  M3_SURVEY_SEEN=$n
}

# One stretch of sampling under a phase NAME, for SECONDS. Returns 1 when a die key read too
# hot: the load is stopped at once and M3_SURVEY_STOP says why.
m3_survey_sample() { # NAME SECONDS
  local i tmp=$M3_WORK/phase.new
  echo "$1" >"$tmp" && mv -f "$tmp" "$M3_WORK/phase"
  echo "$1 $(date +%s.%N) start" >>"$M3_WORK/out/phases.txt"
  for ((i = 0; i < $2; i++)); do
    sleep "$M3_SURVEY_TICK"
    m3_survey_hot
    if [[ -n $M3_SURVEY_HOT ]]; then
      m3_survey_stop_load
      M3_SURVEY_STOP="$M3_SURVEY_HOT during $1"
      echo "$1 $(date +%s.%N) stopped: $M3_SURVEY_STOP" >>"$M3_WORK/out/phases.txt"
      return 1
    fi
  done
  echo "$1 $(date +%s.%N) end" >>"$M3_WORK/out/phases.txt"
}

# A rest, then the phase NAME: idle, a CPU load (on CPUS), or a backlight setting.
m3_survey_phase() { # NAME [CPUS...]
  local name=$1
  shift
  m3_survey_stop_load
  m3_survey_sample "rest-before-$name" "$M3_SURVEY_REST_S" || return 1
  case $name in
    idle) ;;
    backlight-max) m3_survey_bl_set "$(m3_attr "$M3_SURVEY_BL/max_brightness")" ;;
    backlight-min) m3_survey_bl_set 1 ;;
    *) m3_survey_load "$@" ;;
  esac
  m3_survey_sample "$name" "$M3_SURVEY_PHASE_S" || return 1
  m3_survey_stop_load
}

# summary.txt: each key's mean per phase (T* in C, P* in W), the hottest die key per phase, and
# how the run ended.
m3_survey_summary() { # PHASES...
  local out=$M3_WORK/out
  printf '%-6s' "#key"
  printf ' %13s' "$@"
  printf '   (T* and tz:* in C, P* in W; mean per phase)\n'
  awk -v order="$*" '
    BEGIN { np = split(order, o, " ") }
    $2 !~ /^rest-/ { s[$3, $2] += $4; n[$3, $2]++; keys[$3] = 1 }
    END {
      for (k in keys) {
        line = sprintf("%-6s", k)
        for (i = 1; i <= np; i++) line = line sprintf(" %13.2f", n[k, o[i]] ? s[k, o[i]] / n[k, o[i]] / 1000 : 0)
        print line
      }
    }' "$out/samples.txt" | LC_ALL=C sort
  echo
  echo "hottest CPU or SoC die key (${M3_SURVEY_DIE_KEYS}) or thermal zone per phase, C:"
  awk -v re="$M3_SURVEY_DIE_KEYS" -v lo="$M3_SURVEY_MIN_MC" -v hi="$M3_SURVEY_MAX_MC" '
    ($3 ~ re || $3 ~ /^tz:/) && $4 + 0 >= lo && $4 + 0 <= hi && (!($2 in m) || $4 + 0 > m[$2]) { m[$2] = $4 + 0; k[$2] = $3 }
    END { for (p in m) printf "  %-22s %7.1f (%s)\n", p, m[p] / 1000, k[p] }' "$out/samples.txt" | LC_ALL=C sort
  echo
  if [[ -n $M3_SURVEY_STOP ]]; then
    echo "result: stopped early: $M3_SURVEY_STOP (limit $((M3_SURVEY_LIMIT_MC / 1000)) C); the later phases did not run"
  else
    echo "result: completed"
  fi
}

m3_power_survey() {
  local board soc keys out tool total n bl_name="" smc_note="" watch="" plan
  local -a phases=()
  board=$(this_board) soc=$(this_soc)
  is_m3 || die "--m3-power-survey is for an M3 Mac (M3, M3 Pro or M3 Max), and this Mac is ${board:-?} (${soc:-?}).
    Nothing was run."
  for tool in taskset timeout; do
    command -v "$tool" >/dev/null || die "--m3-power-survey needs $tool. Nothing was run."
  done
  if [[ -n $sudo ]]; then
    $sudo true || die "--m3-power-survey needs root (sudo) to read the SMC keys. Nothing was run."
  fi
  [[ -w $PWD ]] || die "can't write to $PWD. Change to a directory you can write to (cd ~) and run this
    again. Nothing was run."
  m3_survey_cpus
  ((${#M3_SURVEY_P[@]})) || die "found no CPUs in $M3_SYSFS/devices/system/cpu. Nothing was run."
  out=$PWD/aurora-m3-power-${board:-mac}-$(date +%Y%m%d-%H%M%S).tgz
  M3_WORK=$(mktemp -d)
  mkdir "$M3_WORK/out"
  trap 'm3_survey_cleanup' EXIT
  trap 'm3_survey_cleanup; warn "interrupted: the load is stopped and anything changed is put back. Nothing was written."; exit 130' INT TERM HUP
  keys=$(m3_smc_keys_file)
  if [[ -n $keys ]]; then
    # shellcheck disable=SC2016 # awk's fields
    $sudo awk 'NR == 1 || $2 ~ /^[TP]/' "$keys" >"$M3_WORK/out/smc-keys.txt" 2>/dev/null || true
  else
    smc_note=$(m3_smc_missing)
    echo "$smc_note" >"$M3_WORK/out/smc-keys.txt"
    warn "$smc_note. The survey records the CPU topology and the thermal zones only."
  fi
  m3_survey_zones >"$M3_WORK/zones"
  # A load runs only while a die temperature is watched: an SMC die key, or a thermal zone of the
  # SoC or the CPUs.
  n=$(awk -v re="$M3_SURVEY_DIE_KEYS" '$2 ~ re && NF == 6 && $6 ~ /^-?[0-9]+$/' "$M3_WORK/out/smc-keys.txt" | wc -l)
  if ((n)); then watch="$n SMC die keys (Tp*, Te*, Tf*, Tg*)"; fi
  n=$(awk '$2 ~ /^tz:.*(die|cpu|soc|hotspot)/' "$M3_WORK/zones" | wc -l)
  if ((n)); then watch+="${watch:+ and }$n SoC thermal zones"; fi
  M3_SURVEY_BL=$(m3_survey_backlight)
  if [[ -z $watch ]]; then
    phases=(idle)
    plan="idle only: there is no CPU or SoC die temperature to watch on this kernel, so the load
      and backlight phases are left out."
  else
    phases=(idle cpu-all cpu-p)
    plan="idle;
      all CPUs busy (${M3_SURVEY_P[*]} ${M3_SURVEY_E[*]});
      the P-cores only (${M3_SURVEY_P[*]});"
    if ((${#M3_SURVEY_E[@]})); then
      phases+=(cpu-e)
      plan+=$'\n'"      the E-cores only (${M3_SURVEY_E[*]});"
    else
      plan+=$'\n'"      (no E-cores found: that phase is left out);"
    fi
    if [[ -n $M3_SURVEY_BL ]]; then
      phases+=(backlight-max backlight-min) bl_name=${M3_SURVEY_BL##*/}
      plan+=$'\n'"      the backlight ($bl_name) at maximum, then at minimum, then back as it was."
    else
      plan+=$'\n'"      no backlight device, so the backlight phases are left out."
    fi
    plan+=$'\n'"    It stops early if a watched temperature ($watch, or any thermal zone)
    reads $((M3_SURVEY_LIMIT_MC / 1000)) C."
  fi
  total=$(awk -v n="${#phases[@]}" -v p="$M3_SURVEY_PHASE_S" -v r="$M3_SURVEY_REST_S" -v t="$M3_SURVEY_TICK" \
    'BEGIN { printf "%d", n * (p + r) * t }')
  say "M3 power survey on this ${board:-Mac} (${soc:-?}): about $(((total + 59) / 60)) minutes.
    It samples the SMC's temperature (T*) and power (P*) keys and the thermal zones about once a
    second through short, fixed phases, each $M3_SURVEY_PHASE_S s after $M3_SURVEY_REST_S s at rest:
      $plan
    The load stops and the backlight goes back at the end, on an error and on Ctrl-C.
    Nothing else changes. It writes one file in this directory: $out
    Close other programs, keep the display on and the Mac on power."
  say "Starting in $M3_SURVEY_WAIT_S seconds. Press Ctrl-C now to cancel."
  sleep "$M3_SURVEY_WAIT_S"
  m3_privacy_secrets >"$M3_WORK/secrets"
  {
    echo "board: ${board:-?} soc: ${soc:-?}"
    echo "model: $(m3_dt_words "$DT/model")"
    echo "uname: $(uname -srvm)"
    echo "installer: $TAG"
    echo "phases: ${phases[*]}; $M3_SURVEY_PHASE_S s each after $M3_SURVEY_REST_S s at rest (one second = $M3_SURVEY_TICK s)"
    echo "watched: ${watch:-nothing (no load phases)}; stop at $M3_SURVEY_LIMIT_MC mC on a die key (${M3_SURVEY_DIE_KEYS}) or any thermal zone"
    echo "smc: ${smc_note:-key list $keys}"
  } >"$M3_WORK/out/system.txt"
  {
    echo "P-cores: ${M3_SURVEY_P[*]}"
    echo "E-cores: ${M3_SURVEY_E[*]:-none}"
    for n in "${M3_SURVEY_P[@]}" ${M3_SURVEY_E[@]+"${M3_SURVEY_E[@]}"}; do
      echo "cpu$n capacity $(m3_attr "$M3_SYSFS/devices/system/cpu/cpu$n/cpu_capacity") cluster $(m3_attr "$M3_SYSFS/devices/system/cpu/cpu$n/topology/cluster_id")"
    done
    for n in "$M3_SYSFS"/devices/system/cpu/cpufreq/policy*; do
      [[ -d $n ]] && echo "${n##*/}: cpus $(m3_attr "$n/related_cpus"), $(m3_attr "$n/cpuinfo_min_freq")-$(m3_attr "$n/cpuinfo_max_freq") kHz, $(m3_attr "$n/scaling_driver")"
    done
    echo "thermal zones: $(awk '{ print $2 }' "$M3_WORK/zones" | paste -sd' ')"
    if [[ -n $bl_name ]]; then
      echo "backlight: $bl_name, max_brightness $(m3_attr "$M3_SURVEY_BL/max_brightness")"
    elif [[ -n $watch ]]; then
      echo "no backlight device: backlight phases skipped"
    fi
  } >"$M3_WORK/out/cpus.txt"
  : >"$M3_WORK/out/samples.txt"
  : >"$M3_WORK/out/phases.txt"
  if [[ -n $bl_name ]]; then M3_SURVEY_BL_START=$(m3_attr "$M3_SURVEY_BL/brightness"); fi
  [[ $M3_SURVEY_BL_START != - ]] || M3_SURVEY_BL_START=""
  m3_survey_sampler_start "$keys" "$((total + 60))"
  for n in "${phases[@]}"; do
    case $n in
      cpu-all) m3_survey_phase "$n" "${M3_SURVEY_P[@]}" ${M3_SURVEY_E[@]+"${M3_SURVEY_E[@]}"} ;;
      cpu-p) m3_survey_phase "$n" "${M3_SURVEY_P[@]}" ;;
      cpu-e) m3_survey_phase "$n" "${M3_SURVEY_E[@]}" ;;
      *) m3_survey_phase "$n" ;;
    esac || break
  done
  m3_survey_restore
  {
    m3_survey_summary "${phases[@]}"
    if [[ -n $smc_note ]]; then echo "smc: $smc_note"; fi
    if [[ -z $watch ]]; then echo "loads: none (no CPU or SoC die temperature to watch on this kernel)"; fi
  } >"$M3_WORK/out/summary.txt"
  cp "$M3_WORK/sampler.err" "$M3_WORK/out/sampler-errors.txt" 2>/dev/null || true
  m3_privacy_pack "$M3_WORK/out" "$out" "$M3_WORK/secrets" ||
    die "the survey's file was not kept (see above). Please tell us at
    https://github.com/iconidentify/aurora-linux/issues what this printed, without any file."
  m3_survey_cleanup
  trap - EXIT INT TERM HUP
  if [[ -n $M3_SURVEY_STOP ]]; then
    warn "the survey stopped early: $M3_SURVEY_STOP, at or over the $((M3_SURVEY_LIMIT_MC / 1000)) C limit.
    The load stopped at once; the phases after it did not run."
  fi
  if [[ -n $smc_note ]]; then warn "$smc_note: the file has the CPU topology and the thermal zones only."; fi
  say "Power survey written to $out
    Everything it changed is back as it was. The host name, user names, serial numbers and MAC
    addresses in it are masked, and the file was checked for them before it was kept.
    Attach it to your issue at https://github.com/iconidentify/aurora-linux/issues (drag the file
    into the comment box)."
}

# ---- the end of an install on an M3 with no handoff path yet -----------------------------------
# The M3 Max (t6031, t6034) and the T8122 Macs that are not an Air (J504, J433, J434): case C of
# the test plan. m3_plan always keeps them kernel-only.
is_m3_kernel_only_chip() {
  tr '\0' '\n' <"$DT/compatible" 2>/dev/null | grep -Eqx 'apple,(t6031|t6034)' && return 0
  tr '\0' '\n' <"$DT/compatible" 2>/dev/null | grep -qx 'apple,t8122' && ! is_m3_air
}

# The release of this script's kernel as uname -r prints it (VERSION 7.1.12.aurora2-12.2 is
# 7.1.12-2-12.2-sep-ARCH), or nothing when VERSION has another form.
m3_kernel_release() {
  [[ $VERSION =~ ^([0-9]+\.[0-9]+\.[0-9]+)\.aurora([0-9]+)-(.+)$ ]] || return 0
  echo "${BASH_REMATCH[1]}-${BASH_REMATCH[2]}-${BASH_REMATCH[3]}-sep-ARCH"
}

# The last block of an install's summary on those Macs: what to do next, in order.
m3_next_steps() {
  local rel
  [[ $M3_MODE == kernel ]] && is_m3_kernel_only_chip || return 0
  rel=$(m3_kernel_release)
  echo
  echo "======================== NEXT STEPS: M3 BRING-UP ($(this_board)) ========================"
  echo "  1. Reboot."
  echo "  2. Check that the Mac runs the new kernel:"
  echo "       uname -r"
  echo "     It should print ${rel:-a version that ends in -sep-ARCH}."
  echo "  3. Write the bring-up report. It only reads, and writes one file in the current"
  echo "     directory, aurora-m3-report-$(this_board)-<date>.tgz, with the host name, user names,"
  echo "     serial numbers and MAC addresses masked:"
  echo "       curl -fsSL $LATEST_URL | bash -s -- --m3-report"
  echo "  4. Attach that file to an issue at https://github.com/iconidentify/aurora-linux/issues"
  echo "=========================================================================================="
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
   On an M3 MacBook Air, follow case D.

   A. apple,j516s + apple,t6030 (MacBook Pro 16" M3 Pro): SUPPORTED, handoff on.
      The plain one-liner installs the kernel and replaces m1n1 with one
      that hands the built-in display and the GPU over to Linux. While it
      runs it prints:
        M3 Pro (j516s, macOS 14.8.3 stub): installing m1n1 with the display and GPU handoff
      then the macOS steps to put the old boot loader back (it is kept as
      m1n1/boot.bin.before-<version> on the EFI partition), and at the end:
        m1n1's boot.bin is this release's m1n1 with the M3 Pro handoff switches
      Reboot with the owner watching. Expected within about a minute: the
      Omarchy logo, the boot menu, then the desktop on the built-in display
      at 3456x2234, GPU accelerated. Then run the checks below and report.
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
      The install ends with a NEXT STEPS block. After the reboot, check that
      "uname -r" names this release's kernel, then run the one-liner with
      --m3-report ("bash -s -- --m3-report"). It only reads, and writes one
      file in the current directory, aurora-m3-report-<board>-<date>.tgz,
      with the host name, user names, serial numbers and MAC addresses
      masked; a file that still has any is not kept. On an M3 it also
      loads the phram module for a moment, to read the boot loader's copy
      of the ADT through a read-only node, and unloads it again (one it
      did not load stays as it is). Attach the file to the issue.
      Only when the maintainer asks: --m3-power-survey (about five minutes,
      needs sudo) runs short CPU and backlight loads while it samples the
      SMC's temperature and power keys, stops early if a CPU or SoC die
      key reads 100 C, puts everything back, and writes
      aurora-m3-power-<board>-<date>.tgz.

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
preflight_needed() { case ${1:-} in --agent-prompt | --reset-touchid | --m3-report | --m3-power-survey) return 1 ;; *) return 0 ;; esac; }

# Tests source this file for its functions only.
if [[ ${AURORA_SEP_SOURCE_ONLY:-} == 1 ]]; then return 0; fi

# --m3-handoff and --m3-gpu-experiment go with an install, alone, together or with --read-only.
args=()
for a in "$@"; do
  case $a in
    --m3-handoff) M3_TRY=1 ;;
    --m3-gpu-experiment) M3_GPU_EXPERIMENT=1 ;;
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
  --m3-power-survey) m3_power_survey ;;
  --agent-prompt) release_source >&2; prompt_notice; agent_prompt ;;
  *) die "unknown option $1 (--read-only, --uninstall, --reset-touchid, --agent-prompt, --m3-report, --m3-power-survey, --m3-handoff or --m3-gpu-experiment)" ;;
esac
