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
#   NOT-ARMED            the analysed boot did not have asahi.t8122_start=1 (no one-shot boot)
#   ARMED-NO-LOG         the last arming's boot left no kernel log (most likely a hang in the GPU
#                        start, before the journal was written); no older boot's result is shown
#   ARMED-NOT-STARTED    armed, but the driver/firmware stopped before the firmware ran
#   FW-BOOT-FAILED       the firmware was started but did not come up
#   INITDATA-REJECTED    the firmware came up but did not accept the InitData
#   FW-RUNNING           the firmware accepted the InitData and runs; no GPU job ran
#   FW-CRASHED           the firmware ran, then crashed or a post-boot/cap check failed
#   JOB-NOT-DISPATCHED   a GPU job ran and did not complete correctly (or got stuck/killed)
#   JOB-COMPLETED        air-gpu-job.sh ran, and every submission completed with correct results
#   UNKNOWN              a kernel verdict this script does not recognise
set -euo pipefail

# ---- the verdict table: the kernel log lines each step is judged by --------------------------
# Keep this in step with the kernel branch (air/t8122-gpu-start: t8122_start.rs).
#
# 1. The kernel's own verdict lines, on an armed T8122 ("M3 G15G verdict: <outcome>"), and the
#    verdict each outcome gives. The full label set is K1's (air/t8122-gpu-start dbdd6c6f,
#    t8122_start.rs; impl-k1-t8122-start.md "Verdict strings for the S1 collect script"). Each
#    label has a rank: the highest-ranked label present decides, so a job failure outranks
#    job-completed, and a crash/cap/check failure (FW-CRASHED) outranks firmware-running. The
#    match is on the label up to the first ':' or '('.
#   label                           verdict             rank
K1_VERDICTS='
firmware-boot-failed            FW-BOOT-FAILED      30
driver-refused                  ARMED-NOT-STARTED   30
publish-failed                  FW-BOOT-FAILED      30
device-control-failed           INITDATA-REJECTED   30
initdata-rejected               INITDATA-REJECTED   30
cap-violated                    FW-CRASHED          40
firmware-running-check-failed   FW-CRASHED          40
job-faulted                     JOB-NOT-DISPATCHED  50
job-accepted-never-dispatched   JOB-NOT-DISPATCHED  50
job-timed-out-powered           JOB-NOT-DISPATCHED  50
job-timed-out                   JOB-NOT-DISPATCHED  50
job-ran-completion-missed       JOB-NOT-DISPATCHED  50
job-retired-without-timestamps  JOB-NOT-DISPATCHED  50
job-failed-before-wait          JOB-NOT-DISPATCHED  50
firmware-running                FW-RUNNING          10
job-completed                   JOB-COMPLETED       20
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
# The firmware crashed, reported an error, or the GPU faulted: any of these overrides a
# firmware-running or job-completed verdict (A2), since the firmware did not run cleanly.
RE_FW_CRASH='\.gpu: RTKit: co-processor has crashed|M3 G15[A-Z]*: firmware crashed|firmware crashed while waiting for init|GPU firmware crashed|M3 firmware error event|M3 bank [0-9]+ fault=0x[0-9a-f]*[1-9a-f]|M3 fault render|M3 fault mapping'
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
# SMC power keys that read zero at idle on an M3 Air (J613) sample (2026-10-06): the GPU rail
# candidates. One of them should rise the first time the GPU draws power.
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
# Run as the invoking user when root under sudo, else directly: the tgz is written this way, so a
# symlink in the user's home can't redirect a root write or chown (A13).
runu() {
  if [[ $(id -u) == 0 && -n ${SUDO_USER:-} && $SUDO_USER != root ]]; then
    runuser -u "$SUDO_USER" -- "$@"
  else
    "$@"
  fi
}
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
# The job records of the analysed boot, folded into one state so the verdict never rests on a
# single record (A16). Set by job_aggregate, read by classify.
#   JOB_STATE: pass (>=1 pass, 0 fail) | fail (>=1 fail) | not-reached | none
#   JOB_DETAIL: a short reason for the chosen state
JOB_STATE=none JOB_DETAIL=""

# The outcome of one job record (A3): the result alone decides, never reached_gpu. Only
# no-render-node/no-device mean the job never reached the GPU; stuck, killed, device-lost,
# fence-timeout, stalled, wrong-result, running and anything else are failures.
job_outcome() { # record -> echoes pass | fail | not-reached
  local r
  r=$(rec_key "$1" result)
  case $r in
    pass) echo pass ;;
    no-render-node | no-device) echo not-reached ;;
    *) echo fail ;;
  esac
}

