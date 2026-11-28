# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Copyright 2026 Ryan Murray
"""RTKit event dispatch and ACK policies with public synthetic messages."""
from common import block

def oslog(root):
    source = (root / 'drivers/soc/apple/rtkit.c').read_text()
    constants = '\n'.join(line for line in source.splitlines()
                          if line.startswith('#define APPLE_RTKIT_OSLOG_'))
    # The echo's predicate, where the receiver is built on one.
    helpers = ''.join(block(source, marker) + '\n'
                      for marker in ('static bool apple_rtkit_oslog_unanswered(',
                                     'static bool apple_rtkit_oslog_echoed(')
                      if marker in source)
    return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint8_t u8;
#define GENMASK_ULL(h,l) ((~0ULL << (l)) & (~0ULL >> (63-(h))))
#define FIELD_GET(mask,value) (((value) & (mask)) >> __builtin_ctzll(mask))
#define APPLE_RTKIT_EP_OSLOG 8
struct buffer { unsigned sentinel; unsigned size; };
/* v27_rx_failed: read only by the J714s firmware 27 sleep; a failed echo sets it. */
struct apple_rtkit { void *dev; struct buffer oslog_buffer; bool oslog_inherited, sync_syslog_ack, oslog_echo; u64 oslog_owed; bool v27_rx_failed; };
#define cmpxchg(p,o,n) __sync_val_compare_and_swap((p),(o),(n))
#define WRITE_ONCE(x,v) ((x)=(v))
/* As the shared Apple configuration builds it. */
#define CONFIG_APPLE_SOC_SLEEP 1
#define IS_ENABLED(option) (option)
static unsigned warnings, notifications, requests, acks, errors;
static u64 observed;
static bool neo, m5;
#define of_machine_is_compatible(s) ((!strcmp(s,"apple,t8140") && neo) || (!strcmp(s,"apple,t8142") && m5))
#define dev_warn(dev,fmt,msg) do { (void)(dev); (void)(fmt); warnings++; observed=(msg); } while (0)
#define dev_warn_ratelimited(dev,fmt,msg) dev_warn(dev,fmt,msg)
#define dev_dbg(dev,fmt,msg) do { (void)(dev); (void)(fmt); notifications++; observed=(msg); } while (0)
#define dev_err(dev,fmt,msg) do { (void)(dev); (void)(fmt); (void)(msg); errors++; } while (0)
static int __attribute__((unused)) apple_rtkit_send_message(struct apple_rtkit *rtk, u8 ep, u64 msg, void *completion, bool atomic)
{ (void)rtk; assert(ep==8 && !completion && !atomic); acks++; observed=msg; return 0; }
static void apple_rtkit_common_rx_get_buffer(struct apple_rtkit *rtk, struct buffer *b, u8 ep, u64 msg)
{ assert(b==&rtk->oslog_buffer && ep==8); requests++; observed=msg; }
''' + constants + '\n' + helpers + block(source, 'static void apple_rtkit_oslog_rx(') + r'''
static void reset(void) { warnings=notifications=requests=acks=errors=0; observed=0; }
int main(void)
{
    struct apple_rtkit rtk={.oslog_buffer={.sentinel=0x12345678}};
    neo=true;
    for (u8 type=2; type<=5; type++) {
        for (unsigned edge=0; edge<2; edge++) {
            u64 msg=((u64)type<<56) | (edge ? ((1ULL<<56)-1) : 0);
            reset(); apple_rtkit_oslog_rx(&rtk,msg);
            assert(!warnings && notifications==1 && !requests && !acks && !errors);
            assert(observed==msg);
            assert(rtk.oslog_buffer.sentinel==0x12345678);
        }
    }
    puts("PASS recognized opaque types preserve both payload bounds without buffer/ACK operations");
    const u8 unknown[]={0,6,0xff};
    for (unsigned i=0;i<sizeof(unknown);i++) {
        u64 msg=((u64)unknown[i]<<56) | 42;
        reset(); apple_rtkit_oslog_rx(&rtk,msg);
        assert(warnings==1 && !notifications && !requests && observed==msg);
    }
    puts("PASS unknown types remain warnings");
    reset(); apple_rtkit_oslog_rx(&rtk,1ULL<<56);
    assert(requests==1 && !warnings && !notifications && observed==(1ULL<<56));
    puts("PASS buffer requests retain existing dispatch");
#ifdef APPLE_RTKIT_OSLOG_LOG
    /* Other machines acknowledge only admitted channel-zero counters. */
    neo=m5=false;
    reset(); apple_rtkit_oslog_rx(&rtk,2ULL<<56);
    assert(warnings==1 && !acks && !notifications);
    for (unsigned inherited=0; inherited<2; inherited++) {
        rtk.oslog_buffer.size=inherited ? 0 : 4096;
        rtk.oslog_inherited=inherited;
        reset(); apple_rtkit_oslog_rx(&rtk,(2ULL<<56)|UINT32_MAX);
        assert(acks==1 && !warnings && observed==((2ULL<<56)|UINT32_MAX));
        reset(); apple_rtkit_oslog_rx(&rtk,(2ULL<<56)|(1ULL<<32));
        assert(warnings==1 && !acks);
    }
    /* Each tested platform policy overrides channel and buffer admission. */
    rtk.oslog_inherited=false;
    for (unsigned policy=0; policy<3; policy++) {
        neo=policy==0; m5=policy==1; rtk.sync_syslog_ack=policy==2;
        reset(); apple_rtkit_oslog_rx(&rtk,(2ULL<<56)|(1ULL<<32));
        assert(notifications==1 && !warnings && !acks);
    }
    puts("PASS channel-zero ACK admission and Neo/M5/synchronous no-ACK policies");
#else
    (void)neo; (void)m5;
#endif
    return 0;
}
'''


def syslog(root):
    source = (root / 'drivers/soc/apple/rtkit.c').read_text()
    dcp = (root / 'drivers/gpu/drm/apple/dcp.c').read_text()
    header = (root / 'drivers/gpu/drm/apple/dcp-syslog.h').read_text()
    helper = header[header.index('static inline bool'):header.index('\n#endif')]
    # The policy covers both DCP transports, the internal panel's and the
    # external H17P one. It demotes only the known records it matches, so other
    # records, other firmware and other RTKit clients keep info logging.
    assert '.syslog_is_debug = dcp_rtk_syslog_is_debug' in block(dcp, 'static struct apple_rtkit_ops rtkit_ops =')
    assert '.syslog_is_debug = dcp_rtk_syslog_is_debug' in block(dcp, 'static const struct apple_rtkit_ops external_h17p_rtkit_ops =')
    return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint64_t u64;
#define APPLE_RTKIT_EP_SYSLOG 2
/* is_mapped without a buffer: a firmware-owned log, which no buffer here is. */
struct buffer { size_t size; unsigned char *buffer; bool iomem, is_mapped; };
struct apple_rtkit_ops {
    bool (*syslog_is_debug)(void *, const char *, const char *);
};
struct apple_rtkit {
    void *dev, *cookie;
    struct apple_rtkit_ops *ops;
    bool syslog_inherited;
    char *syslog_msg_buffer;
    size_t syslog_msg_size, syslog_n_entries;
    struct buffer syslog_buffer;
};
static unsigned infos, debugs, warnings, copies, acks, calls;
#define dev_info(...) (++infos)
#define dev_dbg(...) (++debugs)
#define dev_warn_ratelimited(...) (++warnings)
static void apple_rtkit_memcpy(struct apple_rtkit *rtk, void *out,
                              struct buffer *b, size_t off, size_t len)
{ (void)rtk; assert(off + len <= b->size); copies++; memcpy(out, b->buffer+off, len); }
static int apple_rtkit_send_message(struct apple_rtkit *rtk, u8 ep, u64 msg, void *c, bool atomic)
{ (void)rtk; assert(ep==2 && msg==0 && c==NULL && !atomic); acks++; return 0; }
typedef uint32_t u32;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
struct apple_dcp { struct { bool only_120hz_modes; } hw; u32 boot_rate; };
''' + helper + '\n' + block(dcp, 'static void dcp_rtk_syslog_boot_rate(') + '\n' + block(dcp, 'static bool dcp_rtk_syslog_is_debug(') + r'''
static bool policy(void *cookie, const char *context, const char *message)
{ calls++; return dcp_rtk_syslog_is_debug(cookie, context, message); }
''' + block(source, 'static bool should_crop_syslog_char(') + '\n' + block(source, 'static void apple_rtkit_syslog_rx_log(') + r'''
static void reset(void) { infos=debugs=warnings=copies=acks=calls=0; }
static void record(struct apple_rtkit *rtk, const char *context, const char *message)
{
    memset(rtk->syslog_buffer.buffer,0,rtk->syslog_buffer.size);
    memcpy(rtk->syslog_buffer.buffer+8,context,strlen(context));
    memcpy(rtk->syslog_buffer.buffer+32,message,strlen(message));
}
int main(void)
{
    unsigned char ring[288]; char decoded[256];
    struct apple_rtkit_ops ops={.syslog_is_debug=policy};
    struct apple_dcp dcp={.hw={.only_120hz_modes=true}};
    struct apple_rtkit rtk={.ops=&ops, .cookie=&dcp, .syslog_msg_buffer=decoded,
        .syslog_msg_size=256, .syslog_n_entries=1,
        .syslog_buffer={.size=sizeof(ring),.buffer=ring}};
    const char *messages[]={
        "set_run_mode_safe: deferring: 4 -> 1",
        "set_run_mode_safe: deferring: 2 -> 4",
        "ready_for_run_mode_change: initiating deferred run mode change: 4 -> 1",
        "ready_for_run_mode_change: initiating deferred run mode change: 2 -> 4"
    };
    for (unsigned i=0;i<4;i++) {
        const char *ctx=i<2?"UPPipeDCP_H17P.cpp:2096":"UPPipeDCP_H17P.cpp:2014";
        record(&rtk,ctx,messages[i]); reset(); apple_rtkit_syslog_rx_log(&rtk,0);
        assert(debugs==1 && !infos && !warnings && acks==1 && copies==2 && calls==1);
        char fault[192]; snprintf(fault,sizeof(fault),"%s: timeout",messages[i]);
        record(&rtk,ctx,fault); reset(); apple_rtkit_syslog_rx_log(&rtk,0);
        assert(infos==1 && !debugs && acks==1);
        record(&rtk,"other.cpp:2096",messages[i]); reset(); apple_rtkit_syslog_rx_log(&rtk,0);
        assert(infos==1 && !debugs && acks==1);
    }
    puts("PASS complete transition records use debug; faults and foreign contexts use info; all ACK");
    record(&rtk,"UPPipeDCP_H17P.cpp:2096","set_run_mode_safe: deferring: 4 -> 1\r\n ");
    reset(); apple_rtkit_syslog_rx_log(&rtk,0);
    assert(debugs==1 && acks==1); /* Existing trimming precedes policy. */
    ops.syslog_is_debug=NULL;
    reset(); apple_rtkit_syslog_rx_log(&rtk,0);
    assert(infos==1 && !debugs && !calls && acks==1);
    ops.syslog_is_debug=policy;
    record(&rtk,"UPPipeDCP_H17P.cpp:2096","set_run_mode_safe: error -110");
    reset(); apple_rtkit_syslog_rx_log(&rtk,0);
    assert(infos==1 && !debugs && acks==1);
    rtk.syslog_inherited=true;
    reset(); apple_rtkit_syslog_rx_log(&rtk,0);
    assert(!calls && !copies && !infos && !debugs && acks==1);
    rtk.syslog_inherited=false; rtk.syslog_n_entries=0;
    reset(); apple_rtkit_syslog_rx_log(&rtk,0);
    assert(warnings==1 && !calls && !copies && acks==1);
    puts("PASS default info, trimming, inherited and invalid records retain transport behavior");
    rtk.syslog_n_entries=1;
    /* The rate iBoot left the T6050 panel at: the first one counts, the line stays at info. */
    record(&rtk,"PBlock_PDC_v17.cpp:1954","IOMFB update_rr_index syncrate 60");
    reset(); apple_rtkit_syslog_rx_log(&rtk,0);
    assert(dcp.boot_rate==60 && infos==1 && !debugs && acks==1);
    record(&rtk,"PBlock_PDC_v17.cpp:1954","IOMFB update_rr_index syncrate 120");
    reset(); apple_rtkit_syslog_rx_log(&rtk,0);
    assert(dcp.boot_rate==60 && infos==1);
    dcp.boot_rate=0; dcp.hw.only_120hz_modes=false;
    reset(); apple_rtkit_syslog_rx_log(&rtk,0);
    assert(dcp.boot_rate==0 && infos==1);
    dcp.hw.only_120hz_modes=true;
    record(&rtk,"PBlock_PDC_v17.cpp:1954","IOMFB update_rr_index syncrate 7");
    reset(); apple_rtkit_syslog_rx_log(&rtk,0);
    assert(dcp.boot_rate==0 && infos==1);
    puts("PASS T6050 boot rate: first rate latched, later and out-of-range rates ignored, other panels untouched");
    return 0;
}
'''


