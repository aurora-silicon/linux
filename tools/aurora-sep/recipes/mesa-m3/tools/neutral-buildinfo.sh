#!/bin/bash
# neutral-buildinfo.sh IN.pkg.tar.zst OUT.pkg.tar.zst
#
# The same package with .BUILDINFO's builddir and startdir set to /build (the
# lab builder's paths are recorded there by makepkg, which keeps startdir
# read-only during the build), and .MTREE regenerated the way makepkg does.
# Every other member keeps its bytes, mode, owner and mtime; the script checks
# that and prints both sha256s.
set -euo pipefail
in=$(realpath "$1")
out=$(realpath -m "$2")
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
mkdir "$w/x"
fakeroot -- bash -euo pipefail -c '
	cd "$1"
	bsdtar -xpf "$2"
	touch -r .BUILDINFO ../buildinfo.time
	touch -r .MTREE ../mtree.time
	sed -i -e "s|^builddir = .*|builddir = /build|" -e "s|^startdir = .*|startdir = /build|" .BUILDINFO
	touch -r ../buildinfo.time .BUILDINFO
	list() { (find . -path "./.*" ! -name . ! -name .MTREE; find . ! -path "./.*" ! -name . | LC_ALL=C sort) |
		sed -e "s|^\./||" | tr "\n" "\0"; }
	list | LANG=C bsdtar -cnf - --format=mtree \
		--options="!all,use-set,type,uid,gid,mode,time,size,md5,sha256,link" --null --files-from - |
		gzip -c -f -n > .MTREE
	touch -r ../mtree.time .MTREE
	{ printf ".MTREE\0"; list; } | LANG=C bsdtar --no-fflags -cnf - --null --files-from - | zstd -q -c -T0 -19 > "$3"
' _ "$w/x" "$in" "$out"
# Every member except .BUILDINFO and .MTREE is identical; those two differ only as intended.
python3 - "$in" "$out" <<'PY'
import io, subprocess, sys, tarfile
def members(p):
    t = tarfile.open(fileobj=io.BytesIO(subprocess.run(['zstd', '-dc', p], capture_output=True, check=True).stdout))
    return {m.name: (m.type, m.mode, m.uid, m.gid, int(m.mtime), m.linkname,
                     t.extractfile(m).read() if m.isfile() else b'') for m in t.getmembers()}
a, b = members(sys.argv[1]), members(sys.argv[2])
assert set(a) == set(b), set(a) ^ set(b)
for k in a:
    if k in ('.BUILDINFO', '.MTREE'):
        continue
    assert a[k] == b[k], k
old = a['.BUILDINFO'][6].decode().splitlines(); new = b['.BUILDINFO'][6].decode().splitlines()
changed = [(x, y) for x, y in zip(old, new) if x != y]
assert len(old) == len(new) and all(x.split(' = ')[0] in ('builddir', 'startdir') for x, _ in changed), changed
for x, y in changed:
    print(f'  {x}  ->  {y}')
print(f'  {len(a) - 2} other members identical')
PY
sha256sum "$in" "$out"