# Fold every record of the boot (oldest first). Any failure makes the whole state fail (A16).
job_aggregate() { # record paths...
  local f o npass=0 nfail=0 nreach=0 n=0 failrec="" passrec="" reachrec=""
  JOB_STATE=none JOB_DETAIL=""
  for f in "$@"; do
    [[ -f $f ]] || continue
    n=$((n + 1))
    o=$(job_outcome "$f")
    case $o in
      pass) npass=$((npass + 1)); passrec=$f ;;
      fail) nfail=$((nfail + 1)); failrec=${failrec:-$f} ;;
      not-reached) nreach=$((nreach + 1)); reachrec=$f ;;
    esac
  done
  if ((nfail > 0)); then
    JOB_STATE=fail
    JOB_DETAIL="air-gpu-job: $(rec_key "$failrec" result): $(rec_key "$failrec" error) (stage $(rec_key "$failrec" stage); $npass of $n runs passed)"
  elif ((npass > 0)); then
    JOB_STATE=pass
    JOB_DETAIL="air-gpu-job: $npass of $n runs completed with correct results (first in $(rec_key "$passrec" first_submit_ms) ms) on $(rec_key "$passrec" device)"
  elif ((nreach > 0)); then
    JOB_STATE=not-reached
    JOB_DETAIL="air-gpu-job: the job did not reach the GPU ($(rec_key "$reachrec" result))"
  else
    JOB_STATE=none
  fi
}

# classify LOG CMDLINE  (JOB_STATE/JOB_DETAIL set by job_aggregate beforehand). It decides the
# verdict, then applies the A2 crash override to both the kernel-line and the structural paths.
classify() { classify_core "$@"; crash_override "$1"; }
classify_core() {
  local log=$1 cmd=$2 accept_ln crash_ln line
  first_ln() { { grep -naE "$1" "$log" || true; } | head -1 | cut -d: -f1; }
  last_line() { { grep -aE "$1" "$log" || true; } | tail -1 | sed -E 's/^\[[^]]*\] *//; s/^[^ ]+ kernel: //'; }
  if ! [[ " $cmd " =~ $RE_ARMED ]]; then
    VERDICT=NOT-ARMED REASON="asahi.t8122_start=1 is not on the analysed boot's command line"
    return 0
  fi
  if grep -qaE "$RE_K1_VERDICT" "$log"; then
    classify_k1 "$log"
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
  # The firmware accepted the InitData.
  crash_ln=$({ grep -naE "$RE_FW_CRASH" "$log" || true; } | awk -F: -v a="$accept_ln" '$1 > a { print $1; exit }')
  if [[ -n $crash_ln && $JOB_STATE != pass && $JOB_STATE != fail ]]; then
    VERDICT=FW-CRASHED REASON="the firmware accepted the InitData, then crashed: $(last_line "$RE_FW_CRASH")"
    return 0
  fi
  # A job failure line, or a job record that failed or got stuck (A3), overrides a clean run.
  if [[ $JOB_STATE == fail ]]; then
    VERDICT=JOB-NOT-DISPATCHED
    if line=$(last_line "$RE_JOB_FAILED") && [[ -n $line ]]; then REASON="$line; $JOB_DETAIL"
    else REASON=$JOB_DETAIL; fi
    return 0
  fi
  if grep -qaE "$RE_JOB_FAILED" "$log"; then
    VERDICT=JOB-NOT-DISPATCHED REASON=$(last_line "$RE_JOB_FAILED")
    return 0
  fi
  if [[ $JOB_STATE == pass ]]; then
    VERDICT=JOB-COMPLETED REASON=$JOB_DETAIL
    return 0
  fi
  VERDICT=FW-RUNNING
  if [[ $JOB_STATE == not-reached ]]; then REASON="the firmware accepted the InitData and runs; $JOB_DETAIL"
  else REASON="the firmware accepted the InitData and runs; no GPU job ran in this boot"; fi
}

