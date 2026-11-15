/* Copyright 2026 Chris Kearney
 * SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *thread(void *p) { return p; }
int main(int argc, char **argv)
{
   if (argc != 3) return 2;
   pthread_t t;
   if (pthread_create(&t, NULL, thread, NULL) || pthread_join(t, NULL)) return 1;
   unsigned loaded = 0;
   for (unsigned i = 0; i < 512; ++i) {
      char path[4096];
      snprintf(path, sizeof(path), "%s/pad-%u.so", argv[1], i);
      void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
      if (!h) {
         const char *e = dlerror();
         if (!e || !strstr(e, "static TLS")) {
            fprintf(stderr, "pad: %s\n", e ? e : "unknown"); return 1;
         }
         break;
      }
      ++loaded;
   }
   if (!loaded || loaded == 512) { fprintf(stderr, "TLS exhaustion not established\n"); return 1; }
   /* All implementation objects remain mapped together. */
   const char *objects[] = {"lib/libgallium-26.1.4.so", "lib/libEGL_mesa.so.0", "lib/libGLX_mesa.so.0", "lib/libgbm.so.1", "lib/gbm/dri_gbm.so", "lib/dri/libdril_dri.so"};
   for (unsigned i = 0; i < sizeof(objects)/sizeof(objects[0]); ++i) {
      char path[4096]; snprintf(path, sizeof(path), "%s/%s", argv[2], objects[i]);
      if (!dlopen(path, RTLD_NOW | RTLD_LOCAL)) { fprintf(stderr, "late-load: %s: %s\n", objects[i], dlerror()); return 1; }
   }
   printf("late-load=pass exhausted-static-TLS-after=%u profile=%s\n", loaded, argv[2]);
   return 0;
}
