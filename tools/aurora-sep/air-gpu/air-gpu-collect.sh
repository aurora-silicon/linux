#!/bin/bash
# air-gpu-collect.sh: after a boot, collect what the M3 Air GPU experiment needs into one tgz in
# your home directory, and print one verdict line.
#
#   sudo air-gpu-collect.sh             the armed boot: this one, or the one before it when this
#                                       boot is a normal one (the armed boot hung, say), read back
#                                       from the journal
#   sudo air-gpu-collect.sh --boot 0    this boot
#   sudo air-gpu-collect.sh --boot -1   the boot before this one
#
# Read-only: apart from the tgz (and a temporary directory) it writes nothing. The kernel log in
# the tgz has lines naming a serial number dropped and MAC addresses masked.
#
# The last line printed is the verdict, one of:
#   NOT-ARMED            the boot did not have asahi.t8122_start=1 (no one-shot boot)
#   ARMED-NOT-STARTED    armed, but the driver stopped before it started the GPU firmware
#   FW-BOOT-FAILED       the firmware was started but did not come up (or crashed, with no job)
#   INITDATA-REJECTED    the firmware came up but did not accept the InitData
#   FW-RUNNING           the firmware accepted the InitData and runs; no GPU job ran
#   JOB-NOT-DISPATCHED   a GPU job was submitted and did not complete correctly
#   JOB-COMPLETED        air-gpu-job.sh ran, and every submission completed with correct results
set -euo pipefail

# ---- the verdict table: the kernel log lines each step is judged by --------------------------
# Keep this in step with the kernel branch (air/t8122-gpu-start: t8122_start.rs).
#
# 1. The kernel's own verdict lines, on an armed T8122 ("M3 G15G verdict: <outcome>"), and the
#    verdict each outcome gives. A job outcome outranks a firmware one; among job outcomes a
#    failure outranks job-completed.
K1_VERDICTS='
firmware-boot-failed            FW-BOOT-FAILED
driver-refused                  ARMED-NOT-STARTED
initdata-rejected               INITDATA-REJECTED
firmware-running-check-failed   FW-BOOT-FAILED
firmware-running                FW-RUNNING
job-accepted-never-dispatched   JOB-NOT-DISPATCHED
job-timed-out-powered           JOB-NOT-DISPATCHED
job-timed-out                   JOB-NOT-DISPATCHED
job-faulted                     JOB-NOT-DISPATCHED
job-retired-without-timestamps  JOB-NOT-DISPATCHED
job-completed                   JOB-COMPLETED
'
RE_K1_VERDICT='M3 G15G verdict: [a-z-]+'
# The start experiment's own lines: the values it armed with, and its refusals.
RE_K1_ARMED='M3 G15G start: armed'
#
# 2. Without those lines (a kernel before them, or the M3 Pro rehearsal), these extended regexes
#    decide, in this order: armed? -> firmware started? -> InitData sent? -> accepted? -> job?
# The boot was armed: on its command line.
RE_ARMED='(^| )asahi\.t8122_start=1( |$)'
# The driver refused or stopped before it started the firmware (the last such line is the reason).
RE_REFUSED='M3 G15G start: (refused|not armed)|GPU startup disabled|identity or ABI rejected|startup refused|no GPU backend|invalid for parameter .asahi\.|InitData self-check failed'
# The probe ended in an error (the reason when nothing more specific was logged).
RE_PROBE_FAILED='probe with driver asahi failed'
# The driver probed the GPU at all (without a boot-loader handoff the GPU node is disabled).
RE_PROBED='asahi [0-9a-f]+\.gpu: Probing'
# The GPU coprocessor's firmware was started.
RE_FW_STARTED='asahi [0-9a-f]+\.gpu: RTKit: Initializing|M3: publishing owned initdata'
# The firmware crashed.
RE_FW_CRASH='\.gpu: RTKit: co-processor has crashed|M3 G15[A-Z]*: firmware crashed|firmware crashed while waiting for init|GPU firmware crashed'
# The InitData was handed to the firmware, and the firmware accepted it.
RE_INITDATA_SENT='M3: publishing owned initdata'
RE_INITDATA_OK='M3: firmware accepted owned initdata'
# The firmware's readiness report (the reason when the InitData was not accepted).
RE_READINESS='M3 firmware readiness:|M3 firmware error event'
# The kernel saw a GPU job fail or never run.
RE_JOB_FAILED='M3 scheduler: execution failed|M3 completion events=|GPU is powered down|M3 fault render|M3 bank [0-9]+ fault=0x[0-9a-f]*[1-9a-f]|M3 fault mapping'
#
# The GPU lines kept in gpu-log.txt, and the firmware's own log lines in firmware-log.txt.
RE_GPU_LINES='asahi|agx|G15|M3[ _:]|t8122|gpu|RTKit|devcoredump|macsmc'
RE_FW_LOG='\.gpu: RTKit: (syslog|crashlog|co-processor|Unknown)|firmware crashed|crashlog|M3 firmware|M3 G15G (verdict|start)'
# SMC power keys that read zero at idle on Scott's J613 (2026-10-06): the GPU rail candidates.
GPU_RAIL_CANDIDATES="PP1b PP2l PP3b PP9l PPdb PR6b PRab PRcb PReb PZC0 PZC1 PZCB PZCU PZD1"
# ----------------------------------------------------------------------------------------------

