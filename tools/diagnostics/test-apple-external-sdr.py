#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Parse real-format TimingElements blobs with the production SDR selection code."""
import hashlib
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
base = root / 'drivers/gpu/drm/apple'
source = (base / 'parser.c').read_text()
header = (base / 'parser.h').read_text()
body = source[source.index('#define DCP_PARSE_HEADER'):source.index('int parse_display_attributes(')]
header = header[header.index('struct apple_dcp;'):header.index('int parse(')]
# Pin the integration scope: the template passes its explicit external profile,
# independent of the existing internal-panel boolean.
template = (base / 'iomfb_template.c').read_text()
call = re.search(r'enumerate_modes\(&ctx,.*?\);', template, re.S).group()
assert re.search(r'dcp->hw\.iomfb_method_profile\s*==\s*DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL\)', call)
print('Production mode parser SHA256:', hashlib.sha256((header + body).encode()).hexdigest())


class Pairs(list):
    """A wire dictionary which preserves duplicate keys for rejection tests."""


def wire(value, out):
    out += b'\0' * (-len(out) % 4)
    if isinstance(value, bool):
        out += struct.pack('<I', (11 << 24) | value)
    elif isinstance(value, int):
        out += struct.pack('<Iq', (4 << 24) | 8, value)
    elif isinstance(value, str):
        raw = value.encode()
        out += struct.pack('<I', (9 << 24) | len(raw)) + raw
    elif isinstance(value, bytes):
        out += struct.pack('<I', (10 << 24) | len(value)) + value
    elif isinstance(value, Pairs):
        out += struct.pack('<I', (1 << 24) | len(value))
        for key, child in value:
            wire(key, out)
            wire(child, out)
    elif isinstance(value, list):
        out += struct.pack('<I', (2 << 24) | len(value))
        for child in value:
            wire(child, out)
    elif isinstance(value, dict):
        out += struct.pack('<I', (1 << 24) | len(value))
        for key, child in value.items():
            wire(key, out)
            wire(child, out)
    else:
        raise TypeError(value)


def color(identity, score, **changes):
    result = dict(Colorimetry=10, Depth=8, DynamicRange=0, EOTF=0,
                  ID=identity, PixelEncoding=0, Score=score, IsVirtual=False)
    result.update(changes)
    return result


def timing(colors, rate=60, identity=203, score=10):
    return dict(HorizontalAttributes=dict(Active=1920, Total=2200, FrontPorch=88,
                                         SyncWidth=44, PreciseSyncRate=rate << 16),
                VerticalAttributes=dict(Active=1080, Total=1125, FrontPorch=4,
                                       SyncWidth=5, PreciseSyncRate=rate << 16),
                ColorModes=colors, ID=identity, Score=score, IsVirtual=False)


cases = []

def case(name, timings, external=True, count=1, identity=9, depth=8, clock=148500,
         diagnostic=False, diagnostic_selected=False, preferred_index=0):
    out = bytearray(struct.pack('<I', 0xd3))
    wire(timings, out)
    cases.append((name, bytes(out), external, count, identity, depth, clock,
                  diagnostic, diagnostic_selected, preferred_index))


wide = color(84, 1000, Colorimetry=9, Depth=10)
srgb = color(9, 10)
case('external ignores high-score BT2020', [timing([wide, srgb])])
case('legacy preserves high-score BT2020', [timing([wide, srgb])], False, identity=84, depth=10)
case('internal preserves high-score BT2020', [timing([wide, srgb])], False, identity=84, depth=10)
case('explicit gamut before default RGB', [timing([color(4, 99999, Colorimetry=16), srgb])])
case('default RGB does not displace explicit', [timing([srgb, color(4, 99999, Colorimetry=16)])])
case('firmware score among suitable gamuts', [timing([srgb, color(12, 11, Colorimetry=1, Depth=10)])], identity=12, depth=10)
case('default fallback retains firmware ID', [timing([color(123456, 4, Colorimetry=16)])], identity=123456)
case('ties preserve firmware order', [timing([srgb, color(12, 10, Colorimetry=1)])])
case('360Hz is not filtered by color policy', [timing([wide, srgb], rate=360)], clock=891000)
case('VRR-compatible high refresh color', [dict(timing([srgb], rate=240), MinimumVariableRefreshRate=48 << 16, MaximumVariableRefreshRate=240 << 16)], clock=594000)
for name, changes in [
    ('BT2020 RGB', dict(Colorimetry=9)), ('BT2020 YCC', dict(Colorimetry=7)),
    ('BT2020 cYCC', dict(Colorimetry=6)), ('P3 D65', dict(Colorimetry=14)),
    ('P3 theater', dict(Colorimetry=15)), ('scRGB', dict(Colorimetry=11)),
    ('scRGB fixed', dict(Colorimetry=12)), ('AdobeRGB', dict(Colorimetry=13)),
    ('unknown gamut', dict(Colorimetry=17)), ('missing gamut', dict(Colorimetry=-1)),
    ('HDR gamma', dict(EOTF=1)), ('PQ', dict(EOTF=2)), ('HLG', dict(EOTF=3)),
    ('YCbCr444', dict(PixelEncoding=2)), ('YCbCr420', dict(PixelEncoding=1)),
    ('limited range', dict(DynamicRange=1)), ('unknown range', dict(DynamicRange=2)),
    ('12-bit', dict(Depth=12)), ('9-bit', dict(Depth=9)), ('7-bit', dict(Depth=7)),
    ('out-of-range ID', dict(ID=1 << 32)), ('virtual color', dict(IsVirtual=True)),
]:
    bad = color(99, 99999, **changes)
    case('reject ' + name + ' beside SDR', [timing([bad, srgb])])
    case('reject sole ' + name, [timing([bad])], count=0)
