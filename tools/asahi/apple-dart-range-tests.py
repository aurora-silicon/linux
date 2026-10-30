#!/usr/bin/env python3
"""Execute the production DART flush path with device-address window controls."""
from pathlib import Path
import argparse
import subprocess
import tempfile
import re


def function(text, name):
    match = re.search(r'^static int ' + re.escape(name) + r'\(', text, re.M)
    start = text.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path,
                        default=Path(__file__).resolve().parents[2] / 'drivers/iommu/apple-dart.c')
    args = parser.parse_args()
    production = function(args.source.read_text(), 'apple_dart_domain_flush_tlb_range')
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
typedef uint64_t u64;
typedef uint32_t u32;
#define BITS_TO_LONGS(n) 1
#define atomic_long_read(x) (*(x))
#define bitmap_empty(x, n) (!(x)[0])
#define dev_err_ratelimited(...) ((void)0)
#define DART_T8110_TLB_CMD_OP_FLUSH_SID 1
struct io_pgtable_cfg { int unused; };
struct io_pgtable { struct io_pgtable_cfg cfg; };
#define io_pgtable_ops_to_pgtable(x) ((struct io_pgtable *)(x))
struct apple_dart;
struct apple_dart_stream_map { struct apple_dart *dart; unsigned long sidmap[1]; };
struct apple_dart_hw { int (*invalidate_tlb)(struct apple_dart_stream_map *); };
struct apple_dart { int num_streams, locked, version; bool fw_mirror, fw_handoff, managed_sids; void *dev; struct apple_dart_hw *hw; };
struct apple_dart_atomic_stream_map { struct apple_dart *dart; unsigned long sidmap[1]; };
struct apple_dart_domain {
 void *pgtbl_ops; bool dcp; struct apple_dart_atomic_stream_map streams[2]; int count;
 u64 dma_offset, mask;
};
#define for_each_stream_map(i,d,m) for ((i)=0; (i)<(d)->count && ((m)=&(d)->streams[i],1); (i)++)
static int powered, power_error, sync_error, range_error, syncs, ranges, fulls, firmware_flushes, quarantines, dcp_flushes;
static u64 seen_first, seen_last;
static int pm_runtime_resume_and_get(void *dev) { if (power_error && !dev) return power_error; powered++; return 0; }
static void pm_runtime_put(void *dev) { assert(powered>0); powered--; }
static int apple_dart_hw_sync_locked(struct io_pgtable_cfg *cfg, struct apple_dart_stream_map *stream, bool invalidate) {
 assert(invalidate); syncs++; return sync_error;
}
static int apple_dart_t8110_hw_tlb_command_range(struct apple_dart_stream_map *stream,
 int command, bool range, u64 first, u64 last) {
 assert(command==1 && range); ranges++; seen_first=first; seen_last=last; return range_error;
}
static int apple_dart_dcp_domain_flush(struct apple_dart_domain *domain) { assert(domain->dcp); dcp_flushes++; return -EAGAIN; }
static void apple_dart_managed_quarantine(struct apple_dart *dart) { assert(dart->managed_sids); quarantines++; }
static int apple_dart_fw_invalidate(struct apple_dart_stream_map *stream, u64 first, u64 last) {
 firmware_flushes++; seen_first=first; seen_last=last; return range_error;
}
static int full(struct apple_dart_stream_map *stream) { fulls++; return 0; }
'''
    source += production
    source += r'''
