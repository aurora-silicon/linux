# Native OpenGL glyph correction

The kernel and Vulkan packages remain the release 16 binaries.
Native OpenGL uses Mesa commit `99d5b9d16e779ad9a0879a71578da0a6536cf006`,
with `mesa-m3-native-pro` package release 3. The new driver widens each packed
16-bit vertex attribute before exporting 32-bit unsigned coordinates. This fixes
Chrome glyph atlas sampling on T6030/G15S.

The included pixel probe compares float, unsigned-short and unsigned-int vertex
coordinates with exact RGBA output. The Pro ABI marker, firmware selection,
kernel batching and Vulkan compression settings stay unchanged.

Mesa source archives remain private build inputs, not release assets.
The native archive uses prefix `mesa-native-pro/` and checksum
`1bad7a31263973ca239f46bc8a21744842313b824a101aaa04e10aaf206b9429`.
The remaining source inputs retain their recipe checksums.