def _v27_prelude(root):
    """Host stand-ins for OF and legacy board checks, then the live selector."""
    display_h = (root / 'include/linux/soc/apple/display.h').read_text()
    uuid_defines = [line for line in display_h.splitlines()
                    if line.startswith(('#define APPLE_T6041_DCP_26B123_UUID ',
                                        '#define APPLE_T8122_DCP_26B123_UUID ',
                                        '#define APPLE_T8132_DCP_26B123_UUID '))]
    assert len(uuid_defines) == 3
    helpers = ('static inline bool apple_dcp_image_v27_2(',
               'static inline bool apple_t8132_dcp_v27_2(',
               'static inline bool apple_t6041_dcp_any_v27_2(',
               'static inline bool apple_t6041_dcp_v27_2(',
               'static inline bool apple_t6041_dcpext_v27_2(',
               'static inline bool apple_t8122_dcp_v27_2(',
               'static inline bool\napple_t8122_dcpext_v27_2_for_internal(',
               'static inline bool apple_t8122_dcpext_v27_2(')
    # The T8132 27.2 dcpext admission: by processor and image, never by board. No
    # of_machine_is_compatible() exists in these units, so neither it nor the display.h
    # helpers can depend on the machine.
    t8132_h = (root / 'include/linux/soc/apple/t8132.h').read_text()
    t8132_admission = block(t8132_h, 'static inline bool apple_t8132_dcpext_v27_2(')
    assert 'of_machine_is_compatible' not in t8132_admission
    helper = '\n'.join((*uuid_defines, *(block(display_h, marker) for marker in helpers),
                        t8132_admission))
    iomfb_h = (root / 'drivers/gpu/drm/apple/iomfb.h').read_text()
    methods_decl = iomfb_h[iomfb_h.index('enum dcpep_method {'):
                           iomfb_h.index('#define IOMFB_MAX_CB')]
    v27_c = (root / 'drivers/gpu/drm/apple/iomfb_v27.c').read_text()
    v27_tables = v27_c[v27_c.index('static const struct dcp_method_entry dcp_methods_27_0'):
                       v27_c.index('/* The template\'s calls resolve through')]
    apple = root / 'drivers/gpu/drm/apple'
    swap_h = (apple / 'iomfb_v27_3_swap.h').read_text()
    swap_defines = '\n'.join(line for line in swap_h.splitlines()
                             if line.startswith('#define DCP_V27'))
    dcp_c = (apple / 'dcp.c').read_text()
    version_defs = '\n'.join(line for line in dcp_c.splitlines()
                             if line.startswith('#define DCP_FW_VERSION_'))
    version_enum = block((apple / 'version_utils.h').read_text(),
                         'enum dcp_firmware_version {') + ';'
    admission_enum = block((apple / 'dcp.h').read_text(), 'enum dcp_v27_admission {') + ';'
    selector = '\n'.join(block(dcp_c, marker) for marker in (
        'static int dcp_read_fw_version(',
        'static enum dcp_firmware_version dcp_v27_method_table(',
        'enum dcp_v27_admission dcp_v27_admission(',
        'static bool dcp_firmware_27(',
        'static enum dcp_firmware_version dcp_check_firmware_version(',
    ))
    return r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
