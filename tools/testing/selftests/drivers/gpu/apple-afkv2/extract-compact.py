#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the production compact slot allocator across DCP power cycles.

Poweroff leaves compact services enabled; poweron announces fresh interfaces
without teardown. Synchronization, debugfs and the owner teardown are mocked.
"""
import hashlib
from pathlib import Path
import re
import sys

source = Path(sys.argv[1]).read_text()
start = source.index("static struct apple_epic_service *\nafk_compact_slot(")
end = source.index("\nstatic void afk_recv_handle_compact_init(", start)
body = source[start:end]
header = Path(sys.argv[1]).with_name("afk.h").read_text()
limit = re.search(r"^#define AFK_MAX_CHANNEL\s+(\d+)$", header, re.M).group(1)
directory = Path(__file__).resolve().parent
fixture = (directory / "compact.c").read_text()
assert fixture.count("/* PRODUCTION_SLOT */") == 1
Path(sys.argv[2]).write_text("#define AFK_MAX_CHANNEL " + limit + "\n" +
                           fixture.replace("/* PRODUCTION_SLOT */", body))
print("Production compact slot SHA256:", hashlib.sha256(body.encode()).hexdigest())
