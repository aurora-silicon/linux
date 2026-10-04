#!/usr/bin/env python3
"""Exercise actual allocation tickets: last owner, overflow and concurrent churn."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/agx_memory_stats.rs').read_text()
body=s[s.index('struct Counters {'):s.index('pub(crate) struct View;')]
harness=r'''
use std::sync::{Arc,atomic::{AtomicU64,Ordering}};
const EINVAL:i32=-22;const EOVERFLOW:i32=-75;type Result<T>=std::result::Result<T,i32>;
/*BODY*/
fn main(){
 assert!(matches!(Allocation::gem(0,false),Err(EINVAL)));
 assert_eq!(USER_GEM.snapshot(),[0,0,0,0]);
 let a=Arc::new(Allocation::gem(16384,false).unwrap());let b=a.clone();
 drop(a);assert_eq!(&USER_GEM.snapshot()[..2],&[1,16384]);
 drop(b);assert_eq!(USER_GEM.snapshot(),[0,0,1,16384]);
 let mut threads=Vec::new();
 for _ in 0..8{threads.push(std::thread::spawn(||{for _ in 0..10000{let a=Allocation::gem(32768,true).unwrap();let b=Allocation::coherent(65536).unwrap();drop(a);drop(b);}}));}
 for t in threads{t.join().unwrap();}
 assert_eq!(&KERNEL_GEM.snapshot()[..2],&[0,0]);assert_eq!(&COHERENT.snapshot()[..2],&[0,0]);
 assert!(KERNEL_GEM.snapshot()[2]>=1);assert!(COHERENT.snapshot()[3]>=65536);
 let huge=Allocation::gem(usize::MAX,false).unwrap();
 let before=USER_GEM.snapshot();
 assert!(matches!(Allocation::gem(1,false),Err(EOVERFLOW)));
 assert_eq!(USER_GEM.snapshot(),before,"overflow changed accounting");
 drop(huge);assert_eq!(&USER_GEM.snapshot()[..2],&[0,0]);
 println!("PASS last-owner release, concurrent balanced churn, high water and overflow rollback");
}
'''.replace('/*BODY*/',body)
with tempfile.TemporaryDirectory(prefix='memory-accounting-') as tmp:
 p=Path(tmp)/'test.rs';p.write_text(harness);exe=p.with_suffix('')
 subprocess.run(['rustc','--edition=2021',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
