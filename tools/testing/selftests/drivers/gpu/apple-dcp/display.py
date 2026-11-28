# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Copyright 2026 Ryan Murray
"""Internal panel policy and real TimingElements parser, without hardware."""
import copy
import struct
from common import block
import rate


def indicator_dpms(root):
    source = (root / 'drivers/gpu/drm/apple/iomfb.c').read_text()
    return r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#define SWAP_SURFACES 4
#define BIT(i) (1U << (i))
#define IOMFB_SET_BACKGROUND BIT(31)
#define lockdep_assert_held(lock) ((void)(lock))
#define scoped_guard(kind, lock) for (int once = ((void)(lock), 1); once; once = 0)
struct drm_framebuffer { unsigned int refs; };
struct dcp_swap_submit_req_h17p {
    struct {
        unsigned int swap_enabled, swap_completed, bg_color;
        unsigned int src_rect[4], dst_rect[4], surf_ids[4], surf_flags[4], surf_unk[4];
    } swap;
    unsigned int surf[4], surf_iova[4];
    bool surf_null[4];
};
struct iomfb_scanout_h17p {
    struct drm_framebuffer *fb[4];
    struct dcp_swap_submit_req_h17p request;
};
struct apple_dcp {
    struct dcp_camera_indicator {
        bool has_surf;
        unsigned int surf;
        int lock;
        struct { bool wanted; } state;
    } camera_ind;
    struct { struct dcp_swap_submit_req_h17p h17p; } swap;
    struct { int lock; struct iomfb_scanout_h17p *scanout, *next_scanout; } iomfb;
};
static void drm_framebuffer_put(struct drm_framebuffer *fb)
{
    assert(fb->refs);
    fb->refs--;
}
static void iomfb_scanout_release_h17p(struct iomfb_scanout_h17p *scanout)
{
    if (scanout)
        for (unsigned int i = 0; i < SWAP_SURFACES; ++i)
            if (scanout->fb[i])
                drm_framebuffer_put(scanout->fb[i]);
}
static struct drm_framebuffer *confirmed;
static void dcp_indicator_swap_done(struct apple_dcp *dcp, struct drm_framebuffer *fb)
{
    assert(dcp->camera_ind.has_surf);
    confirmed = fb;
}
''' + '\n'.join(block(source, name) for name in (
    'static void iomfb_null_surface_h17p(',
    'void iomfb_prepare_indicator_h17p(',
    'static void iomfb_retire_indicator_h17p(',
    'void iomfb_scanout_complete_h17p(',
)) + r'''
int main(void)
{
    struct drm_framebuffer desktop = { 1 }, dot = { 2 };
    struct iomfb_scanout_h17p old = { .fb = { [3] = &dot } };
    struct iomfb_scanout_h17p next = { .fb = { [0] = &desktop, [3] = &dot } };
    struct apple_dcp dcp = {
        .camera_ind = { .has_surf = true, .surf = 3 },
        .iomfb = { .scanout = &old, .next_scanout = &next },
        .swap.h17p = {
            .swap.swap_enabled = BIT(0),
            .swap.swap_completed = BIT(0),
            .surf_null = { false, true, true, true },
        },
    };
    iomfb_prepare_indicator_h17p(&dcp);
    assert(dcp.swap.h17p.swap.swap_enabled == (BIT(0) | BIT(3)));
    assert(dcp.swap.h17p.swap.swap_completed == dcp.swap.h17p.swap.swap_enabled);
    assert(old.fb[3] == &dot && dot.refs == 2);
    confirmed = &dot;
    iomfb_scanout_complete_h17p(&dcp);
    assert(dcp.iomfb.scanout == &next && !dcp.iomfb.next_scanout);
    assert(!next.fb[3] && !dot.refs && !confirmed);
    assert(next.request.surf_null[3] && !(next.request.swap.swap_enabled & BIT(3)));
    assert(next.fb[0] == &desktop && desktop.refs == 1);
    assert(next.request.swap.swap_enabled == BIT(0));

    /* A brightness or wake replay shares the retained scanout, with no next one. */
    dot.refs = 1;
    next.fb[3] = &dot;
    next.request.surf_null[3] = false;
    next.request.surf_iova[3] = 42;
    next.request.swap.swap_enabled |= BIT(3);
    dcp.swap.h17p = next.request;
    dcp.camera_ind.state.wanted = true;
    iomfb_prepare_indicator_h17p(&dcp);
    assert(!dcp.swap.h17p.surf_null[3] && dcp.swap.h17p.surf_iova[3] == 42);
    dcp.camera_ind.state.wanted = false;
    iomfb_prepare_indicator_h17p(&dcp);
    assert(dcp.swap.h17p.surf_null[3] && !dcp.swap.h17p.surf_iova[3]);
    assert(next.fb[3] == &dot && dot.refs == 1);
    iomfb_retire_indicator_h17p(&dcp, &next);
    assert(!next.fb[3] && !dot.refs && !next.request.surf_iova[3]);
    assert(next.request.surf_null[3] && next.request.swap.swap_enabled == BIT(0));
    return 0;
}
'''


def backlight(root):
    header = (root / 'drivers/gpu/drm/apple/dcp_backlight.h').read_text()
    header = '\n'.join(line for line in header.splitlines()
                       if not line.startswith('#include'))
    header = header.replace('static inline ', 'static inline __attribute__((unused)) ')
    return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint8_t u8;
#define min(a,b) ((a)<(b)?(a):(b))
#define mul_u32_u32(a,b) ((u64)(a)*(b))
#define div_u64(a,b) ((a)/(b))
''' + header + r'''
static void completed(struct dcp_backlight_state *s, struct dcp_backlight_swap *p,
                      u32 id, bool early)
{
    u32 old=s->actual;
    if(early) {
        assert(!dcp_bl_swap_finish(s,p,id,false,true));
        assert(s->actual==old && s->in_flight);
        assert(dcp_bl_swap_finish(s,p,id,true,true));
    } else {
        assert(!dcp_bl_swap_finish(s,p,id,true,true));
        assert(s->actual==old && s->in_flight);
        assert(dcp_bl_swap_finish(s,p,id,false,true));
    }
    assert(!dcp_bl_swap_finish(s,p,id,false,true));
}
int main(void)
{
    struct dcp_backlight_state s;
    struct dcp_backlight_swap p={0};
    u32 id=1, nits;
    assert(!dcp_bl_takeover_nits(600,600000,&nits) && nits==600);
    /* A registration default must never change the loader's visible level. */
    assert(!dcp_bl_init(&s,600,false,0,true,300)); s.fade=true;s.preserve_inherited=true;
    assert(!s.dirty && !s.level_known && dcp_bl_actual(&s)==300);
    assert(!dcp_bl_request(&s,300,true,true));
    assert(!dcp_bl_request(&s,300,false,false));
    assert(!s.controlled && !s.dirty && dcp_bl_actual(&s)==300);
    assert(dcp_bl_swap_prepare(&s,&p,id,true)==-EALREADY);
    assert(!dcp_bl_request(&s,250,false,false));
    assert(s.fading && !s.dirty);
    assert(dcp_bl_fade_step(&s,125,250) && s.level==275);
    assert(!dcp_bl_fade_step(&s,250,250) && s.level==250);
    assert(dcp_bl_swap_prepare(&s,&p,id,false)==-ENODATA);
    assert(!dcp_bl_swap_prepare(&s,&p,id,true));
    completed(&s,&p,id++,true);
    assert(s.actual==250 && s.level_known && dcp_bl_actual(&s)==250);
    assert(!dcp_bl_seed(&s,300));
    /* Same state as a slider request or an ALS retarget through backlight. */
    assert(!dcp_bl_request(&s,600,false,false) && s.fading);
    for(u32 t=16;t<250;t+=16) {
        u32 before=s.level;
        assert(dcp_bl_fade_step(&s,t,250));
        assert(s.level>=before && s.level<600);
        if(t==16) {
            assert(!dcp_bl_swap_prepare(&s,&p,id,true));
            assert(dcp_bl_swap_finish(&s,&p,id++,true,false));
            assert(s.actual==250 && s.dirty && dcp_bl_retry_delay(&s)==100);
        }
    }
    assert(!dcp_bl_fade_step(&s,250,250));
    assert(!dcp_bl_swap_prepare(&s,&p,id,true));
    assert(!dcp_bl_swap_finish(&s,&p,id+100,false,true));
    completed(&s,&p,id++,false); assert(s.actual==600);
    assert(dcp_bl_request(&s,601,false,false)==-ERANGE);
    assert(!dcp_bl_request(&s,0,false,false));
    assert(dcp_bl_fade_step(&s,125,250) && s.level==300);
    assert(!dcp_bl_request(&s,100,false,false) && s.fade_from==300);
    assert(dcp_bl_fade_step(&s,125,250) && s.level==200);
    assert(!dcp_bl_request(&s,100,true,false));
    assert(!s.fading && dcp_bl_effective(&s)==0);
    assert(!dcp_bl_request(&s,450,true,false) && dcp_bl_effective(&s)==0);
    assert(!dcp_bl_request(&s,450,false,false) && s.level==450);
    assert(!dcp_bl_request(&s,0,false,true) && dcp_bl_effective(&s)==0);
    assert(!dcp_bl_request(&s,450,false,false) && s.level==450);
    assert(!dcp_bl_dpms(&s,false) && dcp_bl_effective(&s)==0);
    assert(!dcp_bl_dpms(&s,true) && dcp_bl_effective(&s)==450);
    assert(!dcp_bl_resend(&s));
    /*
     * DAC firmware: the report stays 0 after a display power cycle while the
     * panel is lit again (J414s, 13.5). Boot and resume end at the same level.
     */
    {
        struct dcp_bl_presented boot={0}, resume={0};
        assert(dcp_bl_presented_actual(&boot,140)==140);
        dcp_bl_presented_set(&boot,500);
        dcp_bl_presented_set(&boot,0);
        assert(dcp_bl_presented_actual(&boot,0)==0);
        dcp_bl_presented_set(&boot,500);
        assert(dcp_bl_presented_actual(&boot,0)==500);
        dcp_bl_presented_set(&resume,500);
        dcp_bl_presented_set(&resume,0);
        dcp_bl_presented_set(&resume,0);
        assert(dcp_bl_presented_actual(&resume,0)==0);
        dcp_bl_presented_set(&resume,500);
        assert(dcp_bl_presented_actual(&resume,500)==dcp_bl_presented_actual(&boot,0));
    }
    return 0;
}
'''



