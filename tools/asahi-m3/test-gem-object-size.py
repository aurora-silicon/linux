#!/usr/bin/env python3
"""Check actual GEM size validation at zero, page boundaries and usize overflow."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/gem.rs').read_text()
method=s[s.index('fn checked_object_size('):s.index('\nfn new_kernel_object_with_cpu_mapping(')]
harness=r'''
const EINVAL:i32=-22;const EOVERFLOW:i32=-75;type Result<T>=std::result::Result<T,i32>;
mod mmu{pub const UAT_PGMSK:usize=16383;}
/*METHOD*/
fn main(){
 assert_eq!(checked_object_size(0),Err(EINVAL));
 for size in 1..100000 {
  let actual=checked_object_size(size).unwrap();
  assert_eq!(actual%16384,0);assert!(actual>=size);assert!(actual-size<16384);
 }
 let aligned=usize::MAX&!16383;
 assert_eq!(checked_object_size(aligned),Ok(aligned));
 for size in aligned+1..=usize::MAX {assert_eq!(checked_object_size(size),Err(EOVERFLOW));}
 println!("PASS zero rejection, page rounding, and every overflowing final-page size");
}
'''.replace('/*METHOD*/',method)
with tempfile.TemporaryDirectory(prefix='gem-size-') as tmp:
 p=Path(tmp)/'test.rs';p.write_text(harness);exe=p.with_suffix('')
 for flags in [[],['-O']]:
  subprocess.run(['rustc','--edition=2021',*flags,str(p),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