typedef uint16_t u16;
typedef uint32_t u32;
''' + swap_defines + '\n' + version_defs + '\n' + version_enum + '\n' + admission_enum + r'''
struct dcp_v14_board;
struct apple_dcp_hw_data { enum dcp_firmware_version firmware_compat; };
struct device_node {
    const char *compatible;
    const char *uuid;
    int uuid_count;
    u32 fw[DCP_FW_VERSION_MAX_LEN];
    int fw_cells;
    u32 compat[DCP_FW_VERSION_MAX_LEN];
    int compat_cells;
    bool clock_ids;
};
struct device {
    struct device_node *of_node;
    const struct apple_dcp_hw_data *hw;
};
static int of_device_is_compatible(const struct device_node *np, const char *s)
{
    return np && np->compatible && !strcmp(np->compatible, s);
}
/* The tree's display processors, for of_find_compatible_node(). */
static struct device_node *of_nodes[2];
static int of_node_refs;
static struct device_node *of_find_compatible_node(struct device_node *from,
                                                    const char *type, const char *s)
{
    assert(!from && !type);
    for (unsigned i = 0; i < 2; i++)
        if (of_device_is_compatible(of_nodes[i], s)) {
            of_node_refs++;
            return of_nodes[i];
        }
    return NULL;
}
static void of_node_put(struct device_node *np)
{
    if (np)
        assert(of_node_refs-- > 0);
}
static int of_property_count_strings(const struct device_node *np, const char *name)
{
    assert(!strcmp(name, "apple,firmware-uuid"));
    return np ? np->uuid_count : -1;
}
static int of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
    assert(!strcmp(name, "apple,firmware-uuid"));
    if (!np || np->uuid_count < 1 || !np->uuid)
        return -1;
    *out = np->uuid;
    return 0;
}
static int of_property_count_u32_elems(const struct device_node *np, const char *name)
{
    if (!np)
        return -1;
    if (!strcmp(name, "apple,firmware-version"))
        return np->fw_cells;
    if (!strcmp(name, "apple,firmware-compat"))
        return np->compat_cells;
    return -1;
}
static int of_property_read_variable_u32_array(const struct device_node *np, const char *name,
                                               u32 *out, int min, int max)
{
    int cells = of_property_count_u32_elems(np, name);
    const u32 *src;

    if (cells < min || cells > max)
        return -EINVAL;
    src = !strcmp(name, "apple,firmware-version") ? np->fw : np->compat;
    memcpy(out, src, cells * sizeof(*out));
    return cells;
}
static int of_property_read_u32_array(const struct device_node *np, const char *name, u32 *out, int n)
{
    int ret = of_property_read_variable_u32_array(np, name, out, n, n);

    return ret < 0 ? ret : 0;
}
#define scnprintf snprintf
#define strscpy(dst, src, size) snprintf((dst), (size), "%s", (src))
#define GFP_KERNEL 0
#define dev_warn(...) ((void)0)
#define IS_ERR(ptr) false
static void dev_err(struct device *dev, const char *format, ...) {}
#define dev_info(...) ((void)0)
/* linux/soc/apple/t8132.h: the M4 backend's 26.6.2 firmware path (or an Air) */
static bool t8132_firmware_path;
static bool apple_t8132_firmware_path(void)
{
    return t8132_firmware_path;
}
static void *devm_kmemdup(struct device *dev, const void *src, size_t len, unsigned int gfp)
{
    (void)dev; (void)src; (void)len; (void)gfp;
    return NULL;
}
static const struct apple_dcp_hw_data *of_device_get_match_data(struct device *dev)
{
    return dev->hw;
}
/* No internal board profile is present in these fixtures; apple,clock-ids is the handoff flag. */
static bool of_property_present(const struct device_node *np, const char *name)
{
    if (!strcmp(name, "apple,clock-ids"))
        return np && np->clock_ids;
    assert(!strcmp(name, "apple,j613-25g83-profile"));
    return false;
}
static bool apple_t8122_25g83_board(void) { return false; }
static int of_property_read_u32(struct device_node *np, const char *name, u32 *out)
{
    return -ENOENT;
}
static const struct dcp_v14_board *iomfb_v14_7_board(struct device *dev) { return NULL; }
static const char *iomfb_v14_7_board_name(const struct dcp_v14_board *board) { return ""; }
static bool ext26;
static bool dcp_ext26_admitted(struct device_node *np) { return ext26; }
''' + helper + '\n' + methods_decl + '\n' + v27_tables + '\n' + selector + '\n'


def v27_panel_handoff(root):
    source = (root / 'drivers/gpu/drm/apple/dcp.c').read_text()
    profiles = ('t6030', 't8122', 't6031', 't6041')
    match = '\n'.join(block(source, f'static const struct apple_dcp_hw_data apple_dcp_hw_{soc} =')
                      + ';' for soc in profiles)
    profile = block(source, 'static void dcp_apply_dt_profile(')
    return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
enum { DCP_FIRMWARE_V_27_0 = 27 };
struct device_node {
    bool rtkit_handoff;
    const char *marker;
    u32 value;
};
struct device { struct device_node *of_node; };
struct apple_dcp_hw_data {
    bool adopt_live_session;
    const char *iboot_handoff;
    unsigned num_dptx_ports;
    u8 firmware_dma_bits;
};
struct apple_dcp {
    struct device *dev;
    struct apple_dcp_hw_data hw;
    int fw_compat;
};
static bool of_property_present(struct device_node *np, const char *name)
{
    assert(!strcmp(name, "apple,rtkit-handoff"));
    return np->rtkit_handoff;
}
static int of_property_read_variable_u32_array(struct device_node *np, const char *name,
                                               u32 *value, int min, int max)
{
    assert(min == 1 && max == 1);
    if (!np->marker || strcmp(name, np->marker))
        return -1;
    *value = np->value;
    return 1;
}
/* All probes see the same DART aperture and panel-only topology. */
static u8 dcp_dart_dma_bits(struct device_node *np) { return 41; }
static bool dcp_is_internal_panel_only(struct apple_dcp *dcp) { return true; }
''' + match + '\n' + profile + r'''
static void check(struct apple_dcp_hw_data hw, struct device_node node, bool live)
{
    struct device dev = { .of_node = &node };
    struct apple_dcp dcp = { .dev = &dev, .hw = hw, .fw_compat = DCP_FIRMWARE_V_27_0 };

    dcp_apply_dt_profile(&dcp);
    assert(dcp.hw.adopt_live_session == live);
    assert(dcp.hw.firmware_dma_bits == 41);
    assert(dcp.hw.num_dptx_ports == 0);
}

int main(void)
{
    const struct {
        const struct apple_dcp_hw_data *hw;
        const char *marker;
    } panels[] = {
        { &apple_dcp_hw_t6030, "apple,t6030-handoff" },
        { &apple_dcp_hw_t8122, "apple,t8122-handoff" },
        { &apple_dcp_hw_t6031, "apple,t6031-handoff" },
        { &apple_dcp_hw_t6041, "apple,t6041-handoff" },
    };

    for (unsigned i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
        struct apple_dcp_hw_data hw = *panels[i].hw;
        struct device_node node = {};

        check(hw, node, false);
        node.rtkit_handoff = true;
        check(hw, node, true);
        node.rtkit_handoff = false;
        node.marker = panels[i].marker;
        check(hw, node, false);
        node.value = 1;
        check(hw, node, true);
        node.marker = panels[i ? 0 : 1].marker;
        check(hw, node, false);
    }
    return 0;
}
'''


