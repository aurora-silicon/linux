#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate the production DCP v27 serializer and framebuffer bounds."""
from pathlib import Path
import re
import shutil
import os
import subprocess
import tempfile
from common import block

ROOT = Path(__file__).resolve().parents[4]
DRIVER = ROOT / "drivers/gpu/drm/apple"


def without_includes(source):
    return re.sub(r"^#include .*\n", "", source, flags=re.M)


def main():
    print("TAP version 13\n1..1", flush=True)
    if not shutil.which("cc"):
        print("ok 1 - DCP firmware-27 wire and allocation bounds # SKIP cc unavailable")
        return 4
    prolog = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define __packed __attribute__((packed))
#define static_assert _Static_assert
#define SWAP_SURFACES 4
#define DCP_FW_VER (26 << 16)
#define DCP_FW_VERSION(a,b,c) ((a << 16) | (b << 8) | c)
#define DCP_FW_NAME(name) name##_h17p
#define U32_MAX UINT32_MAX
#define ALIGN(v,a) (((v) + (a) - 1) & ~((a) - 1))
#define DIV_ROUND_UP(v,a) (((v) + (a) - 1) / (a))
#define fls64(v) ((v) ? 64 - __builtin_clzll(v) : 0)
#define ilog2(v) (63 - __builtin_clzll(v))
#define roundup_pow_of_two(v) (1ULL << (64 - __builtin_clzll((v) - 1)))
struct apple_dcp;
struct drm_crtc;
struct drm_crtc_state;
struct drm_atomic_state;
struct drm_atomic_commit;
"""
    program = without_includes((DRIVER / "version_utils.h").read_text())
    program += "\n#undef DCP_FW_NAME\n#undef DCP_FW_VERSION\n" + prolog
    program += without_includes((DRIVER / "iomfb_plane.h").read_text())
    program += without_includes((DRIVER / "iomfb_template.h").read_text())
    program += without_includes((DRIVER / "iomfb_v27.h").read_text())
    program += without_includes((DRIVER / "iomfb-dfb.h").read_text())
    program += "\ntypedef int32_t s32; typedef uint32_t __le32;\n"
    program += "#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))\n"
    program += "#define max_t(t,a,b) ((t)(a)>(t)(b)?(t)(a):(t)(b))\n#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))\n"
    program += "#define div_u64(a,b) ((uint64_t)(a)/(b))\n"
    program += "static u32 get_unaligned_le32(const void*p){u32 v;memcpy(&v,p,4);return v;}\n"
    program += "static void put_unaligned_le32(u32 v,void*p){memcpy(p,&v,4); }\n"
    program += without_includes((DRIVER / "iomfb-peer-v27.h").read_text())
    program += without_includes((DRIVER / "dcp-peer-frames-v27.h").read_text())
    program += block((DRIVER / "iomfb_v27.c").read_text(),
                     r"^void iomfb_serialize_present_v27\(")
    driver = (DRIVER / "iomfb_v27.c").read_text()
    handlers = block(driver, r"^static const iomfb_cb_handler cb_handlers")
    callback_sizes = block(driver, r"^static const struct \{\n\tu16 input, output;")
    names = sorted(set(re.findall(r"= (\w+),", handlers)))
    program += "\n#define IOMFB_MAX_CB 1000\n"

    program += "typedef bool (*iomfb_cb_handler)(void);\n"
    for name in names:
        program += f"static bool {name}(void) {{ return true; }}\n"
    program += handlers + ";\n" + callback_sizes + ";\n"
    program += block(driver, r"^bool iomfb_v27_callback_size_valid\(")
    program += r"""
