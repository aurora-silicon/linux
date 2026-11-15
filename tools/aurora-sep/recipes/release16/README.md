# Release 16 package recipes

The kernel source is Omacom linux-aurora commit `d49021440d8ca02bbc8ecc17c19ce76c048b6236`.
Pro OpenGL uses iconidentify/mesa-omacom commit `2dc61e4b462639d29ca846a3ad2dfd1f28629c4e`.
Pro Vulkan uses iconidentify/mesa-omacom commit `b0f56952b7f06955b143fbfa90c07de4c5864964`.
Both Mesa sources are based on Mesa 26.1.4. LLVM 22 is a build dependency.

The local source archives in each recipe are generated with `git archive` from
those exact commits. Kernel archives use tar.gz and prefix `aurora-linux-<commit>/`.
Mesa archives use tar, the recipe's `_src` prefix, and `zstd -T4 -3`.
The five SPIR-V/libclc build inputs retain the versions and checksums recorded in
the recipes. Mesa source archives are build inputs, not release assets.

The common Mesa package retains legacy profiles and the separate Air 26.6.2
native-OpenGL profile. The Pro packages add `/opt/mesa-m3/native-pro` and
`/opt/mesa-m3/pro-vulkan`; they do not replace the Air or Neo package payloads.
The Pro runtime requires the paired kernel marker and the legacy T6030/G15S
identity. Kernel batching controls remain disabled outside that profile.

`--no-m3-mesa`, `mesa_m3=off` and the existing Mesa disable files are supported.
`/etc/mesa-m3/pro-disable` leaves the common Mesa selection in use.
