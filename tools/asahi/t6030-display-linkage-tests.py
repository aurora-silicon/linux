#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual G15 Kconfig stanza through the kernel's conf in 18 combinations.

The test uses a minimal Kconfig containing the actual production G15 stanza and
its dependency/select names, so unrelated architecture/compiler options do not
hide invalid provider/consumer linkage. It writes only the supplied output dir.
"""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import subprocess
ROOT = Path(__file__).resolve().parents[2]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--conf', required=True, type=Path, help='built scripts/kconfig/conf')
    args = p.parse_args(); args.output.mkdir(parents=True, exist_ok=False)
    source = (ROOT / 'drivers/gpu/drm/asahi/Kconfig').read_text()
    stanza = source.split('config DRM_ASAHI_G15\n', 1)[1].split('\nconfig ', 1)[0]
    stanza = 'config DRM_ASAHI_G15\n' + stanza
    assert 'depends on DRM_APPLE=y || (DRM_APPLE=m && DRM_ASAHI=m)' in stanza
    kconfig = '''mainmenu "T6030 display linkage controls"
config MODULES
    bool "Modules"
    modules
    default y
config DRM_APPLE
    tristate "Display"
config DRM_ASAHI
    tristate "GPU"
config APPLE_T6030_GPU_PMP_LINK
    bool
config CRYPTO_LIB_SHA256
    bool
''' + stanza
    source_path = args.output.resolve() / 'Kconfig'
    source_path.write_text(kconfig)
    results = []
    for gpu, display, request in itertools.product(('n', 'm', 'y'), ('n', 'm', 'y'), ('n', 'y')):
        case = args.output / f'asahi-{gpu}-display-{display}-g15-{request}'
        case.mkdir()
        config = case.resolve() / 'config'
        config.write_text(f'CONFIG_MODULES=y\nCONFIG_DRM_ASAHI={gpu}\nCONFIG_DRM_APPLE={display}\nCONFIG_DRM_ASAHI_G15={request}\n')
        env = os.environ | {'KCONFIG_CONFIG': str(config)}
        result = subprocess.run([str(args.conf.resolve()), '--olddefconfig', str(source_path)],
                                env=env, cwd=case, capture_output=True, text=True)
        (case / 'conf.log').write_text(result.stdout + result.stderr)
        actual = dict(re.findall(r'^CONFIG_(\w+)=(\w+)$', config.read_text(), re.M))
        expected = 'y' if request == 'y' and gpu != 'n' and (display == 'y' or display == 'm' and gpu == 'm') else 'n'
        got = actual.get('DRM_ASAHI_G15', 'n')
        good = result.returncode == 0 and got == expected and actual.get('DRM_ASAHI', 'n') == gpu and actual.get('DRM_APPLE', 'n') == display
        results.append({'gpu': gpu, 'display': display, 'g15_requested': request,
                        'g15_expected': expected, 'g15_actual': got, 'conf_rc': result.returncode, 'pass': good})
    glue = (ROOT / 'drivers/gpu/drm/asahi/g15/glue.c').read_text()
    start = glue.index('int asahi_g15_pmp_prepare(struct device *dev)\n')
    body = glue[start:glue.index('\nint asahi_g15_pmp_get(', start)]
    assert 'ret = apple_drm_t6030_link_device(dev);' in body
    assert 'symbol_get(' not in body
    # The provider does not import any of Asahi's glue symbols. Full modpost
    # and the actual .modinfo dependency are separately required build gates.
    provider = '\n'.join(f.read_text() for f in (ROOT / 'drivers/gpu/drm/apple').glob('*.c'))
    assert not re.search(r'\basahi_g15_\w+\s*\(', provider)
    good = all(row['pass'] for row in results)
    (args.output / 'receipt.json').write_text(json.dumps({'passed': good,
        'stanza_sha256': hashlib.sha256(stanza.encode()).hexdigest(),
        'direct_import_checked': True, 'modpost_or_hardware_claimed': False, 'results': results}, indent=2) + '\n')
    print(json.dumps({'passed': good, 'configurations': len(results), 'accepted_g15_linkages': [[r['gpu'], r['display']] for r in results if r['g15_actual'] == 'y']}))
    return not good
if __name__ == '__main__':
    raise SystemExit(main())
