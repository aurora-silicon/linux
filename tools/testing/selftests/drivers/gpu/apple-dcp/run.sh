#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only OR MIT
PYTHONDONTWRITEBYTECODE=1; export PYTHONDONTWRITEBYTECODE
exec python3 "$(dirname "$0")/run.py" "$@"
