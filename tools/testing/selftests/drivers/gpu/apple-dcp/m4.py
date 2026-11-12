# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""M4 request compatibility and shared appledrm integration regressions.

The immutable request fixture is generated from the previous working C driver,
not the implementation under test. Transport, MMIO and DRM core are not emulated.
"""
import base64
import copy
import hashlib
import json
import struct
import subprocess
import zlib
from pathlib import Path

from common import block, run_unit

HERE = Path(__file__).resolve().parent
APPLE = Path('drivers/gpu/drm/apple')
HEADERS = ('dcp-m4-profile.h', 'dcp-m4-frame.h', 'dcp-m4-protocol.h')

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define U64_MAX UINT64_MAX
#define BIT(n) (1U << (n))
static inline void put_unaligned_le16(u16 x, void *p) {
    u8 *b = p; b[0] = x; b[1] = x >> 8;
}
static inline void put_unaligned_le32(u32 x, void *p) {
    u8 *b = p; for (unsigned i = 0; i < 4; i++) b[i] = x >> (8 * i);
}
static inline void put_unaligned_le64(u64 x, void *p) {
    u8 *b = p; for (unsigned i = 0; i < 8; i++) b[i] = x >> (8 * i);
}
static inline u32 get_unaligned_le32(const void *p) {
    const u8 *b = p; return (u32)b[0] | ((u32)b[1] << 8) |
        ((u32)b[2] << 16) | ((u32)b[3] << 24);
}
static inline u32 get_unaligned_be32(const void *p) {
    const u8 *b = p; return (u32)b[3] | ((u32)b[2] << 8) |
        ((u32)b[1] << 16) | ((u32)b[0] << 24);
}
'''


def header_body(source):
    return '\n'.join(line for line in source.splitlines()
                     if not line.startswith('#include ')) + '\n'


def production(root):
    return (PREFIX + ''.join(header_body((root / APPLE / p).read_text()) for p in HEADERS) +
            'const struct dcp_m4_profile *dcp_m4_pf = &dcp_m4_profile_j713;\n')


def request_golden(root, output):
    fixture = json.loads((HERE / 'm4-fixtures/wire-golden.json').read_text())
    expected = zlib.decompress(base64.b64decode(fixture['zlib_base64']))
    assert len(expected) == fixture['bytes']
    assert hashlib.sha256(expected).hexdigest() == fixture['sha256']
    # Deliberate common-DRM policy change: logical blanking is black. The old
    # lab driver used a purple diagnostic background. Keep its fixture intact
    # and allow only these two documented four-byte field replacements.
    expected = bytearray(expected)
    for case in (2, 9):
        offset = case * 0x1bd8 + 0x154
        assert expected[offset:offset + 4] == bytes.fromhex('602040ff')
        expected[offset:offset + 4] = bytes.fromhex('000000ff')
    cases = (HERE / 'm4-fixtures/wire-cases.c').read_text()
    assert hashlib.sha256(cases.encode()).hexdigest() == fixture['cases_sha256']
    actual_path = output / 'm4-requests.bin'
    unit = production(root) + cases
    digest = run_unit('m4-wire-golden', unit, output, ((str(actual_path),),),
                      extra=('-Wno-unused-function', '-Wno-unused-const-variable'))
    actual = actual_path.read_bytes()
    assert len(actual) == len(expected), (len(actual), len(expected))
    if actual != expected:
        first = next(i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1])
        raise AssertionError('M4 request wire drift at byte %#x: got %#x expected %#x' %
                             (first, actual[first], expected[first]))
    # The fixture covers primary XRGB/ARGB, blanking, 0/120/500 nits,
    # primary+cursor, cursor-only, cursor removal, and blanking with a cursor.
    return digest, len(expected)


def golden_check(root, output):
    digest, size = request_golden(root, output)
    (output / 'm4-wire-golden.json').write_text(json.dumps({
        'passed': True, 'cases': 10, 'bytes': size,
        'translation_unit_sha256': digest, 'hardware_access': False,
        'fixture_sha256': hashlib.sha256((HERE / 'm4-fixtures/wire-golden.json').read_bytes()).hexdigest(),
        'intentional_compatibility_changes': ['Opaque black background for two blank requests, field 0x154 only'],
    }, indent=2) + '\n')


def protocol(root):
    return production(root) + r'''
static void completion(void)
{
    u8 reply[12] = {0};
    for (unsigned order = 0; order < 2; order++) {
        struct dcp_m4_swap_state swap = {0};
        assert(!dcp_m4_swap_begin(&swap, 17));
        assert(!dcp_m4_swap_take_completion(&swap));
        if (order) assert(!dcp_m4_swap_record_reply(&swap, reply, sizeof(reply)));
        else assert(!dcp_m4_swap_record_done(&swap, 17));
        assert(!dcp_m4_swap_take_completion(&swap));
        if (order) assert(!dcp_m4_swap_record_done(&swap, 17));
        else assert(!dcp_m4_swap_record_reply(&swap, reply, sizeof(reply)));
        assert(dcp_m4_swap_take_completion(&swap) == 1);
        assert(!dcp_m4_swap_take_completion(&swap));
        assert(!dcp_m4_swap_begin(&swap, 18));
        assert(dcp_m4_swap_record_done(&swap, 17) == -EPROTO);
        assert(dcp_m4_swap_record_reply(&swap, reply, sizeof(reply)) == -EPROTO);
        assert(dcp_m4_swap_take_completion(&swap) == -EPROTO);
        assert(dcp_m4_swap_begin(&swap, 19) == -EPROTO);
    }
    struct dcp_m4_swap_state busy = {0};
    assert(!dcp_m4_swap_begin(&busy, 1));
    assert(dcp_m4_swap_begin(&busy, 2) == -EBUSY);
    assert(dcp_m4_swap_take_completion(&busy) == -EBUSY);
    struct dcp_m4_swap_state duplicate = {0};
    assert(!dcp_m4_swap_begin(&duplicate, 1));
    assert(!dcp_m4_swap_record_reply(&duplicate, reply, sizeof(reply)));
    assert(dcp_m4_swap_record_reply(&duplicate, reply, sizeof(reply)) == -EPROTO);
    struct dcp_m4_swap_state malformed = {0};
    assert(!dcp_m4_swap_begin(&malformed, 1));
    reply[5] = 1;
    assert(dcp_m4_swap_record_reply(&malformed, reply, sizeof(reply)) == -EPROTO);
    assert(!dcp_m4_swap_submit_reply((u8[12]){0}, 12));
    assert(dcp_m4_swap_submit_reply(reply, 11) == -EMSGSIZE);
    puts("PASS two independent swap receipts, exact ID, one completion and sticky failures");
}

static void routes(void)
{
    struct dcp_m4_packet packet;
    u64 word, ack;
    u16 length, offset;
    assert(!dcp_m4_init_word(0x12340000, &word));
    assert(word == 0x123400000040ULL);
    assert(dcp_m4_init_word(0, &word) == -EINVAL);
    assert(dcp_m4_init_word(0x12340001, &word) == -EINVAL);
    word = (64ULL << 48) | ((u64)DCP_M4_READY_HASH << 16) | 0x101;
    assert(dcp_m4_ready(word));
    assert(dcp_m4_ready_notification(word, true, 0) == 1);
    assert(dcp_m4_ready_notification(word, false, 0) == -EPROTO);
    assert(dcp_m4_ready_notification(word, true, word) == -EPROTO);
    assert(!dcp_m4_ready(word ^ (1ULL << 16)));
    assert(!dcp_m4_ready_notification(0, false, 0));
    for (unsigned remote = 0; remote < 2; remote++) {
        u64 route = 2 | ((u64)remote << 8);
        assert(!dcp_m4_decode_word(route | (64ULL << 16) | (128ULL << 32), &packet));
        assert(packet.offset == 64 && packet.length == 128 && packet.remote == remote);
        assert(dcp_m4_packet_offset(&packet, false) == (remote ? 0x20040 : 0x40));
        assert(dcp_m4_packet_offset(&packet, true) == (remote ? 0x40040 : 0x60040));
        assert(!dcp_m4_callback_ack(&packet, &ack));
        assert(ack == (0x42U | (remote << 8)));
        assert(!dcp_m4_nested_offset(&packet, remote, 12, &offset) && offset == 192);
        assert(!dcp_m4_nested_offset(&packet, !remote, 12, &offset) && !offset);
        assert(!dcp_m4_decode_word(route | 0x40, &packet));
        assert(packet.reply && dcp_m4_packet_offset(&packet, false) == -EINVAL);
        assert(dcp_m4_decode_word(route | 0x40 | (64ULL << 16), &packet) == -EPROTO);
        assert(dcp_m4_decode_word(route | (1ULL << 16) | (12ULL << 32), &packet) == -EMSGSIZE);
        assert(dcp_m4_decode_word(route | (0x7fc0ULL << 16) | (65ULL << 32), &packet) == -EMSGSIZE);
    }
    assert(dcp_m4_decode_word(2 | (1ULL << 48) | (12ULL << 32), &packet) == -EPROTO);
    assert(dcp_m4_decode_word(0x202 | (12ULL << 32), &packet) == -EPROTO);
    assert(!dcp_m4_decode_word(0x302 | (12ULL << 32), &packet));
    assert(!dcp_m4_callback_ack(&packet, &ack) && ack == 0x342);
    assert(!dcp_m4_packet_size(0, 0, &length) && length == 12);
    assert(!dcp_m4_packet_size(0x7ff4, 0, &length) && length == 0x8000);
    assert(dcp_m4_packet_size(0x7ff4, 1, &length) == -EMSGSIZE);
    assert(dcp_m4_packet_size(UINT32_MAX, 0, &length) == -EMSGSIZE);
    u8 bytes[32] = {'7','5','4','A'};
    packet = (struct dcp_m4_packet){.length=32};
    put_unaligned_le32(8, bytes + 4); put_unaligned_le32(12, bytes + 8);
    assert(!dcp_m4_decode_packet(bytes, sizeof(bytes), &packet));
    assert(packet.tag == 0x37353441 && packet.input_size == 8 && packet.output_size == 12);
    put_unaligned_le32(13, bytes + 8);
    assert(dcp_m4_decode_packet(bytes, sizeof(bytes), &packet) == -EMSGSIZE);
    put_unaligned_le32(12, bytes + 8); bytes[3] = 'X';
    assert(dcp_m4_decode_packet(bytes, sizeof(bytes), &packet) == -EPROTO);
    for (unsigned stream = 0; stream < 4; stream++) {
        word = 0x302 | ((u64)stream << 10) | (12ULL << 32);
        assert(!dcp_m4_decode_word_ext(word, &packet));
        assert(dcp_m4_packet_offset(&packet, true) == (int)(0x40000 + stream * 0x8000));
        assert(!dcp_m4_callback_ack(&packet, &ack));
        assert(ack == (0x342U | (stream << 10)));
    }
    /* Out of band: stream 1 for the AP packet, its reply and its callbacks. */
    assert(dcp_m4_decode_word(0x402 | (12ULL << 32), &packet) == -EPROTO);
    assert(!dcp_m4_decode_word_oob(0x402 | (64ULL << 16) | (36ULL << 32), &packet));
    assert(packet.stream == 1 && packet.offset == 64 && !packet.reply);
    assert(dcp_m4_packet_offset(&packet, false) == 0x8040);
    assert(dcp_m4_packet_offset(&packet, true) == 0x68040);
    assert(!dcp_m4_callback_ack(&packet, &ack) && ack == 0x442);
    assert(!dcp_m4_nested_offset(&packet, false, 12, &offset) && offset == 128);
    assert(!dcp_m4_decode_word_oob(0x442, &packet) && packet.reply && packet.stream == 1);
    assert(!dcp_m4_decode_word_oob(2 | (12ULL << 32), &packet) && !packet.stream);
    assert(dcp_m4_decode_word_oob(0x802 | (12ULL << 32), &packet) == -EPROTO);
    assert(dcp_m4_decode_word_oob(0xc02 | (12ULL << 32), &packet) == -EPROTO);
    packet = (struct dcp_m4_packet){.stream = 2, .length = 12};
    assert(dcp_m4_packet_offset(&packet, false) == -EINVAL);
    puts("PASS READY identity, bounded packet parsing, nested offset and callback routing");
}

static void surfaces(void)
{
    u8 bytes[0x1bd8], unchanged[0x1bd8];
    struct dcp_m4_frame frame = {
        .dva=0x10000004000ULL, .backing_size=0x4000000,
        .width=2560, .height=1664, .stride=10240, .format=DCP_M4_XRGB8888,
    };
    memset(bytes, 0xa5, sizeof(bytes)); memcpy(unchanged, bytes, sizeof(bytes));
    assert(dcp_m4_swap_encode(bytes, sizeof(bytes)-1, 1, &frame, false, 0) == -EINVAL);
    assert(!memcmp(bytes, unchanged, sizeof(bytes)));
    assert(dcp_m4_swap_encode(bytes, sizeof(bytes), 0, &frame, false, 0) == -EINVAL);
    assert(!memcmp(bytes, unchanged, sizeof(bytes)));
    assert(dcp_m4_swap_encode(bytes, sizeof(bytes), 1, &frame, true, 501) == -EINVAL);
    assert(!memcmp(bytes, unchanged, sizeof(bytes)));
    frame.dva++; assert(dcp_m4_frame_check(&frame) == -EINVAL); frame.dva--;
    frame.stride++; assert(dcp_m4_frame_check(&frame) == -EINVAL); frame.stride--;
    frame.backing_size=10240*1664-1; assert(dcp_m4_frame_check(&frame) == -EINVAL);
    frame.backing_size=0x4000000;
    frame.width--; assert(dcp_m4_frame_check(&frame) == -EINVAL); frame.width++;
    assert(!dcp_m4_frame_check(&frame));
    u64 start=0, length=0;
    assert(!dcp_m4_extent_add(&start, &length, DCP_M4_SCANOUT_BASE, 0x4000));
    assert(!dcp_m4_extent_add(&start, &length, DCP_M4_SCANOUT_BASE+0x4000, 0x4000));
    assert(!dcp_m4_direct_extent(start, length, 0x8000));
    assert(dcp_m4_extent_add(&start, &length, DCP_M4_SCANOUT_BASE+0xc000, 0x4000) == -ERANGE);
    assert(dcp_m4_direct_extent(DCP_M4_SCANOUT_BASE-0x4000, 0x8000, 1) == -ERANGE);
    assert(dcp_m4_direct_extent(DCP_M4_SCANOUT_BASE+DCP_M4_SCANOUT_SIZE-0x4000,
                                  0x8000, 1) == -ERANGE);
    assert(dcp_m4_direct_extent(start, length, 0x8001) == -ERANGE);
    start=0; length=0;
    assert(dcp_m4_extent_add(&start, &length, UINT64_MAX-3, 4) == -ERANGE);
    struct dcp_m4_frame cursor = {.dva=0x10000008000ULL, .backing_size=0x4000,
        .width=64, .height=64, .stride=256, .format=DCP_M4_ARGB8888};
    struct dcp_m4_upper upper={.frame=&cursor,.index=3,.x=2496,.y=1600};
    assert(!dcp_m4_upper_check(&upper));
    upper.x++; assert(dcp_m4_upper_check(&upper) == -EINVAL); upper.x--;
    cursor.width=31; assert(dcp_m4_upper_check(&upper) == -EINVAL); cursor.width=64;
    cursor.format=DCP_M4_XRGB8888; assert(dcp_m4_upper_check(&upper) == -EINVAL);
    upper.frame=NULL; assert(!dcp_m4_upper_check(&upper));
    upper.index=4; assert(dcp_m4_upper_check(&upper) == -EINVAL);
    puts("PASS prepublication rejection, DMA aperture/extent bounds and cursor constraints");
}

static void deadline(void)
{
    struct dcp_m4_frame frame = {.dva=DCP_M4_SCANOUT_BASE, .backing_size=0x4000000,
        .width=2560, .height=1664, .stride=10240, .format=DCP_M4_XRGB8888};
    u8 bytes[DCP_M4_SWAP_SIZE], plain[DCP_M4_SWAP_SIZE];
    assert(!dcp_m4_swap_encode(plain, sizeof(plain), 7, &frame, false, 0));
    memcpy(bytes, plain, sizeof(bytes));
    dcp_m4_swap_set_deadline(bytes, 120);
    for (size_t i = 0; i < sizeof(bytes); i++) {
        bool field = i < 0x10 || (i >= 0x30 && i < 0x38);
        assert(bytes[i] == (field && (i % 8) == 0 ? 120 : plain[i]));
    }
    dcp_m4_swap_set_deadline(bytes, 0);
    assert(!memcmp(bytes, plain, sizeof(bytes)));
    puts("PASS swap deadline: timestamp types 1, 2 and 7 only");
}

int main(void) { completion(); routes(); surfaces(); deadline(); return 0; }
'''


def plane(root):
    source = (root / APPLE / 'plane.c').read_text()
    present = header_body((root / APPLE / 'iomfb_m4_present.h').read_text())
    atomic = block(source, 'static int apple_plane_atomic_check_v26_6(')
    return production(root) + r'''
#define DRM_FORMAT_XRGB8888 0x34325258U
#define DRM_FORMAT_ARGB8888 0x34325241U
#define DRM_FORMAT_MOD_LINEAR 0ULL
#define DRM_PLANE_NO_SCALING (1U << 16)
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((intptr_t)(p))
struct drm_format_info { u32 format; unsigned num_planes; };
struct drm_gem_object { u64 size; };
struct drm_gem_dma_object { struct drm_gem_object base; u64 dma_addr; };
struct drm_framebuffer {
    const struct drm_format_info *format;
    u64 modifier;
    u32 offsets[4], pitches[4], width, height;
    struct drm_gem_dma_object *dma;
};
struct drm_crtc {};
struct drm_plane_state {
    struct drm_framebuffer *fb;
    struct drm_crtc *crtc;
    u32 src_x, src_y, src_w, src_h, crtc_w, crtc_h;
    int crtc_x, crtc_y;
    bool visible;
};
struct drm_plane {};
struct drm_crtc_state { struct { u32 hdisplay, vdisplay; } mode; bool active; };
struct drm_atomic_commit {
    struct drm_plane_state *plane;
    struct drm_crtc_state *crtc;
    int crtc_error;
};
static struct drm_gem_dma_object *drm_fb_dma_get_gem_obj(struct drm_framebuffer *fb, unsigned plane)
{ assert(plane == 0); return fb->dma; }
static struct drm_plane_state *drm_atomic_get_new_plane_state(struct drm_atomic_commit *s, struct drm_plane *p)
{ (void)p; return s->plane; }
static struct drm_crtc_state *drm_atomic_get_crtc_state(struct drm_atomic_commit *s, struct drm_crtc *crtc)
{ assert(crtc == s->plane->crtc); return s->crtc_error ? (void *)(intptr_t)s->crtc_error : s->crtc; }
static unsigned checks;
static int helper_error;
static int drm_atomic_helper_check_plane_state(struct drm_plane_state *plane,
    struct drm_crtc_state *crtc, int min, int max, bool position, bool disabled)
{
    assert(plane && crtc);
    assert(min == DRM_PLANE_NO_SCALING && max == DRM_PLANE_NO_SCALING);
    assert(!position && disabled);
    checks++;
    return helper_error;
}
''' + present + '\n' + atomic + r'''
int main(void)
{
    struct drm_format_info format = {.format=DRM_FORMAT_XRGB8888,.num_planes=1};
    struct drm_gem_dma_object obj={.dma_addr=0x10000100000ULL,.base={.size=0x2000000}};
    struct drm_framebuffer fb={.format=&format,.width=2560,.height=1664,
        .pitches={10240},.modifier=DRM_FORMAT_MOD_LINEAR,.dma=&obj};
    struct drm_crtc crtc;
    struct drm_plane plane;
    struct drm_plane_state ps={.fb=&fb,.crtc=&crtc,.src_w=2560U<<16,.src_h=1664U<<16,
        .crtc_w=2560,.crtc_h=1664,.visible=true};
    struct drm_crtc_state cs={.mode={.hdisplay=2560,.vdisplay=1664},.active=true};
    struct drm_atomic_commit state={.plane=&ps,.crtc=&cs};
    struct dcp_m4_frame frame;
    assert(!apple_plane_atomic_check_v26_6(&plane,&state));
    assert(checks==1);
    assert(!iomfb_m4_plane_frame(&ps,2560,1664,&frame));
    assert(frame.dva==obj.dma_addr && frame.backing_size==obj.base.size);
    assert(frame.width==2560 && frame.height==1664 && frame.stride==10240);
    assert(frame.format==DCP_M4_XRGB8888);
    format.format=DRM_FORMAT_ARGB8888;
    assert(!apple_plane_atomic_check_v26_6(&plane,&state));
    assert(!iomfb_m4_plane_frame(&ps,2560,1664,&frame) && frame.format==DCP_M4_ARGB8888);
    format.format=DRM_FORMAT_XRGB8888;
    #define REJECT(change,restore) do { change; assert(apple_plane_atomic_check_v26_6(&plane,&state)<0); restore; } while(0)
    REJECT(ps.fb=NULL,ps.fb=&fb);
    REJECT(format.num_planes=2,format.num_planes=1);
    REJECT(fb.offsets[0]=64,fb.offsets[0]=0);
    REJECT(fb.modifier=1,fb.modifier=0);
    REJECT(format.format=0,format.format=DRM_FORMAT_XRGB8888);
    REJECT(ps.src_x=1,ps.src_x=0);
    REJECT(ps.src_y=1,ps.src_y=0);
    REJECT(ps.src_w--,ps.src_w++);
    REJECT(ps.src_h--,ps.src_h++);
    REJECT(ps.crtc_x=1,ps.crtc_x=0);
    REJECT(ps.crtc_y=-1,ps.crtc_y=0);
    REJECT(ps.crtc_w--,ps.crtc_w++);
    REJECT(ps.crtc_h--,ps.crtc_h++);
    REJECT(fb.width--,fb.width++);
    REJECT(fb.height--,fb.height++);
    REJECT(fb.pitches[0]++,fb.pitches[0]--);
    REJECT(obj.base.size=10240*1664-1,obj.base.size=0x2000000);
    REJECT(obj.dma_addr++,obj.dma_addr--);
    REJECT(obj.dma_addr=0x4000,obj.dma_addr=0x10000100000ULL);
    REJECT(obj.dma_addr=DCP_M4_SCANOUT_BASE+DCP_M4_SCANOUT_SIZE-0x4000,
           obj.dma_addr=0x10000100000ULL);
    REJECT(fb.dma=NULL,fb.dma=&obj);
    puts("PASS actual common plane callback enforces qualified linear DMA GEM and full-panel geometry");
    unsigned before=checks;
    ps.crtc=NULL;
    assert(!apple_plane_atomic_check_v26_6(&plane,&state) && checks==before);
    ps.crtc=&crtc;
    state.crtc_error=-ENOMEM;
    assert(apple_plane_atomic_check_v26_6(&plane,&state)==-ENOMEM && checks==before);
    state.crtc_error=0;
    helper_error=-ERANGE;
    assert(apple_plane_atomic_check_v26_6(&plane,&state)==-ERANGE && checks==before+1);
    helper_error=0;
    ps.visible=false; ps.fb=NULL;
    assert(apple_plane_atomic_check_v26_6(&plane,&state)==-EINVAL);
    cs.active=false;
    assert(!apple_plane_atomic_check_v26_6(&plane,&state));
    puts("PASS detached/disabled states, active invisible rejection, DRM-core errors and no-scaling helper policy");
    return 0;
}
'''


def prepare(root):
    source = (root / APPLE / 'plane.c').read_text()
    return PREFIX + r'''
#include <stdlib.h>
struct drm_framebuffer { unsigned refs; };
struct apple_dcp { bool v26_6; };
struct drm_crtc { struct apple_dcp *dcp; };
struct drm_atomic_commit;
struct drm_plane_state {
    struct drm_framebuffer *fb;
    struct drm_crtc *crtc;
    struct drm_atomic_commit *state;
};
struct drm_atomic_commit { struct drm_plane_state *old; };
struct drm_plane {};
struct dcp_fb_reference { struct drm_framebuffer *fb; };
struct apple_plane_state {
    struct drm_plane_state base;
    struct dcp_fb_reference *retirement;
};
#define to_apple_plane_state(s) ((struct apple_plane_state *)(s))
#define to_apple_crtc(c) (c)
static struct apple_dcp *platform_get_drvdata(struct apple_dcp *p) { return p; }
static bool iomfb_is_m4(struct apple_dcp *dcp) { return dcp->v26_6; }
static unsigned prepare_calls, allocations, frees;
static int prepare_error;
static bool fail_alloc;
static void *allocate(size_t size) { allocations++; return fail_alloc ? NULL : calloc(1,size); }
#define kzalloc_obj(x) allocate(sizeof(x))
static void kfree(void *p) { assert(p); frees++; free(p); }
static void drm_framebuffer_get(struct drm_framebuffer *fb) { fb->refs++; }
static void drm_framebuffer_put(struct drm_framebuffer *fb) { assert(fb->refs); fb->refs--; }
static int drm_gem_plane_helper_prepare_fb(struct drm_plane *p, struct drm_plane_state *s)
{ assert(p && s); prepare_calls++; return prepare_error; }
static struct drm_plane_state *drm_atomic_get_old_plane_state(struct drm_atomic_commit *s, struct drm_plane *p)
{ assert(p); return s->old; }
''' + block(source, 'static void apple_plane_cleanup_fb(') + '\n' + block(source, 'static int apple_plane_prepare_fb(') + r'''
int main(void)
{
    struct drm_plane plane;
    struct apple_dcp dcp={.v26_6=true};
    struct drm_crtc crtc={.dcp=&dcp};
    struct drm_framebuffer fb1={.refs=1},fb2={.refs=1};
    struct drm_plane_state old={.fb=&fb1,.crtc=&crtc};
    struct drm_atomic_commit state={.old=&old};
    struct apple_plane_state next={.base={.fb=&fb2,.crtc=&crtc,.state=&state}};
    assert(!apple_plane_prepare_fb(&plane,&next.base));
    assert(prepare_calls==1 && !allocations && !next.retirement && fb1.refs==1);
    prepare_error=-EIO;
    assert(apple_plane_prepare_fb(&plane,&next.base)==-EIO);
    assert(prepare_calls==2 && !allocations && !next.retirement);
    prepare_error=0;
    dcp.v26_6=false;
    assert(!apple_plane_prepare_fb(&plane,&next.base));
    assert(prepare_calls==3 && allocations==1 && next.retirement && fb1.refs==2);
    assert(next.retirement->fb==&fb1);
    apple_plane_cleanup_fb(&plane,&next.base);
    assert(!next.retirement && fb1.refs==1 && frees==1);
    apple_plane_cleanup_fb(&plane,&next.base);
    assert(frees==1);
    fail_alloc=true;
    assert(apple_plane_prepare_fb(&plane,&next.base)==-ENOMEM && fb1.refs==1);
    fail_alloc=false;
    next.base.fb=&fb1;
    assert(!apple_plane_prepare_fb(&plane,&next.base) && !next.retirement);
    state.old=NULL;
    assert(!apple_plane_prepare_fb(&plane,&next.base) && !next.retirement);
    assert(prepare_calls==6);
    puts("PASS v26_6 keeps real GEM producer preparation without redundant old-FB retirement");
    puts("PASS legacy retirement keeps its reference, cleanup and allocation error behavior");
    return 0;
}
'''


def dart(root):
    source = (root / 'drivers/iommu/apple-dart-dcp.h').read_text()
    return PREFIX + r'''
struct device_node { const char *compatible[3]; };
static bool of_device_is_compatible(struct device_node *node,const char *name)
{
    if(!node) return false;
    for(unsigned i=0;i<3;i++)
        if(node->compatible[i] && !strcmp(node->compatible[i],name)) return true;
    return false;
}
''' + block(source, 'static bool apple_dart_j713_scanout_node(') + r'''
int main(void)
{
    struct device_node canonical={{"apple,t8132-display-subsystem"}};
    struct device_node legacy={{"apple,t8132-display-diagnostics"}};
    struct device_node multi={{"vendor,other","apple,t8132-display-subsystem"}};
    struct device_node foreign={{"apple,t6030-display-diagnostics"}};
    struct device_node typo={{"apple,t8132-display-subsystems"}};
    struct device_node generic={{"apple,display-subsystem"}};
    struct device_node empty={0};
    assert(apple_dart_j713_scanout_node(&canonical));
    assert(apple_dart_j713_scanout_node(&legacy));
    assert(apple_dart_j713_scanout_node(&multi));
    assert(!apple_dart_j713_scanout_node(&foreign));
    assert(!apple_dart_j713_scanout_node(&typo));
    assert(!apple_dart_j713_scanout_node(&generic));
    assert(!apple_dart_j713_scanout_node(&empty));
    assert(!apple_dart_j713_scanout_node(NULL));
    puts("PASS actual shared DART consumer predicate accepts canonical+legacy identities and rejects foreign nodes");
    return 0;
}
'''


def gate(root):
    """The physical power-down and power-up of the internal DCP: their order,
    the providers asked before the first step, and a late firmware message."""
    backend = (root / APPLE / 'iomfb_m4.c').read_text()
    return PREFIX + r'''
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define container_of(p,t,m) ((t *)((char *)(p) - __builtin_offsetof(t,m)))
#define le32_to_cpu(x) (x)
#define dev_dbg(...) ((void)0)
#define dev_err(d,...) ((void)(d),(void)fprintf(stderr,__VA_ARGS__))
#define spin_lock_irqsave(p,f) do { (void)(p); (f)=0; } while(0)
#define spin_unlock_irqrestore(p,f) do { (void)(p); (void)(f); } while(0)
typedef u32 __le32;
typedef int64_t s64;
typedef s64 ktime_t;
static ktime_t clock_ns;
static ktime_t ktime_get(void) { return clock_ns += 1000; }
static s64 ktime_us_delta(ktime_t later,ktime_t earlier) { return (later-earlier)/1000; }
struct device { int unused; };
struct platform_device { struct device dev; };
struct apple_rtkit { int unused; };
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
#define to_delayed_work(w) container_of(w,struct delayed_work,work)
struct apple_dcp { bool quiescing; };
struct dcp_m4_frame { u64 dva; };
struct dcp_m4_transaction { unsigned calls; };
struct iomfb_m4;
struct dcp_m4_runtime { struct iomfb_m4 *dcp; unsigned count; };
struct iomfb_m4 {
    struct apple_dcp *owner;
    struct device *dev;
    struct platform_device *piodma;
    struct apple_rtkit *rtkit;
    struct dcp_m4_runtime *runtime;
    int lock,state_lock,error;
    struct dcp_m4_transaction transaction;
    struct dcp_m4_frame active_frame;
    void *active_fb;
    bool gate_capable,gated,display_off,powered_off;
    struct delayed_work gate_work;
    u64 gate_cycles;
    s64 gate_off_us,gate_on_us;
};
static unsigned power_gate_ms;
static struct iomfb_m4 *the_dcp;
/* Every step taken, in order. */
static char steps[512];
static void trace(const char *s) { strcat(steps,s); strcat(steps," "); }
static int refuse;		/* the provider check that refuses (1..5), or 0 */
static bool late_word;		/* a DCP message arrives during the idle handshake */
static void mutex_lock(int *l) { assert(!*l); *l=1; }
static void mutex_unlock(int *l) { assert(*l==1); *l=0; }
static void lockdep_assert_held(int *l) { assert(*l==1); }
static void dcp_m4_failed(struct iomfb_m4 *dcp,int error) { if(!dcp->error) dcp->error=error; }
static int dcp_m4_runtime_live(struct dcp_m4_runtime *r) { return r->dcp->error; }
static int dcp_m4_runtime_call(void *cookie,unsigned int method,bool remote,
    const void *input,u32 input_size,void *output,u32 output_size)
{
    char name[16];
    (void)cookie; (void)remote; (void)input; (void)input_size;
    snprintf(name,sizeof(name),"A%u",method); trace(name);
    if(output) memset(output,0,output_size);
    if(method==412) { assert(output_size==4); put_unaligned_le32(1,output); }
    return 0;
}
static int dcp_m4_runtime_drain(struct dcp_m4_runtime *r,unsigned budget)
{ (void)r; (void)budget; trace("drain"); return 0; }
static int apple_rtkit_idle(struct apple_rtkit *rtk) { (void)rtk; trace("idle"); return 0; }
static void apple_rtkit_hold_rx(struct apple_rtkit *rtk) { (void)rtk; trace("hold"); }
static int apple_rtkit_poll(struct apple_rtkit *rtk) { (void)rtk; trace("poll"); return 0; }
static void apple_rtkit_flush_rx(struct apple_rtkit *rtk)
{ (void)rtk; trace("flush"); if(late_word) the_dcp->runtime->count++; }
static int iomfb_m4_cpu_run(struct iomfb_m4 *dcp,bool run) { (void)dcp; trace(run?"run":"stop"); return 0; }
static int iomfb_m4_cpu_start(void *data) { return iomfb_m4_cpu_run(data,true); }
static int apple_rtkit_wake_retained(struct apple_rtkit *rtk,int (*start)(void *),void *ctx)
{ (void)rtk; trace("wake"); return start(ctx); }
static bool dcp_dev(struct device *dev) { return dev==the_dcp->dev; }
static int apple_dart_dcp_power_check(struct device *dev) { return refuse==(dcp_dev(dev)?1:2)?-EAGAIN:0; }
static int apple_pmp_j713_display_check_release(struct device *dev) { (void)dev; return refuse==3?-ETIMEDOUT:0; }
static int apple_pmc_interface_consumer_check(struct device *dev) { (void)dev; return refuse==4?-ENODEV:0; }
static int apple_pmgr_ps_consumer_check(struct device *dev) { (void)dev; return refuse==5?-EBUSY:0; }
static int apple_dart_dcp_power_off(struct device *dev) { trace(dcp_dev(dev)?"dart-off":"piodma-off"); return 0; }
static int apple_dart_dcp_power_on(struct device *dev) { trace(dcp_dev(dev)?"dart-on":"piodma-on"); return 0; }
static int apple_pmp_j713_display_put(struct device *dev) { (void)dev; trace("pmp-put"); return 0; }
static int apple_pmp_j713_display_get(struct device *dev) { (void)dev; trace("pmp-get"); return 0; }
static int apple_pmc_interface_consumer_off(struct device *dev) { (void)dev; trace("pmc-off"); return 0; }
static int apple_pmc_interface_consumer_on(struct device *dev) { (void)dev; trace("pmc-on"); return 0; }
static int apple_pmgr_ps_consumer_off(struct device *dev) { (void)dev; trace("pmgr-off"); return 0; }
static int apple_pmgr_ps_consumer_on(struct device *dev) { (void)dev; trace("pmgr-on"); return 0; }
''' + '\n'.join(block(backend, name) for name in (
        'static int iomfb_m4_gate_check(', 'static int iomfb_m4_power_gate(',
        'static int iomfb_m4_power_ungate(', 'static void iomfb_m4_gate_work(')) + r'''
int main(void)
{
    struct apple_dcp owner={0};
    struct device dev={0};
    struct platform_device piodma={{0}};
    struct apple_rtkit rtk={0};
    struct iomfb_m4 dcp={.owner=&owner,.dev=&dev,.piodma=&piodma,.rtkit=&rtk,
                         .gate_capable=true,.display_off=true};
    struct dcp_m4_runtime r={.dcp=&dcp};

    dcp.runtime=&r; the_dcp=&dcp;
    /* Turned off after the blank: the queued power-down does nothing. */
    iomfb_m4_gate_work(&dcp.gate_work.work);
    assert(!steps[0] && !dcp.gated);
    /* Wanted: the whole power-down, in its order. */
    power_gate_ms=1000;
    iomfb_m4_gate_work(&dcp.gate_work.work);
    assert(!strcmp(steps,"A500 drain idle hold stop poll flush dart-off piodma-off "
                         "pmp-put pmc-off pmgr-off "));
    assert(dcp.gated && !dcp.error && dcp.gate_cycles==1 && !dcp.lock);

    /* The power-up, in macOS's order: domains, PMP vote, PMC enables, DARTs, CPU. */
    steps[0]=0; dcp.lock=1;
    assert(!iomfb_m4_power_ungate(&dcp));
    assert(!strcmp(steps,"pmgr-on pmp-get pmc-on dart-on piodma-on wake run A414 A412 "));
    assert(!dcp.gated && !dcp.error);

    /* A provider that cannot take part now: no step at all, and no failure. */
    for(refuse=1;refuse<=5;refuse++) {
        steps[0]=0;
        assert(iomfb_m4_power_gate(&dcp)<0);
        assert(!steps[0] && !dcp.gated && !dcp.error);
    }
    refuse=0;

    /* A message from the idle handshake: failed with nothing powered down. */
    steps[0]=0; late_word=true;
    assert(iomfb_m4_power_gate(&dcp)==-EBUSY);
    assert(!strcmp(steps,"A500 drain idle hold stop poll flush "));
    assert(!dcp.gated && dcp.error==-EBUSY);
    dcp.lock=0;
    puts("PASS the power-down and power-up keep their order, a refusing provider changes nothing, "
         "and a late message fails before anything is powered down");
    return 0;
}
'''


def blank(root):
    backend = (root / APPLE / 'iomfb_m4.c').read_text()
    backlight = (root / APPLE / 'dcp_backlight.c').read_text()
    functions = '\n'.join(block(backlight, name) for name in (
        'bool dcp_backlight_active(', 'int dcp_backlight_dpms(',
        'int dcp_backlight_prepare(', 'bool dcp_backlight_complete('))
    # The 14.7 panel's nits policy (an M3 internal panel) never runs here.
    if 'static bool dcp_backlight_presents_nits(' in backlight:
        functions = ('static bool iomfb_v14_7_panel_nits(struct apple_dcp *dcp) '
                     '{ (void)dcp; return false; }\n' +
                     block(backlight, 'static bool dcp_backlight_presents_nits(') +
                     '\n' + functions)
    return production(root) + r'''
#define min(x,y) ((x)<(y)?(x):(y))
static u64 div_u64(u64 x,u32 y) { return x/y; }
static u64 mul_u32_u32(u32 x,u32 y) { return (u64)x*y; }
''' + header_body((root / APPLE / 'dcp_backlight.h').read_text()) + r'''
#define DRM_FORMAT_MOD_LINEAR 0
#define DRM_FORMAT_XRGB8888 0x34325258U
#define DRM_FORMAT_ARGB8888 0x34325241U
#define DCP_FIRMWARE_H17P 1
#define DCP_FIRMWARE_V_27_0 3
#define DCP_IOMFB_METHODS_H17G 2
#define READ_ONCE(x) (x)
#define spin_lock_irqsave(p,f) do { (void)(p); (f)=0; } while(0)
#define spin_unlock_irqrestore(p,f) do { (void)(p); (void)(f); } while(0)
typedef u64 dma_addr_t;
typedef int64_t s64;
typedef s64 ktime_t;
#define max(x,y) ((x)>(y)?(x):(y))
static ktime_t clock_ns;
static ktime_t ktime_get(void) { return clock_ns += 1000; }
static s64 ktime_us_delta(ktime_t later,ktime_t earlier) { return (later-earlier)/1000; }
struct iomfb_m4;
struct apple_dcp {
    struct iomfb_m4 *v26_6;
    struct { struct dcp_backlight_state state; int lock; } backlight;
    int fw_compat;
    struct { int iomfb_method_profile; } hw;
};
struct drm_format_info { u32 format; unsigned num_planes; };
struct drm_framebuffer {
    const struct drm_format_info *format;
    u64 modifier;
    u32 width,height,pitches[4],offsets[4];
    unsigned refs;
};
struct dcp_m4_runtime { struct iomfb_m4 *dcp; };
struct iomfb_m4 {
    struct apple_dcp *owner;
    struct dcp_m4_runtime *runtime;
    int lock,error;
    struct drm_framebuffer *active_fb,*uncertain_fb;
    struct dcp_m4_frame active_frame;
    u64 flips,blanks;
    bool powered_off;
    bool display_off;
    u64 display_off_cycles;
    s64 display_off_us,display_off_us_max,display_on_us,display_on_us_max;
    bool gate_capable,gated;
    int gate_work;
};
/* The power-down after a blank: its scheduling and the power-up before ON. */
static unsigned power_gate_ms,scheduled,cancelled,ungates,ungate_error,ons_at_ungate;
#define system_dfl_wq NULL
static unsigned long msecs_to_jiffies(unsigned ms) { return ms; }
static bool mod_delayed_work(void *wq,int *work,unsigned long delay)
{ (void)wq; (void)work; assert(delay==power_gate_ms); scheduled++; return true; }
static bool cancel_delayed_work(int *work) { (void)work; cancelled++; return false; }
static void mutex_lock(int *lock) { assert(!*lock); *lock=1; }
static void mutex_unlock(int *lock) { assert(*lock==1); *lock=0; }
static void lockdep_assert_held(int *lock) { assert(*lock==1); }
static bool iomfb_is_v14_7(struct apple_dcp *dcp) { (void)dcp; return false; }
static bool iomfb_is_m4(struct apple_dcp *dcp) { return dcp->v26_6!=NULL; }
static unsigned deferred_kicks;
static void dcp_backlight_kick(struct apple_dcp *dcp) { assert(dcp->v26_6); deferred_kicks++; }
static void drm_framebuffer_get(struct drm_framebuffer *fb) { fb->refs++; }
static void drm_framebuffer_put(struct drm_framebuffer *fb) { assert(fb->refs); fb->refs--; }
static void dcp_m4_failed(struct iomfb_m4 *dcp,int error) { if(!dcp->error) dcp->error=error; }
static unsigned cache_kicks;
static void iomfb_m4_idle_cache_kick(struct iomfb_m4 *dcp) { assert(dcp->lock==1); cache_kicks++; }
static unsigned power_offs,power_ons;
static int dcp_m4_set_power_state(struct iomfb_m4 *dcp,bool on)
{ assert(dcp->lock==1); if(on) power_ons++; else power_offs++; return 0; }
static int dcp_m4_runtime_live(struct dcp_m4_runtime *r) { return r->dcp->error; }
static int iomfb_m4_power_ungate(struct iomfb_m4 *dcp)
{
    assert(dcp->lock==1 && dcp->gated && dcp->display_off);
    ons_at_ungate=power_ons; ungates++;
    if(ungate_error) { dcp_m4_failed(dcp,ungate_error); return ungate_error; }
    dcp->gated=false; return 0;
}
static int iomfb_m4_check_frame(const struct dcp_m4_frame *f)
{ return dcp_m4_frame_check(f); }
static unsigned order,failure,publications;
static u32 carried_nits;
static bool carried_brightness;
static int dcp_m4_runtime_swap_brightness(struct dcp_m4_runtime *r,
    const struct dcp_m4_frame *frame,bool brightness,u32 nits)
{
    u8 bytes[0x1bd8],reply[12]={0};
    struct dcp_m4_swap_state swap={0};
    u32 before=r->dcp->owner->backlight.state.actual;
    assert(r->dcp->lock==1);
    assert(!dcp_m4_swap_encode(bytes,sizeof(bytes),++publications,frame,brightness,nits));
    assert(!dcp_m4_swap_begin(&swap,publications));
    carried_brightness=brightness; carried_nits=nits;
    if(order) assert(!dcp_m4_swap_record_reply(&swap,reply,sizeof(reply)));
    else assert(!dcp_m4_swap_record_done(&swap,publications));
    assert(!dcp_m4_swap_take_completion(&swap));
    assert(r->dcp->owner->backlight.state.actual==before);
    if(failure) return -ETIMEDOUT;
    if(order) assert(!dcp_m4_swap_record_done(&swap,publications));
    else assert(!dcp_m4_swap_record_reply(&swap,reply,sizeof(reply)));
    assert(dcp_m4_swap_take_completion(&swap)==1);
    return 0;
}
''' + functions + '\n' + '\n'.join(block(backend, name) for name in (
        'static int iomfb_m4_swap(', 'int iomfb_m4_present(', 'int iomfb_m4_blank(')) + r'''
int main(void)
{
    for(order=0;order<2;order++) {
        unsigned kicks_before=deferred_kicks;
        struct drm_format_info fmt={.format=DRM_FORMAT_XRGB8888,.num_planes=1};
        struct drm_framebuffer fb={.format=&fmt,.width=2560,.height=1664,.pitches={10240},.refs=1};
        struct drm_framebuffer next=fb;
        struct apple_dcp owner={0};
        struct iomfb_m4 dcp={.owner=&owner};
        struct dcp_m4_runtime runtime={.dcp=&dcp};
        owner.v26_6=&dcp; dcp.runtime=&runtime;
        assert(!dcp_bl_init(&owner.backlight.state,500,true,120,false,0));
        assert(!iomfb_m4_present(&owner,&fb,0x10000100000ULL,0x2000000));
        assert(fb.refs==2 && dcp.active_fb==&fb && !dcp.uncertain_fb);
        assert(!dcp_backlight_dpms(&owner,false));
        assert(deferred_kicks==kicks_before+1 && owner.backlight.state.actual==120);
        unsigned offs_before=power_offs,ons_before=power_ons;
        /* No queued backlight worker has run: blank itself must carry zero. */
        assert(!iomfb_m4_blank(&owner));
        assert(carried_brightness && carried_nits==0 && owner.backlight.state.actual==0);
        assert(fb.refs==1 && !dcp.active_fb && !dcp.active_frame.dva && dcp.blanks==1);
        /* The blank powered the display down, once. */
        assert(dcp.display_off && dcp.display_off_cycles==1 && power_offs==offs_before+1);
        /* Powered down for system sleep: refused, no reference, session intact. */
        dcp.powered_off=true;
        assert(iomfb_m4_present(&owner,&fb,0x10000100000ULL,0x2000000)==-EAGAIN);
        assert(fb.refs==1 && !dcp.active_fb && !dcp.error);
        assert(dcp.display_off && power_ons==ons_before);
        dcp.powered_off=false;
        assert(!dcp_backlight_dpms(&owner,true));
        assert(deferred_kicks==kicks_before+2); /* This second kick is still deferred. */
        assert(!iomfb_m4_present(&owner,&fb,0x10000100000ULL,0x2000000));
        /* The first frame after the blank powered it back up. */
        assert(!dcp.display_off && power_ons==ons_before+1);
        /* Every shown frame restarts the idle-cache countdown. */
        assert(cache_kicks==2*order+2);
        assert(carried_brightness && carried_nits==120 && owner.backlight.state.actual==120);
        assert(fb.refs==2 && dcp.active_fb==&fb);
        failure=1;
        assert(iomfb_m4_present(&owner,&next,0x10000300000ULL,0x2000000)==-ETIMEDOUT);
        assert(dcp.active_fb==&fb && dcp.uncertain_fb==&next && fb.refs==2 && next.refs==2);
        assert(iomfb_m4_blank(&owner)==-ETIMEDOUT);
        assert(fb.refs==2 && next.refs==2 && dcp.active_fb==&fb && dcp.uncertain_fb==&next);
        failure=0;
    }
    /* Not opted in: a blank schedules no power-down; a frame still cancels one. */
    {
        struct drm_format_info fmt={.format=DRM_FORMAT_XRGB8888,.num_planes=1};
        struct drm_framebuffer fb={.format=&fmt,.width=2560,.height=1664,.pitches={10240},.refs=1};
        struct apple_dcp owner={0};
        struct iomfb_m4 dcp={.owner=&owner};
        struct dcp_m4_runtime runtime={.dcp=&dcp};
        unsigned c;
        owner.v26_6=&dcp; dcp.runtime=&runtime;
        assert(!dcp_bl_init(&owner.backlight.state,500,true,120,false,0));
        c=cancelled;
        assert(!iomfb_m4_present(&owner,&fb,0x10000100000ULL,0x2000000));
        assert(cancelled==c+1);
        assert(!iomfb_m4_blank(&owner) && scheduled==0);
        dcp.gate_capable=true;
        assert(!iomfb_m4_present(&owner,&fb,0x10000100000ULL,0x2000000));
        assert(!iomfb_m4_blank(&owner) && scheduled==0); /* power_gate_ms is 0 */

        /* Opted in: one power-down after the blank, after power_gate_ms. */
        power_gate_ms=1000;
        assert(!iomfb_m4_present(&owner,&fb,0x10000100000ULL,0x2000000));
        assert(!iomfb_m4_blank(&owner) && scheduled==1);
        /* Powered down: the next frame powers it up first, then setPowerState(1). */
        dcp.gated=true;
        unsigned ons=power_ons;
        assert(!iomfb_m4_present(&owner,&fb,0x10000100000ULL,0x2000000));
        assert(ungates==1 && ons_at_ungate==ons && power_ons==ons+1);
        assert(!dcp.gated && !dcp.display_off && dcp.active_fb==&fb);
        /* A failed power-up refuses the frame; the session is failed by it. */
        assert(!iomfb_m4_blank(&owner) && scheduled==2);
        dcp.gated=true; ungate_error=-EIO; ons=power_ons;
        assert(iomfb_m4_present(&owner,&fb,0x10000100000ULL,0x2000000)==-EIO);
        assert(ungates==2 && power_ons==ons && dcp.error==-EIO && dcp.display_off);
        assert(fb.refs==1 && !dcp.active_fb);
        ungate_error=0; power_gate_ms=0;
    }
    puts("PASS DPMS-off blank carries zero before delayed work and completes only after both exact receipts");
    puts("PASS actual present/blank retain active+uncertain DMA framebuffer references across a missing receipt");
    puts("PASS a present while the display is powered off for sleep is refused without a reference");
    puts("PASS a blank powers the display down once, and the next frame powers it back up");
    return 0;
}
'''


# macOS's preferred internal-panel timings (ID 2), horizontal then vertical, in
# the profile's field order. The J604's also come from its fixture report.
TIMING_FIELDS = ('Active', 'Total', 'FrontPorch', 'SyncWidth', 'BackPorch',
                 'PreciseSyncRate', 'SyncPolarity', 'PixelRepetition')
PANEL_TIMINGS = {
    'j713': ((2560, 2640, 8, 32, 40, 6877358, 1, 0), (1664, 1749, 33, 8, 44, 3932166, 1, 0)),
    'j715': ((2880, 2960, 8, 32, 40, 7710975, 1, 0), (1864, 1961, 47, 8, 42, 3932165, 1, 0)),
    'j604': ((3024, 3104, 8, 32, 40, 16986999, 1, 0), (1964, 2160, 102, 8, 86, 7864351, 1, 0)),
}
# hdisplay, hsync_start, hsync_end, htotal, vdisplay, vsync_start, vsync_end,
# vtotal, clock_khz, refresh_16_16
PANEL_MODES = {
    'j713': (2560, 2568, 2600, 2640, 1664, 1697, 1705, 1749, 277042, 3932166),
    'j715': (2880, 2888, 2920, 2960, 1864, 1911, 1919, 1961, 348274, 3932165),
    'j604': (3024, 3032, 3064, 3104, 1964, 2066, 2074, 2160, 804560, 7864351),
}

TIMING_SHIMS = r'''
#include <stdlib.h>
#define GFP_KERNEL 0
static void *kzalloc(size_t size, int flags) { (void)flags; return calloc(1, size); }
static void kfree(void *p) { free(p); }
static inline u64 get_unaligned_le64(const void *p) {
    const u8 *b = p; u64 v = 0;
    for (unsigned i = 0; i < 8; i++) v |= (u64)b[i] << (8 * i);
    return v;
}
'''

TIMING_MAIN = r'''
struct timing_case {
    const char *name;
    const struct dcp_m4_profile *profile;
    const u8 *data;
    size_t size;
    int expect;
    u32 mode[10];
};
static const struct timing_case cases[] = {
%s
};
int main(void)
{
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct timing_case *c = &cases[i];
        struct dcp_m4_timing t;
        int ret;
        memset(&t, 0xa5, sizeof(t));
        dcp_m4_pf = c->profile;
        ret = dcp_m4_parse_timing(c->data, c->size, &t);
        if (ret != c->expect) {
            printf("FAIL %%s: %%d, expected %%d\n", c->name, ret, c->expect);
            return 1;
        }
        if (!ret) {
            const u32 got[10] = {t.hdisplay, t.hsync_start, t.hsync_end, t.htotal,
                t.vdisplay, t.vsync_start, t.vsync_end, t.vtotal, t.clock_khz, t.refresh_16_16};
            if (t.id != 2 || t.color_id != 1 || memcmp(got, c->mode, sizeof(got))) {
                printf("FAIL %%s: wrong mode\n", c->name);
                return 1;
            }
        }
        printf("PASS %%s: %%s\n", c->name, ret ? "refused" : "adopted");
    }
    return 0;
}
'''


def serialize(elements):
    """The DCP's binary dictionary encoding of a TimingElements array."""
    out = bytearray(struct.pack('<I', 0xd3))

    def put(kind, count, payload=b''):
        out.extend(bytes(-len(out) % 4))
        out.extend(struct.pack('<I', kind << 24 | count) + payload)

    def emit(value):
        if isinstance(value, bool):
            put(11, int(value))
        elif isinstance(value, int):
            put(4, 64, struct.pack('<Q', value))
        elif isinstance(value, str):
            put(9, len(value), value.encode())
        elif isinstance(value, bytes):
            put(10, len(value), value)
        elif isinstance(value, list):
            put(2, len(value))
            for item in value:
                emit(item)
        else:
            put(1, len(value))
            for key, item in value.items():
                emit(key)
                emit(item)

    emit(elements)
    return bytes(out)