case('legacy allows sole wide gamut', [timing([wide])], False, identity=84, depth=10)
case('legacy range ranking unchanged', [timing([color(77, 100, DynamicRange=1), srgb])], False, identity=77)
for key, invalid in [('Colorimetry', 'sRGB'), ('Depth', True), ('DynamicRange', b'\0'), ('EOTF', '0')]:
    malformed = color(99, 99999, **{key: invalid})
    case('wrong type ' + key, [timing([malformed])], count=0)
    case('recover after wrong type ' + key, [timing([malformed]), timing([srgb])])
case('skip unsuitable timing without poisoning later mode', [timing([wide]), timing([srgb], rate=120)], clock=297000)

# Per-timing restriction shapes and IDs reflect the public runtime schema;
# pixel data and device-specific property blobs are not included.
color80 = color(80, 242999)
color85 = color(85, 442999, Depth=10)


def restricted(rate=60, unsafe=None, dsc=None, colors=None, before=False):
    item = timing(colors if colors is not None else [color80, color85], rate=rate)
    limits = {}
    if unsafe is not None:
        limits['UnsafeColorElementIDs'] = unsafe
    if dsc is not None:
        limits['DSCRequiredColorElementIDs'] = dsc
    return {**limits, **item} if before else {**item, **limits}


case('R30 60Hz keeps safe 10bit', [restricted(60, [], [])], identity=85, depth=10)
case('R30 120Hz rejects unsafe 10bit', [restricted(120, [85], [])], identity=80, clock=297000)
case('R30 144Hz no eligible color', [restricted(144, [85], [], [color85])], count=0)
case('R30 180Hz all colors unsafe', [restricted(180, [80, 85], [])], count=0)
case('DSC required rejects higher score', [restricted(60, [], [85])], identity=80)
case('DSC required all colors rejected', [restricted(60, [], [80, 85])], count=0)
case('unsafe plus DSC union', [restricted(60, [80], [85])], count=0)
case('swapped unsafe plus DSC union', [restricted(60, [85], [80])], count=0)
case('safe no lists', [restricted()], identity=85, depth=10)
case('no promoted blanket', [dict(restricted(240, [85], []), IsPromoted=False)], identity=80, clock=594000)
for before in (False, True):
    case('restriction order ' + str(before), [restricted(120, [85], [], before=before)], identity=80, clock=297000)
for field in ('UnsafeColorElementIDs', 'DSCRequiredColorElementIDs'):
    for label, invalid in [('string', '85'), ('integer', 85), ('bool', True),
                           ('blob', b'\x55'), ('dictionary', {'ID': 85}),
                           ('negative', [-1]), ('u32 overflow', [1 << 32]),
                           ('mixed string', [85, '80']), ('mixed bool', [True]),
                           ('duplicate ID', [85, 85]), ('over limit', list(range(65)))]:
        item = restricted()
        item[field] = invalid
        case('malformed ' + field + ' ' + label, [item], count=0)
        # Neither legacy nor internal modes interpret these new restrictions.
        case('legacy ignores ' + field + ' ' + label, [item], False, identity=85, depth=10)
        case('internal ignores ' + field + ' ' + label, [item], False, identity=85, depth=10)
    duplicate = Pairs(list(restricted().items()) + [(field, []), (field, [85])])
    case('duplicate field ' + field, [duplicate], count=0)
    case('legacy duplicate field ' + field, [duplicate], False, identity=85, depth=10)
    item = restricted()
    item[field] = list(range(1000, 1064))
    case('exact limit ' + field, [item], identity=85, depth=10)
    item[field] = [0xffffffff]
    case('u32 max accepted ' + field, [item], identity=85, depth=10)