PROG=air-gpu-collect
TAG=air_gpu.oneshot
DT=/proc/device-tree
EFIVARS=/sys/firmware/efi/efivars
BLI_GUID=4a67b082-0a4c-41cf-b6c7-440b29bb8c4f
DEBUGFS=/sys/kernel/debug
SYS=/sys
CMDLINE=/proc/cmdline
BOOT_ID=/proc/sys/kernel/random/boot_id
STATE_DIR=/var/lib/air-gpu

is_root() { ((EUID == 0)); }
# The tgz goes to this user's home (the invoking user's under sudo).
user_home() {
  local h
  h=$(getent passwd "$1" | cut -d: -f6)
  echo "${h:-/root}"
}
say() { printf '%s: %s\n' "$PROG" "$*"; }

# Kernel log text without serial numbers and MAC addresses.
scrub() {
  { grep -aviE 'serialnumber|serial number|serial-number' || true; } |
    sed -E 's/([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}/xx:xx:xx:xx:xx:xx/g'
}

cmdline_of() { # boot: 0 or -1
  if [[ $1 == 0 ]]; then cat "$CMDLINE"; return; fi
  { journalctl -k -b "$1" -o cat --no-pager 2>/dev/null || true; } | sed -n 's/^.*Kernel command line: //p' | head -1
}
boot_id_of() {
  if [[ $1 == 0 ]]; then tr -d '-' <"$BOOT_ID"; return; fi
  { journalctl --list-boots --no-pager 2>/dev/null || true; } | awk -v i="$1" '$1 == i { print $2; exit }'
}
oneshot_of() { tr ' ' '\n' <<<"$1" | sed -n "s/^${TAG//./\\.}=//p" | head -1; }
knobs_of() { tr ' ' '\n' <<<"$1" | grep '^asahi\.' | tr '\n' ' ' | sed 's/ $//'; }

# The kernel log of a boot, oldest first: this boot's from the journal when the journal has its
# start, otherwise dmesg; an earlier boot's from the journal.
kernel_log() {
  local j
  if [[ $1 == 0 ]]; then
    j=$(journalctl -k -b 0 -o short-monotonic --no-pager 2>/dev/null || true)
    if grep -qaE 'Kernel command line:|Linux version' <<<"$j"; then printf '%s\n' "$j"; return; fi
    dmesg 2>/dev/null || true
    return 0
  fi
  journalctl -k -b "$1" -o short-monotonic --no-pager 2>/dev/null || true
}

# The analysed boot's job records (air-gpu-job.sh), oldest first.
job_records() { # boot_id home
  local f
  [[ -n $1 && -d $2/air-gpu-runs ]] || return 0
  for f in "$2"/air-gpu-runs/job-*.txt; do
    [[ -f $f ]] && grep -qx "boot_id=$1" "$f" && echo "$f"
  done
  return 0
}
rec_key() { sed -n "s/^$2=//p" "$1" | head -1; }

