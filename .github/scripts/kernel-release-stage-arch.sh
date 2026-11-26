#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Stage standalone release artifacts from the Arch runtime package.

set -euo pipefail

package=$(realpath "$1")
release=$2
system_map=$(realpath "$3")
output=$(realpath "$4")
stage=$(mktemp -d "$output/.arch-runtime.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT

member="usr/lib/modules/$release"
tar --no-same-owner --zstd -xf "$package" -C "$stage" "$member"
mkdir -p "$stage/lib/modules"
mv "$stage/$member" "$stage/lib/modules/"
runtime="$stage/lib/modules/$release"
mv "$runtime/dtb" "$stage/dtb"
python3 - "$runtime/vmlinuz" <<'PY' | gzip -n > "$output/Image-$release.gz"
import gzip
from pathlib import Path
import struct
import subprocess
import sys

image = Path(sys.argv[1]).read_bytes()
if image.startswith(b"\x1f\x8b"):
    image = gzip.decompress(image)
elif image[:2] == b"MZ" and image[4:8] == b"zimg":
    offset, length = struct.unpack_from("<II", image, 8)
    if (image[24:32].rstrip(b"\0") != b"zstd"
            or not (32 <= offset < len(image) and 0 < length <= len(image) - offset)):
        sys.exit("Invalid ARM64 EFI zboot payload")
    image = subprocess.check_output(["zstd", "-dq"], input=image[offset:offset + length])
if image[56:60] != b"ARM\x64":
    sys.exit("The Arch runtime package did not contain an ARM64 Linux Image")
sys.stdout.buffer.write(image)
PY
rm -f "$runtime/vmlinuz" "$runtime/pkgbase"
depmod -b "$stage" -F "$system_map" "$release"
TAR=(tar --sort=name --mtime="$KBUILD_BUILD_TIMESTAMP" --owner=0 --group=0 --numeric-owner)
"${TAR[@]}" --zstd -C "$stage" -cf "$output/modules-$release.tar.zst" "lib/modules/$release"
"${TAR[@]}" --zstd -C "$stage/dtb" -cf "$output/dtbs-$release.tar.zst" \
	$(cd "$stage/dtb" && find . -name '*.dtb' | sort)