duplicate_colors = Pairs(list(restricted().items()) + [('ColorModes', [color80])])
case('duplicate ColorModes fail closed', [duplicate_colors], count=0)
case('bad restriction skips only its timing', [restricted(180, [80, 85], []), restricted(60, [], [])], identity=85, depth=10)

# Diagnostic-only retry: no private monitor identity or captured blob is embedded.
# The binary dictionaries still go through the complete production parser.
def trial_timing(colors=None, rate=180):
    item = restricted(rate, [80, 85], [], colors)
    item['IsInterlaced'] = False
    item['HorizontalAttributes']['Total'] = 2080
    item['VerticalAttributes'].update(Total=1106, PreciseSyncRate=11796583)
    for axis in ('HorizontalAttributes', 'VerticalAttributes'):
        item[axis].update(PixelRepetition=0, SyncPolarity=1)
    return item


def trial_case(name, item=None, **kwargs):
    defaults = dict(identity=80, clock=414090, diagnostic=True,
                    diagnostic_selected=True, preferred_index=-1)
    defaults.update(kwargs)
    case('diagnostic ' + name, [item if item is not None else trial_timing()], **defaults)

trial_case('explicit enable exposes only RGB8')
trial_case('default remains refused', diagnostic=False, count=0)
trial_case('legacy keeps existing 10bit selection', external=False, identity=85,
           depth=10, diagnostic_selected=False, preferred_index=0)
case('internal diagnostic flag cannot admit unsafe timing', [trial_timing()],
     diagnostic=True, count=0)
item = trial_timing(); item['UnsafeColorElementIDs'] = [80]
trial_case('existing safe candidate is not replaced', item, identity=85, depth=10,
           diagnostic_selected=False, preferred_index=0)
item = trial_timing(); item['DSCRequiredColorElementIDs'] = [80]
trial_case('DSC remains forbidden', item, count=0)
# No timing-local 8-bit entry: a global/sibling timing cannot provide one.
trial_case('10bit-only timing remains refused', trial_timing([color85]), count=0)
for label, changes in [
    ('limited range', dict(DynamicRange=1)), ('HDR', dict(EOTF=2)),
    ('YUV', dict(PixelEncoding=2)), ('wide gamut', dict(Colorimetry=9)),
    ('12bit', dict(Depth=12)), ('virtual color', dict(IsVirtual=True)),
    ('negative score', dict(Score=-1)), ('invalid ID', dict(ID=1 << 32)),
]:
    trial_case(label, trial_timing([color(80, 242999, **changes)]), count=0)
for axis, key, value in [
    ('HorizontalAttributes', 'Active', 1919),
    ('VerticalAttributes', 'Active', 1079),
    ('HorizontalAttributes', 'Total', 100),
    ('VerticalAttributes', 'PreciseSyncRate', 11796582),
    ('VerticalAttributes', 'PreciseSyncRate', 180 << 16),
    ('HorizontalAttributes', 'Total', 2200),
    ('VerticalAttributes', 'Total', 1125),
    ('VerticalAttributes', 'PreciseSyncRate', 181 << 16),
    ('HorizontalAttributes', 'PixelRepetition', 1),
    ('VerticalAttributes', 'PixelRepetition', 1),
    ('HorizontalAttributes', 'SyncPolarity', 2),
]:
    item = trial_timing(); item[axis][key] = value
    trial_case(axis + key + str(value), item, count=0)
for key in ('IsVirtual', 'IsInterlaced'):
    item = trial_timing(); item.pop(key)
    trial_case('missing ' + key, item, count=0)
    item = trial_timing(); item[key] = True
    trial_case('true ' + key, item, count=0)
for axis in ('HorizontalAttributes', 'VerticalAttributes'):
    for key in ('PixelRepetition', 'SyncPolarity'):
        item = trial_timing(); item[axis].pop(key)
        trial_case('missing ' + axis + key, item, count=0)
for key in ('HorizontalAttributes', 'VerticalAttributes', 'IsVirtual'):
    item = trial_timing()
    duplicate = Pairs(list(item.items()) + [(key, item[key])])
    trial_case('duplicate scope ' + key, duplicate, count=0)
for key in ('UnsafeColorElementIDs', 'DSCRequiredColorElementIDs'):
    item = trial_timing(); item[key] = [80, 80]
    trial_case('duplicate restriction ' + key, item, count=0)