# ---- the verdict -----------------------------------------------------------------------------

VERDICT="" REASON=""
# classify LOG CMDLINE LAST_JOB_RECORD (the record may be empty)
classify() {
  local log=$1 cmd=$2 rec=${3:-} accept_ln crash_ln line job_seen result reached
  # Line number of the first match, and the text of the last one (both empty when none).
  first_ln() { { grep -naE "$1" "$log" || true; } | head -1 | cut -d: -f1; }
  last_line() { { grep -aE "$1" "$log" || true; } | tail -1 | sed -E 's/^\[[^]]*\] *//; s/^[^ ]+ kernel: //'; }
  if ! [[ " $cmd " =~ $RE_ARMED ]]; then
    VERDICT=NOT-ARMED REASON="asahi.t8122_start=1 is not on the analysed boot's command line"
    return 0
  fi
  if grep -qaE "$RE_K1_VERDICT" "$log"; then
    classify_k1 "$log" "$rec"
    return 0
  fi
  if ! grep -qaE "$RE_FW_STARTED" "$log"; then
    VERDICT=ARMED-NOT-STARTED
    if line=$(last_line "$RE_REFUSED") && [[ -n $line ]]; then REASON=$line
    elif line=$(last_line "$RE_PROBE_FAILED") && [[ -n $line ]]; then REASON=$line
    elif ! grep -qaE "$RE_PROBED" "$log"; then
      REASON="the GPU driver never probed: the GPU node was disabled (the boot loader did not hand the GPU over)"
    else REASON="the driver stopped before it started the GPU firmware"; fi
    return 0
  fi
  crash_ln=$(first_ln "$RE_FW_CRASH")
  if ! grep -qaE "$RE_INITDATA_SENT" "$log"; then
    VERDICT=FW-BOOT-FAILED
    if [[ -n $crash_ln ]]; then REASON=$(last_line "$RE_FW_CRASH")
    elif line=$(last_line "$RE_PROBE_FAILED") && [[ -n $line ]]; then REASON="the firmware did not come up: $line"
    else REASON="the firmware was started but never took the InitData"; fi
    return 0
  fi
  accept_ln=$(first_ln "$RE_INITDATA_OK")
  if [[ -z $accept_ln ]]; then
    VERDICT=INITDATA-REJECTED
    if [[ -n $crash_ln ]]; then REASON="the firmware crashed after taking the InitData: $(last_line "$RE_FW_CRASH")"
    elif line=$(last_line "$RE_READINESS") && [[ -n $line ]]; then REASON=$line
    else REASON="the InitData was sent but the firmware never accepted it"; fi
    return 0
  fi
  # The firmware accepted the InitData. Did it crash afterwards, and did a job run?
  crash_ln=$({ grep -naE "$RE_FW_CRASH" "$log" || true; } | awk -F: -v a="$accept_ln" '$1 > a { print $1; exit }')
  result="" reached=""
  if [[ -n $rec ]]; then result=$(rec_key "$rec" result) reached=$(rec_key "$rec" reached_gpu); fi
  job_seen=0
  if grep -qaE "$RE_JOB_FAILED" "$log" || [[ $reached == yes || $result == running ]]; then job_seen=1; fi
  if [[ -n $crash_ln ]]; then
    if ((job_seen)); then
      VERDICT=JOB-NOT-DISPATCHED REASON="the firmware crashed during the job: $(last_line "$RE_FW_CRASH")"
    else
      VERDICT=FW-BOOT-FAILED REASON="the firmware accepted the InitData, then crashed: $(last_line "$RE_FW_CRASH")"
    fi
    return 0
  fi
  if ((!job_seen)); then
    VERDICT=FW-RUNNING
    if [[ -n $result ]]; then REASON="the firmware accepted the InitData and runs; the job did not reach the GPU ($result)"
    else REASON="the firmware accepted the InitData and runs; no GPU job ran in this boot"; fi
    return 0
  fi
  if [[ $result == pass ]] && ! grep -qaE "$RE_JOB_FAILED" "$log"; then
    VERDICT=JOB-COMPLETED
    REASON="air-gpu-job: $(rec_key "$rec" submits) submissions completed with correct results (first in $(rec_key "$rec" first_submit_ms) ms) on $(rec_key "$rec" device)"
    return 0
  fi
  VERDICT=JOB-NOT-DISPATCHED
  if line=$(last_line "$RE_JOB_FAILED") && [[ -n $line ]]; then REASON=$line
  elif [[ $result == running ]]; then REASON="the boot ended while air-gpu-job was running (a hang or a power cycle)"
  else REASON="air-gpu-job: $result: $(rec_key "$rec" error)"; fi
}