def dac_words(root):
    """The production DAC tables, interpolate() and calculate_dac() at every level."""
    import re
    source = (root / 'drivers/gpu/drm/apple/dcp_backlight.c').read_text()
    defines = '\n'.join(re.findall(r'^#define (?:MIN|MAX)_BRIGHTNESS_PART[12]\s.*$', source, re.M))
    tables = '\n'.join(re.findall(r'^static u32 brightness_part[0-9]+\[\] = \{.*?\};', source, re.M | re.S))
    functions = '\n'.join(block(source, marker) for marker in (
        'static u32 interpolate(', 'static u32 calculate_dac('))
    return r"""
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define WARN(c, ...) (c)
#define SCALE_FACTOR (1 << 10)
struct apple_dcp;
""" + defines + '\n' + tables + '\n' + functions + r"""
/* Every word is two 16-bit DAC halves, each a table value times 16. */
static void check_word(u32 word, u32 *hi, u32 *lo)
{
    *hi = word >> 16;
    *lo = word & 0xffff;
    assert(!(*hi & 0xf) && !(*lo & 0xf));
    assert(*hi <= 0x7ff0 && *lo <= *hi);
}
/* The widest gap between a table word's two halves, times 16. */
static u32 widest(const u32 *tbl, size_t n, u32 gap)
{
    for (size_t i = 0; i < n; i++)
        if (16 * ((tbl[i] >> 16) - (tbl[i] & 0xffff)) > gap)
            gap = 16 * ((tbl[i] >> 16) - (tbl[i] & 0xffff));
    return gap;
}
int main(void)
{
    u32 prev_hi = 0, prev_lo = 0, hi, lo, gap;
    gap = widest(brightness_part1, ARRAY_SIZE(brightness_part1), 0);
    gap = widest(brightness_part12, ARRAY_SIZE(brightness_part12), gap);
    gap = widest(brightness_part2, ARRAY_SIZE(brightness_part2), gap);
    /* The table points themselves, as before. */
    assert(calculate_dac(NULL, 2) == 0);
    assert(calculate_dac(NULL, MAX_BRIGHTNESS_PART1) == 16 * brightness_part1[ARRAY_SIZE(brightness_part1) - 1]);
    assert(calculate_dac(NULL, MIN_BRIGHTNESS_PART2) == 16 * brightness_part2[0]);
    assert(calculate_dac(NULL, MAX_BRIGHTNESS_PART2) == 16 * brightness_part2[ARRAY_SIZE(brightness_part2) - 1]);
    for (int nits = 0; nits <= (int)MAX_BRIGHTNESS_PART2; nits++) {
        check_word(calculate_dac(NULL, nits), &hi, &lo);
        /* Both halves rise with the level; neither picks up the other's fraction. */
        assert(hi >= prev_hi && lo >= prev_lo);
        /* The lower half stays the half step below, as in the table. */
        assert(hi - lo <= gap);
        prev_hi = hi;
        prev_lo = lo;
    }
    /* 370 nits (J313): the lower half was 0x10a0, a 4 to 5 nit value. */
    check_word(calculate_dac(NULL, 370), &hi, &lo);
    assert(hi > 0x7800 && lo > 0x7800);
    return 0;
}
"""


def modes(root):
    f = rate.parser_fixture(root)
    # Use the exact production parser and public serializer already used by rate.
    parser = f['parser']
    prefix = (f['prefix'] + f['epoch'] + f['metadata'] + f['header']).replace('static inline ', 'static inline __attribute__((unused)) ')
    baseline = copy.deepcopy(f['timing_fixture'][0])
    checks = []
    blobs = []

    def case(elements, expected, ranges=None, desktop=True):
        raw = bytearray(struct.pack('<I', 0xd3))
        f['wire'](elements, raw)
        i = len(blobs)
        blobs.append('static const unsigned char b%u[]={%s};' %
                     (i, ','.join(map(str, raw))))
        check = (f'{{ struct dcp_parse_ctx c; unsigned n=99; '
                 f'assert(!parse(b{i},sizeof(b{i}),&c)); c.zero_vrr_range_fallback={int(desktop)}; c.internal_present_deadline={int(desktop)}; '
                 'struct dcp_display_mode *m=enumerate_modes(&c,&n,355,230,0,true,false); '
                 f'assert(!IS_ERR(m) && n=={len(expected)});')
        for j, hz in enumerate(expected):
            check += (f'assert(m[{j}].mode.clock=={round(2200*1125*hz/1000)}); '
                      f'assert(m[{j}].timing_mode_id=={elements[j]["ID"]}); '
                      f'assert(m[{j}].mode.htotal==2200 && m[{j}].mode.vtotal==1125); '
                      f'assert(m[{j}].mode.flags==(DRM_MODE_FLAG_PHSYNC|DRM_MODE_FLAG_PVSYNC)); ')
        for j, hz in enumerate(expected):
            # A desktop internal panel takes the deadline at every timing.
            if desktop or hz >= 119.88:
                check += f'assert(m[{j}].needs_timestamps); '
            elif not (ranges and ranges[j][0]):
                check += f'assert(!m[{j}].needs_timestamps); '
        if ranges:
            for j, (lo, hi) in enumerate(ranges):
                check += f'assert(m[{j}].vrr=={int(bool(lo))}); '
                if lo:
                    check += f'assert(m[{j}].min_vrr=={lo} && m[{j}].max_vrr=={hi}); '
        checks.append(check + 'free(m); }')

    supported = []
    # Synthetic rates exercise catalog handling, not a claim about a panel dump.
    for i, hz in enumerate((120, 60, 59.94, 50, 48, 47.952)):
        m = copy.deepcopy(baseline)
        m['ID'] = 100+i
        m['VerticalAttributes']['PreciseSyncRate'] = round(hz*65536)
        supported.append(m)
    case(supported, (120, 60, 59.94, 50, 48, 47.952),
         [(24<<16, 120<<16)] + [(0,0)]*5)
    for hz in (119.88, 120, 120.01):
        m=copy.deepcopy(supported[0]); m['VerticalAttributes']['PreciseSyncRate']=round(hz*65536)
        case([m], [hz], [(24<<16, round(hz*65536))])
    for lo, hi, valid in ((48<<16,120<<16,True), (0,120<<16,False),
                          (60<<16,0,False), (0,0,True), (120<<16,24<<16,False),
                          (24<<16,144<<16,False), (120<<16,120<<16,False)):
        m=copy.deepcopy(supported[0]); m['MinimumVariableRefreshRate']=lo; m['MaximumVariableRefreshRate']=hi
        case([m],[120],[(lo or 24<<16,hi or 120<<16) if valid else (0,0)])
    m=copy.deepcopy(supported[0]);m['MinimumVariableRefreshRate']=0;m['MaximumVariableRefreshRate']=0
    case([m],[120],[(0,0)],desktop=False)
    for key in ('MinimumVariableRefreshRate', 'MaximumVariableRefreshRate'):
        m=copy.deepcopy(supported[0]);m[key]=0
        case([m],[120],[(0,0)])
    m=copy.deepcopy(supported[0]);m['MinimumVariableRefreshRate']=24<<16
    m=f['ObjectPairs'](list(m.items())+[('MinimumVariableRefreshRate',48<<16)])
    # Check duplicates against the parser directly without dict indexing in case().
    raw=bytearray(struct.pack('<I',0xd3));f['wire']([m],raw)
    blobs.append('static const unsigned char dup[]={%s};' % ','.join(map(str,raw)))
    checks.append('{struct dcp_parse_ctx c;unsigned n;assert(!parse(dup,sizeof(dup),&c));'
                  'struct dcp_display_mode *m=enumerate_modes(&c,&n,355,230,0,true,false);'
                  'assert(!IS_ERR(m) && n==1 && !m[0].vrr);free(m);}')
    # Bad geometry and virtual modes are still rejected.
    for field, value in (('Total', 1000), ('FrontPorch', 65536)):
        m=copy.deepcopy(supported[0]);m['HorizontalAttributes'][field]=value
        case([m],[])
    m=copy.deepcopy(supported[0]);m['IsVirtual']=True;case([m],[])
    return prefix + parser + '\n'.join(blobs) + '\nint main(void){' + '\n'.join(checks) + 'return 0;}'


def notch(root):
    """Both notch geometries of a panel timing (dcp-notch.h), through the
    production parser and catalogue code: the notch-free twin is exactly the
    mode the boot-time hidden notch gave, and offer, offset and ownership
    follow the policy."""
    f = rate.parser_fixture(root)
    iomfb = (root / 'drivers/gpu/drm/apple/iomfb.c').read_text()
    twins = (block(iomfb, 'static unsigned int dcp_notch_twin_count(') + '\n' +
             block(iomfb, 'static int dcp_notch_add_twins('))
    prefix = (f['prefix'] + f['epoch'] + f['metadata'] + f['header']).replace(
        'static inline ', 'static inline __attribute__((unused)) ')
    # The J615 panel timing as its DCP reports it (2880x1864 at 60 Hz, from our
    # own boot log), with the 64 rows of its apple,notch-height.
    m = copy.deepcopy(f['timing_fixture'][0])
    m['HorizontalAttributes'].update(Active=2880, FrontPorch=8, SyncWidth=32, Total=2960)
    m['VerticalAttributes'].update(Active=1864, FrontPorch=47, SyncWidth=8, Total=1961)
    raw = bytearray(struct.pack('<I', 0xd3))
    f['wire']([m], raw)
    blob = 'static const unsigned char j615[]={%s};' % ','.join(map(str, raw))
    return prefix + f['parser'] + twins + '\n' + blob + NOTCH_MAIN



def notch_console(root):
    """Who re-probes the console when a DRM master drops (apple_drv.c): only
    the first drop after a console probe that a master's full-panel offer
    left with full modes (a boot splash with the notch shown).  Other
    handoffs (greeter to session, user switch) must not make the fbdev
    client re-probe and commit in the gap."""
    drv = (root / 'drivers/gpu/drm/apple/apple_drv.c').read_text()
    return (NOTCH_CONSOLE_PREFIX +
            block(drv, 'static int apple_drm_fbdev_probe(') + '\n' +
            block(drv, 'static void apple_drm_notch_owner(') + '\n' +
            block(drv, 'static void apple_drm_master_set(') + '\n' +
            block(drv, 'static void apple_drm_master_drop(') + '\n' +
            NOTCH_CONSOLE_MAIN)