# All key orders must preserve the same selection and non-preferred status.
item = trial_timing()
trial_case('reversed dictionary order', dict(reversed(list(item.items()))))
item = trial_timing(); item['IsPreferred'] = True
trial_case('firmware preferred does not promote trial', item)
# A high firmware score must not make the trial the desktop default.
item = trial_timing(); item['Score'] = 999999
case('diagnostic trial cannot displace normal preferred mode', [item, restricted(60, [], [])],
     count=2, identity=80, depth=8, clock=414090, diagnostic=True,
     diagnostic_selected=True, preferred_index=1)
# No shared candidate pool: successful sibling timing does not rescue an unsafe-only 10bit mode.
case('diagnostic candidates remain timing-local', [trial_timing([color85]), restricted(60, [], [])],
     identity=85, depth=10, diagnostic=True)

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
#define __packed __attribute__((packed))
#define IS_ENABLED(x) 0
#define U32_MAX UINT32_MAX
#define U16_MAX UINT16_MAX
#define S64_MAX INT64_MAX
#define S64_MIN INT64_MIN
#define round_up(n, a) (((n) + (a) - 1) & ~((a) - 1))
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define GFP_KERNEL 0
#define kmalloc(n, f) malloc(n)
#define kcalloc(n, s, f) calloc(n,s)
#define kfree(p) free(p)
#define mul_u32_u32(a, b) ((u64)(u32)(a) * (u32)(b))
#define DIV_ROUND_CLOSEST_ULL(n, d) (((n) + (d) / 2) / (d))
#define trace_iomfb_color_mode(...) ((void)0)
#define trace_iomfb_parse_mode_fail(...) ((void)0)
#define trace_iomfb_parse_mode_success(...) ((void)0)
#define trace_iomfb_timing_mode(...) ((void)0)
#define DRM_MODE_FLAG_PHSYNC 1
#define DRM_MODE_FLAG_NHSYNC 2
#define DRM_MODE_FLAG_PVSYNC 4
#define DRM_MODE_FLAG_NVSYNC 8
#define DRM_MODE_TYPE_DRIVER 1
#define DRM_MODE_TYPE_PREFERRED 2
struct drm_display_mode {
    unsigned int type, flags;
    int clock, vdisplay, vsync_start, vsync_end, vtotal;
    int hdisplay, hsync_start, hsync_end, htotal, width_mm, height_mm;
};
static void drm_mode_set_name(struct drm_display_mode *mode) { (void)mode; }
'''
data = '\n'.join('static const unsigned char blob_%u[] = {%s};' %
                 (i, ','.join(map(str, case[1]))) for i, case in enumerate(cases))
suffix = r'''
int main(void)
{
'''
for i, (name, blob, external, count, identity, depth, clock, diagnostic, diagnostic_selected, preferred_index) in enumerate(cases):
    suffix += f'''
    {{
        struct dcp_parse_ctx ctx = {{ .diagnostic_1080p180_rgb8 = true }};
        unsigned int count = 99;
        assert(parse(blob_{i}, sizeof(blob_{i}), &ctx) == 0);
        assert(!ctx.diagnostic_1080p180_rgb8);
        ctx.diagnostic_1080p180_rgb8 = {str(diagnostic).lower()};
        struct dcp_display_mode *modes = enumerate_modes(&ctx, &count, 600, 340, 0,
                                                        {str(name.startswith('internal')).lower()}, {str(external).lower()});
        if (IS_ERR(modes) || count != {count}) {{
            fprintf(stderr, "case {i}: {name} count=%u\\n", count);
            abort();
        }}
'''
    if count:
        suffix += f'''
        assert(modes[0].color_mode_id == {identity});
        assert(modes[0].sdr_rgb.depth == {depth});
        assert(modes[0].mode.clock == {clock});
        assert(modes[0].unsafe_rgb8_diagnostic == {str(diagnostic_selected).lower()});
        assert(!modes[0].unsafe_rgb8_diagnostic || !modes[0].metadata_timing_valid);
        for (unsigned int mode = 0; mode < count; mode++)
            assert(!!(modes[mode].mode.type & DRM_MODE_TYPE_PREFERRED) == ((int)mode == {preferred_index}));
'''
    suffix += '        free(modes);\n    }\n'
suffix += f'    puts("PASS: {len(cases)} production binary-parser SDR, malformed-input, legacy and high-refresh cases");\n    return 0;\n}}\n'
with tempfile.TemporaryDirectory(prefix='apple-external-sdr-') as directory:
    directory = Path(directory)
    unit = directory / 'test.c'
    unit.write_text(prefix + header + body + data + suffix)
    command = shlex.split(os.environ.get('CC', 'clang'))
    command += ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command + [str(unit), '-o', str(directory / 'test')], check=True)
    subprocess.run([str(directory / 'test')], check=True)
