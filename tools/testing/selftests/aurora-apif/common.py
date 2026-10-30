# SPDX-License-Identifier: GPL-2.0-only
"""Production-source extraction shared by the APIF host tests."""
import re

def block(source, pattern):
    match = re.search(pattern, source, re.M)
    if not match:
        raise RuntimeError(f"missing production block {pattern}")
    start = match.start()
    brace = source.index("{", match.end())
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if not depth:
                return source[start:pos + 1]
    raise RuntimeError("unterminated production block")
