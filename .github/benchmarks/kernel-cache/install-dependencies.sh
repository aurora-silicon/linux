#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
df -B1 /
test "$(df -B1 --output=avail / | tail -1)" -ge 45097156608
apt-get update
apt-get -y --no-install-recommends install \
  build-essential git ca-certificates curl clang-19 llvm-19 lld-19 libclang-19-dev \
  rustc rust-src rustfmt cargo bindgen libssl-dev libelf-dev bison flex bc dwarves \
  ccache python3 time xz-utils zstd patch file