def t6041_v27_admission(root):
    return _v27_prelude(root) + r'''
static struct device_node node;
static const struct apple_dcp_hw_data hw;
static struct device dev = { .of_node = &node, .hw = &hw };

static void reset_t6041_26b123(bool ext)
{
    node = (struct device_node){
        .compatible = ext ? "apple,t6041-dcpext" : "apple,t6041-dcp",
        .uuid = APPLE_T6041_DCP_26B123_UUID,
        .uuid_count = 1,
        .fw = { 27, 2, 0 },
        .fw_cells = 3,
        .compat = { 27, 2, 0 },
        .compat_cells = 3,
    };
}

static void expect_rejected(void)
{
    assert(!apple_t6041_dcp_v27_2(&node));
    assert(!apple_t6041_dcpext_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_UNKNOWN);
}

int main(void)
{
    assert(!apple_t6041_dcp_v27_2(NULL));
    assert(!apple_t6041_dcpext_v27_2(NULL));

    /*
     * Internal only matches apple_t6041_dcp_v27_2; external only matches
     * apple_t6041_dcpext_v27_2. No of_machine_is_compatible() stub is defined
     * in this translation unit, enforcing at compile time that admission keys
     * solely on the DCP IP compatible and firmware identity.
     */
    reset_t6041_26b123(false);
    assert(apple_t6041_dcp_v27_2(&node));
    assert(!apple_t6041_dcpext_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_V_27_0);

    reset_t6041_26b123(true);
    assert(!apple_t6041_dcp_v27_2(&node));
    assert(apple_t6041_dcpext_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_V_27_0);

    /* A different SoC follows generic admission, without the T6041 UUID pin. */
    node.compatible = "apple,t6031-dcp";
    node.uuid_count = 0;
    assert(!apple_t6041_dcp_v27_2(&node));
    assert(!apple_t6041_dcpext_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_V_27_0);

    for (unsigned ext = 0; ext < 2; ext++) {
        /* Reject missing, multi-string, or non-26B123 firmware UUIDs (including 26B131). */
        reset_t6041_26b123(ext); node.uuid_count = 0; expect_rejected();
        reset_t6041_26b123(ext); node.uuid_count = 2; expect_rejected();
        reset_t6041_26b123(ext); node.uuid = "00000000-0000-0000-0000-000000000000"; expect_rejected();
        reset_t6041_26b123(ext); node.uuid = "71F140DC-F18D-31B3-9772-537A5CC8400D"; expect_rejected();

        /* Reject malformed or non-27.2.0 firmware/compat tuples. */
        reset_t6041_26b123(ext); node.fw_cells = 2; expect_rejected();
        reset_t6041_26b123(ext); node.compat_cells = 4; expect_rejected();
        reset_t6041_26b123(ext); node.fw[1] = 0; expect_rejected();
        reset_t6041_26b123(ext); node.compat[1] = 0; expect_rejected();
        reset_t6041_26b123(ext); node.fw[2] = 1; node.compat[2] = 1; expect_rejected();
    }

    /* Desktop 26.5.2 and 26.6.2 remain DCP_FIRMWARE_V_26_6_DESKTOP on internal and external. */
    for (unsigned minor = 5; minor <= 6; minor++) {
        for (unsigned ext = 0; ext < 2; ext++) {
            reset_t6041_26b123(ext);
            node.uuid = "other";
            node.fw[0] = 26; node.fw[1] = minor; node.fw[2] = 2;
            node.compat[0] = 26; node.compat[1] = minor; node.compat[2] = 2;
            assert(!apple_t6041_dcp_v27_2(&node));
            assert(!apple_t6041_dcpext_v27_2(&node));
            assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_V_26_6_DESKTOP);
        }
    }
    /*
     * The M3 Air's (T8122) internal DCP handed over with its 27.2 image runs
     * the 27.2 table. The external processor needs the internal handoff,
     * other firmware-27 images are refused and 14.7 stays on a legacy ABI.
     */
    reset_t6041_26b123(false);
    node.compatible = "apple,t8122-dcp";
    node.uuid = APPLE_T8122_DCP_26B123_UUID;
    assert(apple_t8122_dcp_v27_2(&node));
    assert(!apple_t6041_dcp_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_V_27_0);
    node.compatible = "apple,t8122-dcpext";
    assert(!apple_t8122_dcp_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_UNKNOWN);
    node.compatible = "apple,t8122-dcp";
    node.uuid = APPLE_T6041_DCP_26B123_UUID;
    assert(!apple_t8122_dcp_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_UNKNOWN);
    node.uuid = APPLE_T8122_DCP_26B123_UUID;
    node.uuid_count = 0; assert(!apple_t8122_dcp_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_UNKNOWN);
    node.uuid_count = 2; assert(!apple_t8122_dcp_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_UNKNOWN);
    node.uuid_count = 1;
    node.fw[0] = 14; node.fw[1] = 7; node.fw[2] = 0;
    node.compat[0] = 14; node.compat[1] = 7; node.compat[2] = 0;
    assert(!apple_t8122_dcp_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_V_14_7_TEMPLATE);
    node.fw[0] = 27; node.fw[1] = 2; node.fw[2] = 1;
    node.compat[0] = 27; node.compat[1] = 2; node.compat[2] = 1;
    assert(!apple_t8122_dcp_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_UNKNOWN);
    node.fw[2] = 0; node.compat[2] = 0;
    assert(apple_t8122_dcp_v27_2(&node));

    /*
     * A T8132 processor of the M4 backend keeps it on the M4 firmware path
     * only; off it (a Mac mini booted from 27.2) nobody adopts or touches it.
     */
    static struct apple_dcp_hw_data hw_m4 = { .firmware_compat = DCP_FIRMWARE_M4 };
    dev.hw = &hw_m4;
    for (unsigned ext = 0; ext < 2; ext++) {
        reset_t6041_26b123(ext);
        node.compatible = ext ? "apple,t8132-dcpext" : "apple,t8132-dcp";
        node.uuid = "other";
        t8132_firmware_path = true;
        assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_M4);
        t8132_firmware_path = false;
        assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_UNKNOWN);
    }

    /*
     * A T8132 dcpext handed over with the T8132 27.2 image (first the Mac
     * mini's, J773g) runs the 27.2 table, whatever the firmware path and on
     * any board: the selection is by processor and image.
     */
    reset_t6041_26b123(true);
    node.compatible = "apple,t8132-dcpext";
    node.uuid = APPLE_T8132_DCP_26B123_UUID;
    assert(!apple_t6041_dcpext_v27_2(&node));
    for (unsigned path = 0; path < 2; path++) {
        t8132_firmware_path = path;
        assert(apple_t8132_dcpext_v27_2(&node));
        assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_V_27_0);
    }
    t8132_firmware_path = false;
    /* The internal DCP is never admitted: macOS parks it; aura hands it to nobody. */
    node.compatible = "apple,t8132-dcp";
    assert(!apple_t8132_dcpext_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_UNKNOWN);
    /* The T6041 image, a missing or second UUID, and other releases are refused. */
    node.compatible = "apple,t8132-dcpext";
    node.uuid = APPLE_T6041_DCP_26B123_UUID;
    assert(!apple_t8132_dcpext_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_UNKNOWN);
    node.uuid = APPLE_T8132_DCP_26B123_UUID;
    node.uuid_count = 0; assert(!apple_t8132_dcpext_v27_2(&node));
    node.uuid_count = 2; assert(!apple_t8132_dcpext_v27_2(&node));
    node.uuid_count = 1;
    node.compat[1] = 0; assert(!apple_t8132_dcpext_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_UNKNOWN);
    node.compat[1] = 2; node.fw[2] = 1; assert(!apple_t8132_dcpext_v27_2(&node));
    node.fw[2] = 0; assert(apple_t8132_dcpext_v27_2(&node));

    /*
     * The internal DCP handed over with the T8132 27.2 image and its
     * firmware-27 clocks (the M4 Air's on the 27.2 stub) runs the 27.2
     * table on any board and either firmware path; without the clocks it is
     * the M4 backend's on its path and nobody's off it, as above.
     */
    reset_t6041_26b123(false);
    node.compatible = "apple,t8132-dcp";
    node.uuid = APPLE_T8132_DCP_26B123_UUID;
    node.clock_ids = true;
    for (unsigned path = 0; path < 2; path++) {
        t8132_firmware_path = path;
        assert(apple_t8132_dcp_v27_2(&node));
        assert(!apple_t8132_dcpext_v27_2(&node));
        assert(!apple_t6041_dcp_v27_2(&node));
        assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_V_27_0);
        node.clock_ids = false;
        assert(!apple_t8132_dcp_v27_2(&node));
        assert(dcp_check_firmware_version(&dev) == (path ? DCP_FIRMWARE_M4 : DCP_FIRMWARE_UNKNOWN));
        node.clock_ids = true;
    }
    t8132_firmware_path = true;
    /* Not on a dcpext, nor with the T6041 image, a missing UUID or another release. */
    node.compatible = "apple,t8132-dcpext"; assert(!apple_t8132_dcp_v27_2(&node));
    node.compatible = "apple,t8132-dcp";
    node.uuid = APPLE_T6041_DCP_26B123_UUID; assert(!apple_t8132_dcp_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_M4);
    node.uuid = APPLE_T8132_DCP_26B123_UUID;
    node.uuid_count = 0; assert(!apple_t8132_dcp_v27_2(&node));
    node.uuid_count = 1;
    node.compat[1] = 0; assert(!apple_t8132_dcp_v27_2(&node));
    assert(dcp_check_firmware_version(&dev) == DCP_FIRMWARE_M4);
    node.compat[1] = 2; assert(apple_t8132_dcp_v27_2(&node));
    node.clock_ids = false;

    dev.hw = &hw;
    puts("PASS T6041 26B123 v27.2 internal/external admission, refusal controls, and desktop 26.x preservation");
    puts("PASS T8122 internal DCP needs its 27.2 image for the 27.2 table");
    puts("PASS T8132 M4 backend only on its firmware path; a T8132 dcpext on the 27.2 image runs the 27.2 table on any board");
    puts("PASS T8132 internal DCP with the 27.2 image and its firmware-27 clocks runs the 27.2 table; without them nothing changes");
    puts("PASS T8132 internal DCP on the 27.2 image joins the DRM device on any board; off the M4 path nothing else does");
    return 0;
}
'''


