# J613: boot the native GPU profile from your own 26.6.2 volume

This is the experimental **13-inch M3 Air (J613)** route. It uses a full macOS **26.6.2 (25G83)** volume group, its paired Recovery, and your Mac's EFI partition. Keep your working Linux and daily macOS boot entries. The software installer does not install or flash stage1.

The author booted this factory stage1 from RAM through the Aurora15 `boot.bin`, U-Boot and Limine to the desktop. Installing this exact factory image and booting it from power-off still needs a hardware check. J615 is a separate experimental candidate; these instructions do not qualify it.

1. Keep the previous working boot entry and a known-working proxy stage1 image for recovery. Confirm `/boot/efi` is **this Mac's** mounted ESP and the Aurora `m1n1/boot.bin` exists there. Download the matched installer from [Aurora 2026.10.09.5](https://github.com/iconidentify/aurora-linux/releases/tag/aurora-2026.10.09.5); keep it for step 5. This first boot uses the existing Aurora15 `boot.bin`. In Limine select the retained **Aurora previous (GPU off)** entry; otherwise use your existing GPU-off entry whose command line includes `asahi.t8122_start=0 mesa_m3=off`. If Linux is already booted through the named stage1 on exact 26.6.2, its installer can select `--m3-profile=j613-25g83` now; otherwise select that profile only after reaching Linux through this boot path. The profile command refuses a 14.x or mismatched stage1 boot. Have an external keyboard available: the internal keyboard is not working in U-Boot/Limine on the author's J613.

2. Download the factory image and filler from [the author's stage1 release](https://github.com/aurora-silicon/m1n1/releases/tag/j613-stage1-20261009). Check the factory SHA256:

   ```text
   2cc65ed4dc01c3a7ae26846b1e330f463d5f9c27d5f74ecc80a5c7ff8dc6cc38  m1n1-j613-stage1-v1.6.1-m3next.stage1.bin
   ```

   Fill it **on this Mac**, then inspect the result:

   ```sh
   sudo python3 fill_stage1_config.py --esp-mount /boot/efi \
     m1n1-j613-stage1-v1.6.1-m3next.stage1.bin /boot/efi/j613-stage1-filled.bin
   python3 fill_stage1_config.py --check /boot/efi/j613-stage1-filled.bin
   ```

   The output must be new. Check that its ESP PARTUUID is yours, its target is `m1n1/boot.bin`, and its USB window is `0`. Save the printed size, MD5 and `cksum`; filling changes the factory hash. Never use another Mac's filled image. Keep your saved rollback image on the same ESP.

3. Make the **full 26.6.2 volume** the default Startup Disk before shutting down and holding power to choose **Options**. A one-time volume selection does not select its paired Recovery. In Recovery, verify `sw_vers` reports 26.6.2, identify the correct volume group with `diskutil apfs listVolumeGroups` and `diskutil info`, and mount its ESP. Verify the filled image's size, `md5` and `cksum` against step 2.

4. Configure only that volume's custom boot object; replace both paths with your actual mounted paths:

   ```sh
   kmutil configure-boot -c "/Volumes/<your ESP>/j613-stage1-filled.bin" \
     --raw --entry-point 2048 --lowest-virtual-address 0 \
     -v "/Volumes/<your full 26.6.2 volume>"
   ```

   Restore your working default Startup Disk before testing. Shut down fully, hold power, and explicitly select the 26.6.2 volume for the first installed cold boot. If stage1 cannot load the ESP file, it shows a panel message and enters the proxy; hold power to select your working system. Rollback uses the same paired-Recovery command with your saved, known-working proxy image.

5. Once Linux has booted through the named stage1 on exact 26.6.2, run the matched software installer:

   ```sh
   curl -fsSL https://github.com/iconidentify/aurora-linux/releases/download/aurora-2026.10.09.5/install-aurora-sep.sh | bash -s -- --m3-profile=j613-25g83
   ```

   It installs the matched kernel, headers, bootloader and Mesa, and records persistent experimental GPU intent. Reboot into this volume and log into your desktop, then run:

   ```sh
   aurora-m3-gpu-check
   ```

   Success means the actual native OpenGL shader readback passed in your current session. The native25 profile has **no Vulkan support**. Record the checker output, running kernel and m1n1 versions, and whether this was an installed power-off boot; an offscreen test or a RAM-chainloaded boot alone does not verify that path.