# The kernel's own verdict lines decide. The highest-ranked label present wins (so a failure
# beats job-completed, a crash/cap/check beats firmware-running). JOB_STATE then gates the two
# success verdicts (A4/A16).
classify_k1() { # LOG
  local log=$1 outcomes o v rank best="" bestrank=-1 bestv=""
  k1_line() { { grep -aE "M3 G15G verdict: $1" "$log" || true; } | tail -1 | sed -E 's/^.*(M3 G15G verdict: )/\1/'; }
  outcomes=$({ grep -aoE "$RE_K1_VERDICT" "$log" || true; } | sed 's/^M3 G15G verdict: //' | sort -u)
  for o in $outcomes; do
    v="" rank=""
    # An unknown label gives no awk output, so read hits EOF; || true keeps set -e from aborting.
    read -r v rank < <(printf '%s\n' "$K1_VERDICTS" | awk -v o="$o" '$1 == o { print $2, $3 }') || true
    [[ -n $v ]] || continue
    if ((rank > bestrank)); then bestrank=$rank best=$o bestv=$v; fi
  done
  if [[ -z $bestv ]]; then
    VERDICT=UNKNOWN REASON="a kernel verdict this script does not recognise: $({ grep -aoE "$RE_K1_VERDICT.*" "$log" || true; } | tail -1)"
    return 0
  fi
  case $bestv in
    JOB-COMPLETED)
      # The kernel's line alone is not enough: a passing air-gpu-job record from this boot is
      # required, and any failed record keeps it from being JOB-COMPLETED (A4/A16).
      if [[ $JOB_STATE == pass ]]; then
        VERDICT=JOB-COMPLETED REASON="$(k1_line job-completed); $JOB_DETAIL"
      elif [[ $JOB_STATE == fail ]]; then
        VERDICT=JOB-NOT-DISPATCHED REASON="$JOB_DETAIL (the kernel logged: $(k1_line job-completed))"
      else
        VERDICT=FW-RUNNING REASON="a job retired (kernel: $(k1_line job-completed)), but no air-gpu-job record checked its results on the CPU${JOB_STATE:+; $JOB_DETAIL}"
      fi
      ;;
    FW-RUNNING)
      # firmware-running, no job verdict from the kernel: let a job record decide if there is one.
      if [[ $JOB_STATE == pass ]]; then
        VERDICT=JOB-COMPLETED REASON="$JOB_DETAIL (no job-completed line from the kernel)"
      elif [[ $JOB_STATE == fail ]]; then
        VERDICT=JOB-NOT-DISPATCHED REASON=$JOB_DETAIL
      elif [[ $JOB_STATE == not-reached ]]; then
        VERDICT=FW-RUNNING REASON="$(k1_line firmware-running); $JOB_DETAIL"
      else
        VERDICT=FW-RUNNING REASON=$(k1_line firmware-running)
      fi
      ;;
    *)
      VERDICT=$bestv REASON=$(k1_line "$best")
      ;;
  esac
  return 0
}

# A2: after the kernel's verdict lines, a crash, fault or RTKit-crash line still overrides a
# firmware-running or job-completed verdict; the firmware did not run cleanly.
crash_override() { # LOG
  local log=$1 line
  case $VERDICT in FW-RUNNING | JOB-COMPLETED) ;; *) return 0 ;; esac
  grep -qaE "$RE_FW_CRASH" "$log" || return 0
  line=$({ grep -aE "$RE_FW_CRASH" "$log" || true; } | tail -1 | sed -E 's/^\[[^]]*\] *//; s/^[^ ]+ kernel: //')
  if [[ $VERDICT == JOB-COMPLETED ]]; then
    VERDICT=FW-CRASHED REASON="a job completed, then the firmware crashed or faulted: $line (earlier: ${REASON})"
  else
    VERDICT=FW-CRASHED REASON="the firmware ran, then crashed or faulted: $line"
  fi
}

# ---- collection ------------------------------------------------------------------------------

smc_keys_file() { find "$DEBUGFS" -maxdepth 3 -name keys -path '*smc*' 2>/dev/null | head -1; }

# The LoaderEntryOneShot variable's value now, empty when unset (Limine clears it as it boots).
oneshot_var() {
  local f=$EFIVARS/LoaderEntryOneShot-$BLI_GUID
  [[ -e $f ]] || return 0
  tail -c +5 "$f" | iconv -f UTF-16LE -t UTF-8 2>/dev/null | tr -d '\0'
}
# The boot index (0, -1, -2 ...) whose command line carries air_gpu.oneshot=<id>, or empty.
boot_with_oneshot() {
  local want=$1 idx c
  for idx in 0 $({ journalctl --list-boots --no-pager 2>/dev/null || true; } | awk '$1 < 0 { print $1 }'); do
    c=$(cmdline_of "$idx")
    [[ " $c " == *" ${TAG}=$want "* ]] && { echo "$idx"; return 0; }
  done
  return 1
}

