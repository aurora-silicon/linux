#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Derive a package's runtime dependencies from its ELF files.

needs-derive.py PKGDIR LIBDIR OUT_TSV OUT_NEEDS [--libdir-label PATH] [--linked PKG=VERSION]... [--extra PKG=WHY]...
                [--declared DEP]...

- Every DT_NEEDED soname of every ELF file under PKGDIR is either provided
  inside the package (a file or link of that name under LIBDIR) or found in
  /usr/lib and mapped to its owning package with pacman -Qqo.
- Minimum versions come from the symbol versions the files require:
  glibc from the highest GLIBC_x.y; libstdc++ from the highest GLIBCXX_3.4.N
  and CXXABI_1.3.N; libgcc from the highest GCC_x.y.z. An unknown version is
  an error, so the tables below get extended rather than guessed.
- --linked PKG=VERSION sets the minimum for a library whose soname carries no
  version and whose symbols carry none (the version it was linked against).
- A NEEDED libLLVM.so.N pins llvm-libs to that soname major.
- --extra PKG=WHY adds a dependency that is not an ELF library (an
  interpreter), without a minimum, with WHY recorded.
- Everything else is left without a minimum: its soname is the ABI promise.
- With --declared, the result must equal the declared depends exactly
  (names and minimums), or the script fails.

OUT_TSV lists soname, owner, minimum, why, the files that need it and the
symbol versions they use. OUT_NEEDS is one line: the dependencies as
"name>=version" or "name", sorted.
"""
import collections, os, re, subprocess, sys

# GCC release that first provides each libstdc++/libgcc symbol version.
GLIBCXX = {21: '5.1', 22: '6.1', 23: '7.1', 24: '7.2', 25: '8.1', 26: '9.1', 28: '10.1', 29: '11.1', 30: '12.1',
           31: '13.1', 32: '13.2',
           33: '14.1', 34: '15.1', 35: '16.1'}
CXXABI = {9: '5.1', 10: '6.1', 11: '7.1', 12: '9.1', 13: '11.1', 14: '12.1', 15: '13.1', 16: '14.1'}


def vkey(v):
    return [int(x) for x in re.findall(r'\d+', v)]


def run(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout


args = sys.argv[1:]
pkgdir, libdir, out_tsv, out_needs = args[:4]
linked, declared, extra = {}, [], {}
libdir_label = None
rest = args[4:]
while rest:
    flag, val = rest[0], rest[1]
    rest = rest[2:]
    if flag == '--linked':
        k, v = val.split('=', 1)
        linked[k] = v
    elif flag == '--declared':
        declared.append(val)
    elif flag == '--libdir-label':
        libdir_label = val
    elif flag == '--extra':
        k, v = val.split('=', 1)
        extra[k] = v
    else:
        sys.exit(f'needs-derive: unknown option {flag}')

elfs = []
for d, _, files in os.walk(pkgdir):
    for f in files:
        p = os.path.join(d, f)
        if not os.path.islink(p) and open(p, 'rb').read(4) == b'\x7fELF':
            elfs.append(p)
internal = set(os.listdir(libdir))

needed = collections.defaultdict(set)      # soname -> files
versions = collections.defaultdict(set)    # soname -> symbol versions
for e in sorted(elfs):
    rel = os.path.relpath(e, pkgdir)
    for m in re.finditer(r'\(NEEDED\).*\[(.+?)\]', run('readelf', '-d', '-W', e)):
        needed[m.group(1)].add(rel)
    cur, in_needs = None, False
    for line in run('readelf', '-V', '-W', e).splitlines():
        if line.startswith('Version needs section'):
            in_needs = True
            continue
        if line.startswith('Version ') and 'section' in line:
            in_needs = False
            continue
        if not in_needs:
            continue
        m = re.search(r'File: (\S+)', line)
        if m:
            cur = m.group(1)
            continue
        m = re.search(r'Name: (\S+)', line)
        if m and cur:
            versions[cur].add(m.group(1))

rows, mins, why = [], {}, {}
def want(pkg, v, reason):
    if v is None:
        mins.setdefault(pkg, None)
    elif mins.get(pkg) is None or vkey(v) > vkey(mins[pkg]):
        mins[pkg] = v
    why.setdefault(pkg, set()).add(reason)

errors = []
for so in sorted(needed):
    if so in internal:
        rows.append((so, '(this package)', '-', 'provided in ' + (libdir_label or libdir), needed[so], versions[so]))
        continue
    path = os.path.join('/usr/lib', so)
    owner = run('pacman', '-Qqo', path).strip() if os.path.exists(path) else ''
    if not owner:
        errors.append(f'{so}: not found in /usr/lib or not owned by a package')
        continue
    v, reason = None, 'soname'
    vs = versions[so]
    if any(x.startswith('GLIBC_') for x in vs):
        v = max((x[6:] for x in vs if re.match(r'GLIBC_\d', x)), key=vkey)
        reason = f'GLIBC_{v}'
    elif any(x.startswith(('GLIBCXX_', 'CXXABI_')) for x in vs):
        cands = []
        nx = [int(m.group(1)) for m in (re.match(r'GLIBCXX_3\.4\.(\d+)$', x) for x in vs) if m]
        na = [int(m.group(1)) for m in (re.match(r'CXXABI_1\.3\.(\d+)$', x) for x in vs) if m]
        if nx:
            cands.append((GLIBCXX.get(max(nx)), f'GLIBCXX_3.4.{max(nx)}'))
        if na:
            cands.append((CXXABI.get(max(na)), f'CXXABI_1.3.{max(na)}'))
        if any(c[0] is None for c in cands):
            errors.append(f'{so}: no table entry for {[c[1] for c in cands if c[0] is None]}')
            continue
        v, reason = max(cands, key=lambda c: vkey(c[0]))
    elif any(x.startswith('GCC_') for x in vs):
        top = max((x for x in vs if re.match(r'GCC_\d', x)), key=lambda x: vkey(x[4:]))
        v = '.'.join(str(n) for n in (vkey(top[4:]) + [0])[:2])
        reason = top
    elif re.match(r'libLLVM\.so\.(\d+)', so):
        v = re.match(r'libLLVM\.so\.(\d+)', so).group(1)
        reason = f'soname major {v}'
    if owner in linked:
        if v is None or vkey(linked[owner]) > vkey(v):
            v, reason = linked[owner], 'linked against ' + linked[owner] + ' (soname carries no version)'
    want(owner, v, reason)
    rows.append((so, owner, v or '-', reason, needed[so], versions[so]))

for pkg, reason in sorted(extra.items()):
    want(pkg, None, reason)
    rows.append(('-', pkg, '-', reason, set(), set()))

with open(out_tsv, 'w') as f:
    f.write('# soname\towner\tminimum\twhy\tneeded_by\tsymbol_versions\n')
    for so, owner, v, reason, files, vs in rows:
        f.write(f'{so}\t{owner}\t{v}\t{reason}\t{",".join(sorted(files))}\t{",".join(sorted(vs)) or "-"}\n')
needs = ' '.join(f'{p}>={mins[p]}' if mins[p] else p for p in sorted(mins))
with open(out_needs, 'w') as f:
    f.write(needs + '\n')
print('needs:', needs)
for e in errors:
    print('needs-derive: ERROR', e)
if errors:
    sys.exit(1)
if declared and sorted(declared) != sorted(needs.split()):
    print('needs-derive: the declared depends differ from the derived ones')
    print('  declared:', ' '.join(sorted(declared)))
    print('  derived: ', needs)
    sys.exit(1)