static void reset(void) {
 assert(powered==0); power_error=sync_error=range_error=syncs=ranges=fulls=0;
 seen_first=seen_last=0; firmware_flushes=quarantines=dcp_flushes=0;
}
int main(void) {
 struct apple_dart_hw hw={.invalidate_tlb=full};
 struct apple_dart dart={.num_streams=8,.locked=1,.version=0x202,.hw=&hw};
 struct io_pgtable pg={0};
 struct apple_dart_domain domain={.pgtbl_ops=&pg,.count=1,.dma_offset=1ULL<<40,.mask=(1ULL<<36)-1};
 domain.streams[0].dart=&dart; domain.streams[0].sidmap[0]=1;
 assert(!apple_dart_domain_flush_tlb_range(&domain,true,0x4000,0x7fff));
 assert(ranges==1 && fulls==0 && syncs==1 && powered==0);
 assert(seen_first==(1ULL<<40)+0x4000 && seen_last==(1ULL<<40)+0x7fff);
 reset(); domain.dma_offset=0;
 assert(!apple_dart_domain_flush_tlb_range(&domain,true,0x4000,0x7fff));
 assert(seen_first==0x4000 && seen_last==0x7fff);
 reset(); dart.version=0x201;
 assert(!apple_dart_domain_flush_tlb_range(&domain,true,0x4000,0x7fff));
 assert(fulls==1 && ranges==0);
 reset(); dart.version=0x202; dart.locked=0;
 assert(!apple_dart_domain_flush_tlb_range(&domain,true,0x4000,0x7fff));
 assert(fulls==1 && ranges==0 && syncs==0);
 reset(); dart.locked=1; sync_error=-EBUSY;
 assert(apple_dart_domain_flush_tlb_range(&domain,true,0x4000,0x7fff)==-EBUSY);
 assert(ranges==1 && fulls==0 && powered==0);
 reset(); power_error=-EHOSTDOWN;
 assert(apple_dart_domain_flush_tlb_range(&domain,true,0x4000,0x7fff)==-EHOSTDOWN);
 assert(syncs==0 && ranges==0 && fulls==0 && powered==0);
 reset(); range_error=-ETIMEDOUT;
 assert(apple_dart_domain_flush_tlb_range(&domain,true,0x4000,0x7fff)==-ETIMEDOUT);
 assert(ranges==1 && powered==0);
 reset(); dart.managed_sids=true; range_error=-ETIMEDOUT;
 assert(apple_dart_domain_flush_tlb_range(&domain,true,0x4000,0x7fff)==-ETIMEDOUT);
 assert(ranges==1 && quarantines==1 && powered==0);
 reset(); sync_error=-EBUSY; range_error=-ETIMEDOUT;
 assert(apple_dart_domain_flush_tlb_range(&domain,true,0x4000,0x7fff)==-EBUSY);
 assert(ranges==1 && quarantines==1 && powered==0);
 reset(); dart.fw_mirror=true;
 assert(!apple_dart_domain_flush_tlb_range(&domain,true,0x4000,0x7fff));
 assert(firmware_flushes==1 && ranges==0 && syncs==1);
 reset(); dart.fw_mirror=false; dart.fw_handoff=true;
 assert(!apple_dart_domain_flush_tlb_range(&domain,true,0x4000,0x7fff));
 assert(firmware_flushes==1 && ranges==0);
 reset(); dart.fw_handoff=false; domain.dcp=true;
 assert(apple_dart_domain_flush_tlb_range(&domain,true,0x4000,0x7fff)==-EAGAIN);
 assert(dcp_flushes==1 && ranges==0 && syncs==0);
 reset(); domain.dcp=false;
 struct apple_dart second=dart; second.dev=(void *)1;
 domain.streams[1].dart=&second; domain.streams[1].sidmap[0]=1; domain.count=2;
 power_error=-EHOSTDOWN;
 assert(apple_dart_domain_flush_tlb_range(&domain,true,0x4000,0x7fff)==-EHOSTDOWN);
 assert(ranges==1 && syncs==1 && powered==0);
 reset(); domain.count=1; domain.streams[0].sidmap[0]=0;
 assert(!apple_dart_domain_flush_tlb_range(&domain,true,0x4000,0x7fff));
 assert(syncs==0 && ranges==0 && fulls==0 && powered==0);
 puts("PASS: device DVA, low window, legacy/unlocked fallback, sync/power/command errors and empty stream");
}
'''
    with tempfile.TemporaryDirectory(prefix='dart-range-') as directory:
        p = Path(directory)
        (p / 'controls.c').write_text(source)
        subprocess.run(['cc', '-std=gnu11', '-Werror', str(p / 'controls.c'), '-o', str(p / 'controls')], check=True)
        subprocess.run([str(p / 'controls')], check=True)


if __name__ == '__main__':
    main()
