# Copyright 2026 Chris Kearney
# SPDX-License-Identifier: MIT
import json, pathlib, shlex, subprocess, sys
src, build, out = map(lambda p: pathlib.Path(p).resolve(), sys.argv[1:])
c = next(c for c in json.loads((build/'compile_commands.json').read_text()) if c['file'].endswith('/u_queue.c'))
args=[]; skip=False
for x in shlex.split(c['command']):
    if skip: skip=False; continue
    if x in ('-o','-MQ','-MF','-MT'): skip=True; continue
    if x in ('-c','-MD','-MMD') or x==c['file']: continue
    args.append(x)
libs=[str(build/'src/util'/name) for name in ('libmesa_util.a','libmesa_util_clflush.a','libmesa_util_clflushopt.a','libmesa_util_simd.a') if (build/'src/util'/name).exists()]
cmd=args+[str(src/'src/util/tests/u_queue_sandbox_test.c')]+libs+['-pthread','-ldl','-lm','-lz','-lzstd','-ldrm','-o',str(out)]
subprocess.run(cmd,cwd=build,check=True)
subprocess.run([str(out)],check=True)
