#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Boot a release kernel under QEMU (virt machine) and load every module it ships.
#
# Usage: kernel-smoke-qemu.sh <Image.gz> <modules.tar.zst> <busybox> <outdir>
#
# Loading a KUnit module runs its suites, so this also runs every KUnit suite the
# configuration builds as a module. The Apple drivers find no hardware under
# QEMU and only register themselves; real hardware is covered by the lab gate.
#
# .github/scripts/kernel-smoke-expected.txt lists modules that are not loaded
# (skip) or are known not to load or to fail under QEMU (fail), and KUnit suites
# known to fail (kunit), each with a reason. A module whose init returns -ENODEV
# or -ENXIO (no such device: it probed for hardware QEMU does not have) counts as
# loaded correctly. Anything else that fails to load, hangs, oopses, warns or
# panics fails the run.

set -euo pipefail

IMG=$(realpath "$1")
MODS=$(realpath "$2")
BUSYBOX=$(realpath "$3")
OUT=$(realpath -m "$4")
HERE=$(dirname "$(realpath "$0")")
EXPECTED=$HERE/kernel-smoke-expected.txt
QEMU_TIMEOUT=${QEMU_TIMEOUT:-9000}
# Per-module watchdog. Compute-heavy KUnit suites (raid6) need more than two
# minutes under emulation on the hosted ARM runners.
MOD_TIMEOUT=${MOD_TIMEOUT:-300}

mkdir -p "$OUT"
R=$(mktemp -d "$OUT/initramfs.XXXXXX")
trap 'rm -rf "$R"' EXIT

