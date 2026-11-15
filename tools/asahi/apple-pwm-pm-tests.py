#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the real Apple PWM callbacks against a small runtime-PM/MMIO model.

Tests reference ownership, register-loss recovery and failures. The model does
not qualify physical power gating, output levels or system sleep on hardware.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r'^static [^\n]*\b' + name + r'\([^;]*?\n\{', source, re.M)
    if not match:
        raise ValueError('missing callback: ' + name)
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define __iomem
#define BIT(n) (1U << (n))
#define NSEC_PER_SEC 1000000000ULL
#define PWM_POLARITY_INVERSED 1
#define PWM_POLARITY_NORMAL 0
#define GFP_KERNEL 0
#define IS_ERR(p) ((intptr_t)(p) < 0)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define DIV64_U64_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
typedef uint32_t u32;
typedef uint64_t u64;
struct clk { int refs; };
struct device { void *data; int refs; bool active, rpm; };
struct platform_device { struct device dev; };
struct pwm_chip { struct device *parent; void *data; const void *ops; };
struct pwm_device { int unused; };
struct pwm_state { bool enabled; int polarity; u64 duty_cycle, period; };
static struct pwm_chip chip;
static struct platform_device pdev;
static struct clk clock_model;
static u32 registers[8];
static unsigned char private_data[256];
static int clock_error, active_error, action_error, add_error, writes;
static void (*cleanup)(void *);
static void *cleanup_data;
static const int apple_pwm_ops;
static int apple_pwm_runtime_suspend(struct device *dev);
static int apple_pwm_runtime_resume(struct device *dev);
static void *pwmchip_get_drvdata(struct pwm_chip *p) { return p->data; }
static struct device *pwmchip_parent(struct pwm_chip *p) { return p->parent; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static void platform_set_drvdata(struct platform_device *p, void *v) { p->dev.data = v; }
static u64 mul_u64_u64_div_u64(u64 a, u64 b, u64 c) { return (__uint128_t)a * b / c; }
static u32 readl(void *p) { assert(clock_model.refs == 1); return *(u32 *)p; }
static void writel(u32 v, void *p) { assert(clock_model.refs == 1); ++writes; *(u32 *)p = v; }
static int clk_prepare_enable(struct clk *c) {
    if (clock_error) return clock_error;
    assert(c->refs == 0); ++c->refs; return 0;
}
static void clk_disable_unprepare(struct clk *c) { assert(c->refs == 1); --c->refs; }
static u64 clk_get_rate(struct clk *c) { return 24000000; }
static void pm_runtime_get_noresume(struct device *d) { ++d->refs; }
static void pm_runtime_put_noidle(struct device *d) { assert(d->refs > 0); --d->refs; }
static int pm_runtime_resume_and_get(struct device *d) {
    if (!d->active) {
        int ret = apple_pwm_runtime_resume(d);
        if (ret) return ret;
        d->active = true;
    }
    ++d->refs; return 0;
}
static void pm_runtime_put(struct device *d) {
    pm_runtime_put_noidle(d);
    if (!d->refs && d->rpm && d->active) {
        assert(!apple_pwm_runtime_suspend(d));
        d->active = false;
        memset(registers, 0, sizeof(registers)); /* domain loses context */
    }
}
static void pm_runtime_enable(struct device *d) { d->rpm = true; }
static void pm_runtime_disable(struct device *d) { d->rpm = false; }
static bool pm_runtime_status_suspended(struct device *d) { return !d->active; }
static int pm_runtime_set_active(struct device *d) {
    if (active_error) return active_error;
    d->active = true; return 0;
}
static int pm_runtime_set_suspended(struct device *d) { d->active = false; return 0; }
static void *devm_pwmchip_alloc(struct device *d, unsigned n, size_t size) {
    assert(size <= sizeof(private_data));
    chip = (struct pwm_chip){.parent = d, .data = private_data}; return &chip;
}
static void *devm_platform_ioremap_resource(struct platform_device *p, int n) { return registers; }
static struct clk *devm_clk_get(struct device *d, const char *name) { return &clock_model; }
static int dev_err_probe(struct device *d, int err, const char *msg) { return err; }
static int devm_add_action_or_reset(struct device *d, void (*fn)(void *), void *arg) {
    if (action_error) { fn(arg); return action_error; }
    cleanup = fn; cleanup_data = arg; return 0;
}
static int devm_pwmchip_add(struct device *d, struct pwm_chip *p) { return add_error; }
'''

TESTS = r'''
static struct pwm_device pwm;
static struct pwm_state state = {.period = 40000, .duty_cycle = 10000};
static void reset_model(bool boot_on) {
    assert(!clock_model.refs && !pdev.dev.refs);
    memset(&pdev, 0, sizeof(pdev)); memset(private_data, 0, sizeof(private_data));
    memset(registers, 0, sizeof(registers));
    registers[APPLE_PWM_ON_CYCLES / 4] = 240;
    registers[APPLE_PWM_OFF_CYCLES / 4] = 720;
    registers[APPLE_PWM_CTRL / 4] = boot_on ? APPLE_PWM_CTRL_ENABLE | APPLE_PWM_CTRL_OUTPUT_ENABLE : 0;
    clock_error = active_error = action_error = add_error = writes = 0;
    cleanup = NULL; state.enabled = false; state.polarity = PWM_POLARITY_NORMAL;
}
static void remove_model(void) {
    assert(cleanup); cleanup(cleanup_data); cleanup = NULL;
    assert(!pdev.dev.refs && !pdev.dev.active && !clock_model.refs);
}
int main(void) {
    struct pwm_state got;
    reset_model(false);
    assert(!apple_pwm_probe(&pdev));
    assert(!pdev.dev.active && !pdev.dev.refs && !clock_model.refs);
    assert(!apple_pwm_get_state(&chip, &pwm, &got));
    assert(!got.enabled && got.period == 40000 && got.duty_cycle == 10000);
    assert(!pdev.dev.active && !pdev.dev.refs && !clock_model.refs);
    puts("PASS disabled probe, get_state and register-loss recovery");
    state.enabled = true;
    assert(!apple_pwm_apply(&chip, &pwm, &state));
    assert(pdev.dev.active && pdev.dev.refs == 1 && clock_model.refs == 1);
    for (int i = 0; i < 20; ++i) {
        assert(!apple_pwm_apply(&chip, &pwm, &state));
        assert(!apple_pwm_get_state(&chip, &pwm, &got));
        assert(pdev.dev.refs == 1 && got.enabled && got.period == 40000);
    }
    puts("PASS enabled output pins power; repeated apply/get_state do not leak");
    state.duty_cycle = 0;
    assert(!apple_pwm_apply(&chip, &pwm, &state));
    assert(pdev.dev.active && pdev.dev.refs == 1);
    assert(!apple_pwm_get_state(&chip, &pwm, &got) && got.enabled && !got.duty_cycle);
    state.duty_cycle = 10000;
    assert(!apple_pwm_apply(&chip, &pwm, &state));
    puts("PASS enabled zero-duty output remains powered");
    state.enabled = false;
    assert(!apple_pwm_apply(&chip, &pwm, &state));
    assert(!pdev.dev.active && !pdev.dev.refs && !clock_model.refs);
    assert(!apple_pwm_get_state(&chip, &pwm, &got) && !got.enabled);
    remove_model();
    puts("PASS disable releases output reference and suspended removal is balanced");
    reset_model(true);
    assert(!apple_pwm_probe(&pdev));
    assert(pdev.dev.active && pdev.dev.refs == 1 && clock_model.refs == 1 && !writes);
    assert(!apple_pwm_get_state(&chip, &pwm, &got) && got.enabled);
    state.enabled = true;
    assert(!apple_pwm_apply(&chip, &pwm, &state) && pdev.dev.refs == 1);
    remove_model();
    puts("PASS bootloader output adopted without writes and active removal balanced");
    reset_model(false);
    assert(!apple_pwm_probe(&pdev));
    clock_error = -EIO;
    int before = writes;
    state.enabled = true;
    assert(apple_pwm_apply(&chip, &pwm, &state) == -EIO);
    assert(apple_pwm_get_state(&chip, &pwm, &got) == -EIO);
    assert(!pdev.dev.refs && !clock_model.refs && writes == before);
    state.polarity = PWM_POLARITY_INVERSED;
    assert(apple_pwm_apply(&chip, &pwm, &state) == -EINVAL && writes == before);
    clock_error = 0; remove_model();
    puts("PASS resume failure and unsupported polarity leave registers and refs untouched");
    reset_model(false); clock_error = -EIO;
    assert(apple_pwm_probe(&pdev) == -EIO && !clock_model.refs && !pdev.dev.refs);
    reset_model(false); active_error = -EBUSY;
    assert(apple_pwm_probe(&pdev) == -EBUSY && !clock_model.refs && !pdev.dev.refs);
    puts("PASS initial clock and PM-state failures unwind");
    for (int enabled = 0; enabled <= 1; ++enabled) {
        reset_model(enabled); action_error = -ENOMEM;
        assert(apple_pwm_probe(&pdev) == -ENOMEM && !clock_model.refs && !pdev.dev.refs);
        reset_model(enabled); add_error = -ENOMEM;
        assert(apple_pwm_probe(&pdev) == -ENOMEM);
        remove_model();
    }
    puts("PASS action and PWM registration failures unwind both boot states");
    reset_model(false); assert(!apple_pwm_probe(&pdev));
    assert(!pm_runtime_resume_and_get(&pdev.dev)); /* another consumer/supplier vote */
    state.enabled = true; assert(!apple_pwm_apply(&chip, &pwm, &state));
    assert(pdev.dev.refs == 2);
    state.enabled = false; assert(!apple_pwm_apply(&chip, &pwm, &state));
    assert(pdev.dev.active && pdev.dev.refs == 1 && clock_model.refs == 1);
    pm_runtime_put(&pdev.dev);
    assert(!pdev.dev.active && !clock_model.refs); remove_model();
    puts("PASS another runtime reference prevents premature suspension");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--negative-control', action='store_true')
    args = parser.parse_args()
    source = (ROOT / 'drivers/pwm/pwm-apple.c').read_text()
    if args.negative_control:
        old = '\tif (state->enabled && !fpwm->enabled)\n\t\tpm_runtime_get_noresume(dev);'
        assert old in source
        source = source.replace(old, '\tif (state->enabled && !fpwm->enabled)\n\t\t; /* missing output reference */', 1)
    defines = '\n'.join(re.findall(r'^#define APPLE_PWM_.*$', source, re.M))
    struct = re.search(r'struct apple_pwm \{.*?\n\};', source, re.S).group()
    callbacks = '\n'.join(function(source, name) for name in (
        'to_apple_pwm', 'apple_pwm_apply', 'apple_pwm_get_state',
        'apple_pwm_runtime_suspend', 'apple_pwm_runtime_resume',
        'apple_pwm_runtime_disable', 'apple_pwm_probe'))
    with tempfile.TemporaryDirectory(prefix='apple-pwm-pm-') as tmp:
        tmp = Path(tmp)
        cfile, executable = tmp / 'test.c', tmp / 'test'
        cfile.write_text(PRELUDE + defines + '\n' + struct + callbacks + TESTS)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                        '-Werror', '-Wno-unused-parameter', '-Wno-unused-function',
                        '-fsanitize=address,undefined', str(cfile), '-o', str(executable)], check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        if args.negative_control:
            if result.returncode == 0:
                raise SystemExit('FAIL: missing output reference escaped detection')
            print('PASS negative control: missing output reference was detected')
        else:
            print(result.stdout, end='')
            if result.returncode:
                raise SystemExit(result.stderr)


if __name__ == '__main__':
    main()