int main(void) {
 struct dcp_swap_submit_req_h17p input;
 struct dcp_swap_submit_req_v27 wire;
 memset(&input, 0xa5, sizeof(input));
 input.swap.swap_id = 0x12345678;
 input.swap.swap_enabled = 0x40000005;
 input.swap.bg_color = 0xfedcba98;
 input.surf[0].base.format = 0x42475241;
 input.surf_iova[0] = 0x123456789abULL;
 input.unknown_output_null = 1;
 memset(&wire, 0xcc, sizeof(wire));
 iomfb_serialize_present_v27(&wire, &input);
 assert(sizeof(input) == 0xe9c && sizeof(wire) == 0xff4);
 assert(wire.swap.swap_id == input.swap.swap_id);
 assert(wire.swap.swap_enabled == input.swap.swap_enabled);
 assert(wire.swap.bg_color == input.swap.bg_color);
 assert(!memcmp(wire.swap.timestamp, input.swap.timestamp, 56));
 assert(!memcmp(wire.surf, input.surf, sizeof(wire.surf)));
 assert(wire.surf_iova[0] == input.surf_iova[0]);
 assert(wire.unknown_output_null == 1);
 assert(wire.swap.debug_info[0] == 0 && wire.swap.event_wait[3] == 0);
 assert(wire.swap.event_signal_on_glass == 0 && wire.swap.event_signal[3] == 0);
 assert(wire.swap.brightness_update == 0 && wire.swap.brightness_nits == 0);
 assert(wire.padding[0] == 0 && wire.padding[1] == 0);
 struct dcp_swap_submit_req_h17p unchanged = input;
 struct dcp_swap_submit_req_v27 right;
 memset(wire.surf_null,1,sizeof(wire.surf_null));
 wire.surf_null[0]=0;
 wire.swap.src_rect[0]=(struct dcp_rect){.x=0,.y=0,.w=3840,.h=2160};
 wire.swap.dst_rect[0]=wire.swap.src_rect[0];
 dcp_peer_split_swap_v27(&wire,&right,1920);
 assert(wire.swap.src_rect[0].w==1920 && right.swap.src_rect[0].x==1920);
 assert(right.swap.src_rect[0].w==1920 && right.swap.dst_rect[0].x==0);
 assert(wire.surf_iova[0]==right.surf_iova[0]);
 assert(!memcmp(&input,&unchanged,sizeof(input)));
 struct dcp_peer_frames_v27 frames={.active=true}; u32 watermark=0;
 dcp_peer_frame_submit_v27(&frames,17);
 assert(!dcp_peer_frame_complete_v27(&frames,0,17,&watermark));
 assert(!dcp_peer_frame_complete_v27(&frames,1,16,&watermark));
 assert(!dcp_peer_frame_complete_v27(&frames,1,18,&watermark));
 assert(dcp_peer_frame_complete_v27(&frames,1,17,&watermark)&&watermark==17);
 assert(!dcp_peer_frame_complete_v27(&frames,0,17,&watermark));
 dcp_peer_frame_submit_v27(&frames,18); frames.failed=true;
 assert(!dcp_peer_frame_complete_v27(&frames,0,18,&watermark));
 u32 format, allocation;
 assert(!apple_dcp_dfb_allocation(1920,1080,0x42475241,0,1ULL<<32,
                                  &format,&allocation));
 assert(format == 0x42475241 && allocation == ALIGN(7680ULL * 1080,0x4000));
 assert(apple_dcp_dfb_allocation(1920,1080,0x42475241,0,0x4000,
                                &format,&allocation) == -ENOSPC);
 assert(apple_dcp_dfb_allocation(0,1080,0x42475241,0,1ULL<<32,
                                &format,&allocation) == -EINVAL);
 assert(apple_dcp_dfb_allocation(16385,1080,0x42475241,0,1ULL<<32,
                                &format,&allocation) == -EINVAL);
 assert(apple_dcp_dfb_allocation(1920,1080,0xdeadbeef,0,1ULL<<32,
                                &format,&allocation) == -EOPNOTSUPP);
 assert(apple_dcp_dfb_allocation(1920,1080,0x42475241,1,1ULL<<32,
                                &format,&allocation) == -EOPNOTSUPP);
 assert(!apple_dcp_dfb_allocation(3840,2160,0x62336138,2,1ULL<<32,
                                  &format,&allocation));
 assert(format == 0x77343061 && allocation % 0x4000 == 0);
 assert(apple_dcp_dfb_round_size((1ULL<<24)+1) == (1ULL<<24));
 assert(apple_dcp_dfb_round_size((1ULL<<24)+3) == (1ULL<<24)+4);
 assert(iomfb_v27_callback_size_valid(594, 0x730, 0));
 assert(!iomfb_v27_callback_size_valid(594, 0x72f, 0));
 assert(iomfb_v27_callback_size_valid(451, 0x14, 0x14));
 assert(!iomfb_v27_callback_size_valid(451, 0x13, 0x14));
 assert(!iomfb_v27_callback_size_valid(451, 0x14, 0x13));
 assert(!iomfb_v27_callback_size_valid(999, 0xffff, 0xffff));
 assert(!iomfb_v27_callback_size_valid(1000, 0xffff, 0xffff));
 return 0;
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        source, binary = Path(tmp) / "dcp.c", Path(tmp) / "dcp"
        source.write_text(program)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", str(source), "-o", str(binary)],
                       check=True)
        # stdbuf injects LD_PRELOAD into the runner, ahead of the ASan runtime.
        # Only this isolated sanitizer child needs a clean loader environment.
        env = {**os.environ, "LD_PRELOAD": ""}
        subprocess.run([str(binary)], env=env, check=True)
    print("ok 1 - DCP firmware-27 serializer and framebuffer bounds")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
