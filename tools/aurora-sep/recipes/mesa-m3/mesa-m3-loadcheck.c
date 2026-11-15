/* SPDX-License-Identifier: MIT */
/*
 * mesa-m3-loadcheck: can this system load the /opt/mesa-m3 implementation
 * objects? Run by the session hook at login, with LD_LIBRARY_PATH set to
 * /opt/mesa-m3/lib for this process only. Each object is opened with
 * RTLD_NOW, which resolves every symbol of the object and of the libraries it
 * needs, so a missing library, a missing symbol or a too-old symbol version
 * fails here instead of in the desktop. Nothing touches the GPU: no device is
 * opened and no API is initialised.
 *
 * Prints one line per object ("ok PATH" or "fail PATH: REASON") and exits 0
 * when all load, 1 otherwise, 2 on a usage error.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static const char *const objects[] = {
   "lib/libgallium-26.1.4.so",
   "lib/libgbm.so.1",
   "lib/libEGL_mesa.so.0",
   "lib/libGLX_mesa.so.0",
   "lib/gbm/dri_gbm.so",
   "lib/dri/libdril_dri.so",
#ifndef MESA_M3_GL_ONLY
   "lib/libvulkan_asahi.so",
#endif
};

int
main(int argc, char **argv)
{
   const char *prefix = argc == 2 ? argv[1] : NULL;
   if (!prefix || prefix[0] != '/') {
      fprintf(stderr, "usage: mesa-m3-loadcheck PREFIX\n");
      return 2;
   }
   int bad = 0;
   for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); ++i) {
      char path[4096];
      snprintf(path, sizeof(path), "%s/%s", prefix, objects[i]);
      dlerror();
      void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
      if (h) {
         printf("ok %s\n", path);
      } else {
         const char *e = dlerror();
         char reason[512];
         snprintf(reason, sizeof(reason), "%s", e ? e : "unknown error");
         for (char *c = reason; *c; ++c)
            if (*c == '\n' || *c == '\r')
               *c = ' ';
         printf("fail %s: %s\n", path, reason);
         bad = 1;
      }
   }
   return bad;
}
