#!/usr/bin/env python3
"""Validate native backing density from the unchanged same-quarter oracle."""
import json,sys
from pathlib import Path

def check(path):
 rows=[json.loads(x) for x in Path(path).read_text().splitlines() if x.startswith('{')]
 assert len(rows)==2, 'Require exactly two measured arms'
 arms={x['arm']:x for x in rows}
 assert set(arms)=={'same-quarter-zero','balanced'}
 for name,r in arms.items():
  assert r['logical_parent_bytes']==16*1024*1024 and r['modified_child_bytes']==4*1024*1024
  assert r['content_ok'] and r['disjoint_ok']
  assert r['modified_parent_native_overlap']==0 and r['modified_unchanged_native_overlap']==0
  assert r['modified_child']['represented_bytes']==4*1024*1024
  n=r['modified_child']['native_bytes']
  assert 4*1024*1024<=n<=6*1024*1024, (name,'native backing outside packed bound',n)
 assert sum(arms['same-quarter-zero']['modified_physical_quarter_counts'][1:])>0
 print('PASS same-quarter/balanced native density, actual relocation and isolation')

if __name__=='__main__':
 if len(sys.argv)!=2:sys.exit('usage: check-any-quarter.py same-quarter.log')
 try:check(sys.argv[1])
 except (AssertionError,KeyError,ValueError,OSError) as e:sys.exit('FAIL '+str(e))