NOTCH_CONSOLE_PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define xchg(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define min(a, b) ((a) < (b) ? (a) : (b))
enum { DCP_FIRMWARE_V_14_7, DCP_FIRMWARE_V_26_6, DCP_FIRMWARE_V_27_0,
       DCP_FIRMWARE_H17P, DCP_FIRMWARE_V_13_5 };
struct apple_dcp { int fw_compat; u32 notch_rows, notch_panel_rows; bool notch_owned; };
struct drm_crtc { struct apple_dcp *dcp; bool ext; };
struct drm_file { int unused; };
struct work_struct { int scheduled; };
struct drm_device { struct drm_crtc *crtcs; int n; bool unplugged; };
struct apple_drm_private {
    struct drm_device drm;
    struct work_struct notch_console;
    bool notch_console_stale;
};
struct drm_fb_helper { struct drm_device *dev; };
struct drm_fb_helper_surface_size { u32 surface_width, surface_height, fb_width, fb_height,
                                    surface_bpp, surface_depth; };
static struct apple_drm_private apple;
#define container_of(p, t, m) ((t *)((char *)(p) - __builtin_offsetof(t, m)))
static struct apple_drm_private *to_apple_drm(struct drm_device *drm)
{ return container_of(drm, struct apple_drm_private, drm); }
#define drm_for_each_crtc(c, dev) \
    for (int _i = 0; _i < (dev)->n && ((c) = &(dev)->crtcs[_i]); _i++)
static bool t8132_ext_crtc(struct drm_crtc *c) { return c->ext; }
static struct drm_crtc *to_apple_crtc(struct drm_crtc *c) { return c; }
static struct apple_dcp *platform_get_drvdata(struct apple_dcp *d) { return d; }
static bool dcp_notch_dynamic(const struct apple_dcp *dcp)
{ return dcp->fw_compat != DCP_FIRMWARE_V_14_7 && dcp->fw_compat != DCP_FIRMWARE_V_26_6; }
static bool drm_dev_is_unplugged(struct drm_device *d) { return d->unplugged; }
static int kicks;
static void schedule_work(struct work_struct *w) { w->scheduled++; kicks++; }
static int probes;
static int drm_fbdev_dma_driver_fbdev_probe(struct drm_fb_helper *h,
                                            struct drm_fb_helper_surface_size *s)
{ (void)h; (void)s; probes++; return 0; }
'''

NOTCH_CONSOLE_MAIN = r'''
static struct apple_dcp panel = { DCP_FIRMWARE_V_13_5, 64, 1864, false };
static struct drm_crtc crtcs[2];
static struct drm_file file;

/* The console's first probe, at the height of the mode it picked. */
static u32 first_probe(u32 rows)
{
    struct drm_fb_helper h = { &apple.drm };
    struct drm_fb_helper_surface_size s = { 2880, rows, 2880, rows, 32, 24 };

    assert(!apple_drm_fbdev_probe(&h, &s));
    assert(s.fb_height <= s.surface_height);
    return s.surface_height;
}

int main(void)
{
    crtcs[0].dcp = &panel;
    crtcs[1].dcp = &panel; crtcs[1].ext = true;
    apple.drm.crtcs = crtcs; apple.drm.n = 2;

    /* A normal boot: the console probed with no master, notch-free already. */
    assert(first_probe(1800) == 1800 && !apple.notch_console_stale);
    /* Every handoff afterwards leaves the console alone. */
    for (int i = 0; i < 4; i++) {
        apple_drm_master_set(&apple.drm, &file, true);
        assert(panel.notch_owned);
        apple_drm_master_drop(&apple.drm, &file);
        assert(!panel.notch_owned);
    }
    assert(kicks == 0);

    /* A splash held master with the notch shown: the clamp fires... */
    apple_drm_master_set(&apple.drm, &file, true);
    assert(first_probe(1864) == 1800 && apple.notch_console_stale);
    /* ...and only the first drop after it asks the console to re-probe. */
    apple_drm_master_drop(&apple.drm, &file);
    assert(kicks == 1 && apple.notch_console.scheduled == 1 && !apple.notch_console_stale);
    apple_drm_master_set(&apple.drm, &file, false);
    apple_drm_master_drop(&apple.drm, &file);
    assert(kicks == 1);

    /* An unplugged device schedules nothing. */
    apple_drm_master_set(&apple.drm, &file, true);
    assert(first_probe(1864) == 1800 && apple.notch_console_stale);
    apple.drm.unplugged = true;
    apple_drm_master_drop(&apple.drm, &file);
    assert(kicks == 1);
    apple.drm.unplugged = false;
    apple.notch_console_stale = false;

    /* Pipes that keep one geometry for the boot (14.7, 26.6): no clamp, no kick. */
    panel.fw_compat = DCP_FIRMWARE_V_26_6;
    apple_drm_master_set(&apple.drm, &file, true);
    assert(first_probe(1864) == 1864 && !apple.notch_console_stale);
    apple_drm_master_drop(&apple.drm, &file);
    assert(kicks == 1);

    /* No notch rows: nothing. */
    panel.fw_compat = DCP_FIRMWARE_V_13_5; panel.notch_rows = 0;
    assert(first_probe(1864) == 1864 && !apple.notch_console_stale);
    apple_drm_master_drop(&apple.drm, &file);
    assert(kicks == 1 && probes == 5);
    puts("PASS: notch console re-probe only after a stale first probe");
    return 0;
}
'''

NOTCH_MAIN = r'''
int main(void)
{
    struct dcp_parse_ctx c;
    struct dcp_display_mode *m, *hidden;
    unsigned n = 0, nh = 0;
    u32 panel = 0;
    bool pref;

    /* What the boot-time hidden notch produced. */
    assert(!parse(j615, sizeof(j615), &c));
    hidden = enumerate_modes(&c, &nh, 327, 212, 64, true, false);
    assert(!IS_ERR(hidden) && nh == 1 && hidden[0].mode.vdisplay == 1800);

    assert(!parse(j615, sizeof(j615), &c));
    m = enumerate_modes(&c, &n, 327, 212, 0, true, false);
    assert(!IS_ERR(m) && n == 1 && m[0].mode.vdisplay == 1864);
    assert(m[0].mode.vsync_start == 1911 && m[0].mode.vsync_end == 1919 && m[0].mode.vtotal == 1961);
    assert(!dcp_notch_add_twins(&m, &n, 64, &panel));
    assert(n == 2 && panel == 1864 && dcp_notch_twin_count(m, n) == 1);
    assert(!m[0].notch_free && m[1].notch_free && m[0].notch_twin && !m[1].notch_twin);
    /* get_modes() gives the twin its physical height: adj-height-mm, else scaled by rows. */
    assert(m[0].mode.height_mm == 212 && m[1].mode.height_mm == 212);
    assert(dcp_notch_free_mm(m[1].mode.height_mm, 0, 1864, m[1].mode.vdisplay) == 205);
    assert(dcp_notch_free_mm(212, 199, 1864, 1800) == 199 && dcp_notch_free_mm(212, 0, 0, 1800) == 212);
    /* The twin is that same mode: one timing, the notch rows folded into the sync. */
    assert(m[1].mode.vdisplay == hidden[0].mode.vdisplay &&
           m[1].mode.vsync_start == hidden[0].mode.vsync_start &&
           m[1].mode.vsync_end == hidden[0].mode.vsync_end &&
           m[1].mode.vtotal == hidden[0].mode.vtotal &&
           m[1].mode.clock == hidden[0].mode.clock &&
           m[1].mode.hdisplay == hidden[0].mode.hdisplay &&
           m[1].mode.hsync_start == hidden[0].mode.hsync_start &&
           m[1].mode.htotal == hidden[0].mode.htotal &&
           m[1].mode.flags == hidden[0].mode.flags);
    assert(m[1].mode.vdisplay == 1800 && m[1].mode.vsync_start == 1847);
    assert(m[1].timing_mode_id == m[0].timing_mode_id && m[1].color_mode_id == m[0].color_mode_id);
    assert((m[1].mode.type & DRM_MODE_TYPE_PREFERRED) == (m[0].mode.type & DRM_MODE_TYPE_PREFERRED));
    assert(dcp_notch_twin_of(&m[0].mode, &m[1].mode, 64));
    assert(!dcp_notch_twin_of(&m[1].mode, &m[0].mode, 64));
    assert(dcp_notch_twins(&m[0].mode, &m[1].mode, 64) && dcp_notch_twins(&m[1].mode, &m[0].mode, 64));
    assert(!dcp_notch_twins(&m[0].mode, &m[1].mode, 63) && !dcp_notch_twins(&m[0].mode, &m[0].mode, 64));
    {
        struct drm_display_mode other = m[1].mode;

        other.clock++;
        assert(!dcp_notch_twins(&m[0].mode, &other, 64));
    }
    /* Scan-out offset: below the notch for the twin only. */
    assert(dcp_notch_offset(&m[1].mode, panel, 64) == 64);
    assert(dcp_notch_offset(&m[0].mode, panel, 64) == 0);
    assert(dcp_notch_offset(&m[1].mode, panel, 0) == 0 && dcp_notch_offset(&m[1].mode, 0, 64) == 0);
    /* A twin gets no twin of its own. */
    assert(!dcp_notch_add_twins(&m, &n, 64, &panel) && n == 3 && dcp_notch_twin_count(m, n) == 2);
    assert(m[2].notch_free && m[2].mode.vdisplay == 1800);
    /* A timing too short for the rows gets no twin and stays unmarked (offered as is). */
    {
        unsigned k = 1;
        struct dcp_display_mode *one = calloc(1, sizeof(*one));

        one[0] = hidden[0];
        assert(!dcp_notch_add_twins(&one, &k, 1800, &panel) && k == 1 && !one[0].notch_twin);
        free(one);
    }
    /* Nobody owns the display: the console gets the notch-free geometry. */
    assert(dcp_notch_effective(DCP_NOTCH_SHOWN, false) == DCP_NOTCH_HIDDEN);
    assert(dcp_notch_effective(DCP_NOTCH_HIDDEN, false) == DCP_NOTCH_HIDDEN);
    assert(dcp_notch_effective(DCP_NOTCH_SHOWN, true) == DCP_NOTCH_SHOWN);
    assert(dcp_notch_effective(DCP_NOTCH_HIDDEN, true) == DCP_NOTCH_HIDDEN);
    /* Offered: the effective geometry only, so a compositor follows the policy... */
    pref = true; assert(dcp_notch_offer(false, DCP_NOTCH_SHOWN, true, &pref) && pref);
    pref = true; assert(!dcp_notch_offer(true, DCP_NOTCH_SHOWN, true, &pref) && !pref);
    pref = true; assert(!dcp_notch_offer(false, DCP_NOTCH_HIDDEN, true, &pref) && !pref);
    pref = true; assert(dcp_notch_offer(true, DCP_NOTCH_HIDDEN, true, &pref) && pref);
    pref = false; assert(dcp_notch_offer(true, DCP_NOTCH_HIDDEN, true, &pref) && !pref);
    /* ...and the notch-free one to the console re-probing within its surface. */
    pref = true; assert(dcp_notch_offer(true, DCP_NOTCH_SHOWN, false, &pref) && pref);
    pref = true; assert(!dcp_notch_offer(false, DCP_NOTCH_SHOWN, false, &pref));
    pref = true; assert(dcp_notch_offer(true, DCP_NOTCH_HIDDEN, false, &pref) && pref);
    assert(dcp_notch_full_fits(0, 1864) && dcp_notch_full_fits(1864, 1864));
    assert(!dcp_notch_full_fits(1800, 1864) && dcp_notch_full_fits(1800, 0));
    /*
     * The reported physical height is the notch-free one exactly when the
     * probe offers only the notch-free geometry, so the console's bounded
     * re-probe keeps the size of its first probe whatever the policy.
     */
    for (int e = DCP_NOTCH_HIDDEN; e <= DCP_NOTCH_SHOWN; e++)
        for (int ff = 0; ff <= 1; ff++) {
            bool pf = true, pu = true;
            bool f_off = dcp_notch_offer(true, e, ff, &pf);
            bool u_off = dcp_notch_offer(false, e, ff, &pu);

            assert(dcp_notch_free_only(e, ff) == (f_off && !u_off));
        }
    assert(dcp_notch_free_only(DCP_NOTCH_SHOWN, false));
    assert(!dcp_notch_free_only(DCP_NOTCH_SHOWN, true));
    /* No notch rows: no twins. */
    {
        unsigned k = 1;
        u32 rows = 7;

        assert(!dcp_notch_add_twins(&hidden, &k, 0, &rows) && k == 1 && rows == 0);
    }
    free(hidden);
    free(m);
    return 0;
}
'''


def refresh(root):
    h=(root/'drivers/gpu/drm/apple/dcp-refresh.h').read_text().replace('#include <linux/types.h>','')
    return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
''' + h + r'''
int main(void) {
    u32 lo=0,hi=0;
    assert(dcp_refresh_range(120U<<16,0,0,true,&lo,&hi));
    assert(lo==24U<<16 && hi==120U<<16);
    assert(!dcp_refresh_range(120U<<16,0,0,false,&lo,&hi));
    assert(!dcp_refresh_range(60U<<16,0,0,true,&lo,&hi));
    assert(!dcp_refresh_range(120U<<16,0,120U<<16,true,&lo,&hi));
    assert(!dcp_refresh_range(120U<<16,120U<<16,24U<<16,true,&lo,&hi));
    assert(!dcp_refresh_range(120U<<16,24U<<16,144U<<16,true,&lo,&hi));
    assert(dcp_desktop_present_deadline(true,true)==120);
    assert(dcp_desktop_present_deadline(true,false)==0);
    assert(dcp_desktop_present_deadline(false,true)==0);
    assert(dcp_refresh_needs_deadline(48U<<16,false,true,true));
    assert(dcp_refresh_needs_deadline(60U<<16,false,true,true));
    assert(!dcp_refresh_needs_deadline(48U<<16,false,true,false));
    assert(!dcp_refresh_needs_deadline(60U<<16,false,false,true));
    assert(dcp_refresh_needs_deadline(120U<<16,false,true,false));
    assert(dcp_refresh_needs_deadline(48U<<16,true,false,false));
    return 0;
}
'''


def takeover(root):
    prefix=backlight(root).split('static void completed(')[0]
    prefix=prefix.replace('static inline ', 'static inline __attribute__((unused)) ')
    function=block((root/'drivers/gpu/drm/apple/dcp_backlight.c').read_text(),
                   'int dcp_backlight_takeover(')
    return prefix + r'''
#define DCP_FIRMWARE_V_26_6_DESKTOP 5
#define READ_ONCE(x) (x)
#define spin_lock_irqsave(p,f) do {assert(!*(p));*(p)=true;(f)=0;} while(0)
#define spin_unlock_irqrestore(p,f) do {assert(*(p));*(p)=false;(void)(f);} while(0)
struct apple_dcp {
    int fw_compat;
    struct {u32 maximum;void *bl_dev;} brightness;
    struct {bool lock;struct dcp_backlight_state state;} backlight;
    struct {bool inherited_brightness_valid;u32 inherited_brightness;} *desktop;
};
static unsigned legacy,notifications;
static void dcp_backlight_sync_hint(struct apple_dcp *d)
{assert(d->brightness.bl_dev);notifications++;}
bool dcp_backlight_active(struct apple_dcp *d) {return d->fw_compat==1 && d->backlight.state.ready;}
static int iomfb_configure_backlight_h17p(struct apple_dcp *d,u32 max,bool valid,u32 nits,bool fallback,u32 guessed)
{assert(max==600 && valid && nits==100 && !fallback && !guessed);legacy++;return 0;}
''' + function + r'''
int main(void) {
    struct apple_dcp d={.fw_compat=5,.brightness={600}};
    __typeof__(*d.desktop) saved={0};d.desktop=&saved;
    assert(!dcp_backlight_takeover(&d,0));
    assert(saved.inherited_brightness_valid && !saved.inherited_brightness && !legacy);
    assert(!dcp_backlight_takeover(&d,500000) && !saved.inherited_brightness);
    saved.inherited_brightness_valid=false;
    assert(dcp_backlight_takeover(&d,1300000)==-ERANGE && !saved.inherited_brightness_valid);
    assert(!dcp_backlight_takeover(&d,700000) && saved.inherited_brightness==600);
    assert(!dcp_bl_init(&d.backlight.state,600,false,0,true,300));
    d.brightness.bl_dev=&d;saved.inherited_brightness_valid=false;
    assert(!dcp_backlight_takeover(&d,100000) && saved.inherited_brightness_valid);
    assert(dcp_bl_actual(&d.backlight.state)==100 && !d.backlight.state.dirty && notifications==1);
    assert(!dcp_backlight_takeover(&d,200000) && d.backlight.state.target==100 && notifications==1);
    saved.inherited_brightness_valid=false;
    assert(!dcp_bl_request(&d.backlight.state,250,false,false));
    assert(!dcp_backlight_takeover(&d,100000) && !saved.inherited_brightness_valid);
    d.fw_compat=1;d.backlight.state.ready=false;
    assert(!dcp_backlight_takeover(&d,100000) && legacy==1);
    return 0;
}
'''


def watchdog(root):
    source=(root/'drivers/gpu/drm/apple/dcp.c').read_text()
    production='\n'.join(block(source, marker) for marker in (
        'bool dcp_desktop_present_complete(',
        'VISIBLE_IF_KUNIT void dcp_swap_watchdog(',
        'void dcp_swap_watchdog_arm(',
        'void dcp_swap_watchdog_complete('))
    internal=(root/'drivers/gpu/drm/apple/dcp-internal.h').read_text()
    helpers='\n'.join(block(internal, marker) for marker in (
        'static inline bool dcp_desktop_swap_watched(',
        'static inline void dcp_takeover_adopt_proven('))
    definitions='\n'.join(line for line in source.splitlines()
                          if line.startswith('#define DCP_SWAP_WATCHDOG_'))
    return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef uint32_t u32;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define xchg(p,v) __atomic_exchange_n((p),(v),__ATOMIC_SEQ_CST)
#define VISIBLE_IF_KUNIT
#define DCP_FIRMWARE_V_26_6_DESKTOP 5
#define dev_warn(...) ((void)0)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define to_delayed_work(p) container_of(p,struct delayed_work,work)
#define msecs_to_jiffies(n) (n)
#define time_before(a,b) ((long)((a)-(b))<0)
#define system_dfl_wq NULL
static unsigned long jiffies;
struct work_struct {unsigned unused;};
struct delayed_work {struct work_struct work;};
struct apple_dcp {
    int fw_compat;bool vrr_enabled,typec,panel;
    struct {u32 present_id;bool present_pending,vrr_failed,takeover_adopted,mode_live;} *desktop;
    struct {bool valid;} mode_state;
    struct delayed_work swap_watchdog_wq;
    unsigned long swap_watchdog_deadline;
    unsigned swap_watchdog_retrains;
    void *crtc,*connector,*dev;
};
static unsigned scheduled,cancelled,vblanks,hotplugs,unarmed,rejected;
static bool allow_retire=true;
static bool iomfb_retire_dropped_present_h17p(struct apple_dcp *d) {return allow_retire;}
static void dcp_unarm_retained_framebuffers(struct apple_dcp *d,u32 id)
{assert(id==d->desktop->present_id);unarmed++;}
static void dcp_backlight_desktop_finish(struct apple_dcp *d,u32 id,bool reply,bool accepted)
{assert(id==d->desktop->present_id && reply && !accepted);rejected++;}
static void dcp_mode_invalidate(__typeof__(((struct apple_dcp *)0)->mode_state) *m) {m->valid=false;}
static void dcp_drm_crtc_vblank(void *c) {vblanks++;}
static void dcp_queue_hotplug(void *c) {hotplugs++;}
static bool dcp_is_typec_output(struct apple_dcp *d) {return d->typec;}
static bool dcp_has_panel(struct apple_dcp *d) {return d->panel;}
static void mod_delayed_work(void *q,struct delayed_work *w,unsigned long delay)
{assert(delay==1000);scheduled++;}
static void cancel_delayed_work(struct delayed_work *w) {cancelled++;}
''' + definitions + '\n' + helpers + '\n' + production + r'''
int main(void) {
    struct apple_dcp d={.fw_compat=5,.vrr_enabled=true,.panel=true,.mode_state={true},.connector=(void *)1};
    __typeof__(*d.desktop) state={.present_id=42,.present_pending=true};d.desktop=&state;
    dcp_swap_watchdog_arm(&d);assert(scheduled==1 && d.swap_watchdog_deadline==1000);
    jiffies=999;dcp_swap_watchdog(&d.swap_watchdog_wq.work);
    assert(d.mode_state.valid && !hotplugs && !unarmed && state.present_pending);
    assert(!dcp_desktop_present_complete(&d,41) && state.present_pending);
    assert(dcp_desktop_present_complete(&d,42) && !state.present_pending);
    assert(!dcp_desktop_present_complete(&d,42));
    dcp_swap_watchdog_complete(&d);assert(cancelled==1);
    jiffies=1000;dcp_swap_watchdog(&d.swap_watchdog_wq.work);assert(!hotplugs);
    /* Expiry owns this dropped swap, retains its buffers and triggers reset. */
    state.present_id=43;state.present_pending=true;
    dcp_swap_watchdog(&d.swap_watchdog_wq.work);
    assert(state.vrr_failed && !d.vrr_enabled && !state.present_pending);
    assert(!d.mode_state.valid && hotplugs==1 && vblanks==1 && unarmed==1 && rejected==1);
    assert(!dcp_desktop_present_complete(&d,43));
    /* A late completion cannot finish the new fixed-refresh present. */
    state.present_id=44;state.present_pending=true;
    assert(!dcp_desktop_present_complete(&d,43) && state.present_pending);
    assert(dcp_desktop_present_complete(&d,44));
    unsigned before=scheduled;dcp_swap_watchdog_arm(&d);assert(scheduled==before);
    /* The established external watchdog remains armed on a Type-C output. */
    d.fw_compat=1;d.typec=true;dcp_swap_watchdog_arm(&d);assert(scheduled==before+1);
    allow_retire=false;d.mode_state.valid=true;
    dcp_swap_watchdog(&d.swap_watchdog_wq.work);assert(d.mode_state.valid && hotplugs==1);
    /* A takeover that adopted the loader's mode is watched until a swap
     * completes; with none, the mode is set through the hotplug worker. */
    allow_retire=true;d.fw_compat=5;d.typec=false;d.vrr_enabled=false;
    state.takeover_adopted=true;state.mode_live=true;
    before=scheduled;dcp_swap_watchdog_arm(&d);assert(scheduled==before+1);
    dcp_swap_watchdog_complete(&d);assert(!state.takeover_adopted);
    before=scheduled;dcp_swap_watchdog_arm(&d);assert(scheduled==before);
    state.takeover_adopted=true;state.present_id=45;state.present_pending=true;
    jiffies=d.swap_watchdog_deadline;unsigned hp=hotplugs;
    dcp_swap_watchdog(&d.swap_watchdog_wq.work);
    assert(!state.takeover_adopted && !state.mode_live && !d.mode_state.valid && hotplugs==hp+1);
    /* Any external pipe is watched, not only Type-C: a fixed HDMI port
     * (no panel, no Type-C route, no watched desktop swap) arms too. */
    d.fw_compat=1;d.typec=false;d.vrr_enabled=false;state.takeover_adopted=false;
    before=scheduled;dcp_swap_watchdog_arm(&d);assert(scheduled==before);
    d.panel=false;dcp_swap_watchdog_arm(&d);assert(scheduled==before+1);
    return 0;
}
'''


def brightness_commit(root):
    source=(root/'drivers/gpu/drm/apple/dcp_backlight.c').read_text()
    production=block(source,'static int drm_crtc_set_brightness(')
    return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#define DRM_MODESET_ACQUIRE_INTERRUPTIBLE 0
#define IS_ERR(p) false
#define PTR_ERR(p) (-EINVAL)
struct drm_crtc_state {bool active,color_mgmt_changed,planes_changed;void *gamma_lut;};
struct drm_crtc {void *dev;int mutex;};
struct drm_modeset_acquire_ctx {int unused;};
struct drm_atomic_commit {struct drm_modeset_acquire_ctx *acquire_ctx;};
struct apple_dcp {bool desktop,pending;struct {bool update;} brightness;struct {struct drm_crtc base;} *crtc;};
static struct drm_atomic_commit commit;
static struct drm_crtc_state state;
static unsigned allocations,commits,planes;
static bool fail_alloc;
static bool dcp_backlight_desktop(struct apple_dcp *d) {return d->desktop;}
static bool dcp_backlight_pending(struct apple_dcp *d) {return d->pending;}
static struct drm_atomic_commit *drm_atomic_commit_alloc(void *dev)
{allocations++;return fail_alloc?NULL:&commit;}
static void drm_modeset_acquire_init(struct drm_modeset_acquire_ctx *c,int flags) {}
static int drm_modeset_lock(int *m,struct drm_modeset_acquire_ctx *c) {assert(!*m);*m=1;return 0;}
static struct drm_crtc_state *drm_atomic_get_crtc_state(struct drm_atomic_commit *a,struct drm_crtc *c)
{assert(c->mutex);return &state;}
static int drm_atomic_add_affected_planes(struct drm_atomic_commit *a,struct drm_crtc *c)
{assert(c->mutex);planes++;return 0;}
static int drm_atomic_commit(struct drm_atomic_commit *a) {commits++;return 0;}
static void drm_atomic_commit_clear(struct drm_atomic_commit *a) {assert(false);}
static int drm_modeset_backoff(struct drm_modeset_acquire_ctx *c) {assert(false);return 0;}
static void drm_atomic_commit_put(struct drm_atomic_commit *a) {}
static void drm_modeset_drop_locks(struct drm_modeset_acquire_ctx *c) {}
static void drm_modeset_acquire_fini(struct drm_modeset_acquire_ctx *c) {}
''' + production + r'''
int main(void) {
    struct apple_dcp d={.desktop=true,.pending=true,.brightness={true}};
    __typeof__(*d.crtc) crtc={0};d.crtc=&crtc;
    state.active=true;state.gamma_lut=&d;
    assert(!drm_crtc_set_brightness(&d));
    assert(commits==1 && planes==1 && state.planes_changed && !state.color_mgmt_changed);
    crtc.base.mutex=0;state=(struct drm_crtc_state){.active=true};d.desktop=false;
    assert(!drm_crtc_set_brightness(&d));
    assert(commits==2 && planes==2 && state.color_mgmt_changed);
    crtc.base.mutex=0;state=(struct drm_crtc_state){.active=false};d.desktop=true;
    assert(!drm_crtc_set_brightness(&d) && commits==2);
    crtc.base.mutex=0;state.active=true;d.pending=false;
    assert(!drm_crtc_set_brightness(&d) && commits==2);
    fail_alloc=true;assert(drm_crtc_set_brightness(&d)==-ENOMEM && commits==2);
    assert(allocations==5);
    return 0;
}
'''


def hdr_metadata(root):
    """HDR10 metadata wire encoding and caching over the AV service (legacy and AFKv2)."""
    apple = root / 'drivers/gpu/drm/apple'
    header = (apple / 'dcp-hdr-metadata.h').read_text()
    iomfb = (apple / 'iomfb.c').read_text()
    header = '\n'.join(line for line in header.splitlines()
                       if not line.startswith('#include'))
    header = header.replace('static inline ', 'static inline __attribute__((unused)) ')
    # An independent encoding of the same message: the DCPAV IPC header, then
    # the serialized { "Type": "CEA-861:Type1", "Data": <24 bytes> }.
    def tag(last, kind, size):
        return struct.pack('<I', (last << 31) | (kind << 24) | size)

    def pad(raw):
        return raw + b'\0' * (-len(raw) % 4)

    body = struct.pack('<12H', 35400, 14600, 8500, 39850, 6550, 2300,
                       15635, 16450, 1000, 50, 1000, 400)
    obj = struct.pack('<I', 0xd3) + tag(1, 1, 2)
    obj += tag(0, 8, 5) + pad(b'Type\0')
    obj += tag(0, 9, 13) + pad(b'CEA-861:Type1')
    obj += tag(0, 8, 5) + pad(b'Data\0')
    obj += tag(1, 10, 24) + body
    msg = bytearray(0x60)
    struct.pack_into('<HHIII', msg, 0, 0, 1, 0x15, len(obj) + 0x20, 0x69706378)
    struct.pack_into('<I', msg, 0x50, len(obj))
    msg += obj
    expected = ','.join(map(str, msg))
    # selector 0x14: a fixed 0x80-byte message clearing InfoFrame type 0x87
    stop = bytearray(0x80)
    struct.pack_into('<HHIII', stop, 0, 0, 0, 0x14, 0x40, 0x69706378)
    stop[0x40] = 0x87
    expected_stop = ','.join(map(str, stop))
    return r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef uint8_t __u8; typedef uint16_t __u16; typedef uint32_t __u32;
static void put_unaligned_le16(u16 v, void *p) { u8 *b = p; b[0] = v; b[1] = v >> 8; }
static void put_unaligned_le32(u32 v, void *p) { u8 *b = p; for (int i = 0; i < 4; i++) b[i] = v >> (8 * i); }
static u16 __attribute__((unused)) get_unaligned_le16(const void *p) { const u8 *b = p; return b[0] | b[1] << 8; }
static u32 get_unaligned_le32(const void *p) { const u8 *b = p; return b[0] | b[1] << 8 | b[2] << 16 | (u32)b[3] << 24; }
/* include/uapi/drm/drm_mode.h */
struct hdr_metadata_infoframe {
    __u8 eotf; __u8 metadata_type;
    struct { __u16 x, y; } display_primaries[3];
    struct { __u16 x, y; } white_point;
    __u16 max_display_mastering_luminance, min_display_mastering_luminance;
    __u16 max_cll, max_fall;
};
struct hdr_output_metadata {
    __u32 metadata_type;
    union { struct hdr_metadata_infoframe hdmi_metadata_type1; };
};
""" + header + r"""
typedef uint64_t u64;
#define HDMI_EOTF_TRADITIONAL_GAMMA_SDR 0
#define HDMI_EOTF_SMPTE_ST2084 2
#define EPIC_SUBTYPE_STD_SERVICE 0xc0
#define READ_ONCE(x) (x)
#define scoped_guard(kind, lock) for (bool held = true; held; held = false)
#define lockdep_assert_held(lock) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_dbg(...) ((void)0)
struct apple_dcp_afkep { bool wire_v2; };
struct apple_epic_service { struct apple_dcp_afkep *ep; unsigned int refs; };
struct drm_crtc { int dcp; };
struct drm_atomic_commit { int unused; };
struct drm_connector_state {
    struct { size_t length; const void *data; } *hdr_output_metadata;
};
struct apple_dcp {
    bool output_pq, output_hdr, crashed;
    struct {
        struct apple_epic_service *service;
        u64 generation;
        u8 sent[DCP_HDR_DATA_SIZE];
        bool sent_valid, infoframe;
    } hdr;
};
#define to_apple_crtc(crtc) (crtc)
#define dcp_hdr_metadata_live(pdev) true
static const struct drm_connector_state *test_connector;
static int transport_result;
static u32 dcp_status;
static size_t reply_length;
static unsigned int sends, v2_sends, legacy_sends, stop_sends;
static bool reset_during_send;
static struct apple_dcp test_dcp;
static const struct drm_connector_state *dcp_crtc_connector_state(
    struct apple_dcp *dcp, struct drm_crtc *crtc, struct drm_atomic_commit *commit)
{
    (void)dcp; (void)crtc; (void)commit;
    return test_connector;
}
static struct apple_epic_service *afk_service_get(struct apple_epic_service *service)
{
    if (service)
        service->refs++;
    return service;
}
static void afk_service_put(struct apple_epic_service *service)
{
    assert(service->refs == 1);
    service->refs--;
}
""" + block(iomfb, 'static void dcp_hdr_reset_locked(') + r"""
static int mock_hdr_command(struct apple_epic_service *service, u8 type,
    const void *msg, size_t msg_len, void *reply, size_t capacity, u32 *retcode, size_t *received)
{
    u32 selector = get_unaligned_le32((const u8 *)msg + 0x04);

    assert(service->refs == 1 && type == EPIC_SUBTYPE_STD_SERVICE);
    if (selector == DCP_HDR_SELECTOR_SET_STATIC) {
        assert(msg_len == DCP_HDR_MSG_SIZE && capacity == DCP_HDR_MSG_SIZE);
        sends++;
    } else {
        assert(selector == DCP_HDR_SELECTOR_STOP_INFOFRAME);
        assert(msg_len == DCP_HDR_STOP_SIZE && capacity == DCP_HDR_STOP_SIZE);
        stop_sends++;
    }
    if (reset_during_send)
        dcp_hdr_reset_locked(&test_dcp);
    if (transport_result)
        return transport_result;
    *retcode = 0;
    *received = reply_length;
    if (selector == DCP_HDR_SELECTOR_SET_STATIC)
        put_unaligned_le32(dcp_status, (u8 *)reply + DCP_HDR_MSG_STATUS);
    else
        put_unaligned_le32(dcp_status, (u8 *)reply + DCP_HDR_STOP_STATUS);
    return 0;
}
static int afk_send_command_len(struct apple_epic_service *service, u8 type,
    const void *msg, size_t msg_len, void *reply, size_t capacity, u32 *retcode, size_t *received)
{
    assert(service->ep->wire_v2);
    v2_sends++;
    return mock_hdr_command(service, type, msg, msg_len, reply, capacity, retcode, received);
}
static int afk_send_command_with_reply_len(struct apple_epic_service *service, u8 type,
    const void *msg, size_t msg_len, void *reply, size_t capacity, u32 *retcode, size_t *received)
{
    assert(!service->ep->wire_v2);
    legacy_sends++;
    return mock_hdr_command(service, type, msg, msg_len, reply, capacity, retcode, received);
}
""" + block(iomfb, 'static int dcp_hdr_send(') + block(iomfb, 'static void dcp_hdr_infoframe_stop(') + block(iomfb, 'static void dcp_hdr_metadata_sync(') + r"""
static void test_metadata_cache(struct hdr_output_metadata hdr)
{
    struct apple_dcp_afkep ep = { .wire_v2 = false };
    struct apple_epic_service service = { .ep = &ep };
    struct drm_crtc crtc = {};
    struct drm_atomic_commit commit = {};
    struct drm_connector_state conn;
    __typeof__(*conn.hdr_output_metadata) blob = { sizeof(hdr), &hdr };

    conn.hdr_output_metadata = &blob;
    test_connector = &conn;
    test_dcp.output_pq = true;
    test_dcp.output_hdr = true;
    test_dcp.hdr.service = &service;
    reply_length = DCP_HDR_MSG_SIZE;
    transport_result = -EIO;
    dcp_hdr_metadata_sync(&test_dcp, &crtc, &commit);
    assert(sends == 1 && legacy_sends == 1 && !v2_sends && !test_dcp.hdr.sent_valid && !test_dcp.hdr.infoframe);
    transport_result = 0;
    reply_length = DCP_HDR_MSG_STATUS;
    dcp_hdr_metadata_sync(&test_dcp, &crtc, &commit);
    assert(sends == 2 && !test_dcp.hdr.sent_valid);

    reply_length = DCP_HDR_MSG_SIZE;
    reset_during_send = true;
    dcp_hdr_metadata_sync(&test_dcp, &crtc, &commit);
    assert(sends == 3 && !test_dcp.hdr.sent_valid && !test_dcp.hdr.infoframe);
    reset_during_send = false;
    dcp_hdr_metadata_sync(&test_dcp, &crtc, &commit);
    dcp_hdr_metadata_sync(&test_dcp, &crtc, &commit);
    assert(sends == 4 && test_dcp.hdr.sent_valid && test_dcp.hdr.infoframe);

    dcp_hdr_reset_locked(&test_dcp);
    dcp_status = 1;
    dcp_hdr_metadata_sync(&test_dcp, &crtc, &commit);
    dcp_hdr_metadata_sync(&test_dcp, &crtc, &commit);
    assert(sends == 5 && test_dcp.hdr.sent_valid && !test_dcp.hdr.infoframe);
    hdr.hdmi_metadata_type1.max_cll--;
    dcp_status = 0;
    dcp_hdr_metadata_sync(&test_dcp, &crtc, &commit);
    assert(sends == 6 && test_dcp.hdr.infoframe && !service.refs);
    hdr.hdmi_metadata_type1.max_cll++;
    transport_result = -ETIMEDOUT;
    dcp_hdr_metadata_sync(&test_dcp, &crtc, &commit);
    assert(sends == 7 && !test_dcp.hdr.sent_valid && test_dcp.hdr.infoframe);
    hdr.hdmi_metadata_type1.max_cll--;
    transport_result = 0;
    dcp_hdr_metadata_sync(&test_dcp, &crtc, &commit);
    assert(sends == 8 && test_dcp.hdr.sent_valid && !service.refs);

    /* AFKv2 transport (fw 27 / H17P) uses afk_send_command_len for 0x15 and 0x14. */
    ep.wire_v2 = true;
    dcp_hdr_reset_locked(&test_dcp);
    dcp_hdr_metadata_sync(&test_dcp, &crtc, &commit);
    assert(sends == 9 && v2_sends == 1 && test_dcp.hdr.sent_valid && test_dcp.hdr.infoframe);
    reply_length = DCP_HDR_STOP_SIZE;
    dcp_hdr_infoframe_stop(&test_dcp);
    assert(stop_sends == 1 && v2_sends == 2 && !test_dcp.hdr.infoframe && !service.refs);
    puts("PASS: retry transport and short replies, discard stale replies, deduplicate completed commands, AFKv2 0x15/0x14");
}

static const u8 expected[] = {""" + expected + r"""};
static const u8 expected_stop[] = {""" + expected_stop + r"""};
int main(void)
{
    struct hdr_output_metadata hdr = { .metadata_type = 0, .hdmi_metadata_type1 = {
        .eotf = 2, .metadata_type = 0,
        .display_primaries = { { 35400, 14600 }, { 8500, 39850 }, { 6550, 2300 } },
        .white_point = { 15635, 16450 },
        .max_display_mastering_luminance = 1000, .min_display_mastering_luminance = 50,
        .max_cll = 1000, .max_fall = 400 } };
    u8 data[DCP_HDR_DATA_SIZE], msg[DCP_HDR_MSG_SIZE], reply[DCP_HDR_MSG_SIZE] = {0};

    assert(sizeof(expected) == DCP_HDR_MSG_SIZE);
    assert(!dcp_hdr_data(data, &hdr));
    dcp_hdr_message(msg, data);
    for (size_t i = 0; i < sizeof(msg); i++)
        if (msg[i] != expected[i]) {
            fprintf(stderr, "byte %#zx: %#x, expected %#x\n", i, msg[i], expected[i]);
            return 1;
        }
    test_metadata_cache(hdr);
    /* Only static metadata Type 1 is sent. */
    hdr.metadata_type = 1;
    assert(dcp_hdr_data(data, &hdr) == -EINVAL);
    hdr.metadata_type = 0; hdr.hdmi_metadata_type1.metadata_type = 1;
    assert(dcp_hdr_data(data, &hdr) == -EINVAL);
    /* Transport error, AFK return code, short reply, DCP status at 0x40. */
    assert(dcp_hdr_reply(-ETIMEDOUT, 0, reply, sizeof(reply)) == -ETIMEDOUT);
    assert(dcp_hdr_reply(0, 5, reply, sizeof(reply)) == -EIO);
    assert(dcp_hdr_reply(0, 0, reply, 0x43) == -EPROTO);
    assert(dcp_hdr_reply(0, 0, reply, 0x44) == 0);
    put_unaligned_le32(0xe00002c2, reply + 0x40);
    assert(dcp_hdr_reply(0, 0, reply, sizeof(reply)) == -EIO);
    /* Ending HDR: the selector 0x14 stop message, DCP status at 0x70. */
    {
        u8 stop[DCP_HDR_STOP_SIZE], sreply[DCP_HDR_STOP_SIZE] = {0};

        assert(sizeof(expected_stop) == DCP_HDR_STOP_SIZE);
        memset(stop, 0xa5, sizeof(stop));
        dcp_hdr_stop_message(stop);
        assert(!memcmp(stop, expected_stop, sizeof(stop)));
        assert(dcp_hdr_stop_reply(-EIO, 0, sreply, sizeof(sreply)) == -EIO);
        assert(dcp_hdr_stop_reply(0, 3, sreply, sizeof(sreply)) == -EIO);
        assert(dcp_hdr_stop_reply(0, 0, sreply, 0x73) == -EPROTO);
        assert(dcp_hdr_stop_reply(0, 0, sreply, 0x74) == 0);
        /* the 0x15 status offset does not count for a stop */
        put_unaligned_le32(1, sreply + 0x40);
        assert(dcp_hdr_stop_reply(0, 0, sreply, sizeof(sreply)) == 0);
        put_unaligned_le32(1, sreply + 0x70);
        assert(dcp_hdr_stop_reply(0, 0, sreply, sizeof(sreply)) == -EIO);
    }
    printf("PASS: HDR10 static metadata message (%zu bytes), InfoFrame stop and reply checks\n", sizeof(msg));
    return 0;
}
"""


def color_selection(root):
    """Full colour mode selection across RGB/YCbCr 444/422/420, 8/10/12 bpc, range, gamut, PQ/HLG."""
    apple = root / 'drivers/gpu/drm/apple'
    parser_h = (apple / 'parser.h').read_text()
    color_h = (apple / 'dcp-color.h').read_text()
    # Extract enum + struct definitions from parser.h
    p_start = parser_h.index('enum dcp_color_eotf {')
    p_end = parser_h.index('struct dimension {')
    parser_defs = parser_h[p_start:p_end]
    color_defs = '\n'.join(line for line in color_h.splitlines()
                           if not line.startswith('#include'))
    color_defs = color_defs.replace('static inline ', 'static inline __attribute__((unused)) ')
    return r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef uint64_t u64; typedef int64_t s64;
struct drm_display_mode { int unused; };
struct dcp_metadata_ticket { int unused; };
enum hdmi_eotf {
    HDMI_EOTF_TRADITIONAL_GAMMA_SDR = 0,
    HDMI_EOTF_TRADITIONAL_GAMMA_HDR = 1,
    HDMI_EOTF_SMPTE_ST2084 = 2,
    HDMI_EOTF_BT_2100_HLG = 3,
};
enum drm_colorspace {
    DRM_MODE_COLORIMETRY_DEFAULT = 0,
    DRM_MODE_COLORIMETRY_SMPTE_170M_YCC = 1,
    DRM_MODE_COLORIMETRY_BT709_YCC = 2,
    DRM_MODE_COLORIMETRY_XVYCC_601 = 3,
    DRM_MODE_COLORIMETRY_XVYCC_709 = 4,
    DRM_MODE_COLORIMETRY_SYCC_601 = 5,
    DRM_MODE_COLORIMETRY_OPYCC_601 = 6,
    DRM_MODE_COLORIMETRY_OPRGB = 7,
    DRM_MODE_COLORIMETRY_BT2020_CYCC = 8,
    DRM_MODE_COLORIMETRY_BT2020_RGB = 9,
    DRM_MODE_COLORIMETRY_BT2020_YCC = 10,
    DRM_MODE_COLORIMETRY_DCI_P3_RGB_D65 = 11,
    DRM_MODE_COLORIMETRY_DCI_P3_RGB_THEATER = 12,
    DRM_MODE_COLORIMETRY_RGB_WIDE_FIXED = 13,
    DRM_MODE_COLORIMETRY_RGB_WIDE_FLOAT = 14,
    DRM_MODE_COLORIMETRY_BT601_YCC = 15,
};
enum drm_connector_color_format {
    DRM_CONNECTOR_COLOR_FORMAT_AUTO = 0,
    DRM_CONNECTOR_COLOR_FORMAT_RGB444 = 1,
    DRM_CONNECTOR_COLOR_FORMAT_YCBCR444 = 2,
    DRM_CONNECTOR_COLOR_FORMAT_YCBCR422 = 3,
    DRM_CONNECTOR_COLOR_FORMAT_YCBCR420 = 4,
};
enum drm_hdmi_broadcast_rgb {
    DRM_HDMI_BROADCAST_RGB_AUTO = 0,
    DRM_HDMI_BROADCAST_RGB_FULL = 1,
    DRM_HDMI_BROADCAST_RGB_LIMITED = 2,
};
""" + parser_defs + color_defs + r"""
static void add_entry(struct dcp_display_mode *m, u32 id, u8 eotf, u8 fmt,
                      u8 space, u8 range, u8 depth, u8 flags)
{
    assert(m->nr_colors < DCP_MAX_COLOR_ENTRIES);
    m->colors[m->nr_colors++] = (struct dcp_color_entry) {
        .id = id, .eotf = eotf, .format = fmt, .colorimetry = space,
        .range = range, .depth = depth, .flags = flags,
    };
}

int main(void)
{
    struct dcp_display_mode acer = { .sdr_rgb = { .score = 100, .id = 81, .eotf = DCP_EOTF_SDR_GAMMA,
        .format = DCP_COLOR_FORMAT_RGB, .colorimetry = DCP_COLORIMETRY_SRGB,
        .range = DCP_COLOR_YCBCR_RANGE_FULL, .depth = 8 },
        .pq = { .score = 200, .id = 113, .eotf = DCP_EOTF_ST_2084,
        .format = DCP_COLOR_FORMAT_RGB, .colorimetry = DCP_COLORIMETRY_BT2020_RGB,
        .range = DCP_COLOR_YCBCR_RANGE_FULL, .depth = 10 },
        .color_mode_id = 81 };
    struct dcp_color_request req;
    struct dcp_color_mode out;

    /* Populate Acer 1080p60-shaped catalog (enc 3 = YCbCr444, enc 2 = YCbCr422) */
    add_entry(&acer, 81, DCP_EOTF_SDR_GAMMA, DCP_COLOR_FORMAT_RGB, DCP_COLORIMETRY_SRGB, DCP_COLOR_YCBCR_RANGE_FULL, 8, 0);
    add_entry(&acer, 82, DCP_EOTF_SDR_GAMMA, DCP_COLOR_FORMAT_RGB, DCP_COLORIMETRY_SRGB, DCP_COLOR_YCBCR_RANGE_LIMITED, 8, 0);
    add_entry(&acer, 86, DCP_EOTF_SDR_GAMMA, DCP_COLOR_FORMAT_RGB, DCP_COLORIMETRY_SRGB, DCP_COLOR_YCBCR_RANGE_FULL, 10, 0);
    add_entry(&acer, 85, DCP_EOTF_SDR_GAMMA, DCP_COLOR_FORMAT_RGB, DCP_COLORIMETRY_BT2020_RGB, DCP_COLOR_YCBCR_RANGE_FULL, 10, 0);
    add_entry(&acer, 84, DCP_EOTF_SDR_GAMMA, DCP_COLOR_FORMAT_YCBCR444, DCP_COLORIMETRY_BT709, DCP_COLOR_YCBCR_RANGE_LIMITED, 8, 0);
    add_entry(&acer, 83, DCP_EOTF_SDR_GAMMA, DCP_COLOR_FORMAT_YCBCR444, DCP_COLORIMETRY_BT601, DCP_COLOR_YCBCR_RANGE_LIMITED, 8, 0);
    add_entry(&acer, 90, DCP_EOTF_SDR_GAMMA, DCP_COLOR_FORMAT_YCBCR444, DCP_COLORIMETRY_BT709, DCP_COLOR_YCBCR_RANGE_LIMITED, 10, 0);
    add_entry(&acer, 108, DCP_EOTF_ST_2084, DCP_COLOR_FORMAT_YCBCR422, DCP_COLORIMETRY_BT2020_YCC, DCP_COLOR_YCBCR_RANGE_LIMITED, 12, 0);
    add_entry(&acer, 110, DCP_EOTF_ST_2084, DCP_COLOR_FORMAT_YCBCR422_DP, DCP_COLORIMETRY_BT2020_YCC, DCP_COLOR_YCBCR_RANGE_LIMITED, 12, 0);
    add_entry(&acer, 113, DCP_EOTF_ST_2084, DCP_COLOR_FORMAT_RGB, DCP_COLORIMETRY_BT2020_RGB, DCP_COLOR_YCBCR_RANGE_FULL, 10, 0);
    add_entry(&acer, 114, DCP_EOTF_ST_2084, DCP_COLOR_FORMAT_RGB, DCP_COLORIMETRY_BT2020_RGB, DCP_COLOR_YCBCR_RANGE_LIMITED, 10, 0);
    add_entry(&acer, 116, DCP_EOTF_ST_2084, DCP_COLOR_FORMAT_YCBCR444, DCP_COLORIMETRY_BT2020_YCC, DCP_COLOR_YCBCR_RANGE_LIMITED, 10, 0);
    add_entry(&acer, 117, DCP_EOTF_ST_2084, DCP_COLOR_FORMAT_RGB, DCP_COLORIMETRY_BT2020_RGB, DCP_COLOR_YCBCR_RANGE_FULL, 12, 0);
    add_entry(&acer, 120, DCP_EOTF_ST_2084, DCP_COLOR_FORMAT_YCBCR444, DCP_COLORIMETRY_BT2020_YCC, DCP_COLOR_YCBCR_RANGE_LIMITED, 12, 0);
    add_entry(&acer, 121, DCP_EOTF_BT_2100, DCP_COLOR_FORMAT_YCBCR444, DCP_COLORIMETRY_BT2020_YCC, DCP_COLOR_YCBCR_RANGE_LIMITED, 10, 0);
    add_entry(&acer, 122, DCP_EOTF_SDR_GAMMA, DCP_COLOR_FORMAT_YCBCR420, DCP_COLORIMETRY_BT709, DCP_COLOR_YCBCR_RANGE_LIMITED, 8, 0);

    /* Default SDR fast path */
    req = (struct dcp_color_request){ .allow_hdr = true, .allow_dsc = true, .max_bpc = 12 };
    assert(!dcp_select_color_mode(&acer, &req, &out) && out.id == 81);

    /* Default PQ fast path (Mutter sets BT2020_RGB + PQ) */
    req.eotf = HDMI_EOTF_SMPTE_ST2084; req.colorspace = DRM_MODE_COLORIMETRY_BT2020_RGB;
    assert(!dcp_select_color_mode(&acer, &req, &out) && out.id == 113 && out.depth == 10);

    /* Explicit 12 bpc PQ RGB vs 10 bpc PQ RGB vs Limited range */
    req.color_format = DRM_CONNECTOR_COLOR_FORMAT_RGB444; req.max_bpc = 12;
    assert(!dcp_select_color_mode(&acer, &req, &out) && out.id == 117 && out.depth == 12);
    req.max_bpc = 10;
    assert(!dcp_select_color_mode(&acer, &req, &out) && out.id == 113 && out.depth == 10);
    req.broadcast_rgb = DRM_HDMI_BROADCAST_RGB_LIMITED;
    assert(!dcp_select_color_mode(&acer, &req, &out) && out.id == 114 && out.range == DCP_COLOR_YCBCR_RANGE_LIMITED);

    /* Explicit YCbCr 4:4:4 (10b/12b) and 4:2:2 (plain outranks DP tunnel) PQ */
    req = (struct dcp_color_request){ .eotf = HDMI_EOTF_SMPTE_ST2084, .colorspace = DRM_MODE_COLORIMETRY_BT2020_YCC,
        .color_format = DRM_CONNECTOR_COLOR_FORMAT_YCBCR444, .max_bpc = 10, .allow_hdr = true };
    assert(!dcp_select_color_mode(&acer, &req, &out) && out.id == 116 && out.depth == 10);
    req.max_bpc = 12;
    assert(!dcp_select_color_mode(&acer, &req, &out) && out.id == 120 && out.depth == 12);
    req.color_format = DRM_CONNECTOR_COLOR_FORMAT_YCBCR422; req.max_bpc = 12;
    assert(!dcp_select_color_mode(&acer, &req, &out) && out.id == 108); /* plain 422 outranks 422_DP (110) */

    /* HLG support */
    req = (struct dcp_color_request){ .eotf = HDMI_EOTF_BT_2100_HLG, .colorspace = DRM_MODE_COLORIMETRY_BT2020_YCC,
        .max_bpc = 12, .allow_hdr = true };
    assert(!dcp_select_color_mode(&acer, &req, &out) && out.id == 121);

    /* SDR: RGB 8b Limited, RGB 10b wide-gamut BT.2020, YCbCr 4:4:4 BT.709/BT.601, YCbCr 4:2:0 */
    req = (struct dcp_color_request){ .broadcast_rgb = DRM_HDMI_BROADCAST_RGB_LIMITED, .max_bpc = 8 };
    assert(!dcp_select_color_mode(&acer, &req, &out) && out.id == 82);
    req = (struct dcp_color_request){ .colorspace = DRM_MODE_COLORIMETRY_BT2020_RGB, .max_bpc = 10 };
    assert(!dcp_select_color_mode(&acer, &req, &out) && out.id == 85);
    req = (struct dcp_color_request){ .color_format = DRM_CONNECTOR_COLOR_FORMAT_YCBCR444,
        .colorspace = DRM_MODE_COLORIMETRY_BT709_YCC, .max_bpc = 10 };
    assert(!dcp_select_color_mode(&acer, &req, &out) && out.id == 90);
    req.colorspace = DRM_MODE_COLORIMETRY_SMPTE_170M_YCC; req.max_bpc = 8;
    assert(!dcp_select_color_mode(&acer, &req, &out) && out.id == 83);
    req = (struct dcp_color_request){ .color_format = DRM_CONNECTOR_COLOR_FORMAT_YCBCR420, .max_bpc = 8 };
    assert(!dcp_select_color_mode(&acer, &req, &out) && out.id == 122);

    /* MSI 1440p60-shaped timing: PQ RGB/444 unsafe, PQ 422 safe -> Mutter AUTO BT2020_RGB succeeds via 422 (105) */
    struct dcp_display_mode msi = { .sdr_rgb = acer.sdr_rgb,
        .pq = { .score = 10, .id = 105, .eotf = DCP_EOTF_ST_2084,
        .format = DCP_COLOR_FORMAT_YCBCR422, .colorimetry = DCP_COLORIMETRY_BT2020_YCC,
        .range = DCP_COLOR_YCBCR_RANGE_LIMITED, .depth = 12 },
        .color_mode_id = 81 };
    add_entry(&msi, 105, DCP_EOTF_ST_2084, DCP_COLOR_FORMAT_YCBCR422, DCP_COLORIMETRY_BT2020_YCC, DCP_COLOR_YCBCR_RANGE_LIMITED, 12, 0);
    add_entry(&msi, 107, DCP_EOTF_ST_2084, DCP_COLOR_FORMAT_YCBCR422_DP, DCP_COLORIMETRY_BT2020_YCC, DCP_COLOR_YCBCR_RANGE_LIMITED, 12, 0);
    add_entry(&msi, 113, DCP_EOTF_ST_2084, DCP_COLOR_FORMAT_RGB, DCP_COLORIMETRY_BT2020_RGB, DCP_COLOR_YCBCR_RANGE_FULL, 10, DCP_COLOR_FLAG_UNSAFE);
    req = (struct dcp_color_request){ .eotf = HDMI_EOTF_SMPTE_ST2084, .colorspace = DRM_MODE_COLORIMETRY_BT2020_RGB,
        .max_bpc = 12, .allow_hdr = true };
    assert(!dcp_select_color_mode(&msi, &req, &out) && out.id == 105);
    /* Explicit PQ RGB on MSI 1440p60 MUST be rejected (only unsafe RGB exists) */
    req.color_format = DRM_CONNECTOR_COLOR_FORMAT_RGB444;
    assert(dcp_select_color_mode(&msi, &req, &out) == -EINVAL);
    /* Explicit PQ 4:2:2 on MSI 1440p60 picks plain 4:2:2 (105) over DP tunnel (107) */
    req.color_format = DRM_CONNECTOR_COLOR_FORMAT_YCBCR422; req.colorspace = DRM_MODE_COLORIMETRY_BT2020_YCC;
    assert(!dcp_select_color_mode(&msi, &req, &out) && out.id == 105);

    /* Unsafe-only >340 MHz HDMI 2.0 timing (e.g. 1080p180 / 1440p144): SDR already uses unsafe
     * elements (sdr_usable=false, sdr_unsafe=true), so non-DSC unsafe PQ within sdr_unsafe_max_bpp
     * is permitted, whereas safe timings (sdr_usable=true) strictly reject unsafe SDR and PQ. */
    struct dcp_display_mode msi144 = { .sdr_rgb = acer.sdr_rgb, .color_mode_id = 80,
        .sdr_usable = false, .sdr_unsafe = true, .sdr_unsafe_max_bpp = 24,
        .pq = { .score = 10, .id = 105, .eotf = DCP_EOTF_ST_2084,
        .format = DCP_COLOR_FORMAT_YCBCR422, .colorimetry = DCP_COLORIMETRY_BT2020_YCC,
        .range = DCP_COLOR_YCBCR_RANGE_LIMITED, .depth = 12 } };
    add_entry(&msi144, 80, DCP_EOTF_SDR_GAMMA, DCP_COLOR_FORMAT_RGB, DCP_COLORIMETRY_SRGB, DCP_COLOR_YCBCR_RANGE_FULL, 8, DCP_COLOR_FLAG_UNSAFE);
    add_entry(&msi144, 113, DCP_EOTF_ST_2084, DCP_COLOR_FORMAT_RGB, DCP_COLORIMETRY_BT2020_RGB, DCP_COLOR_YCBCR_RANGE_FULL, 10, DCP_COLOR_FLAG_UNSAFE | DCP_COLOR_FLAG_DSC);
    add_entry(&msi144, 114, DCP_EOTF_ST_2084, DCP_COLOR_FORMAT_RGB, DCP_COLORIMETRY_BT2020_RGB, DCP_COLOR_YCBCR_RANGE_FULL, 10, DCP_COLOR_FLAG_UNSAFE);
    add_entry(&msi144, 105, DCP_EOTF_ST_2084, DCP_COLOR_FORMAT_YCBCR422, DCP_COLORIMETRY_BT2020_YCC, DCP_COLOR_YCBCR_RANGE_LIMITED, 12, DCP_COLOR_FLAG_UNSAFE);
    req = (struct dcp_color_request){ .eotf = HDMI_EOTF_SMPTE_ST2084, .colorspace = DRM_MODE_COLORIMETRY_BT2020_YCC,
        .color_format = DRM_CONNECTOR_COLOR_FORMAT_YCBCR422, .max_bpc = 12,
        .allow_hdr = true, .allow_dsc = true, .allow_unsafe_sdr = true };
    assert(!dcp_select_color_mode(&msi144, &req, &out) && out.id == 105);
    /* Non-DSC 30-bpp RGB PQ (114) exceeds 24-bpp unsafe SDR ceiling -> rejected; 30-bpp SDR admits it */
    req.color_format = DRM_CONNECTOR_COLOR_FORMAT_RGB444;
    req.colorspace = DRM_MODE_COLORIMETRY_BT2020_RGB;
    assert(dcp_select_color_mode(&msi144, &req, &out) == -EINVAL);
    msi144.sdr_unsafe_max_bpp = 30;
    assert(!dcp_select_color_mode(&msi144, &req, &out) && out.id == 114);
    msi144.sdr_unsafe_max_bpp = 24;
    /* DSC-required unsafe PQ (113) MUST be rejected even with allow_dsc=true and 30-bpp SDR */
    msi144.colors[2].flags |= DCP_COLOR_FLAG_DSC;
    msi144.sdr_unsafe_max_bpp = 30;
    assert(dcp_select_color_mode(&msi144, &req, &out) == -EINVAL);
    /* Same unsafe entry on a timing with sdr_usable=true MUST be rejected (for both HDR and SDR) */
    msi144.sdr_usable = true;
    req.color_format = DRM_CONNECTOR_COLOR_FORMAT_YCBCR422;
    req.colorspace = DRM_MODE_COLORIMETRY_BT2020_YCC;
    assert(dcp_select_color_mode(&msi144, &req, &out) == -EINVAL);
    /* Explicit SDR request on a safe timing (e.g. 4K60 where only 4:2:0 is safe) MUST NOT fall through to unsafe SDR */
    struct dcp_display_mode k4 = { .color_mode_id = 119, .sdr_usable = true, .sdr_unsafe = true };
    add_entry(&k4, 80, DCP_EOTF_SDR_GAMMA, DCP_COLOR_FORMAT_RGB, DCP_COLORIMETRY_SRGB, DCP_COLOR_YCBCR_RANGE_FULL, 8, DCP_COLOR_FLAG_UNSAFE);
    add_entry(&k4, 119, DCP_EOTF_SDR_GAMMA, DCP_COLOR_FORMAT_YCBCR444, DCP_COLORIMETRY_BT709, DCP_COLOR_YCBCR_RANGE_LIMITED, 8, DCP_COLOR_FLAG_DOWNSTREAM_420);
    req = (struct dcp_color_request){ .color_format = DRM_CONNECTOR_COLOR_FORMAT_RGB444, .max_bpc = 8, .allow_unsafe_sdr = true };
    assert(dcp_select_color_mode(&k4, &req, &out) == -EINVAL);
    /* DownstreamFormat 4:2:0 matches YCBCR420 and reports wire YCbCr 4:2:0 while preserving upstream format */
    req.color_format = DRM_CONNECTOR_COLOR_FORMAT_YCBCR420;
    assert(!dcp_select_color_mode(&k4, &req, &out) && out.id == 119 && out.format == DCP_COLOR_FORMAT_YCBCR420);
    assert(k4.colors[1].format == DCP_COLOR_FORMAT_YCBCR444);

    puts("PASS: colour mode selection across RGB/YCbCr 444/422/420, 8/10/12 bpc, Full/Limited, wide gamut, PQ/HLG, and -EINVAL rejection");
    return 0;
}
"""