def t8122_dcpext_v27_admission(root):
    return _v27_prelude(root) + r'''
static struct device_node dcp, dcpext;
static const struct apple_dcp_hw_data hw;
static struct device dev = { .of_node = &dcpext, .hw = &hw };

static void set(u32 *v, u32 a, u32 b, u32 c) { v[0] = a; v[1] = b; v[2] = c; }

/* 27.2 on the J615: the internal DCP on 26B123, the dcpext with no image UUID. */
static void reset(void)
{
    dcp = (struct device_node){
        .compatible = "apple,t8122-dcp", .uuid = APPLE_T8122_DCP_26B123_UUID,
        .uuid_count = 1, .fw = { 27, 2, 0 }, .fw_cells = 3,
        .compat = { 27, 2, 0 }, .compat_cells = 3,
    };
    dcpext = (struct device_node){
        .compatible = "apple,t8122-dcpext", .fw = { 27, 2, 0 }, .fw_cells = 3,
        .compat = { 27, 2, 0 }, .compat_cells = 3,
    };
    of_nodes[0] = &dcp;
    of_nodes[1] = &dcpext;
    ext26 = false;
}

static void expect(bool v27_2, enum dcp_firmware_version want)
{
    assert(apple_t8122_dcpext_v27_2(&dcpext) == v27_2);
    assert(dcp_check_firmware_version(&dev) == want);
    assert(!of_node_refs);
}

int main(void)
{
    assert(!apple_t8122_dcpext_v27_2(NULL));

    reset();
    expect(true, DCP_FIRMWARE_V_27_0);
    /* The internal DCP is not the external one. */
    assert(!apple_t8122_dcpext_v27_2(&dcp));
    assert(!of_node_refs);
    /* An image UUID on the dcpext changes nothing. */
    dcpext.uuid = APPLE_T8122_DCP_26B123_UUID; dcpext.uuid_count = 1;
    expect(true, DCP_FIRMWARE_V_27_0);
    puts("PASS T8122 dcpext on 27.2 beside the 26B123 internal DCP runs the 27.2 table");

    /* Only beside an internal DCP handed over on the 27.2 image. */
    reset(); dcp.uuid = APPLE_T6041_DCP_26B123_UUID; expect(false, DCP_FIRMWARE_UNKNOWN);
    reset(); dcp.uuid_count = 0; expect(false, DCP_FIRMWARE_UNKNOWN);
    reset(); set(dcp.fw, 14, 7, 0); set(dcp.compat, 14, 7, 0); expect(false, DCP_FIRMWARE_UNKNOWN);
    reset(); of_nodes[0] = NULL; expect(false, DCP_FIRMWARE_UNKNOWN);
    reset(); dcp.compatible = "apple,t6041-dcp"; expect(false, DCP_FIRMWARE_UNKNOWN);
    /* Another SoC's dcpext is not it. */
    reset(); dcpext.compatible = "apple,t6041-dcpext";
    assert(!apple_t8122_dcpext_v27_2(&dcpext));
    assert(!of_node_refs);
    puts("PASS T8122 dcpext 27.2 needs the internal DCP's 26B123 handoff");

    /* Its own version and compatibility: both 27.2.0, three cells each. */
    reset(); set(dcpext.fw, 27, 0, 0); expect(false, DCP_FIRMWARE_UNKNOWN);
    reset(); set(dcpext.compat, 27, 0, 0); expect(false, DCP_FIRMWARE_UNKNOWN);
    reset(); set(dcpext.fw, 27, 2, 1); set(dcpext.compat, 27, 2, 1);
    expect(false, DCP_FIRMWARE_UNKNOWN);
    reset(); dcpext.fw_cells = 0; expect(false, DCP_FIRMWARE_UNKNOWN);
    reset(); dcpext.compat_cells = 4; expect(false, DCP_FIRMWARE_UNKNOWN);
    /* A 27.x refusal is terminal even if the legacy gate would admit it. */
    reset(); ext26 = true; dcp.uuid_count = 0; expect(false, DCP_FIRMWARE_UNKNOWN);
    puts("PASS T8122 dcpext 27.2 refuses other or malformed versions");

    /* The 14.7 template and the native 26.x path are as before. */
    reset(); set(dcpext.fw, 14, 7, 0); set(dcpext.compat, 14, 7, 0);
    expect(false, DCP_FIRMWARE_V_14_7_TEMPLATE);
    reset(); set(dcpext.fw, 26, 6, 2); set(dcpext.compat, 26, 6, 2);
    expect(false, DCP_FIRMWARE_UNKNOWN);
    ext26 = true; expect(false, DCP_FIRMWARE_H17P);
    puts("PASS T8122 dcpext 14.7.0 keeps the template, native 26.x keeps H17P");
    return 0;
}
'''