# The kernel's verdict lines decide, with the job record for a job that did not reach the kernel.
classify_k1() { # LOG RECORD
  local log=$1 rec=${2:-} outcomes o v line fail="" done_line="" fw="" result=""
  k1_line() { { grep -aE "M3 G15G verdict: $1" "$log" || true; } | tail -1 | sed -E 's/^.*(M3 G15G verdict: )/\1/'; }
  outcomes=$({ grep -aoE "$RE_K1_VERDICT" "$log" || true; } | sed 's/^M3 G15G verdict: //')
  for o in $outcomes; do
    v=$(printf '%s\n' "$K1_VERDICTS" | awk -v o="$o" '$1 == o { print $2 }')
    case $v in
      JOB-NOT-DISPATCHED) fail=$o ;;
      JOB-COMPLETED) done_line=$o ;;
      "") ;;
      *) fw=$o ;;
    esac
  done
  if [[ -n $rec ]]; then result=$(rec_key "$rec" result); fi
  if [[ -n $fail ]]; then
    VERDICT=JOB-NOT-DISPATCHED REASON=$(k1_line "$fail")
  elif [[ -n $done_line && ( -z $rec || $result == pass ) ]]; then
    VERDICT=JOB-COMPLETED REASON=$(k1_line job-completed)
    if [[ -n $rec ]]; then REASON+="; air-gpu-job: $(rec_key "$rec" submits) submissions verified"; fi
  elif [[ -n $done_line ]]; then
    VERDICT=JOB-NOT-DISPATCHED
    REASON="air-gpu-job: $result: $(rec_key "$rec" error) (the kernel: $(k1_line job-completed))"
  elif [[ -n $fw ]]; then
    VERDICT=$(printf '%s\n' "$K1_VERDICTS" | awk -v o="$fw" '$1 == o { print $2 }')
    REASON=$(k1_line "$fw")
    if [[ $VERDICT == FW-RUNNING && $result == pass ]]; then
      # The job checked its results on the CPU, but the kernel logged no job-completed line.
      VERDICT=JOB-COMPLETED
      REASON="air-gpu-job: $(rec_key "$rec" submits) submissions completed with correct results (no job-completed line from the kernel)"
    elif [[ $VERDICT == FW-RUNNING && -n $rec ]]; then
      if [[ $(rec_key "$rec" reached_gpu) == yes || $result == running ]]; then
        VERDICT=JOB-NOT-DISPATCHED
        if [[ $result == running ]]; then REASON="the boot ended while air-gpu-job was running (a hang or a power cycle)"
        else REASON="air-gpu-job: $result: $(rec_key "$rec" error)"; fi
      else
        REASON+="; the job did not reach the GPU ($result)"
      fi
    fi
  else
    line=$({ grep -aoE "$RE_K1_VERDICT.*" "$log" || true; } | tail -1)
    VERDICT=ARMED-NOT-STARTED REASON="a kernel verdict this script does not know: $line"
  fi
  return 0
}

# ---- collection ------------------------------------------------------------------------------

smc_keys_file() { find "$DEBUGFS" -maxdepth 3 -name keys -path '*smc*' 2>/dev/null | head -1; }

