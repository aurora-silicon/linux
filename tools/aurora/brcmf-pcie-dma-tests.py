#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""BCM4388 firmware selection and bounded PCIe DMA controls."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PCIE = 'drivers/net/wireless/broadcom/brcm80211/brcmfmac/pcie.c'
FW = 'drivers/net/wireless/broadcom/brcm80211/brcmfmac/firmware.c'


def function(source, name):
    pattern = (r'^(?:static (?:inline )?)?(?:int|u64|dma_addr_t)\s+' + name + r'\('
               if name != 'brcmf_fw_alloc_request' else
               r'^struct brcmf_fw_request \*\s*' + name + r'\(')
    match = re.search(pattern, source, re.M)
    if not match:
        raise ValueError(name)
    opening = source.index('{', match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;typedef u64 dma_addr_t;
#define PCI_VENDOR_ID_BROADCOM 0x14e4
#define MODULE_FIRMWARE(s)
#define __counted_by(n)
#define BITS_PER_TYPE(t) (sizeof(t)*8)
#define BIT(n) (1U<<(n))
#define DMA_BIT_MASK(n) ((1ULL<<(n))-1)
#define BRCMF_FW_ALTPATH_LEN 256
#define min(a,b) ((a)<(b)?(a):(b))
#define min_not_zero(a,b) ((a)==0?(b):((b)==0?(a):min(a,b)))
#define IOMMU_COOKIE_DMA_MSI 1
#define brcmf_err(...) ((void)0)
#define brcmf_info(...) ((void)0)
#define dev_notice(...) ((void)0)
#define kzalloc_flex(object,member,count) calloc(1,sizeof(object)+sizeof((object).member[0])*(count))
static struct {char firmware_path[256];} brcmf_mp_global;
static void brcmf_chip_name(u32 chip,u32 rev,char *out,size_t n){snprintf(out,n,"%u/%u",chip,rev);}
static size_t strscpy(char *out,const char *in,size_t n){snprintf(out,n,"%s",in);return strlen(out);}
struct iova_domain {unsigned long start_pfn;unsigned int shift;};
struct iommu_dma_cookie {struct iova_domain iovad;};
struct iommu_domain {struct {u64 aperture_start,aperture_end;bool force_aperture;} geometry;struct iommu_dma_cookie *iova_cookie;int cookie_type;struct {u64 msi_iova;} *msi_cookie;};
struct dev_iommu {bool pci_32bit_workaround;};
struct device {u64 *dma_mask,coherent_dma_mask,bus_dma_limit;struct iommu_domain *domain;struct dev_iommu *iommu;};
struct pci_dev {u16 vendor,device;struct device dev;};
struct brcmf_chip {u32 chip,chiprev;};
struct brcmf_pciedev_info {struct pci_dev *pdev;struct brcmf_chip *ci;};
static int support_calls,fail_support,warnings,errors,allocation_calls;
static struct iommu_domain *iommu_get_domain_for_dev(struct device *d){return d->domain;}
static int dma_supported(struct device *d,u64 m){(void)d;(void)m;return ++support_calls!=fail_support;}
static void arch_dma_set_mask(struct device *d,u64 m){(void)d;(void)m;}
static void dma_setup_need_sync(struct device *d){(void)d;}
#define pci_warn(...) (warnings++)
#define pci_err(...) (errors++)
static unsigned int iova_shift(struct iova_domain *d){return d->shift;}
static unsigned long alloc_iova_fast(struct iova_domain *d,unsigned long count,unsigned long limit,bool flush){
 (void)flush;allocation_calls++;assert(count);return d->start_pfn<=limit&&count-1<=limit-d->start_pfn?d->start_pfn:0;
}
'''

TESTS = r'''
static struct iommu_dma_cookie cookie;
static struct dev_iommu di;
static struct iommu_domain domain;
static u64 mask;
static struct pci_dev pci;
static struct brcmf_chip chip;
static struct brcmf_pciedev_info info={.pdev=&pci,.ci=&chip};
static void reset(void){
 mask=DMA_BIT_MASK(32);support_calls=fail_support=warnings=errors=allocation_calls=0;
 cookie.iovad.shift=14;cookie.iovad.start_pfn=(1ULL<<40)>>14;
 domain=(struct iommu_domain){.geometry={.aperture_start=1ULL<<40,.aperture_end=(1ULL<<40)+0x200000-1,.force_aperture=true},.iova_cookie=&cookie};
 di.pci_32bit_workaround=true;
 pci=(struct pci_dev){.vendor=BRCM_PCIE_VENDOR_ID_BROADCOM,.device=BRCM_PCIE_4388_DEVICE_ID,.dev={.dma_mask=&mask,.coherent_dma_mask=DMA_BIT_MASK(32),.domain=&domain,.iommu=&di}};
 chip=(struct brcmf_chip){.chip=BRCM_CC_4388_CHIP_ID,.chiprev=6};
}
static int apply_dma(void){
#ifdef OLD_DMA
 return 0;
#else
 return brcmf_pcie_set_dma_mask(&info);
#endif
}
static void unchanged(void){u64 before=mask,coherent=pci.dev.coherent_dma_mask;assert(apply_dma()==0);assert(mask==before&&pci.dev.coherent_dma_mask==coherent&&support_calls==0);}
static void fw_expected(u32 id,u32 rev,const char *base){
 char bin[320],clm[320];struct brcmf_fw_name names[]={{".bin",bin},{".clm_blob",clm}};
 struct brcmf_fw_request *r=brcmf_fw_alloc_request(id,rev,brcmf_pcie_fwnames,sizeof(brcmf_pcie_fwnames)/sizeof(*brcmf_pcie_fwnames),names,2);
 assert(r);char expected[320];snprintf(expected,sizeof(expected),"brcm/%s.bin",base);assert(!strcmp(bin,expected));
 snprintf(expected,sizeof(expected),"brcm/%s.clm_blob",base);assert(!strcmp(clm,expected));free(r);
}
int main(void){
 int controls=0;
 reset();assert(apply_dma()==0);assert(iommu_dma_alloc_iova(&domain,16384,mask,&pci.dev)==(1ULL<<40));controls++;
 for(u32 r=0;r<32;r++){fw_expected(BRCM_CC_4388_CHIP_ID,r,r<4?"brcmfmac4388b0-pcie":r==6?"brcmfmac4388c2-pcie":"brcmfmac4388c0-pcie");controls++;}
 fw_expected(BRCM_CC_4387_CHIP_ID,7,"brcmfmac4387c2-pcie");controls++;
 fw_expected(BRCM_CC_4378_CHIP_ID,3,"brcmfmac4378b1-pcie");controls++;
 fw_expected(BRCM_CC_4378_CHIP_ID,5,"brcmfmac4378b3-pcie");controls++;
 char path[320];struct brcmf_fw_name name={".bin",path};
 assert(!brcmf_fw_alloc_request(BRCM_CC_4388_CHIP_ID,32,brcmf_pcie_fwnames,sizeof(brcmf_pcie_fwnames)/sizeof(*brcmf_pcie_fwnames),&name,1));controls++;
 for(unsigned rev=4;rev<=6;rev+=2){
  reset();chip.chiprev=rev;assert(apply_dma()==0&&support_calls==2&&mask==DMA_BIT_MASK(42)&&pci.dev.coherent_dma_mask==DMA_BIT_MASK(42));
  assert(iommu_dma_alloc_iova(&domain,16384,mask,&pci.dev)==(1ULL<<40));
  assert(iommu_dma_alloc_iova(&domain,16384,pci.dev.coherent_dma_mask,&pci.dev)==(1ULL<<40));controls++;
 }
 reset();mask=UINT64_MAX;assert(apply_dma()==0&&support_calls==1&&mask==UINT64_MAX&&pci.dev.coherent_dma_mask==DMA_BIT_MASK(42));controls++;
 reset();pci.dev.coherent_dma_mask=UINT64_MAX;assert(apply_dma()==0&&support_calls==1&&mask==DMA_BIT_MASK(42)&&pci.dev.coherent_dma_mask==UINT64_MAX);controls++;
 reset();mask=pci.dev.coherent_dma_mask=UINT64_MAX;unchanged();controls++;
 reset();domain.geometry.aperture_end=DMA_BIT_MASK(42);assert(apply_dma()==0&&support_calls==2);controls++;
 reset();domain.geometry.aperture_end=DMA_BIT_MASK(42)+1;unchanged();assert(warnings==1);controls++;
 reset();domain.geometry.aperture_end=domain.geometry.aperture_start-1;unchanged();assert(warnings==1);controls++;
 reset();domain.geometry.aperture_start=0;unchanged();controls++;
 reset();domain.geometry.aperture_start=DMA_BIT_MASK(32);unchanged();controls++;
 reset();domain.geometry.force_aperture=false;unchanged();controls++;
 reset();pci.dev.domain=NULL;unchanged();controls++;
 reset();chip.chiprev=5;unchanged();controls++;
 reset();chip.chiprev=3;unchanged();controls++;
 reset();chip.chiprev=7;unchanged();controls++;
 reset();chip.chip=BRCM_CC_4387_CHIP_ID;unchanged();controls++;
 reset();pci.device=BRCM_PCIE_4387_DEVICE_ID;unchanged();controls++;
 reset();pci.vendor=0x1234;unchanged();controls++;
 reset();fail_support=1;assert(apply_dma()==-EIO&&support_calls==1&&errors==1&&mask==DMA_BIT_MASK(32)&&pci.dev.coherent_dma_mask==DMA_BIT_MASK(32));controls++;
 reset();fail_support=2;assert(apply_dma()==-EIO&&support_calls==2&&errors==1&&pci.dev.coherent_dma_mask==DMA_BIT_MASK(32));controls++;
 reset();mask=pci.dev.coherent_dma_mask=UINT64_MAX;pci.dev.bus_dma_limit=DMA_BIT_MASK(32);unchanged();assert(!iommu_dma_alloc_iova(&domain,16384,mask,&pci.dev));controls++;
 printf("%d firmware/DMA production controls PASS\n",controls);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--firmware-revision', help='Git revision for firmware mapping control')
    parser.add_argument('--old-dma', help='Git revision without the DMA mask setup')
    parser.add_argument('--mutation', choices=['narrow-stream', 'ignore-aperture', 'revision-wide'])
    args = parser.parse_args()
    source = (args.source / PCIE).read_text()
    fwsource = subprocess.check_output(['git', '-C', str(args.source), 'show', args.firmware_revision + ':' + PCIE], text=True) if args.firmware_revision else source
    old = subprocess.check_output(['git', '-C', str(args.source), 'show', args.old_dma + ':' + PCIE], text=True) if args.old_dma else None
    if old:
        probe = function(old, 'brcmf_pcie_probe')
        assert 'dma_set_mask' not in probe and 'brcmf_pcie_set_dma_mask' not in probe
    ids = (args.source / 'drivers/net/wireless/broadcom/brcm80211/include/brcm_hw_ids.h').read_text()
    ids = re.sub(r'^#include .*$', '', ids, flags=re.M)
    header = (args.source / 'drivers/net/wireless/broadcom/brcm80211/brcmfmac/firmware.h').read_text()
    table = fwsource[fwsource.index('BRCMF_FW_DEF(43602'):fwsource.index('\n#define BRCMF_PCIE_FW_UP_TIMEOUT')]
    dma = function(source, 'brcmf_pcie_set_dma_mask')
    if args.mutation == 'narrow-stream':
        dma = dma.replace('if (dma_get_mask(dev) < end)', 'if (true)')
    if args.mutation == 'ignore-aperture':
        dma = dma.replace('domain->geometry.aperture_start <= DMA_BIT_MASK(32)', 'false')
    if args.mutation == 'revision-wide':
        table = table.replace('0xFFFFFFB0, 4388C0', '0x00000030, 4388C0').replace('0x00000040, 4388C2', '0xFFFFFFC0, 4388C2')
    bodies = {
        'dma_get_mask': function((args.source / 'include/linux/dma-mapping.h').read_text(), 'dma_get_mask'),
        'dma_set_mask': function((args.source / 'kernel/dma/mapping.c').read_text(), 'dma_set_mask'),
        'dma_set_coherent_mask': function((args.source / 'kernel/dma/mapping.c').read_text(), 'dma_set_coherent_mask'),
        'iommu_dma_alloc_iova': function((args.source / 'drivers/iommu/dma-iommu.c').read_text(), 'iommu_dma_alloc_iova'),
        'brcmf_fw_alloc_request': function((args.source / FW).read_text(), 'brcmf_fw_alloc_request'),
        'brcmf_pcie_set_dma_mask': dma,
    }
    prefix = '#define OLD_DMA\n' if old else ''
    generated = '\n'.join([prefix, PREFIX, ids, header, table, *bodies.values(), TESTS])
    output = args.output or Path(tempfile.mkdtemp(prefix='brcmf-pcie-dma-'))
    output.mkdir(parents=True, exist_ok=True)
    cfile = output / 'controls.c'
    cfile.write_text(generated)
    binary = output / 'controls'
    compiled = subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined', str(cfile), '-o', str(binary)], capture_output=True, text=True)
    (output / 'compile.log').write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        raise SystemExit(compiled.stderr)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    (output / 'run.log').write_text(run.stdout + run.stderr)
    receipt = {'returncode': run.returncode, 'stdout': run.stdout,
               'body_sha256': {n: hashlib.sha256(v.encode()).hexdigest() for n, v in bodies.items()},
               'table_sha256': hashlib.sha256(table.encode()).hexdigest(),
               'firmware_revision': args.firmware_revision, 'old_dma': args.old_dma, 'mutation': args.mutation,
               'original_probe_sha256': hashlib.sha256(probe.encode()).hexdigest() if old else None,
               'scope': 'Actual firmware allocator/table, DMA setup/mask API and IOMMU allocator functions; allocation, geometry and platform callbacks controlled. Old DMA control omits new setup as verified absent from original probe; not a compiled original full probe.'}
    (output / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))
    raise SystemExit(run.returncode != 0)


if __name__ == '__main__':
    main()