main() {
  local boot="" explicit=0 dumps=0 armed_id="" sel cmd0 b cmd id home user dir out log recs f board kf n live n_tg rails
  # Removed on any exit (a global, so the EXIT trap still sees it).
  WORK=""
  trap 'rm -rf "${WORK:-}"' EXIT
  while (($#)); do
    case $1 in
      --boot) boot=${2:-}; explicit=1; shift ;;
      --include-dumps) dumps=1 ;;
      -h | --help) sed -n '2,24p' "$0"; exit 0 ;;
      *) say "refused: unknown argument $1 (--boot N or --include-dumps)."; exit 2 ;;
    esac
    shift
  done
  [[ -z $boot || $boot =~ ^-?[0-9]+$ ]] || { say "refused: --boot takes a boot index (0, -1, ...)."; exit 2; }
  is_root || { say "refused: run as root (sudo): the SMC keys and the kernel log need it."; exit 2; }

  cmd0=$(cat "$CMDLINE")
  # A1: judge the last arming by its id, so a hung armed boot never shows an older boot's result.
  [[ -f $STATE_DIR/armed ]] && read -r armed_id _ <"$STATE_DIR/armed"
  if ((! explicit)); then
    if [[ -n $armed_id ]]; then
      sel=$(boot_with_oneshot "$armed_id") || sel=""
      if [[ -n $sel ]]; then
        boot=$sel
      elif [[ -z $(oneshot_var) ]]; then
        # The one-shot was consumed (Limine cleared it) but no boot carries the id: the armed
        # boot ran and left no journal, the signature of a hang in the GPU start.
        boot=0 VERDICT=ARMED-NO-LOG
        REASON="the armed boot (oneshot $armed_id) left no kernel log, most likely a hang in the GPU start before the journal was written; no older boot's result is shown"
      else
        # Still armed, not booted yet: the tester ran this before rebooting.
        boot=0
        say "the armed boot (oneshot $armed_id) has not run yet (LoaderEntryOneShot is still set). Reboot, then run this again."
      fi
    else
      boot=0
      if ! [[ " $cmd0 " =~ $RE_ARMED ]] && [[ " $(cmdline_of -1) " =~ $RE_ARMED ]]; then boot=-1; fi
    fi
  fi
  cmd=$(cmdline_of "$boot")
  b=$(boot_id_of "$boot")
  id=$(oneshot_of "$cmd")
  [[ -z $id && -n $armed_id && $VERDICT == ARMED-NO-LOG ]] && id=$armed_id
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

  # The analysed boot's job records, folded into JOB_STATE across all of them (A16).
  mkdir -p "$d/jobs"
  mapfile -t recs < <(job_records "$b" "$home")
  for f in "${recs[@]}"; do cp "$f" "$d/jobs/"; done
  job_aggregate "${recs[@]}"

  # Judged without the command-line line, which names the knobs. ARMED-NO-LOG was already set.
  if [[ $VERDICT != ARMED-NO-LOG ]]; then
    grep -av 'Kernel command line:' "$log" >"$dir/judged.txt" || true
    classify "$dir/judged.txt" "$cmd"
  fi

  {
    echo "== analysed boot: $boot (boot id ${b:-unknown})"
    echo "cmdline: $cmd"
    echo "oneshot: ${id:-none}"
    echo "knobs: $(knobs_of "$cmd")"
    echo "last armed record: $(cat "$STATE_DIR/armed" 2>/dev/null || echo -)"
    echo "== this boot"
    # A15: no hostname (uname -a would include it); this tgz is meant for a public issue.
    uname -srvm
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
  # A15: a devcoredump is a raw image of GPU-side memory. The default tgz is meant for a public
  # issue, so it records only each dump's presence and size, not its bytes. --include-dumps adds
  # the raw .bin for a private hand-over.
  for f in "$SYS"/class/devcoredump/devcd*; do
    [[ -e $f/data ]] || continue
    echo "${f##*/}: failing_device=$(readlink -f "$f/failing_device" 2>/dev/null) size=$(stat -c %s "$f/data" 2>/dev/null)" >>"$d/devcoredump.txt"
    if ((dumps)); then head -c 25165824 "$f/data" >"$d/${f##*/}.bin" 2>/dev/null || true; fi
  done
  if [[ -e $d/devcoredump.txt ]] && ((! dumps)); then
    echo "(raw dumps left out; run with --include-dumps for a private hand-over, not a public issue)" >>"$d/devcoredump.txt"
  fi

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
    echo "job records: ${#recs[@]} (state: $JOB_STATE)"
  } >"$d/summary.txt"

  # Build the tgz in the work dir (root-owned temp), then copy it out as the user (A13).
  local base summary
  base=$(basename "$out")
  mv "$d" "$dir/$base"
  summary=$dir/$base/summary.txt
  tar -C "$dir" -czf "$dir/out.tgz" "$base"
  # The user (runu) can't read the root-only mktemp dir, so hand the tgz over through its own
  # world-readable temp file (the tgz is meant for a public issue); the collection dir stays 0700.
  local pub
  pub=$(mktemp --tmpdir "air-gpu-collect.XXXXXX.tgz")
  cp -f "$dir/out.tgz" "$pub"
  chmod 0644 "$pub"
  if runu cp -f "$pub" "$out.tgz" 2>/dev/null; then
    say "wrote $out.tgz"
  else
    # The user's home was not writable (a symlink, say); keep the tgz where root can reach it.
    cp -f "$pub" "/tmp/$base.tgz"
    out=/tmp/$base
    say "wrote $out.tgz (could not write $user's home)"
  fi
  rm -f "$pub"
  sed -n '2,6p' "$summary"
  head -1 "$summary"
}

if [[ ${AIR_GPU_SOURCE_ONLY:-} == 1 ]]; then return 0; fi
main "$@"
