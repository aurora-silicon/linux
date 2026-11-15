#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Host-only tests of the production policy. Keep outputs in the caller-owned dir.
set -eu
out=${1:?usage: g17-ray-query-timeout-tests.sh OUTPUT_DIRECTORY}
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "$out"
# timeout.rs reads the driver's clock as crate::g17::now_ns(), so it builds
# as crate::g17::timeout beside a host clock, as it sits in the driver.
cat > "$out/g17-timeout-tests.rs" <<RUST
mod g17 {
    pub(crate) fn now_ns() -> u64 {
        use std::time::{SystemTime, UNIX_EPOCH};
        SystemTime::now().duration_since(UNIX_EPOCH).map_or(1, |t| t.as_nanos() as u64)
    }
    #[path = "$root/drivers/gpu/drm/asahi/g17/timeout.rs"]
    pub(crate) mod timeout;
}
RUST
"${RUSTC:-rustc}" --edition=2021 --test \
    "$out/g17-timeout-tests.rs" -o "$out/g17-timeout-tests"
"$out/g17-timeout-tests"
