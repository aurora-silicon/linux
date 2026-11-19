# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Chris Kearney and omarchy-mac-installer contributors
"""Power-record field helpers adapted from the MIT-licensed installer builder.

Source: omacom/omarchy-mac-installer c8b8a8a7bd0e6e4ee55f9b8c890bc8c80965ecda,
Engine/overlay/src/omarchy_mt7932_policy.py. See mt7932-power-LICENSE.
Only the public record helpers are reused; all inputs remain external.
"""

GROUPS = (
    "cck", "ofdm", "ht20", "ht40", "vht20", "vht40", "vht80", "vht160",
    "ru26", "ru52", "ru106", "ru242", "ru484", "ru996", "ru996X2",
)


GROUP_WIDTH = {
    "cck": 4, "ofdm": 8, "ht20": 24, "ht40": 24,
    "vht20": 30, "vht40": 30, "vht80": 30, "vht160": 30,
}


for _name in GROUPS[8:]:
    GROUP_WIDTH[_name] = 36


DESCRIPTORS = (
    (0x00, 0, 4), (0x04, 1, 8), (0x0D, 1, 8), (0x25, 1, 8),
    (0x3C, 2, 10), (0x5A, 2, 10), (0x78, 2, 10), (0x96, 2, 10),
    (0xB4, 2, 12), (0xD8, 2, 12), (0xFC, 2, 12), (0x120, 2, 12),
    (0x144, 2, 12), (0x168, 2, 12), (0x18C, 2, 12),
)


def power_byte(token):
    """One power-table cell as a byte. Y is 0x7f, X and x are -60, digits are one byte."""
    if token == "Y":
        return 0x7F
    if token in ("X", "x"):
        return 0xC4
    sign = 1
    body = token
    if body.startswith("-"):
        sign = -1
        body = body[1:]
    if not body.isdigit():
        return 0xC4
    return (sign * int(body)) & 0xFF


def value_image(row):
    """432 dense command bytes: each group's cells, back to back, at its own width."""
    image = bytearray([0x7F]) * 432
    offset = 0
    for name in GROUPS:
        width = GROUP_WIDTH[name]
        cells = row.get(name)
        if cells is not None:
            if len(cells) != width:
                raise ValueError(name)
            image[offset:offset + width] = bytes(power_byte(cell) for cell in cells)
        offset += width
    if offset != 432:
        raise RuntimeError("value image")
    return bytes(image)


def parsed_image(row):
    """Fifteen 36-byte slots, the parsed row after the channel byte.

    Short groups occupy the front of their slot. The bitmap and the 1SS pad
    index this layout, not the dense command image.
    """
    image = bytearray([0x7F]) * (15 * 36)
    for index, name in enumerate(GROUPS):
        cells = row.get(name)
        if cells is None:
            continue
        width = GROUP_WIDTH[name]
        if len(cells) != width:
            raise ValueError(name)
        base = index * 36
        for column, token in enumerate(cells):
            image[base + column] = power_byte(token)
    return bytes(image)


def expand_record(values):
    """121 payload bytes from the version-3 descriptor table. Channel byte is separate."""
    if len(values) != 432:
        raise ValueError("values")
    out = bytearray()
    for src, kind, stride in DESCRIPTORS:
        if kind == 0:
            out.append(values[src])
        elif kind == 1 and src == 4:
            out.extend(values[src + delta] for delta in (0, 4, 6))
        elif kind == 1 and src in (0x0D, 0x25):
            for step in range(3):
                base = src + step * stride
                out.extend(values[base + delta] for delta in (0, 4, 6))
        elif kind == 2:
            for step in range(3):
                base = src + step * stride
                out.extend(values[base + delta] for delta in (0, 3, 5))
    if len(out) != 121:
        raise RuntimeError(f"descriptor produced {len(out)}")
    return out


def channel_width_class(band, channel):
    """Width class 0 to 3 from a channel's offset in its block, or 0xff.

    Channels below 15, and channels a multiple of 4 past their block's
    start, are 0. For band 3 the block starts at 1 or 2, so its result is
    not a bandwidth.
    """
    if channel == 0:
        return 0xFF
    if band in (1, 2):
        if channel < 15:
            return 0
        base = 36 if channel < 149 else 149
    elif band == 3:
        base = 2 if channel == 2 else 1
    else:
        return 0xFF
    if base > channel:
        return 0xFF
    delta = (channel - base) & 0xFF
    if (delta & 3) == 0:
        return 0
    result = 0xFF
    if (delta & 0x1F) == 0x0E:
        result = 3
    if (delta & 0x0F) == 6:
        result = 2
    if (delta & 7) == 2:
        result = 1
    return result


def _signed(byte):
    return byte - 256 if byte >= 128 else byte


_PAD_GROUP = (4, 5, 6, 7)


_PAD_COLUMN = (10, 10, 10, 10)


def one_ss_pad(rows):
    """32-byte channel mask of the 1SS pad. Each row's `values` is a parsed image.

    A channel's bit is set when, for every width up to its own, column 0 of
    that width's group is at least 10 above column 10.
    """
    pad = bytearray(32)
    for channel, values in rows:
        bandwidth = channel_width_class(3, channel)
        if bandwidth > 3 or channel > 233:
            continue
        if all(
            _signed(values[group * 36]) - _signed(values[group * 36 + column]) >= 10
            for group, column in zip(_PAD_GROUP[:bandwidth + 1], _PAD_COLUMN)
        ):
            pad[channel >> 3] |= 1 << (channel & 7)
    return bytes(pad)


def _power_span(label):
    """chNNN, chNNN-cdd, chNNN-MMM, or chNNN-MMM-cdd."""
    if len(label) < 5 or label[:2] != "ch" or not label[2:5].isdigit():
        raise ValueError("label")
    start = int(label[2:5])
    if len(label) == 5:
        return start, start, False, "plain"
    if label[5] != "-":
        raise ValueError("label")
    if len(label) == 9 and label[6:9] == "cdd":
        return start, start, True, "bonded"
    if len(label) >= 9 and label[6:9].isdigit():
        end = int(label[6:9])
        if len(label) == 9:
            return start, end, False, "bonded"
        if label[9:] == "-cdd":
            return start, end, True, "bonded"
    raise ValueError("label")


def _put(row, group_index, offset, cells):
    name = GROUPS[group_index]
    width = GROUP_WIDTH[name]
    if offset < 0 or offset + len(cells) > width:
        raise ValueError("range")
    row[name][offset:offset + len(cells)] = list(cells)


def _bonded_destination(group_index, bw_start, bw_end, cdd):
    """Group index and column offset on the end channel. No text-group write."""
    if group_index in (4, 5, 6):
        return bw_start + 4, 0
    if group_index != 1:
        if group_index in (11, 12, 13):
            return bw_start + 11, 0
        return group_index, 0
    if bw_start == bw_end:
        return 2, 8
    if bw_start == 2 and bw_end == 1:
        return (3, 0) if cdd else (2, 16)
    if bw_start == 3 and bw_end == 1:
        return (3, 16) if cdd else (3, 8)
    if bw_start == 3 and bw_end == 2:
        return (3, 0) if cdd else (2, 16)
    return 2, (bw_start - 1) * 8
