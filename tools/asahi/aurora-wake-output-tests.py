#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host tests of where the tools/aurora-wake Bluetooth wake diagnostics write.

They run as root, so their evidence must go to a directory they create for
themselves: open_out() in bt-wake-cycle.sh and bt-wake-power.sh. The tests
extract that function, as test-s2idle-rtc-regression.sh does for its kernel log
collection, and check that:
- the default directory is a fresh private one, so files planted under the old
  predictable /var/tmp/bt-wake paths, symlinked to victims, are never written;
- -o refuses relative, existing, symlinked and dangling paths, and parents that
  others can change, and creates a new private directory otherwise;
- a directory renamed and replaced by a symlink after open_out() still takes
  every write, and noclobber refuses an existing file;
- no script still names a fixed /var/tmp/bt-wake path, writes a kernel
  attribute without >| or pipes evidence through a truncating tee.

Usage: aurora-wake-output-tests.py --out DIR
"""
import argparse
import os
import re
import stat
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WAKE = ROOT / 'tools/aurora-wake'
SCRIPTS = ['bt-wake-cycle.sh', 'bt-wake-power.sh']
ALL = SCRIPTS + ['attended-bt-wake.sh', 'bt-wake-emulated.sh']
BEGIN = '# --- private output directory'
END = '# --- end of private output directory'

failures = []


def check(ok, what):
    print(('ok: ' if ok else 'FAIL line {}: '.format(sys._getframe(1).f_lineno)) + what)
    if not ok:
        failures.append(what)


def section(name):
    text = (WAKE / name).read_text()
    start = text.find(BEGIN)
    end = text.find(END, start)
    if start < 0 or end < 0:
        return None
    return text[start:end + len(END)] + '\n'


def bash(lib, script, cwd):
    """Run `script` in bash with open_out() defined; returns (rc, stdout)."""
    p = subprocess.run(['bash', '-c', 'set -u\n. "$1"\n' + script, 'test', str(lib)],
                       cwd=cwd, capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True, type=Path)
    work = ap.parse_args().out.resolve()
    work.mkdir(parents=True, exist_ok=True)

    libs = {name: section(name) for name in SCRIPTS}
    for name, text in libs.items():
        check(text is not None, f'{name} has an open_out() section')
    check(len(set(libs.values())) == 1, 'bt-wake-cycle.sh and bt-wake-power.sh share one open_out()')
    if None not in libs.values():
        lib = work / 'open_out.sh'
        lib.write_text(libs[SCRIPTS[0]])
        behaviour(lib, work)
    static()

    if failures:
        print(f'{len(failures)} check(s) failed', file=sys.stderr)
        return 1
    print('all checks passed')
    return 0


def behaviour(lib, work):

    # The issue's attack on the old default: someone owns the shared parent
    # and has planted timestamped leaves whose outputs are symlinks to
    # victims. The default directory is now a fresh one beside them.
    base = work / 'var-tmp'
    base.mkdir()
    os.chmod(base, 0o1777)
    victims = work / 'victims'
    victims.mkdir()
    planted = base / 'bt-wake'
    names = ['results.txt', 'before.txt', 'dmesg.txt', 'after.txt', 'verdict.txt', 'suspending',
             'rtcwake.txt', 'hci.btsnoop', 'bad-lines.txt', 'device.txt']
    for leaf in ['cycle-20261126-120000', 'power-20261126-120000', 'attended-120000']:
        (planted / leaf).mkdir(parents=True)
        for n in names:
            victim = victims / f'{leaf}-{n}'
            victim.write_text('ORIGINAL\n')
            (planted / leaf / n).symlink_to(victim)
    for prefix in ['bt-wake-cycle', 'bt-wake-power']:
        rc, log = bash(lib, f'''open_out "" "{base}" {prefix} || exit 9
            echo "$out"; echo header > results.txt; echo row | tee -a results.txt > /dev/null
            dmesg_text=x; echo "$dmesg_text" > dmesg.txt; : > hci.btsnoop
            echo verdict > verdict.txt; pwd -P''', work)
        lines = log.split()
        check(rc == 0 and len(lines) == 2 and lines[0] == lines[1], f'{prefix}: default directory opened and entered')
        if rc == 0:
            made = Path(lines[0])
            check(made.parent == base and made.name.startswith(prefix + '.') and made.name != prefix + '.XXXXXXXX',
                  f'{prefix}: default directory is a fresh name under the base, not under bt-wake/')
            mode = made.stat()
            check(stat.S_IMODE(mode.st_mode) == 0o700 and mode.st_uid == os.getuid(), f'{prefix}: it is private')
            check((made / 'results.txt').read_text() == 'header\nrow\n', f'{prefix}: evidence written inside it')
    check(all(v.read_text() == 'ORIGINAL\n' for v in victims.iterdir()), 'no planted symlink target was written')

    # -o refusals: nothing may be created or written through these.
    private = work / 'private'
    private.mkdir(mode=0o700)
    existing = private / 'existing'
    existing.mkdir()
    (private / 'link').symlink_to(existing)
    (private / 'dangling').symlink_to(private / 'nowhere')
    (private / 'file').write_text('ORIGINAL\n')
    open_dir = work / 'open'
    open_dir.mkdir()
    os.chmod(open_dir, 0o777)
    group_dir = work / 'group'
    group_dir.mkdir()
    os.chmod(group_dir, 0o775)
    under_open = private / 'inner-open'
    under_open.mkdir()
    os.chmod(under_open, 0o757)
    refused = {
        'relative path': 'relative/out',
        'existing directory': str(existing),
        'symlink to a directory': str(private / 'link'),
        'dangling symlink': str(private / 'dangling'),
        'existing file': str(private / 'file'),
        'missing parent': str(private / 'missing' / 'out'),
        'world-writable parent': str(open_dir / 'out'),
        'group-writable parent': str(group_dir / 'out'),
        'writable ancestor': str(under_open / 'deeper' / 'out'),
        'dot leaf': str(private) + '/.',
        'dot-dot leaf': str(private) + '/..',
    }
    (under_open / 'deeper').mkdir()
    for what, path in refused.items():
        before = sorted(p.name for p in private.iterdir())
        rc, log = bash(lib, f'open_out "{path}" "{base}" x || exit 9; echo opened', work)
        check(rc == 9 and 'opened' not in log, f'-o {what} is refused')
        check(sorted(p.name for p in private.iterdir()) == before, f'-o {what} creates nothing')
    check((private / 'file').read_text() == 'ORIGINAL\n' and not (private / 'nowhere').exists(),
          '-o refusals wrote through nothing')
    check(not (open_dir / 'out').exists() and not (group_dir / 'out').exists()
          and not (under_open / 'deeper' / 'out').exists(), '-o below a writable parent or ancestor creates nothing')

    # -o accepted: a new leaf below a private parent, and below a sticky one.
    sticky = work / 'sticky'
    sticky.mkdir()
    os.chmod(sticky, 0o1777)
    for what, path in {'private parent': private / 'new', 'sticky parent': sticky / 'new'}.items():
        rc, log = bash(lib, f'open_out "{path}" "{base}" x || exit 9; pwd -P; echo e > evidence.txt', work)
        check(rc == 0 and log.split() == [str(path)], f'-o below a {what} opens the new directory')
        check(path.is_dir() and stat.S_IMODE(path.stat().st_mode) == 0o700 and (path / 'evidence.txt').exists(),
              f'-o below a {what}: private, with the evidence inside')

    # A directory renamed and replaced after open_out(): the script keeps
    # writing into the directory it holds, never through the replacement.
    swap_victim = work / 'swap-victim'
    swap_victim.mkdir()
    target = private / 'swap'
    rc, log = bash(lib, f'''open_out "{target}" "{base}" x || exit 9
        echo first > before.txt
        mv -- "{target}" "{target}.moved" && ln -s "{swap_victim}" "{target}"
        echo later > dmesg.txt; echo verdict > verdict.txt''', work)
    check(rc == 0 and (Path(f'{target}.moved') / 'dmesg.txt').read_text() == 'later\n',
          'writes after a swap land in the held directory')
    check(not any(swap_victim.iterdir()), 'nothing is written through the swapped-in symlink')

    # noclobber: an existing file is never truncated, kernel attributes use >|.
    attr = work / 'attribute'
    attr.write_text('enabled\n')
    rc, log = bash(lib, f'''open_out "{private / 'clobber'}" "{base}" x || exit 9
        echo old > results.txt
        if (echo new > results.txt) 2> /dev/null; then exit 8; fi
        echo disabled >| "{attr}"
        cat results.txt''', work)
    check(rc == 0 and log.strip() == 'old', 'noclobber refuses to overwrite an existing evidence file')
    check(attr.read_text() == 'disabled\n', '>| still writes a kernel attribute')


def static():
    """The scripts themselves."""
    for name in ALL:
        text = (WAKE / name).read_text()
        code = '\n'.join(line for line in text.splitlines() if not line.lstrip().startswith('#'))
        check('/var/tmp/bt-wake/' not in code and not re.search(r'=/var/tmp/bt-wake\b', code),
              f'{name}: no fixed /var/tmp/bt-wake path')
        check(not re.search(r'(?<![>|])>\s*"?(\$dev|/sys/)', code),
              f'{name}: kernel attributes are written with >|')
        check(not re.search(r'mkdir -p "\$out"', code) or name == 'bt-wake-emulated.sh',
              f'{name}: the evidence directory is not taken over with mkdir -p')
    # The emulated run's own tee writes the emulator's control file over ssh,
    # not evidence on the target.
    for name in SCRIPTS + ['attended-bt-wake.sh']:
        code = (WAKE / name).read_text()
        check(not re.search(r'\btee\s+(?!-a\b)', code), f'{name}: no truncating tee')
    for name in SCRIPTS:
        code = (WAKE / name).read_text()
        check(re.search(r'^open_out "\$out" /var/tmp bt-wake-', code, re.M) is not None,
              f'{name}: opens its evidence directory with open_out()')
        rest = code[code.find(END):] if END in code else code
        check('"$out/' not in rest, f'{name}: writes relative to the held directory, not through $out')
    attended = (WAKE / 'attended-bt-wake.sh').read_text()
    check('mktemp -d /var/tmp/bt-wake-attended.' in attended and 'set -C' in attended,
          'attended-bt-wake.sh: a fresh private directory with noclobber')
    emulated = (WAKE / 'bt-wake-emulated.sh').read_text()
    check('mktemp -d /var/tmp/bt-wake-run.' in emulated and 'rm -rf $d' not in emulated,
          'bt-wake-emulated.sh: the target copy and evidence go to a fresh directory')


if __name__ == '__main__':
    sys.exit(main())
