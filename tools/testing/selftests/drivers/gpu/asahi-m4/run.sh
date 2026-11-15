#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only OR MIT
exec python3 "$(dirname "$0")/run-host-tests.py" "$@"