def v27_tag_translation(root):
    iomfb_h = (root / 'drivers/gpu/drm/apple/iomfb.h').read_text()
    methods_decl = iomfb_h[iomfb_h.index('enum dcpep_method {'):
                           iomfb_h.index('#define IOMFB_MAX_CB')]
    v27_c = (root / 'drivers/gpu/drm/apple/iomfb_v27.c').read_text()
    v27_tables = v27_c[v27_c.index('static const struct dcp_method_entry dcp_methods_27_0'):
                       v27_c.index('/* The template\'s calls resolve through')]
    apple = root / 'drivers/gpu/drm/apple'
    swap_h = (apple / 'iomfb_v27_3_swap.h').read_text()
    swap_defines = '\n'.join(line for line in swap_h.splitlines()
                             if line.startswith('#define DCP_V27'))
    dcp_c = (root / 'drivers/gpu/drm/apple/dcp.c').read_text()
    transport = block(dcp_c, 'static bool dcp_v27_sleep_transport_supported(')
    return r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint16_t u16;
''' + swap_defines + r'''
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CONFIG_APPLE_SOC_SLEEP 1
#define IS_ENABLED(option) (option)
#define GFP_KERNEL 0
enum dcp_firmware_version {
    DCP_FIRMWARE_UNKNOWN,
    DCP_FIRMWARE_V_27_0,
};
enum dcp_iomfb_method_profile {
    DCP_IOMFB_METHODS_DEFAULT,
    DCP_IOMFB_METHODS_H17G,
};
struct device_node { const char *compatible; };
struct device { struct device_node *of_node; };
struct apple_dcp_hw_data {
    bool adopt_live_session;
    enum dcp_iomfb_method_profile iomfb_method_profile;
};
struct apple_dcp {
    struct device *dev;
    struct apple_dcp_hw_data hw;
    enum dcp_firmware_version fw_compat;
    const struct dcp_method_entry *v27_methods;
};
static int of_device_is_compatible(const struct device_node *np, const char *s)
{
    return np && np->compatible && !strcmp(np->compatible, s);
}
static void *devm_kmemdup(struct device *dev, const void *src, size_t len, unsigned int gfp)
{
    void *copy = malloc(len);
    (void)dev; (void)gfp;
    if (copy)
        memcpy(copy, src, len);
    return copy;
}
''' + methods_decl + '\n' + v27_tables + '\n' + transport + r'''
static void expect_tag(const struct dcp_method_entry *methods, const char *in, const char *want)
{
    char out[4] = { 'X', 'X', 'X', 'X' };
    assert(iomfb_v27_tag(methods, in, out) == 0);
    assert(memcmp(out, want, 4) == 0);
}

