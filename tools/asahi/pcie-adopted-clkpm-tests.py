#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise adopted-link Clock PM policy using the production C helpers."""
from pathlib import Path
import re, resource, subprocess, tempfile

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root=Path(__file__).resolve().parents[2]
src=(root/'drivers/pci/controller/pcie-apple.c').read_text()
def function(name):
    start=re.search(r'^static (?:void|int) '+name+r'\(',src,re.M).start()
    end=src.index('{',start)+1;depth=1
    while depth:
        depth+=(src[end]=='{')-(src[end]=='}');end+=1
    return src[start:end]
production='\n'.join(function(n) for n in ['apple_pcie_adopted_no_clkpm','apple_pcie_adopted_no_clkpm_walk','apple_pcie_host_init'])
fixture=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef uint32_t u32;
#define PCI_EXP_LNKCAP 12
#define PCI_EXP_LNKCAP_CLKPM (1U << 18)
#define PCIE_LINK_STATE_CLKPM (1U << 2)
struct pci_bus { bool root; };
struct pci_dev { struct pci_bus *bus; bool pcie; };
struct platform_device { int unused; };
struct hw_info { bool adopt_link; };
struct apple_pcie { struct hw_info *hw; };
struct pci_host_bridge { struct pci_bus *bus; struct apple_pcie *priv; };
static int reads, disables, locks, warnings, infos, walks, common_ret, disable_ret, read_ret;
static u32 cap;
static int apple_pcie_cfg_ecam_ops;
static bool pci_is_root_bus(struct pci_bus *b) { return b->root; }
static bool pci_is_pcie(struct pci_dev *p) { return p->pcie; }
static int pcie_capability_read_dword(struct pci_dev *p, int reg, u32 *v)
{ assert(reg==PCI_EXP_LNKCAP); reads++; *v=cap; return read_ret; }
static int pci_disable_link_state(struct pci_dev *p, int mask)
{ assert(mask==PCIE_LINK_STATE_CLKPM); disables++; return disable_ret; }
static int pci_disable_link_state_locked(struct pci_dev *p, int mask)
{ locks++; return pci_disable_link_state(p,mask); }
#define pci_warn(...) (warnings++)
#define pci_info(...) (infos++)
static struct apple_pcie *pci_host_bridge_priv(struct pci_host_bridge *b) { return b->priv; }
static int pci_host_common_init(struct platform_device *p, struct pci_host_bridge *b, void *ops)
{ assert(ops==&apple_pcie_cfg_ecam_ops); return common_ret; }
static void pci_walk_bus(struct pci_bus *b, int (*fn)(struct pci_dev *,void *), void *data)
{ struct pci_dev p={b,true}; walks++; assert(fn(&p,data)==0); }
/*PRODUCTION*/
static void reset(void)
{ reads=disables=locks=warnings=infos=walks=common_ret=disable_ret=read_ret=0; cap=PCI_EXP_LNKCAP_CLKPM; }
int main(void)
{
 struct pci_bus bus={false}; struct pci_dev dev={&bus,true};
 reset();bus.root=true;apple_pcie_adopted_no_clkpm(&dev,false);assert(!reads&&!disables);bus.root=false;
 reset();dev.pcie=false;apple_pcie_adopted_no_clkpm(&dev,false);assert(!reads&&!disables);dev.pcie=true;
 reset();read_ret=-EIO;apple_pcie_adopted_no_clkpm(&dev,false);assert(reads==1&&!disables);
 reset();cap=0;apple_pcie_adopted_no_clkpm(&dev,false);assert(reads==1&&!disables);
 for(int locked=0;locked<2;locked++) {
  reset();apple_pcie_adopted_no_clkpm(&dev,locked);assert(disables==1&&locks==locked&&infos==locked&&!warnings);
  reset();disable_ret=-EINVAL;apple_pcie_adopted_no_clkpm(&dev,locked);assert(disables==1&&locks==locked&&!infos&&!warnings);
  reset();disable_ret=-EIO;apple_pcie_adopted_no_clkpm(&dev,locked);assert(disables==1&&locks==locked&&!infos&&warnings==1);
 }
 struct hw_info hw={false};struct apple_pcie pcie={&hw};struct pci_host_bridge bridge={&bus,&pcie};struct platform_device platform={0};
 reset();assert(apple_pcie_host_init(&platform,&bridge)==0);assert(!walks&&!disables);
 reset();hw.adopt_link=true;assert(apple_pcie_host_init(&platform,&bridge)==0);assert(walks==1&&disables==1&&locks==1);
 reset();common_ret=-EIO;assert(apple_pcie_host_init(&platform,&bridge)==-EIO);assert(!walks&&!disables);
 return 0;
}
'''
variants={'production':production,'wrong-mask':production.replace('PCIE_LINK_STATE_CLKPM','1',2),'lost-host-gate':production.replace('!ret && pcie->hw->adopt_link','!ret && (pcie->hw->adopt_link || true)')}
with tempfile.TemporaryDirectory() as td:
    d=Path(td)
    for name,body in variants.items():
        c=d/(name+'.c');exe=d/name;c.write_text(fixture.replace('/*PRODUCTION*/',body))
        subprocess.run(['clang','-std=gnu11','-Wall','-Werror','-Wno-unused-parameter',str(c),'-o',str(exe)],check=True)
        r=subprocess.run([str(exe)],capture_output=True)
        assert (r.returncode==0)==(name=='production'),(name,r.stderr.decode())
        print(name,'PASS' if not r.returncode else 'negative control rejected',flush=True)
print('13 production cases; 2 negative controls PASS')
