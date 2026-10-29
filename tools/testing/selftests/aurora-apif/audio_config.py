#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Resolve the actual Apple DRM Kconfig with controlled sound/DRM linkage."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]


def main():
    conf = Path(os.environ.get("KCONFIG_CONF", ROOT / "scripts/kconfig/conf"))
    if not conf.is_file():
        print("TAP version 13\n1..1\nok 1 - audio linkage # SKIP set KCONFIG_CONF to built scripts/kconfig/conf")
        return 4
    conf = conf.resolve()
    # Keep only the surrounding platform dependencies synthetic. The driver
    # dependencies and selects under test are sourced directly from the tree.
    fixture = 'config MODULES\n\tbool\n\tdefault y\n\tmodules\n'
    for name in ("DRM", "OF", "ARM64", "ARCH_APPLE", "APPLE_RTKIT",
                 "OF_ADDRESS", "BACKLIGHT_CLASS_DEVICE"):
        fixture += f'config {name}\n\tbool\n\tdefault y\n'
    for name in ("APPLE_PMP_REPORT", "DRM_CLIENT_SELECTION", "DRM_KMS_HELPER",
                 "DRM_KMS_DMA_HELPER", "DRM_GEM_DMA_HELPER",
                 "VIDEOMODE_HELPERS", "MULTIPLEXER"):
        fixture += f'config {name}\n\ttristate\n'
    fixture += '''config SOUND
\ttristate "Sound"
if SOUND
config SND
\ttristate "ALSA"
if SND
config SND_PCM
\ttristate
config SND_DMAENGINE_PCM
\ttristate
endif
endif
'''
    fixture += f'source "{ROOT / "drivers/gpu/drm/apple/Kconfig"}"\n'
    print("TAP version 13\n1..19", flush=True)
    number = 0
    with tempfile.TemporaryDirectory(prefix="apple-audio-kconfig-") as directory:
        tmp = Path(directory)
        (tmp / "Kconfig").write_text(fixture)
        for drm in ("n", "m", "y"):
            for sound, snd in (("n", "n"), ("m", "n"), ("m", "m"),
                               ("y", "n"), ("y", "m"), ("y", "y")):
                number += 1
                config = tmp / ".config"
                config.write_text(f"CONFIG_DRM_APPLE={drm}\nCONFIG_SOUND={sound}\n"
                                  f"CONFIG_SND={snd}\nCONFIG_DRM_APPLE_AUDIO=y\n")
                result = subprocess.run([str(conf), "--olddefconfig", "Kconfig"],
                                        cwd=tmp, env={**os.environ, "KCONFIG_CONFIG": str(config)},
                                        text=True, capture_output=True, check=True)
                assert not result.stderr, result.stderr
                values = dict(line.split("=", 1) for line in config.read_text().splitlines()
                              if line.startswith("CONFIG_") and "=" in line)
                expected = drm != "n" and snd != "n" and (drm == "m" or snd == "y")
                assert (values.get("CONFIG_DRM_APPLE_AUDIO") == "y") == expected, values
                for symbol in ("SND_PCM", "SND_DMAENGINE_PCM"):
                    assert values.get("CONFIG_" + symbol, "n") == (drm if expected else "n"), values
                assert values.get("CONFIG_DRM_APPLE", "n") == drm, values
                assert values.get("CONFIG_SOUND", "n") == sound, values
                assert values.get("CONFIG_SND", "n") == snd, values
                print(f"ok {number} - DRM_APPLE={drm} SOUND={sound} SND={snd}")
    fragment = (ROOT / "arch/arm64/configs/aurora-apif.config").read_text().splitlines()
    for symbol in ("DRM_APPLE", "SOUND", "SND", "DRM_APPLE_AUDIO"):
        assert f"CONFIG_{symbol}=y" in fragment
    print("ok 19 - APIF fragment preserves built-in display audio")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
