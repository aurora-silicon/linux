/* SPDX-License-Identifier: MIT */
/* Pure board-parser controls: no device opening or EGL calls. */
#define main system_check_main
#include "../mesa-m3-system-check.c"
#undef main
#include <assert.h>

#define CHECK(expected, soc, bytes) \
   assert(board_compat_matches(bytes, sizeof(bytes) - 1, soc) == (expected))

int main(void)
{
   CHECK(true, "t8122", "apple,j613\0apple,t8122\0apple,arm-platform\0");
   CHECK(true, "t6030", "apple,j516s\0apple,t6030\0apple,arm-platform\0");
   CHECK(true, "t6030", "apple,arm-platform\0apple,t6030\0apple,j516s\0");
   /* Any single board of the SoC, not a list of boards. */
   CHECK(true, "t8122", "apple,j615\0apple,t8122\0");
   CHECK(true, "t8122", "apple,j504\0apple,t8122\0");
   CHECK(true, "t6030", "apple,j514s\0apple,t6030\0");
   CHECK(false, "t6030", "apple,j516s\0apple,t6031\0");
   CHECK(false, "t8122", "apple,j613\0apple,j615\0apple,t8122\0");
   CHECK(false, "t8122", "apple,j613\0apple,j613\0apple,t8122\0");
   CHECK(false, "t6030", "apple,j516s\0apple,t6030\0apple,t6030\0");
   CHECK(false, "t6030", "apple,j516s\0apple,t6030\0apple,t8122\0");
   CHECK(false, "t8122", "apple,j613\0apple,t8122-extra\0");
   CHECK(false, "t8122", "apple,j613\0apple,t8122");
   CHECK(false, "t8122", "apple,j613\0\0apple,t8122\0");
   CHECK(false, "t6030", "apple,t6030\0");
   CHECK(false, "t6030", "apple,j516s\0");
   CHECK(false, "t8140", "apple,j516s\0apple,t6030\0");
   CHECK(false, "t6030", "");
   /* The admitted SoC's chip and GPU, with any stepping. */
   assert(native_renderer("Apple M3 (G15G C0)", "t8122"));
   assert(native_renderer("Apple M3 (G15G B0)", "t8122"));
   assert(native_renderer("Apple M3 Pro (G15S B1)", "t6030"));
   assert(native_renderer("Apple M3 Pro (G15S C0)", "t6030"));
   /* The other SoC's GPU, another chip, or a software renderer. */
   assert(!native_renderer("Apple M3 Pro (G15S B1)", "t8122"));
   assert(!native_renderer("Apple M3 (G15G C0)", "t6030"));
   assert(!native_renderer("Apple M2 (G14G B1)", "t8122"));
   assert(!native_renderer("Apple M2 (G14G B1)", "t6030"));
   assert(!native_renderer("Apple M3 Max (G15C C0)", "t6030"));
   assert(!native_renderer("Apple M3 Unknown (G15X C0)", "t8122"));
   assert(!native_renderer("Apple M3 (G15G", "t8122"));
   assert(!native_renderer("Apple M3 (G15G C0)", "t6031"));
   assert(!native_renderer(NULL, "t8122"));
   assert(!native_renderer("", "t8122"));
   assert(!native_renderer("llvmpipe (LLVM 21.1.0, 128 bits)", "t8122"));
   assert(!native_renderer("softpipe", "t6030"));
   assert(!native_renderer("zink Vulkan 1.4(Apple M3 (G15G C0) (MESA_ASAHI))", "t8122"));
   assert(!native_renderer("Apple M3 (G15G llvmpipe)", "t8122"));
   assert(!native_renderer("AppleM3", "t8122"));
   puts("18 board parser and 19 renderer controls passed");
   return 0;
}
