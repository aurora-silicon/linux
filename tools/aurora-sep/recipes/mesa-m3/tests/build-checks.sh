#!/bin/bash
# Copyright 2026 Chris Kearney
# SPDX-License-Identifier: MIT
set -euo pipefail
src=$1
build=$2
prefix=$3
mkdir -p build-checks/tls
cc -shared -fPIC tls-pad.c -o build-checks/tls/pad.so
for ((i=0; i<512; ++i)); do cp build-checks/tls/pad.so "build-checks/tls/pad-$i.so"; done
cc -Wall -Wextra -Werror late-load.c -o build-checks/late-load -ldl -pthread
LD_LIBRARY_PATH="$prefix/lib" GLIBC_TUNABLES=glibc.rtld.optional_static_tls=0 build-checks/late-load "$PWD/build-checks/tls" "$prefix"
python3 queue-check.py "$src" "$build" build-checks/queue-sandbox