main() {
  local boot="" cmd0 b cmd id home user dir out log rec recs f board kf n live n_tg rails
  # Removed on any exit (a global, so the EXIT trap still sees it).
  WORK=""
  trap 'rm -rf "${WORK:-}"' EXIT
  while (($#)); do
    case $1 in
      --boot) boot=${2:-}; shift ;;
      -h | --help) sed -n '2,21p' "$0"; exit 0 ;;
      *) say "refused: unknown argument $1 (--boot 0 or --boot -1)."; exit 2 ;;
    esac
    shift
  done
  [[ -z $boot || $boot == 0 || $boot == -1 ]] || { say "refused: --boot takes 0 or -1."; exit 2; }
  is_root || { say "refused: run as root (sudo): the SMC keys and the kernel log need it."; exit 2; }

  cmd0=$(cat "$CMDLINE")
  if [[ -z $boot ]]; then
    boot=0
    if ! [[ " $cmd0 " =~ $RE_ARMED ]] && [[ " $(cmdline_of -1) " =~ $RE_ARMED ]]; then boot=-1; fi
  fi
  cmd=$(cmdline_of "$boot")
  b=$(boot_id_of "$boot")
  id=$(oneshot_of "$cmd")
  user=${SUDO_USER:-root}
  home=$(user_home "$user")
  board=$(tr '\0' '\n' <"$DT/compatible" 2>/dev/null | sed -n 's/^apple,\(j[0-9a-z]*\)$/\1/p' | head -1)
  out=$home/air-gpu-collect-${board:-mac}-$(date +%Y%m%d-%H%M%S)
  dir=$(mktemp -d)
  WORK=$dir
  mkdir -p "$dir/c"
  local d=$dir/c

  # The analysed boot's kernel log, and this boot's dmesg.
  log=$d/kernel-log-boot$boot.txt
  kernel_log "$boot" | scrub >"$log"
  if [[ $boot != 0 ]]; then dmesg 2>/dev/null | scrub >"$d/dmesg-boot0.txt" || true; fi
  grep -aiE "$RE_GPU_LINES" "$log" >"$d/gpu-log.txt" || true
  grep -aE "$RE_FW_LOG" "$log" >"$d/firmware-log.txt" || true

  # The analysed boot's job records.
  mkdir -p "$d/jobs"
  mapfile -t recs < <(job_records "$b" "$home")
  rec=""
  for f in "${recs[@]}"; do cp "$f" "$d/jobs/"; rec=$f; done

  # Judged without the command-line line, which names the knobs.
  grep -av 'Kernel command line:' "$log" >"$dir/judged.txt" || true
  classify "$dir/judged.txt" "$cmd" "$rec"

  {
    echo "== analysed boot: $boot (boot id ${b:-unknown})"
    echo "cmdline: $cmd"
    echo "oneshot: ${id:-none}"
    echo "knobs: $(knobs_of "$cmd")"
    echo "== this boot"
    uname -a
    echo "cmdline: $cmd0"
    echo "board: ${board:-?} compatible: $(tr '\0' ' ' <"$DT/compatible" 2>/dev/null)"
    pacman -Q linux-aurora m1n1-aurora 2>/dev/null || true
    pacman -Qq 2>/dev/null | grep -i 'mesa' | xargs -r pacman -Q 2>/dev/null || true
    printf 'm3-mode: '; cat /var/lib/aurora-sep/m3-mode 2>/dev/null || echo -
    for f in "$DT"/soc/gpu@*; do
      [[ -e $f/compatible ]] || continue
      echo "gpu node ${f##*/}: $(tr '\0' ' ' <"$f/compatible") status=$(tr -d '\0' <"$f/status" 2>/dev/null || echo okay)"
    done
    echo "chosen:"
    for f in "$DT"/chosen/asahi,t8122-* "$DT"/chosen/asahi,t6030-*; do [[ -e $f ]] && echo "  ${f##*/}"; done
  } >"$d/system.txt" 2>&1

  # The one-shot's state now: the variable, the armed record, the boot entry.
  {
    printf 'LoaderEntryOneShot: '
    if [[ -e $EFIVARS/LoaderEntryOneShot-$BLI_GUID ]]; then
      tail -c +5 "$EFIVARS/LoaderEntryOneShot-$BLI_GUID" | iconv -f UTF-16LE -t UTF-8 2>/dev/null | tr -d '\0'; echo
    else echo "not set"; fi
    printf 'LoaderEntrySelected (this boot): '
    tail -c +5 "$EFIVARS/LoaderEntrySelected-$BLI_GUID" 2>/dev/null | iconv -f UTF-16LE -t UTF-8 2>/dev/null | tr -d '\0'; echo
    printf 'last armed: '; cat "$STATE_DIR/armed" 2>/dev/null || echo -
    command -v air-gpu-oneshot.sh >/dev/null && air-gpu-oneshot.sh --status 2>&1 || true
  } >"$d/oneshot.txt" 2>&1

  # The asahi parameters: the ones sysfs shows, and the ones the analysed boot asked for.
  {
    echo "== /sys/module/asahi/parameters"
    for f in "$SYS"/module/asahi/parameters/*; do [[ -r $f ]] && echo "${f##*/}=$(cat "$f" 2>/dev/null)"; done
    echo "== asked for on the command line (boot $boot)"
    tr ' ' '\n' <<<"$cmd" | grep '^asahi\.' || echo -
  } >"$d/asahi-params.txt" 2>&1
  {
    for f in progress activity memory timing; do
      echo "== $DEBUGFS/asahi-m3/$f"
      head -c 65536 "$DEBUGFS/asahi-m3/$f" 2>&1 || true
    done
  } >"$d/debugfs-asahi-m3.txt" 2>&1
  for f in "$SYS"/class/devcoredump/devcd*; do
    [[ -e $f/data ]] || continue
    echo "${f##*/}: failing_device=$(readlink -f "$f/failing_device" 2>/dev/null)" >>"$d/devcoredump.txt"
    head -c 25165824 "$f/data" >"$d/${f##*/}.bin" 2>/dev/null || true
  done

  # Every SMC T* and P* key, with its value (mC, mW), from this boot.
  kf=$(smc_keys_file)
  if [[ -n $kf ]]; then
    { head -1 "$kf"; awk '$2 ~ /^[TP]/' "$kf"; } >"$d/smc-keys.txt" 2>&1 || true
  else
    echo "no SMC key list in debugfs" >"$d/smc-keys.txt"
  fi

  {
    echo "== /dev/dri"; ls -l /dev/dri 2>&1 || true
    for f in "$SYS"/class/drm/renderD*; do
      [[ -e $f/device/driver ]] && echo "${f##*/}: driver $(basename "$(readlink -f "$f/device/driver")")"
    done
  } >"$d/render-node.txt" 2>&1
  if grep -q 'driver asahi' "$d/render-node.txt"; then n=present; else n=absent; fi

  n_tg=$(awk '$2 ~ /^Tg/' "$d/smc-keys.txt" | wc -l)
  live=$(awk '$2 ~ /^Tg/ && $6 + 0 >= 10000' "$d/smc-keys.txt" | wc -l)
  rails=$(for k in $GPU_RAIL_CANDIDATES; do awk -v k="$k" '$2 == k { printf "%s=%s ", k, $6 }' "$d/smc-keys.txt"; done)
  {
    echo "AIR-GPU VERDICT: $VERDICT | boot $boot | oneshot ${id:-none} | $REASON"
    echo "knobs: $(knobs_of "$cmd")"
    echo "kernel armed with: $({ grep -aoE "$RE_K1_ARMED.*" "$log" || true; } | tail -1 | sed -n 's/^.*admitted): //p')"
    echo "render node: $n"
    echo "SMC (this boot): $live of $n_tg Tg* keys read 10 C or more; GPU rail candidates (mW): ${rails:-none found}"
    echo "job records: ${#recs[@]}"
  } >"$d/summary.txt"

  mv "$d" "$dir/$(basename "$out")"
  tar -C "$dir" -czf "$out.tgz" "$(basename "$out")"
  chown "$user": "$out.tgz" 2>/dev/null || true
  say "wrote $out.tgz"
  sed -n '2,6p' "$dir/$(basename "$out")/summary.txt"
  head -1 "$dir/$(basename "$out")/summary.txt"
}

if [[ ${AIR_GPU_SOURCE_ONLY:-} == 1 ]]; then return 0; fi
main "$@"
