# M3 GPU quick start

For the **13-inch M3 MacBook Air (J613)** on the supported 14.8.3 stub,
use the current released installer. No 25G83 migration is needed.

1. Enable the matched persistent GPU profile:

   ```sh
   curl -fsSL https://github.com/iconidentify/aurora-linux/releases/latest/download/install-aurora-sep.sh | bash -s -- --m3-gpu-persistent
   ```

2. Reboot and log into your desktop normally.

3. Check the session and run the real GPU readback probe:

   ```sh
   cat /run/user/$(id -u)/mesa-m3-session.state
   uwsm-app -- /opt/mesa-m3/bin/mesa-m3-probe --out "$HOME/mesa-m3-probe.txt"
   cat "$HOME/mesa-m3-probe.txt"
   ```

Expect an **active** session, an **Apple M3** hardware renderer (legacy
may say Zink), `gl.render=pass` with red/blue known-answer pixels, a
hardware Apple M3 Vulkan device, and final `result=pass`. The probe also
records its loaded libraries and GPU file descriptors. A working panel
or an installed package alone does not prove GPU acceleration.

If the boot or desktop fails, select **Aurora previous (GPU off)** in
Limine or the retained previous kernel in GRUB; keep the exact error and
run the same installer with `--m3-report` for a read-only report.

The install command is an explicit experimental opt-in. It installs the
matched kernel, Mesa and bootloader together and keeps a GPU-off fallback.
`--m3-gpu-experiment` only installs one-shot tools; it is not needed for
this persistent route. The shorter **`--m3-gpu`** alias in new matched
installers selects the supported profile automatically; released 12.6
predates that alias, so use the command above today.

| Mac | Current route |
| --- | --- |
| Air 13-inch J613 | The persistent command above |
| Pro 16-inch J516S | Default supported display/GPU handoff install |
| Pro 14-inch J514S | First activate with `--m3-handoff` |
| Air 15-inch J615, base-M3 Pro/iMac, M3 Max | GPU handoff support remains pending |

## Firmware profiles

System firmware 26.6.2 does **not** select the native GPU profile. The
legacy OS-firmware label can be 14.7 while its actual stub and GPU image
are 14.8.3. New `--m3-gpu` installers read the GPU's exact compatibility
when present; unarmed legacy boots may omit it, so existing exact stub
and iBoot checks apply. Updated installers admit clean `v1.6.1` stage 1
with either supported OS label only in the checked legacy J613
configuration; an older installer may refuse the 14.7 label. Unsupported
descriptors or failed boot/package checks stop activation.

J613 already booted from its own exact 26.6.2/25G83 volume group with an
existing matched `v1.6.1-m3next.stage1` handoff can instead select
`--m3-profile=j613-25g83`. It uses `/opt/mesa-m3/25g83` and selector
`j613-25g83-hal200`; its probe is
`/opt/mesa-m3/25g83/bin/mesa-m3-probe` and reports
`vk.capability=unavailable`. This is experimental OpenGL only, with no
hardware Vulkan. Scratch shaders are refused; power calibration and
native OpenGL hardware runtime/conformance remain unqualified. Legacy
14/Pro Vulkan stays separate. The installer migrates neither firmware
nor stage 1; firmware and calibration remain specific to each Mac.
