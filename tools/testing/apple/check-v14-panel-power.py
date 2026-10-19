#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the production 14.7 panel power sequence without display hardware."""

import argparse
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
TEST = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define A(n) (n)
#define D(n) (n)
#define READ_ONCE(x) (x)
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
#define dev_warn(...) ((void)0)
struct dcp_v14_board { bool panel_power; };
struct apple_dcp_v14 {
    const struct dcp_v14_board *board;
    bool started, failed, panel_off, ctm_valid, opened, powered, external;
    int lock;
};
struct apple_dcp { struct apple_dcp_v14 *v14; bool external; void *dev; int mode_state; };
static u32 get_unaligned_le32(const void *p)
{
    const u8 *b = p;
    return (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
}

struct firmware {
    unsigned int calls, method[4], refuse_method;
    int transport;
    bool on;
};

static struct firmware firmware;
static bool black_ok = true;
static unsigned int black_frames;
static int dcp_v14_call(struct apple_dcp_v14 *v14, unsigned int method,
                        const void *in, u32 in_size, void *out, u32 out_size,
                        u32 completion)
{
    struct firmware *fw = &firmware;
    const u8 *bytes = in;
    unsigned int i;

    (void)v14;
    assert(completion == 0);

    assert(fw->calls < 4);
    fw->method[fw->calls++] = method;
    if (method == 475) {
        assert(in_size == 4 && out_size == 0 && !out);
        assert(bytes[0] == 1);
    } else {
        assert(method == 472);
        assert(in_size == 12 && out_size == 8 && out);
        assert(bytes[0] == fw->on);
    }
    for (i = 1; i < in_size; i++)
        assert(bytes[i] == 0);
    if (method == fw->refuse_method) {
        if (fw->transport)
            return fw->transport;
        assert(method == 472);
        ((u8 *)out)[4] = 1;
    }
    return 0;
}

static bool dcp_v14_present(struct apple_dcp_v14 *v14, void *fb,
                            u32 width, u32 height, u32 y, void *ctm)
{
    assert(!fb && !width && !height && !y && !ctm);
    assert(!v14->panel_off);
    black_frames++;
    return black_ok;
}
/* The external branch is outside this regression; it must never run here. */
static int dcp_v14_set_power(struct apple_dcp_v14 *v14, bool on)
{
    (void)v14;
    (void)on;
    assert(0);
    return 0;
}
static void dcp_mode_invalidate(void *state) { (void)state; assert(0); }
@@CALLBACK@@
@@FUNCTIONS@@

static void power_callbacks(void)
{
    struct dcp_v14_board board = {.panel_power = true};
    struct apple_dcp_v14 v14 = {.board = &board};
    u8 in[20] = {}, out[4] = {};

    assert(dcp_v14_callback(&v14, D(2), NULL, 0, NULL, 0) == 0);
    assert(dcp_v14_callback(&v14, D(591), in, 20, NULL, 0) == 0);
    assert(dcp_v14_callback(&v14, D(593), in, 4, NULL, 0) == 0);
    assert(dcp_v14_callback(&v14, D(574), in, 4, out, 4) == 0);
    in[0] = 1;
    assert(dcp_v14_callback(&v14, D(574), in, 4, out, 4) == 0);
    in[0] = 2;
    assert(dcp_v14_callback(&v14, D(574), in, 4, out, 4) == -EINVAL);
    assert(dcp_v14_callback(&v14, D(2), in, 4, NULL, 0) == -ENOENT);
    board.panel_power = false;
    assert(dcp_v14_callback(&v14, D(2), NULL, 0, NULL, 0) == -ENOENT);
}

static void sleep_wake(void)
{
    const struct dcp_v14_board board = {.panel_power = true};
    struct apple_dcp_v14 v14 = {.board = &board, .started = true, .ctm_valid = true};
    struct apple_dcp dcp = {.v14 = &v14};

    firmware = (struct firmware){0};
    black_frames = 0;
    iomfb_v14_7_poweron(&dcp);
    assert(!firmware.calls && !v14.panel_off);
    iomfb_v14_7_poweroff(&dcp);
    assert(v14.panel_off && firmware.calls == 1 && black_frames == 1);
    assert(firmware.method[0] == 472);
    iomfb_v14_7_poweroff(&dcp);
    assert(firmware.calls == 1 && black_frames == 1);
    firmware.on = true;
    v14.ctm_valid = true;
    iomfb_v14_7_poweron(&dcp);
    assert(!v14.panel_off && !v14.ctm_valid && firmware.calls == 3);
    assert(firmware.method[1] == 475 && firmware.method[2] == 472);
    iomfb_v14_7_poweron(&dcp);
    assert(firmware.calls == 3);
}

static void refused_transition(bool on, int error)
{
    struct apple_dcp_v14 v14 = {.panel_off = on};
    firmware = (struct firmware){.on = on, .refuse_method = 472, .transport = error};

    assert(dcp_v14_panel_set_power(&v14, on) == (error ? error : -EIO));
    assert(v14.panel_off == on);
    assert(firmware.calls == (on ? 2 : 1));
    /* A refusal must leave the state retryable, particularly a failed wake. */
    firmware.calls = 0;
    firmware.refuse_method = 0;
    assert(dcp_v14_panel_set_power(&v14, on) == 0);
    assert(v14.panel_off == !on);
}

static void incomplete_or_unsupported(void)
{
    struct dcp_v14_board board = {.panel_power = true};
    struct apple_dcp_v14 v14 = {.board = &board, .started = true};
    struct apple_dcp dcp = {.v14 = &v14};

    firmware = (struct firmware){0};
    black_frames = 0;
    black_ok = false;
    iomfb_v14_7_poweroff(&dcp);
    assert(!v14.panel_off && !firmware.calls && black_frames == 1);
    black_ok = true;
    board.panel_power = false;
    iomfb_v14_7_poweroff(&dcp);
    assert(!v14.panel_off && !firmware.calls && black_frames == 2);
    iomfb_v14_7_poweron(&dcp);
    assert(!firmware.calls);
    board.panel_power = true;
    v14.started = false;
    iomfb_v14_7_poweron(&dcp);
    iomfb_v14_7_poweroff(&dcp);
    assert(!firmware.calls && black_frames == 2);
}

static void refused_panel_selection(void)
{
    struct apple_dcp_v14 v14 = {.panel_off = true};
    firmware = (struct firmware){.on = true, .refuse_method = 475, .transport = -EIO};

    assert(dcp_v14_panel_set_power(&v14, true) == -EIO);
    assert(v14.panel_off && firmware.calls == 1 && firmware.method[0] == 475);
}

int main(void)
{
    unsigned int i;

    for (i = 0; i < 100; i++)
        sleep_wake();
    refused_transition(false, 0);
    refused_transition(true, 0);
    refused_transition(false, -110);
    refused_transition(true, -110);
    refused_panel_selection();
    incomplete_or_unsupported();
    power_callbacks();
    puts("PASS: sleep/wake ordering, idempotence, wire layouts, refusals, "
         "transport failures and internal power callbacks");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=pathlib.Path,
                        default=ROOT / "drivers/gpu/drm/apple/iomfb_v14_7.c")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="v14-panel-power-") as temporary:
        scratch = pathlib.Path(temporary)
        source = scratch / "test.c"
        driver = args.source.read_text()
        functions = []
        for name in ("dcp_v14_panel_set_power", "iomfb_v14_7_poweron", "iomfb_v14_7_poweroff"):
            match = re.search(r"^\w[\w\s*]*?\b" + name + r"\(", driver, re.M)
            assert match, name
            start = driver.index("{", match.end())
            end, depth = start + 1, 1
            while depth:
                depth += (driver[end] == "{") - (driver[end] == "}")
                end += 1
            functions.append(driver[match.start():end])
        # Exercise the real callback entry and dispatch, before unrelated handlers.
        # This catches accidentally placing panel handling in the external-only path.
        begin = driver.rindex("static int dcp_v14_callback(")
        end = driver.index("\t/*\n\t * Backlight correction notification", begin)
        callback = driver[begin:end]
        callback += r"""
    (void)p;
    (void)count;
    (void)offset;
    (void)service;
    (void)key_offset;
    (void)id;
    return -ENOENT;
}
#undef SHAPE
"""
        stub = r"""
static int dcp_v14_external_callback(struct apple_dcp_v14 *v, u32 tag,
                                     const u8 *in, u32 a, u8 *out, u32 b)
{
    (void)v;
    (void)tag;
    (void)in;
    (void)a;
    (void)out;
    (void)b;
    assert(0);
    return -ENOENT;
}
"""
        program = TEST.replace("@@FUNCTIONS@@", "\n".join(functions))
        source.write_text(program.replace("@@CALLBACK@@", stub + callback))
        binary = scratch / "test"
        subprocess.run(
            ["clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-g", str(source), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
