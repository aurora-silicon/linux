# SPDX-License-Identifier: GPL-2.0-only
"""Extract production C functions and structures for sleep fault-injection tests."""

import re


def function(source, name):
    # Keep offsets while hiding documentation and commented-out declarations.
    code = re.sub(
        r"/\*.*?\*/|//[^\n]*",
        lambda match: re.sub(r"[^\n]", " ", match[0]),
        source,
        flags=re.S,
    )
    match = re.search(
        r"(?m)^[\w *\t]+\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", code
    )
    assert match, f"missing exact function: {name}"
    start = match.start()
    opening = match.end() - 1
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def structure(source, name):
    start = source.index("struct " + name + " {")
    end = source.index("\n};", start) + 3
    return source[start:end] + "\n"