# sbin too: request_module() runs /sbin/modprobe.
mkdir -p "$R"/{bin,sbin,usr/bin,usr/sbin,dev,proc,sys,tmp,etc}
install -m 0755 "$BUSYBOX" "$R/bin/busybox"
tar --zstd -xf "$MODS" -C "$R"
KREL=$(ls "$R/lib/modules")
# skip entries may be shell globs (speakup_*); expand them against modules.order.
mapfile -t SKIPPAT < <(awk '$1 == "skip" { print $2 }' "$EXPECTED")
while read -r path; do
	m=${path##*/}; m=${m%%.ko*}
	for pat in "${SKIPPAT[@]}"; do
		# shellcheck disable=SC2053
		if [[ $m == $pat ]]; then echo "$m"; break; fi
	done
done < "$R/lib/modules/$KREL/modules.order" > "$R/etc/smoke-skip"
echo "skipping $(wc -l < "$R/etc/smoke-skip") modules"

cat > "$R/init" <<EOF
#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo "SMOKE-BEGIN \$(uname -r) \$(grep -c . /lib/modules/\$(uname -r)/modules.order) modules"
while read -r path; do
	m=\${path##*/}; m=\${m%%.ko*}
	if grep -qxF "\$m" /etc/smoke-skip; then echo "SMOKE-SKIP \$m"; continue; fi
	echo "smoke-load \$m" > /dev/kmsg
	modprobe "\$m" > /tmp/out 2>&1 &
	pid=\$!; t=0
	while kill -0 \$pid 2>/dev/null && [ \$t -lt $MOD_TIMEOUT ]; do sleep 1; t=\$((t + 1)); done
	if kill -0 \$pid 2>/dev/null; then echo "SMOKE-HANG \$m"; continue; fi
	if wait \$pid; then echo "SMOKE-OK \$m"
	elif grep -q "No such device" /tmp/out; then echo "SMOKE-NODEV \$m"
	else echo "SMOKE-FAIL \$m \$(head -c 300 /tmp/out | tr '\n' ' ')"; fi
done < /lib/modules/\$(uname -r)/modules.order
echo SMOKE-DMESG-BEGIN
dmesg
echo SMOKE-DMESG-END
echo SMOKE-END
poweroff -f
EOF
chmod 0755 "$R/init"

(cd "$R" && find . | LC_ALL=C sort | cpio -o -H newc --quiet | zstd -T0 -q -3 > "$OUT/initramfs.cpio.zst")
rm -rf "$R"; trap - EXIT

ACCEL=tcg
if [ -w /dev/kvm ] && [ "$(uname -m)" = aarch64 ]; then ACCEL=kvm; fi
# pauth-impdef: the architected pointer-authentication algorithm is slow to
# emulate, and this kernel signs every return address.
CPU=max,pauth-impdef=on; [ $ACCEL = kvm ] && CPU=host
echo "qemu: accel=$ACCEL kernel=$KREL"
set +e
: > "$OUT/console.log"
timeout -k 10 "$QEMU_TIMEOUT" qemu-system-aarch64 -M virt -accel "$ACCEL" -cpu "$CPU" -smp 4 -m 6G \
	-nographic -no-reboot -nic none -kernel "$IMG" -initrd "$OUT/initramfs.cpio.zst" \
	-append "console=ttyAMA0 earlycon=pl011,0x09000000 rdinit=/init panic=-1 loglevel=4 log_buf_len=128M kunit.enable=1" \
	>> "$OUT/console.log" 2>&1 &
QEMU_PID=$!
trap 'kill -TERM "$QEMU_PID" 2>/dev/null || true' EXIT

BOOT_DEADLINE=$((SECONDS + 300))
while kill -0 "$QEMU_PID" 2>/dev/null && ! grep -q '^SMOKE-BEGIN ' "$OUT/console.log"; do
	if (( SECONDS >= BOOT_DEADLINE )); then
		echo "qemu: smoke init did not start within 300 seconds"
		kill -TERM "$QEMU_PID" 2>/dev/null
		break
	fi
	sleep 2
done
wait "$QEMU_PID"
QRC=$?
trap - EXIT
set -e
echo "qemu exit $QRC"

python3 - "$OUT/console.log" "$EXPECTED" "$OUT" <<'PY'
import fnmatch, re, sys, json
log, expected, out = sys.argv[1:]
text = open(log, errors='replace').read().replace('\r', '')
exp = {'skip': {}, 'fail': {}, 'warn': {}, 'kunit': {}}
for line in open(expected):
    line = line.strip()
    if not line or line.startswith('#'):
        continue
    kind, name, *why = line.split(None, 2)
    exp[kind][name] = why[0] if why else ''
def expected(kind, name):
    return any(fnmatch.fnmatchcase(name, pat) for pat in exp[kind])
res = {}
for m in re.finditer(r'^SMOKE-(OK|NODEV|FAIL|HANG|SKIP) (\S+)(.*)$', text, re.M):
    res[m.group(2)] = (m.group(1), m.group(3).strip())
begin = re.search(r'^SMOKE-BEGIN (\S+) (\d+) modules', text, re.M)
ended = re.search(r'^SMOKE-END$', text, re.M) is not None
d = re.search(r'^SMOKE-DMESG-BEGIN$(.*)^SMOKE-DMESG-END$', text, re.M | re.S)
dmesg = d.group(1) if d else text
open(f'{out}/dmesg.txt', 'w').write(dmesg)
strip = lambda l: re.sub(r'^\[\s*[\d.]+\]\s?', '', l)
lines = [strip(l) for l in dmesg.splitlines()]
kunit = {}
for l in lines:
    m = re.match(r'^(not ok|ok) \d+ (\S+)(.*)$', l)
    if m:
        kunit[m.group(2)] = 'SKIP' if '# SKIP' in m.group(3) else ('PASS' if m.group(1) == 'ok' else 'FAIL')
# Kernel warnings, oopses and stalls, attributed to the module being loaded
# (the init writes "smoke-load <module>" to /dev/kmsg before each modprobe).
EVENT = re.compile(r'^(WARNING:|Oops|BUG:|Kernel panic|Unable to handle kernel|INFO: task .* blocked for more than|watchdog: BUG: soft lockup|rcu: INFO: .*detected stall)')
# KUnit modules provoke warnings and KFENCE reports on purpose; for them the
# suite results below are the verdict.
cur, events, kunit_mods = None, [], set()
for l in lines:
    m = re.match(r'^smoke-load (\S+)$', l)
    if m:
        cur = m.group(1)
        continue
    if l.startswith('KTAP version') and cur:
        kunit_mods.add(cur)
    if EVENT.search(l):
        events.append((cur, l))
bad_kernel, tolerated, kunit_warn = [], [], 0
for mod, l in events:
    if mod in kunit_mods:
        kunit_warn += 1
    elif mod and expected('warn', mod):
        tolerated.append(f'{mod}: {l}')
    else:
        bad_kernel.append(f'{mod}: {l}')
problems = []
if not begin:
    problems.append('kernel did not reach the smoke init')
if not ended:
    problems.append('run did not finish (panic, hang or timeout)')
counts = {}
for name, (st, msg) in res.items():
    counts[st] = counts.get(st, 0) + 1
    if st in ('FAIL', 'HANG') and not expected('fail', name):
        problems.append(f'module {name}: {st} {msg}')
stale = [n for n in exp['fail'] if res.get(n, ('', ''))[0] == 'OK']
kfail = [s for s, r in kunit.items() if r == 'FAIL' and not expected('kunit', s)]
problems += [f'KUnit suite {s}: FAIL' for s in kfail]
if bad_kernel:
    problems += [f'kernel: {l}' for l in bad_kernel[:40]]
kc = {}
for r in kunit.values():
    kc[r] = kc.get(r, 0) + 1
summary = {'kernel': begin.group(1) if begin else None, 'modules': int(begin.group(2)) if begin else 0,
           'finished': ended, 'loads': counts, 'kunit_suites': kc,
           'kunit_modules': len(kunit_mods), 'kunit_module_warnings': kunit_warn,
           'stale_expected_failures': stale, 'tolerated_warnings': tolerated,
           'kernel_warnings': bad_kernel[:50], 'problems': problems}
json.dump(summary, open(f'{out}/smoke-summary.json', 'w'), indent=1)
with open(f'{out}/smoke-results.tsv', 'w') as f:
    for name, (st, msg) in sorted(res.items()):
        f.write(f'{name}\t{st}\t{msg}\n')
    for s, r in sorted(kunit.items()):
        f.write(f'kunit:{s}\t{r}\t\n')
print(json.dumps({k: v for k, v in summary.items() if k != 'kernel_warnings'}, indent=1)[:6000])
sys.exit(1 if problems else 0)
PY
