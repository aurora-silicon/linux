#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Host-only tests of the production admission state for ray-traversal commands
# (drivers/gpu/drm/asahi/g17/traversal.rs). Keep outputs in the caller-owned dir.
set -eu
out=${1:?usage: g17-traversal-admission-tests.sh OUTPUT_DIRECTORY}
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "$out"
"${RUSTC:-rustc}" --edition=2021 --test \
    "$root/drivers/gpu/drm/asahi/g17/traversal.rs" -o "$out/g17-traversal-admission-tests"
"$out/g17-traversal-admission-tests"