static void expect_tag_fail(const struct dcp_method_entry *methods, const char *in)
{
    char out[4] = { 'X', 'X', 'X', 'X' };
    assert(iomfb_v27_tag(methods, in, out) == -EOPNOTSUPP);
    assert(memcmp(out, "XXXX", 4) == 0);
}

int main(void)
{
    struct device_node node = { .compatible = "apple,t6050-dcp" };
    struct device dev = { .of_node = &node };
    const struct iomfb_v27_build *b27_0 = iomfb_v27_methods("27.0.0");
    const struct iomfb_v27_build *b27_2 = iomfb_v27_methods("27.2.0");
    const struct iomfb_v27_build *b27_3 = iomfb_v27_methods("27.3.0");
    const struct dcp_method_entry *m27_0 = b27_0 ? b27_0->methods : NULL;
    const struct dcp_method_entry *m27_2 = b27_2 ? b27_2->methods : NULL;
    const struct dcp_method_entry *m27_3 = b27_3 ? b27_3->methods : NULL;
    const struct dcp_method_entry *j714s = iomfb_v27_methods_j714s(&dev, m27_0);
    struct dcp_method_entry partial_27_2[dcpep_num_methods];
    unsigned int missing_count;

    assert(m27_0 && m27_2 && m27_3 && j714s);
    assert(!iomfb_v27_methods_renumbered(NULL));
    assert(!iomfb_v27_methods_renumbered(m27_0));
    assert(!iomfb_v27_methods_renumbered(j714s));
    assert(iomfb_v27_methods_renumbered(m27_2));
    assert(iomfb_v27_methods_renumbered(m27_3));

    /* NULL table fails closed. */
    expect_tag_fail(NULL, "A411");
    assert(!iomfb_v27_sleep_methods_complete(NULL));

    /* 27.0 and J714s copy keep 27.0 IDs unchanged. */
    expect_tag(m27_0, "A411", "A411");
    expect_tag(m27_0, "A484", "A484");
    expect_tag(m27_0, "A500", "A500");
    expect_tag(m27_0, "A442", "A442");
    expect_tag(m27_0, "A449", "A449");
    expect_tag(m27_0, "A115", "A115");
    expect_tag(m27_0, "A364", "A364");
    expect_tag(j714s, "A411", "A411");
    expect_tag(j714s, "A408", "A408");
    expect_tag(j714s, "A500", "A500");
    assert(iomfb_v27_sleep_methods_complete(m27_0));
    assert(iomfb_v27_sleep_methods_complete(j714s));
    assert(!iomfb_v27_missing_method(m27_0, &missing_count) && missing_count == 0);

    /* 27.2 translates every recorded shifted and unchanged paired/sleep tag. */
    const struct { const char *in, *out; } map_27_2[] = {
        { "A406", "A456" }, { "A408", "A458" }, { "A411", "A461" }, { "A412", "A462" },
        { "A413", "A463" }, { "A414", "A464" }, { "A423", "A473" }, { "A428", "A478" },
        { "A442", "A492" }, { "A449", "A499" }, { "A450", "A500" }, { "A453", "A503" },
        { "A484", "A534" }, { "A487", "A537" }, { "A489", "A539" }, { "A500", "A550" },
        { "A115", "A115" },
        { "A360", "A360" }, { "A361", "A361" }, { "A362", "A362" }, { "A364", "A364" },
        { "A365", "A365" }, { "A366", "A366" }, { "A367", "A367" }, { "A368", "A368" },
        { "A369", "A369" }, { "A370", "A370" }, { "A371", "A371" }, { "A372", "A372" },
        { "A373", "A373" },
    };
    for (unsigned int i = 0; i < ARRAY_SIZE(map_27_2); i++)
        expect_tag(m27_2, map_27_2[i].in, map_27_2[i].out);
    assert(iomfb_v27_sleep_methods_complete(m27_2));
    assert(!iomfb_v27_missing_method(m27_2, &missing_count) && missing_count == 0);

    /* Unrecorded tags on 27.2 fail closed (no +50 arithmetic, no 27.0 fallback). */
    expect_tag_fail(m27_2, "A495");
    expect_tag_fail(m27_2, "A499");
    expect_tag_fail(m27_2, "A375");
    expect_tag_fail(m27_2, "A999");

    /* The completed 27.3 table translates its recorded entries; the optional
     * sleep/peer entries it still does not record fail closed. */
    expect_tag(m27_3, "A453", "A503");
    expect_tag(m27_3, "A411", "A461");
    expect_tag(m27_3, "A413", "A463");
    expect_tag(m27_3, "A484", "A535");
    const char *missing_27_3[] = {
        "A500", "A442", "A449", "A115", "A364", "A367",
    };
    for (unsigned int i = 0; i < ARRAY_SIZE(missing_27_3); i++)
        expect_tag_fail(m27_3, missing_27_3[i]);
    assert(!iomfb_v27_sleep_methods_complete(m27_3));
    assert(!iomfb_v27_missing_method(m27_3, &missing_count) && missing_count == 0);

    /* Dropping an optional sleep/paired entry does not block basic boot admission,
     * but does mark sleep incomplete when a sleep entry is missing, and any table
     * other than 27.0 or the J714s copy fails closed as renumbered. */
    memcpy(partial_27_2, m27_2, sizeof(partial_27_2));
    partial_27_2[dcpep_v27_wake_a442].tag[0] = '\0';
    assert(iomfb_v27_methods_renumbered(partial_27_2));
    expect_tag_fail(partial_27_2, "A442");
    assert(!iomfb_v27_missing_method(partial_27_2, &missing_count) && missing_count == 0);
    assert(!iomfb_v27_sleep_methods_complete(partial_27_2));

    struct dcp_method_entry future_27_x[dcpep_num_methods];
    memcpy(future_27_x, m27_0, sizeof(future_27_x));
    memcpy(future_27_x[dcpep_start_signal].tag, "A501", 4);
    future_27_x[dcpep_v27_wake_a442].tag[0] = '\0';
    assert(iomfb_v27_methods_renumbered(future_27_x));
    expect_tag_fail(future_27_x, "A442");

    /*
     * Sleep transport admission keys on T6050 DCP/DCPEXT and table completeness.
     * No of_machine_is_compatible() stub exists in this translation unit, enforcing
     * at compile time that admission does not gate on a board model.
     */
    struct apple_dcp dcp = {
        .dev = &dev,
        .hw = { .adopt_live_session = true, .iomfb_method_profile = DCP_IOMFB_METHODS_DEFAULT },
        .fw_compat = DCP_FIRMWARE_V_27_0,
    };
    const char *t6050_compats[] = { "apple,t6050-dcp", "apple,t6050-dcpext" };
    for (unsigned int i = 0; i < ARRAY_SIZE(t6050_compats); i++) {
        node.compatible = t6050_compats[i];
        dcp.v27_methods = m27_0; assert(dcp_v27_sleep_transport_supported(&dcp));
        dcp.v27_methods = j714s; assert(dcp_v27_sleep_transport_supported(&dcp));
        dcp.v27_methods = m27_2; assert(dcp_v27_sleep_transport_supported(&dcp));
        dcp.v27_methods = m27_3; assert(!dcp_v27_sleep_transport_supported(&dcp));
        dcp.v27_methods = partial_27_2; assert(!dcp_v27_sleep_transport_supported(&dcp));
        dcp.v27_methods = NULL; assert(!dcp_v27_sleep_transport_supported(&dcp));
    }
    dcp.v27_methods = m27_2;
    node.compatible = "apple,t6041-dcp"; assert(!dcp_v27_sleep_transport_supported(&dcp));
    node.compatible = "apple,t6050-dcp";
    dcp.hw.adopt_live_session = false; assert(!dcp_v27_sleep_transport_supported(&dcp));
    dcp.hw.adopt_live_session = true;
    dcp.hw.iomfb_method_profile = DCP_IOMFB_METHODS_H17G; assert(!dcp_v27_sleep_transport_supported(&dcp));
    dcp.hw.iomfb_method_profile = DCP_IOMFB_METHODS_DEFAULT;
    dcp.fw_compat = DCP_FIRMWARE_UNKNOWN; assert(!dcp_v27_sleep_transport_supported(&dcp));

    free((void *)j714s);
    puts("PASS v27 tag translation fail-closed and table-driven sleep transport admission");
    return 0;
}
'''
