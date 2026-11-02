# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""M4 request compatibility and shared appledrm integration regressions.

The immutable request fixture is generated from the previous working C driver,
not the implementation under test. Transport, MMIO and DRM core are not emulated.
"""
import base64
import hashlib
import json
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
    return PREFIX + ''.join(header_body((root / APPLE / p).read_text()) for p in HEADERS)


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

int main(void) { completion(); routes(); surfaces(); return 0; }
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


def blank(root):
    backend = (root / APPLE / 'iomfb_m4.c').read_text()
    backlight = (root / APPLE / 'dcp_backlight.c').read_text()
    functions = '\n'.join(block(backlight, name) for name in (
        'bool dcp_backlight_active(', 'int dcp_backlight_dpms(',
        'int dcp_backlight_prepare(', 'bool dcp_backlight_complete('))
    return production(root) + r'''
#define min(x,y) ((x)<(y)?(x):(y))
static u64 div_u64(u64 x,u32 y) { return x/y; }
static u64 mul_u32_u32(u32 x,u32 y) { return (u64)x*y; }
''' + header_body((root / APPLE / 'dcp_backlight.h').read_text()) + r'''
#define DRM_FORMAT_MOD_LINEAR 0
#define DRM_FORMAT_XRGB8888 0x34325258U
#define DRM_FORMAT_ARGB8888 0x34325241U
#define DCP_FIRMWARE_H17P 1
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
};
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
    puts("PASS DPMS-off blank carries zero before delayed work and completes only after both exact receipts");
    puts("PASS actual present/blank retain active+uncertain DMA framebuffer references across a missing receipt");
    puts("PASS a present while the display is powered off for sleep is refused without a reference");
    puts("PASS a blank powers the display down once, and the next frame powers it back up");
    return 0;
}
'''
