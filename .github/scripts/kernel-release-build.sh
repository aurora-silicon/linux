#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Build the Aurora kernel release artifacts from this tree.
#
# Usage: kernel-release-build.sh <objdir> <distdir> <localversion> <pkgbase>
#
# The configuration is the fleet configuration: arch/arm64/configs/aurora_defconfig
# (savedefconfig output of the fleet .config) with arch/arm64/configs/asahi.config
# merged on top, so options added to asahi.config reach the release before
# aurora_defconfig is regenerated. Requires ARCH=arm64 LLVM=1 toolchain in PATH
# (clang/lld, rustc + rust-src, bindgen, pahole).
#
# Modules are signed with the key the build generates (certs/signing_key.pem).
# The private key is deleted at the end and never packaged.

set -euo pipefail

O=$(realpath -m "$1")
D=$(realpath -m "$2")
LV=$3
PKGBASE=$4
S=$(git rev-parse --show-toplevel)
JOBS=${JOBS:-$(nproc)}

export ARCH=arm64 LLVM=1
export KBUILD_BUILD_USER=${KBUILD_BUILD_USER:-aurora} KBUILD_BUILD_HOST=${KBUILD_BUILD_HOST:-github}
export KBUILD_BUILD_TIMESTAMP=${KBUILD_BUILD_TIMESTAMP:-$(git -C "$S" log -1 --format=%cD HEAD)}

mk() { make -C "$S" O="$O" LOCALVERSION="$LV" -j"$JOBS" "$@"; }

mkdir -p "$O" "$D"
rm -f "$O/.config"

echo "::group::toolchain"
clang --version | head -1
ld.lld --version | head -1
rustc --version
bindgen --version
pahole --version
echo "::endgroup::"

echo "::group::config"
mk aurora_defconfig asahi.config
mk savedefconfig
if cmp -s "$O/defconfig" "$S/arch/arm64/configs/aurora_defconfig"; then
	echo "config: identical to aurora_defconfig"
else
	echo "::warning::generated config differs from aurora_defconfig (asahi.config or Kconfig changed); regenerate it with savedefconfig"
	diff -u "$S/arch/arm64/configs/aurora_defconfig" "$O/defconfig" || true
fi
python3 - "$S/arch/arm64/configs/asahi.config" "$O/.config" <<'PY'
import re, sys
def load(p):
    d = {}
    for l in open(p):
        l = l.strip()
        m = re.match(r'^(CONFIG_\w+)=(.*)$', l)
        if m:
            d[m.group(1)] = m.group(2)
            continue
        m = re.match(r'^# (CONFIG_\w+) is not set$', l)
        if m:
            d[m.group(1)] = 'n'
    return d
want, have = load(sys.argv[1]), load(sys.argv[2])
bad = [(k, v, have.get(k, 'n')) for k, v in want.items() if have.get(k, 'n') != v]
print(f"asahi.config requests {len(want)}; mismatches {len(bad)}")
for k, v, h in bad:
    print(f"MISMATCH {k}: want {v} got {h}")
sys.exit(1 if bad else 0)
PY
grep -qx 'CONFIG_MODULE_SIG_KEY="certs/signing_key.pem"' "$O/.config"
echo "::endgroup::"

echo "::group::build"
mk Image.gz modules dtbs
echo "::endgroup::"

KREL=$(cat "$O/include/config/kernel.release")
M=$O/modroot
rm -rf "$M"
mk INSTALL_MOD_PATH="$M" INSTALL_MOD_STRIP=1 modules_install
rm -f "$M/lib/modules/$KREL/build" "$M/lib/modules/$KREL/source"

TAR=(tar --sort=name --mtime="$KBUILD_BUILD_TIMESTAMP" --owner=0 --group=0 --numeric-owner)
cp "$O/arch/arm64/boot/Image.gz" "$D/Image-$KREL.gz"
cp "$O/.config" "$D/config-$KREL"
cp "$O/System.map" "$D/System.map-$KREL"
"${TAR[@]}" --zstd -C "$M" -cf "$D/modules-$KREL.tar.zst" "lib/modules/$KREL"
"${TAR[@]}" --zstd -C "$O/arch/arm64/boot/dts" -cf "$D/dtbs-$KREL.tar.zst" \
	$(cd "$O/arch/arm64/boot/dts" && find . -name '*.dtb' | sort)
cp "$O/certs/signing_key.x509" "$D/module-signing-cert-$KREL.der"
git -C "$S" rev-parse HEAD > "$D/source.commit"
echo "$KREL" > "$D/kernel.release"
echo "modules: $(find "$M/lib/modules/$KREL" -name '*.ko*' | wc -l)"
echo "dtbs: $(cd "$O/arch/arm64/boot/dts" && find . -name '*.dtb' | wc -l)"

echo "::group::pacman-pkg"
if command -v makepkg >/dev/null; then
	rm -f "$O"/*.pkg.tar.*
	if mk PACMAN_PKGBASE="$PKGBASE" PACMAN_EXTRAPACKAGES=headers \
		MAKEPKGOPTS="--nodeps --noconfirm" pacman-pkg; then
		cp "$O"/*.pkg.tar.zst "$D/"
	else
		echo "::warning::make pacman-pkg failed; release ships without the Arch package"
	fi
else
	echo "::warning::makepkg not installed; skipping the Arch package"
fi
echo "::endgroup::"

# The private signing key stays on the builder for this run only.
rm -f "$O/certs/signing_key.pem"
for f in "$D"/*.tar.* ; do
	# List first: grep -q in a pipeline would end it early and, under
	# pipefail, turn a match into a pass.
	list=$(tar -tf "$f") || { echo "::error::cannot list $f"; exit 1; }
	if grep -q 'signing_key\.pem$' <<<"$list"; then
		echo "::error::$f contains the private signing key"
		exit 1
	fi
done

(cd "$D" && sha256sum $(ls | grep -v -x SHA256SUMS) > SHA256SUMS)
cat "$D/SHA256SUMS"
echo "BUILD-OK $KREL"
