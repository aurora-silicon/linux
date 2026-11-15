#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise production RTI publication with actual chip register profiles."""
from pathlib import Path
import re
import resource
import subprocess
import tempfile


def definition(source, name):
    start = source.index(name)
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def program(source):
    names = ['BCM4377_BAR0_RTI_CONTROL', 'BCM4377_BAR2_RTI_WINDOW_LO',
             'BCM4377_BAR2_RTI_WINDOW_HI', 'BCM4377_BAR2_RTI_WINDOW_SIZE',
             'BCM4377_BAR2_CONTEXT_ADDR_LO', 'BCM4377_BAR2_CONTEXT_ADDR_HI',
             'BCM4388_BAR2_CONTEXT_ADDR_HI']
    defines = '\n'.join(re.search(r'^#define\s+' + name + r'\s+[^\n]+', source,
                                  re.M).group() for name in names)
    profiles = []
    for chip in [4377, 4378, 4387, 4388]:
        entry = definition(source, f'\t[BCM{chip}] = {{')
        offset = re.search(r'\.bar2_offset\s*=\s*(\w+)', entry)
        high = re.search(r'\.bar2_context_addr_hi\s*=\s*(\w+)', entry)
        profiles.append('{%s,%s}' % (offset.group(1) if offset else '0',
                                    high.group(1) if high else '0'))
    code = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct { unsigned value; } atomic_t;
struct bcm4377_hw { u32 bar2_offset, bar2_context_addr_hi; };
struct bcm4377_data {
    void *bar0, *bar2;
    const struct bcm4377_hw *hw;
    u64 ctx_dma, dma_window_base;
    u32 dma_window_size;
    atomic_t irq_count;
    struct { bool enabled; } control_ack_ring;
};
struct write_event { uintptr_t addr; u32 value; };
static struct write_event writes[16];
static unsigned n_writes, fail_phase, failures, waits;
static u32 capabilities;
#define BCM4377_TIMEOUT 1000
#define lower_32_bits(v) ((u32)(v))
#define upper_32_bits(v) ((u32)((u64)(v)>>32))
#define dev_dbg(...) ((void)0)
#define dev_info(...) ((void)0)
static unsigned atomic_read(const atomic_t *v) { return v->value; }
static void iowrite32(u32 value, void *address)
{
    assert(n_writes<16);
    writes[n_writes++]=(struct write_event){(uintptr_t)address,value};
    if ((uintptr_t)address==0x20400450) capabilities=value;
}
static int bcm4377_wait_for_state(struct bcm4377_data *b, bool rti,
                                 u32 state, unsigned long timeout)
{
    assert(rti && timeout==1000);
    assert(writes[n_writes-1].addr==0x10000144);
    assert(writes[n_writes-1].value==state);
    waits++;
    if (state==fail_phase) return -ETIMEDOUT;
    b->irq_count.value++;
    return 0;
}
static void bcm4377_report_state_failure(struct bcm4377_data *b,
                                         const char *phase, u32 expected,
                                         int error, u32 irq_before)
{
    assert(b && !strcmp(phase,"RTI") && expected==fail_phase);
    assert(error==-ETIMEDOUT && irq_before==expected-1);
    failures++;
}
'''
    code += defines + '\n'
    code += 'static const struct bcm4377_hw profiles[]={' + ','.join(profiles) + '};\n'
    code += definition(source, 'static int bcm4377_setup_rti(struct bcm4377_data *bcm4377)\n{') + '\n'
    code += r'''
static void check_write(unsigned index, uintptr_t addr, u32 value)
{
    assert(index<n_writes);
    assert(writes[index].addr==addr && writes[index].value==value);
}
static void test(unsigned profile, u64 context, bool high_window, unsigned failure)
{
    struct bcm4377_data b={
        .bar0=(void *)0x10000000,.bar2=(void *)0x20000000,
        .hw=&profiles[profile],.ctx_dma=context,
        .dma_window_base=high_window?0x10000100000ULL:0,
        .dma_window_size=high_window?0x3fe00000:0xffffffff,
    };
    /* Source-backed BCM4388 effective ABI; legacy profiles stay unchanged. */
    uintptr_t base=profile==3 ? 0x20400000 : 0x20200000;
    uintptr_t high=profile==3 ? (high_window?0x20400490:0x20400450) : 0x20200450;
    n_writes=failures=waits=0; fail_phase=failure; capabilities=0x0a;
    int ret=bcm4377_setup_rti(&b);
    check_write(0,0x10000144,1);
    if (failure==1) {
        assert(ret==-ETIMEDOUT && n_writes==1 && waits==1 && failures==1);
        assert(!b.control_ack_ring.enabled);
        return;
    }
    assert(n_writes==7 && waits==2);
    check_write(1,base+0x494,high_window?0x00100000:0);
    check_write(2,base+0x498,high_window?0x100:0);
    check_write(3,base+0x49c,high_window?0x3fe00000:0xffffffff);
    check_write(4,base+0x48c,(u32)context);
    check_write(5,high,(u32)(context>>32));
    check_write(6,0x10000144,2); /* publish both pointer halves before request2 */
    if (profile==3) assert(capabilities==(high_window?0x0a:(u32)(context>>32)));
    if (failure==2)
        assert(ret==-ETIMEDOUT && failures==1 && !b.control_ack_ring.enabled);
    else
        assert(!ret && !failures && b.control_ack_ring.enabled);
}
int main(void)
{
    for (unsigned profile=0; profile<4; profile++) {
        test(profile,0x12345678,false,0);
        test(profile,0x12345678,false,1);
        test(profile,0x12345678,false,2);
    }
    test(3,0x12345678,true,0);
    test(3,0x100001a0000ULL,true,0);
    test(3,0x100001a0000ULL,true,1);
    test(3,0x100001a0000ULL,true,2);
    puts("BCM4377 RTI addresses: all chip profiles, full-width publication, capability preservation and phase failures PASS");
}
'''
    return code


def main():
    root = Path(__file__).resolve().parents[5]
    source = (root / 'drivers/bluetooth/hci_bcm4377.c').read_text()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='bcm4377-rti-address-') as tmp:
        c, exe = Path(tmp) / 'rti.c', Path(tmp) / 'rti'
        for control in ["production", "old-high-register", "unscoped-register"]:
            tested = source
            if control == "old-high-register":
                tested = source.replace('.bar2_context_addr_hi = BCM4388_BAR2_CONTEXT_ADDR_HI',
                                        '.bar2_context_addr_hi = BCM4377_BAR2_CONTEXT_ADDR_HI')
            elif control == "unscoped-register":
                tested = source.replace('bcm4377->dma_window_base ?\n\t\tbcm4377->hw->bar2_context_addr_hi : BCM4377_BAR2_CONTEXT_ADDR_HI',
                                        'bcm4377->hw->bar2_context_addr_hi')
            if control != "production":
                assert tested != source
            c.write_text(program(tested))
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            if control != "production":
                assert result.returncode and 'Assertion' in result.stderr, result
                print('BCM4388 ' + control + ' negative control: detected at runtime')
            else:
                assert result.returncode == 0, result.stderr
                print(result.stdout.strip())


if __name__ == '__main__':
    main()
