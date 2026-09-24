#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Native fault injection into the actual Apple target_index callback.

Does not touch hardware or emulate an Apple CPU. Requires a host C compiler.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
driver = (root / 'drivers/cpufreq/apple-soc-cpufreq.c').read_text()
defs = driver[driver.index('#define APPLE_DVFS_CMD'):driver.index('static const struct of_device_id')]
defs = defs.replace('static struct cpufreq_driver apple_soc_cpufreq_driver;', '')
callback = driver[driver.index('static int apple_soc_cpufreq_set_target('):
                  driver.index('static unsigned int apple_soc_cpufreq_fast_switch(')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uint64_t u64;
#define __iomem
#define dev_err(dev, ...) do { (void)(dev); } while (0)
#define BIT(n) (1ULL << (n))
#define GENMASK(h,l) (((~0ULL) >> (63-(h))) & ((~0ULL) << (l)))
#define FIELD_GET(m,v) (((v) & (m)) >> __builtin_ctzll(m))
#define FIELD_PREP(m,v) (((u64)(v) << __builtin_ctzll(m)) & (m))
struct cpufreq_frequency_table { unsigned int driver_data; };
struct cpufreq_policy { void *driver_data; struct cpufreq_frequency_table *freq_table; unsigned cpu; };
struct freq_qos_request { int unused; };
struct apple_smc_thermal_cpu;
static void apple_smc_thermal_cpu_fault(struct apple_smc_thermal_cpu *cpu) { (void)cpu; }
static u64 command, written;
static unsigned int reads, writes, mode;
static u64 sample(void) {
    reads++;
    if (mode == 1 || (mode == 2 && writes)) return command | BIT(31);
    if (mode == 3 && writes) return (command & ~31ULL) | 1;
    return command;
}
#define readq_poll_timeout_atomic(addr, val, cond, delay, limit) ({ \
    int result = -ETIMEDOUT; (void)(addr); (void)(delay); (void)(limit); \
    for (int attempt = 0; attempt < 4; attempt++) { \
        (val) = sample(); if (cond) { result = 0; break; } \
    } result; })
#define writeq_relaxed(value, addr) do { (void)(addr); writes++; \
    written = (value); command = written & ~BIT(25); } while (0)
'''
test = r'''
static void reset(struct apple_cpu_priv *p) {
    command = 0xa550000000400101ULL; reads = writes = mode = 0;
    p->transition_failed = false;
    p->expected_pstate = 1;
}
int main(void) {
    unsigned char mmio[0x28];
    struct apple_cpu_priv p = {.reg_base = mmio, .info = &soc_t8140_info};
    struct cpufreq_frequency_table table[2] = {{1}, {2}};
    struct cpufreq_policy policy = {&p, table, 7};
    reset(&p);
    assert(apple_soc_cpufreq_set_target(&policy, 0) == 0 && writes == 0);
    assert(apple_soc_cpufreq_set_target(&policy, 1) == 0 && writes == 1);
    assert(command == 0xa550000000400102ULL);
    assert(written == (0xa550000000400102ULL | BIT(25)));
    assert(apple_soc_cpufreq_set_target(&policy, 0) == 0 && writes == 2);
    for (unsigned int state = 0; state <= 32; state++) {
        reset(&p); table[0].driver_data = state;
        /* The j700 tree exposes the full DT ladder under the SMC two-tier
         * policy; the PMP policy and its QoS constraints own the cap.
         */
        const unsigned maximum = 31;
        if (state >= 1 && state <= maximum) {
            assert(apple_soc_cpufreq_set_target(&policy, 0) == 0);
            assert(FIELD_GET(APPLE_DVFS_CMD_PS1, command) == state);
            assert((command & ~31ULL) == 0xa550000000400100ULL);
            continue;
        }
        assert(apple_soc_cpufreq_set_target(&policy, 0) == -EINVAL);
        assert(reads == 0 && writes == 0);
    }
    /* An inherited P17 request is honoured as the current state, and the
     * minimum state is reached with one verified command.
     */
    reset(&p); command=(command & ~31ULL)|17; p.expected_pstate=17;
    table[0].driver_data=17;
    assert(apple_soc_cpufreq_set_target(&policy,0)==0 && reads==1 && !writes);
    table[0].driver_data=1;
    assert(apple_soc_cpufreq_set_target(&policy,0)==0 && writes==1);
    assert(p.expected_pstate==1);
    table[0].driver_data = 1;
    reset(&p); command = (command & ~31ULL) | 2;
    assert(apple_soc_cpufreq_set_target(&policy, 1) == -EIO);
    assert(p.transition_failed && writes == 0);
    for (unsigned int failure = 1; failure <= 3; failure++) {
        reset(&p); mode = failure;
        assert(apple_soc_cpufreq_set_target(&policy, 1) < 0);
        assert(p.transition_failed && writes == (failure == 1 ? 0U : 1U));
        unsigned int saved_reads = reads, saved_writes = writes;
        mode = 0;
        assert(apple_soc_cpufreq_set_target(&policy, 0) == -EIO);
        assert(reads == saved_reads && writes == saved_writes);
    }
    const struct apple_soc_cpufreq_info *older[] = {
        &soc_s5l8960x_info, &soc_t8103_info, &soc_t8112_info,
        &soc_t8132_info, &soc_default_info,
    };
    for (unsigned int i = 0; i < sizeof(older)/sizeof(older[0]); i++) {
        p.info = older[i]; reset(&p);
        table[0].driver_data = p.info->max_pstate;
        u64 expected = (command & ~p.info->ps1_mask) |
            ((u64)table[0].driver_data << p.info->ps1_shift) | BIT(25);
        if (p.info->has_ps2)
            expected = (expected & ~APPLE_DVFS_CMD_PS2) |
                FIELD_PREP(APPLE_DVFS_CMD_PS2, table[0].driver_data);
        assert(apple_soc_cpufreq_set_target(&policy, 0) == 0);
        assert(writes == 1 && reads == 1 && written == expected);
        reset(&p); table[0].driver_data = p.info->max_pstate + 1;
        assert(apple_soc_cpufreq_set_target(&policy, 0) == -EINVAL && !writes);
    }
    puts("Apple cpufreq command callback: PASS (including older profiles)");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='apple-cpufreq-test-') as tmp:
    source = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    source.write_text(prefix + defs + callback + test)
    for flags in ([], ['-DCONFIG_APPLE_PMP_V2_THERMAL']):
        subprocess.run([os.environ.get('CC', 'cc'), '-O1', '-g', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=address,undefined',
                        *flags, str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