def timing_report():
    def load(value):
        if isinstance(value, dict) and list(value) == ['data']:
            return bytes.fromhex(value['data'])
        if isinstance(value, dict):
            return {k: load(v) for k, v in value.items()}
        if isinstance(value, list):
            return [load(v) for v in value]
        return value
    fixture = json.loads((HERE / 'm4-fixtures/timing-elements.json').read_text())
    return load(fixture['j604'])


def edit(elements, index, change):
    elements = copy.deepcopy(elements)
    change(elements[index])
    return elements


def setter(*path, value):
    def change(element):
        for key in path[:-1]:
            element = element[key]
        element[path[-1]] = value
    return change


def timing_cases():
    """(name, profile, report, adopted mode or None) for the panel timing parser."""
    j604 = timing_report()
    assert [e['ID'] for e in j604] == [2, 3, 4, 5, 6, 7]
    assert [e['IsPreferred'] for e in j604] == [True] + [False] * 5
    axes = ('HorizontalAttributes', 'VerticalAttributes')
    for axis, name in enumerate(axes):
        assert tuple(j604[0][name][f] for f in TIMING_FIELDS) == PANEL_TIMINGS['j604'][axis]

    def panel(board):
        element = copy.deepcopy(j604[0])
        for axis, name in enumerate(axes):
            element[name].update(zip(TIMING_FIELDS, PANEL_TIMINGS[board][axis]))
        return [element]

    cases = []

    def case(name, profile, report, mode=None):
        cases.append((name, profile, report, mode))

    def perturbed(name, profile, report):
        for axis in axes:
            for field in TIMING_FIELDS:
                case('%s %s %s changed' % (name, axis, field), profile,
                     edit(report, 0, setter(axis, field, value=report[0][axis][field] + 1)))

    j713, j715 = panel('j713'), panel('j715')
    case('J713 single element', 'j713', j713, 'j713')
    case('J715 single element', 'j715', j715, 'j715')
    case('J713 report on the J715', 'j715', j713)
    case('J715 report on the J713', 'j713', j715)
    case('J713 with a second, 60 Hz element', 'j713', j713 + [j604[1]])
    case('J713 without ID 2', 'j713', edit(j713, 0, setter('ID', value=3)))
    case('J713 element not preferred', 'j713', edit(j713, 0, setter('IsPreferred', value=False)))
    case('J713 empty report', 'j713', [])
    perturbed('J713', 'j713', j713)
    perturbed('J715', 'j715', j715)

    case('J604 six elements', 'j604', j604, 'j604')
    case('J604 six elements, ID 2 last', 'j604', j604[1:] + j604[:1], 'j604')
    case('J604 preferred element only', 'j604', j604[:1], 'j604')
    case('J604 changed non-preferred timing', 'j604',
         edit(j604, 1, setter('VerticalAttributes', 'PreciseSyncRate', value=3932161)), 'j604')
    perturbed('J604', 'j604', j604)
    for field, value in (('IsVirtual', True), ('IsInterlaced', True), ('IsSplit', True),
                         ('IsOverscanned', True), ('TimingType', 4),
                         ('MinimumVariableRefreshRate', 48 << 16),
                         ('MaximumVariableRefreshRate', 120 << 16)):
        case('J604 %s changed' % field, 'j604', edit(j604, 0, setter(field, value=value)))
    case('J604 color depth changed', 'j604',
         edit(j604, 0, setter('ColorModes', 0, 'Depth', value=10)))
    case('J604 second color mode', 'j604',
         edit(j604, 0, lambda e: e['ColorModes'].append(dict(e['ColorModes'][0], ID=2))))
    case('J604 without ID 2 (renumbered)', 'j604', edit(j604, 0, setter('ID', value=8)))
    case('J604 without ID 2 (removed)', 'j604', j604[1:])
    case('J604 with two ID 2 elements', 'j604', edit(j604, 1, setter('ID', value=2)))
    case('J604 with a second preferred element', 'j604',
         edit(j604, 1, setter('IsPreferred', value=True)))
    case('J604 element without IsPreferred', 'j604', edit(j604, 1, lambda e: e.pop('IsPreferred')))
    case('J604 element without an ID', 'j604', edit(j604, 1, lambda e: e.pop('ID')))
    case('J604 element that is not a dictionary', 'j604', j604[:1] + [7] + j604[2:])
    case('J604 seven elements', 'j604', j604 + [dict(j604[5], ID=9)])
    case('J604 report on the J713', 'j713', j604)
    case('J604 preferred element on the J713', 'j713', j604[:1])
    case('J713 report on the J604', 'j604', j713)
    case('J604 report on the J715', 'j715', j604)
    return cases


def timing(root):
    arrays, rows = [], []
    for index, (name, profile, report, mode) in enumerate(timing_cases()):
        data = serialize(report)
        arrays.append('static const u8 report_%d[] = {%s};' % (index, ','.join(map(str, data))))
        expected = ','.join(map(str, PANEL_MODES[mode])) if mode else '0'
        rows.append('    {"%s", &dcp_m4_profile_%s, report_%d, sizeof(report_%d), %s, {%s}},' % (
            name, profile, index, index, '0' if mode else '-EINVAL', expected))
    return (production(root) + TIMING_SHIMS +
            header_body((root / APPLE / 'dcp-m4-timing.h').read_text()) +
            '\n'.join(arrays) + TIMING_MAIN % '\n'.join(rows))
