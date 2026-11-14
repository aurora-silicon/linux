#!/usr/bin/env python3
"""The repository-documented Rust 1.85 lint compatibility adapter."""
from pathlib import Path
import subprocess
import sys

args = sys.argv[1:]
dest = None
for i, arg in enumerate(args):
    if arg == '--':
        break
    if arg in ('-o', '--output'):
        dest = args[i + 1]
    elif arg.startswith('--output='):
        dest = arg.split('=', 1)[1]
result = subprocess.run([str(Path(__file__).resolve().parent / 'native-bindgen'), *args], stdout=subprocess.PIPE)
attribute = b'#[allow(unnecessary_transmutes)]'
if result.returncode == 0 and dest and dest != '-':
    path = Path(dest)
    data = path.read_bytes()
    if attribute in data:
        path.write_bytes(data.replace(attribute, b''))
sys.stdout.buffer.write(result.stdout.replace(attribute, b''))
raise SystemExit(result.returncode)
