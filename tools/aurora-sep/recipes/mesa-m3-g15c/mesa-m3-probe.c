/* SPDX-License-Identifier: MIT */
/*
 * mesa-m3-probe [--out FILE]: report, from inside the rendering process, which
 * Mesa renders for it and on what. It sets no environment variable. Start it
 * the way an ordinary app is started (for example from the compositor with
 * `uwsm-app -- /opt/mesa-m3/bin/mesa-m3-probe`, or as a systemd user service)
 * so that it shows what such an app gets.
 *
 * In one process, with the GL and Vulkan contexts alive until the end:
 *   1. prints its pid, start time (clock ticks after boot), boot id, cgroup and
 *      executable, and the graphics and session variables it received (only
 *      the names listed in env_prefixes/env_names below, never others);
 *   2. GL: connects to the session's compositor (WAYLAND_DISPLAY), makes a
 *      Wayland EGL display, a 64x64 EGL window surface on a wl_surface (never
 *      given a role, so nothing appears) and a GLES 3 context on it; clears to
 *      blue, draws one red triangle over the lower-left half, reads two pixels
 *      back (which waits for the GPU), checks them, and hands the frame to the
 *      compositor (eglSwapBuffers). Prints gl.platform=wayland, GL_VENDOR,
 *      GL_RENDERER and GL_VERSION. Without a usable Wayland connection it says
 *      gl.platform=surfaceless and why (gl.wayland=fail ...), renders into an
 *      offscreen framebuffer for diagnosis only, and the result is fail: only
 *      a Wayland client is an ordinary app;
 *   3. Vulkan: picks the Apple device, prints its name, type and driver, and
 *      runs one compute job whose 65536 result words are checked on the CPU;
 *   4. prints its own /proc/self/maps entries for GL, EGL, GBM, DRI, DRM and
 *      Vulkan objects: class, path, address range, the device and inode the
 *      maps line gives, whether the mapping is deleted, the device and inode of
 *      the mapped object (stat through /proc/self/map_files) and of the file
 *      now at that path, and whether the two are the same file;
 *   5. prints result=pass or result=fail with the reasons.
 *
 * Classes: "implementation" is a Mesa object that must come from /opt/mesa-m3
 * (libEGL_mesa, libGLX_mesa, libgallium, libgbm, the gbm backend, the dri
 * drivers, libvulkan_asahi). "system" is a dispatch, loader or support library
 * that the system provides (the GLVND libEGL, libGL, libGLX, libGLdispatch,
 * libOpenGL and libGLESv2, the Vulkan loader, libdrm). Anything else that
 * matches is printed as "other".
 *
 * Output: key=value lines on stdout. With --out FILE the same lines are also
 * written to FILE.tmp, which is renamed to FILE when the report is complete
 * (the last line is exit=0 or exit=1). Exit status 0 on pass, 1 on fail,
 * 2 on a usage error. A 90 s alarm ends the process if the GPU never answers.
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <fcntl.h>
#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "compute-check.spv.h"

#define COUNT 65536u
#define ITERATIONS 256u
#ifdef MESA_M3_GL_ONLY
#define PREFIX "/opt/mesa-m3/25g83/"
#else
#define PREFIX "/opt/mesa-m3/"
#endif

extern char **environ;

/* Graphics variables: every name with one of these prefixes, plus env_names. */
static const char *const env_prefixes[] = {
   "LD_LIBRARY_PATH", "LD_PRELOAD", "LIBGL_", "MESA_", "__EGL_", "__GLX_", "EGL_", "GBM_",
   "VK_", "DRI_", "GALLIUM_", "ZINK_", "ASAHI_", "AGX_", "HK_", "NIR_", "QSG_RHI_BACKEND",
};
static const char *const env_names[] = {
   "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XDG_SESSION_ID", "XDG_SESSION_TYPE",
   "XDG_CURRENT_DESKTOP",
};

static FILE *out_file;
static char reasons[2048];

static void
out(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   if (out_file) {
      va_start(ap, fmt);
      vfprintf(out_file, fmt, ap);
      va_end(ap);
   }
}

static void
fail_reason(const char *why)
{
   if (strstr(reasons, why))
      return;
   size_t n = strlen(reasons);
   snprintf(reasons + n, sizeof(reasons) - n, "%s%s", n ? "; " : "", why);
}

static bool
software_name(const char *s)
{
   return !s || strstr(s, "llvmpipe") || strstr(s, "softpipe") || strstr(s, "swrast") ||
          strstr(s, "lavapipe") || strstr(s, "Software");
}

/* ---------------------------------------------------------------- GL ---- */

static EGLDisplay gl_dpy = EGL_NO_DISPLAY;
static EGLContext gl_ctx = EGL_NO_CONTEXT;
static EGLSurface gl_surf = EGL_NO_SURFACE;
static struct wl_display *wl;
static struct wl_compositor *wl_comp;
static struct wl_surface *wl_surf;
static struct wl_egl_window *wl_win;

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
   (void)data;
   if (strcmp(iface, wl_compositor_interface.name) == 0 && !wl_comp)
      wl_comp = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
}

static void
registry_remove(void *data, struct wl_registry *reg, uint32_t name)
{
   (void)data, (void)reg, (void)name;
}

static const struct wl_registry_listener registry_listener = {registry_global, registry_remove};

static GLuint
compile(GLenum stage, const char *src)
{
   GLuint s = glCreateShader(stage);
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   GLint ok = 0;
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   return ok ? s : 0;
}

/* Connect to the session's compositor and make a Wayland EGL display. Returns
 * NULL on success, else why not. */
static const char *
wayland_display(void)
{
   const char *name = getenv("WAYLAND_DISPLAY");
   if (!name || !name[0])
      return "WAYLAND_DISPLAY is not set";
   if (!(wl = wl_display_connect(NULL)))
      return "cannot connect to the compositor";
   struct wl_registry *reg = wl_display_get_registry(wl);
   wl_registry_add_listener(reg, &registry_listener, NULL);
   if (wl_display_roundtrip(wl) < 0)
      return "the compositor connection failed";
   if (!wl_comp)
      return "the compositor offers no wl_compositor";
   gl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wl, NULL);
   EGLint maj, min;
   if (gl_dpy == EGL_NO_DISPLAY || !eglInitialize(gl_dpy, &maj, &min)) {
      gl_dpy = EGL_NO_DISPLAY;
      return "EGL cannot initialise the Wayland display";
   }
   return NULL;
}

static bool
gl_probe(void)
{
   const char *why = wayland_display();
   bool wayland = why == NULL;
   EGLint maj, min;
   if (wayland) {
      out("gl.platform=wayland\n");
   } else {
      /* Diagnostic only: a surfaceless context is not what an ordinary app gets. */
      out("gl.platform=surfaceless\n");
      out("gl.wayland=fail %s\n", why);
      fail_reason("gl: not a Wayland client (surfaceless fallback, diagnostic only)");
      gl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
      if (gl_dpy == EGL_NO_DISPLAY || !eglInitialize(gl_dpy, &maj, &min)) {
         fail_reason("gl: no EGL display");
         return false;
      }
   }
   out("egl.vendor=%s\n", eglQueryString(gl_dpy, EGL_VENDOR));
   out("egl.version=%s\n", eglQueryString(gl_dpy, EGL_VERSION));
   eglBindAPI(EGL_OPENGL_ES_API);
   const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};

   if (wayland) {
      /* A real Wayland surface: an EGL window surface on a wl_surface, whose
       * buffers come through the compositor. It is never given a role, so
       * nothing appears on screen. */
      const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                  EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_RENDERABLE_TYPE,
                                  EGL_OPENGL_ES3_BIT, EGL_NONE};
      EGLConfig cfg;
      EGLint n = 0;
      if (!eglChooseConfig(gl_dpy, cfg_attrs, &cfg, 1, &n) || n < 1) {
         fail_reason("gl: no RGBA8 GLES 3 window config");
         return false;
      }
      wl_surf = wl_compositor_create_surface(wl_comp);
      wl_win = wl_surf ? wl_egl_window_create(wl_surf, 64, 64) : NULL;
      gl_surf = wl_win ? eglCreatePlatformWindowSurface(gl_dpy, cfg, wl_win, NULL) : EGL_NO_SURFACE;
      if (gl_surf == EGL_NO_SURFACE) {
         fail_reason("gl: no Wayland window surface");
         return false;
      }
      gl_ctx = eglCreateContext(gl_dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
      if (gl_ctx == EGL_NO_CONTEXT || !eglMakeCurrent(gl_dpy, gl_surf, gl_surf, gl_ctx)) {
         fail_reason("gl: no GLES 3 context on the Wayland surface");
         return false;
      }
      out("gl.surface=wayland-window 64x64\n");
   } else {
      gl_ctx = eglCreateContext(gl_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
      if (gl_ctx == EGL_NO_CONTEXT || !eglMakeCurrent(gl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, gl_ctx)) {
         fail_reason("gl: no GLES 3 context");
         return false;
      }
      GLuint fb, rb;
      glGenRenderbuffers(1, &rb);
      glBindRenderbuffer(GL_RENDERBUFFER, rb);
      glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 64, 64);
      glGenFramebuffers(1, &fb);
      glBindFramebuffer(GL_FRAMEBUFFER, fb);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
         fail_reason("gl: framebuffer incomplete");
         return false;
      }
      out("gl.surface=offscreen 64x64\n");
   }
   const char *renderer = (const char *)glGetString(GL_RENDERER);
   out("gl.vendor=%s\n", (const char *)glGetString(GL_VENDOR));
   out("gl.renderer=%s\n", renderer ? renderer : "");
   out("gl.version=%s\n", (const char *)glGetString(GL_VERSION));

   glViewport(0, 0, 64, 64);
   glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);

   /* One red triangle over the lower-left half (below the x = -y diagonal). */
   GLuint vs = compile(GL_VERTEX_SHADER,
      "#version 300 es\n"
      "void main(){vec2 p=vec2(gl_VertexID==1?2.0:0.0,gl_VertexID==2?2.0:0.0);"
      "gl_Position=vec4(p.x*2.0-3.0,p.y*2.0-1.0,0.0,1.0);}\n");
   GLuint fs = compile(GL_FRAGMENT_SHADER,
      "#version 300 es\nprecision mediump float;out vec4 o;"
      "void main(){o=vec4(1.0,0.0,0.0,1.0);}\n");
   if (!vs || !fs) {
      fail_reason("gl: shader compile failed");
      return false;
   }
   GLuint prog = glCreateProgram();
   glAttachShader(prog, vs);
   glAttachShader(prog, fs);
   glLinkProgram(prog);
   GLint linked = 0;
   glGetProgramiv(prog, GL_LINK_STATUS, &linked);
   if (!linked) {
      fail_reason("gl: program link failed");
      return false;
   }
   glUseProgram(prog);
   GLuint vao;
   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);
   glDrawArrays(GL_TRIANGLES, 0, 3);

   uint8_t left[4] = {0}, right[4] = {0};
   glReadPixels(4, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left);
   glReadPixels(60, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, right);
   GLenum err = glGetError();
   bool ok = err == GL_NO_ERROR && left[0] == 255 && left[1] == 0 && left[2] == 0 &&
             right[0] == 0 && right[1] == 0 && right[2] == 255;
   out("gl.render=%s left=%u,%u,%u,%u right=%u,%u,%u,%u glerror=0x%x\n", ok ? "pass" : "fail",
          left[0], left[1], left[2], left[3], right[0], right[1], right[2], right[3], err);
   if (!ok)
      fail_reason("gl: the draw and clear did not produce the expected pixels");
   if (wayland) {
      /* Hand the frame to the compositor (no frame-callback wait). */
      eglSwapInterval(gl_dpy, 0);
      bool swapped = eglSwapBuffers(gl_dpy, gl_surf) == EGL_TRUE && wl_display_roundtrip(wl) >= 0;
      out("gl.swap=%s\n", swapped ? "pass" : "fail");
      if (!swapped) {
         fail_reason("gl: the frame could not be handed to the compositor");
         ok = false;
      }
   }
#ifdef MESA_M3_GL_ONLY
   /* Native selection must not pass through Zink or a software renderer. */
   if (software_name(renderer) || !renderer || strncmp(renderer, "Apple M3", 8) || strstr(renderer, "zink")) {
      fail_reason("gl: the renderer is not native Apple M3");
      ok = false;
   }
   uint8_t pixels[64 * 64 * 4];
   GLuint indices;
   glGenBuffers(1, &indices);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices);
   const unsigned sizes[] = {1, 1, 2, 2, 2, 4, 4};
   const unsigned offsets[] = {0, 1, 0, 2, 6, 0, 4};
   for (unsigned test = 0; test < 7; ++test) {
      uint8_t data[32] = {0};
      for (unsigned i = 0; i < 3; ++i)
         memcpy(data + offsets[test] + i * sizes[test], &i, sizes[test]);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(data), data, GL_STATIC_DRAW);
      glClear(GL_COLOR_BUFFER_BIT);
      GLenum type = sizes[test] == 1 ? GL_UNSIGNED_BYTE : sizes[test] == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
      glDrawElements(GL_TRIANGLES, 3, type, (void *)(uintptr_t)offsets[test]);
      glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
      unsigned mismatches = 0;
      for (unsigned y = 0; y < 64; ++y) {
         for (unsigned x = 0; x < 64; ++x) {
            if (x + y == 63) continue;
            const uint8_t *v = pixels + (y * 64 + x) * 4;
            bool red = x + y < 63;
            if (v[0] != (red ? 255 : 0) || v[1] || v[2] != (red ? 0 : 255) || v[3] != 255)
               ++mismatches;
         }
      }
      bool pass = glGetError() == GL_NO_ERROR && !mismatches;
      out("gl.index size=%u offset=%u mismatches=%u result=%s\n", sizes[test], offsets[test], mismatches, pass ? "pass" : "fail");
      ok &= pass;
   }
   GLint original_fb;
   glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original_fb);
   for (unsigned format = 0; format < 2; ++format) {
      for (unsigned sample = 0; sample < 2; ++sample) {
         GLuint fbo, color, depth, resolve_fbo, resolve;
         glGenFramebuffers(1, &fbo);
         glBindFramebuffer(GL_FRAMEBUFFER, fbo);
         glGenRenderbuffers(1, &color);
         glBindRenderbuffer(GL_RENDERBUFFER, color);
         glRenderbufferStorageMultisample(GL_RENDERBUFFER, sample ? 4 : 1, format ? GL_RGB10_A2 : GL_RGBA8, 64, 64);
         glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
         glGenRenderbuffers(1, &depth);
         glBindRenderbuffer(GL_RENDERBUFFER, depth);
         glRenderbufferStorageMultisample(GL_RENDERBUFFER, sample ? 4 : 1, GL_DEPTH_COMPONENT24, 64, 64);
         glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
         bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
         glClearDepthf(1.0f);
         glEnable(GL_DEPTH_TEST);
         glDepthFunc(GL_LESS);
         glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
         glDrawArrays(GL_TRIANGLES, 0, 3);
         glDepthFunc(GL_NEVER);
         glDrawArrays(GL_TRIANGLES, 0, 3);
         glDisable(GL_DEPTH_TEST);
         glGenFramebuffers(1, &resolve_fbo);
         glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo);
         glGenRenderbuffers(1, &resolve);
         glBindRenderbuffer(GL_RENDERBUFFER, resolve);
         glRenderbufferStorage(GL_RENDERBUFFER, format ? GL_RGB10_A2 : GL_RGBA8, 64, 64);
         glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, resolve);
         glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
         glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo);
         glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
         unsigned mismatches = 0;
         for (unsigned y = 0; y < 64; ++y) {
            for (unsigned x = 0; x < 64; ++x) {
               if (x+y >= 61 && x+y <= 65) continue;
               const uint8_t *v = pixels + (y * 64 + x) * 4;
               bool red = x+y < 63;
               if (v[0] != (red ? 255 : 0) || v[1] || v[2] != (red ? 0 : 255) || v[3] != 255) ++mismatches;
            }
         }
         bool pass = complete && glGetError() == GL_NO_ERROR && !mismatches;
         out("gl.depth-resolve format=%s samples=%u mismatches=%u result=%s\n", format ? "RGB10_A2" : "RGBA8", sample ? 4 : 1, mismatches, pass ? "pass" : "fail");
         ok &= pass;
         glBindFramebuffer(GL_FRAMEBUFFER, original_fb);
         glDeleteFramebuffers(1, &fbo); glDeleteFramebuffers(1, &resolve_fbo);
         glDeleteRenderbuffers(1, &color); glDeleteRenderbuffers(1, &depth); glDeleteRenderbuffers(1, &resolve);
      }
   }
   for (unsigned valid = 0; valid < 2; ++valid) {
      GLuint fbo, targets[2];
      glGenFramebuffers(1, &fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glGenTextures(2, targets);
      uint8_t seed[64 * 64 * 4];
      for (unsigned i = 0; i < sizeof(seed); i += 4) {
         seed[i] = 0x31; seed[i+1] = 0x72; seed[i+2] = 0xb4; seed[i+3] = 0xff;
      }
      for (unsigned rt = 0; rt < 2; ++rt) {
         glBindTexture(GL_TEXTURE_2D, targets[rt]);
         glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA,
                      GL_UNSIGNED_BYTE, valid && rt == 1 ? seed : NULL);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + rt,
                                GL_TEXTURE_2D, targets[rt], 0);
      }
      const GLenum draw_buffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
      glDrawBuffers(2, draw_buffers);
      bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
      const GLfloat red[] = {1.0f, 0.0f, 0.0f, 1.0f};
      glClearBufferfv(GL_COLOR, 0, red);
      glFinish();
      unsigned mismatches = 0;
      for (unsigned rt = 0; rt < (valid ? 2u : 1u); ++rt) {
         glReadBuffer(GL_COLOR_ATTACHMENT0 + rt);
         glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
         const uint8_t expected[4] = {rt ? 0x31 : 0xff, rt ? 0x72 : 0,
                                      rt ? 0xb4 : 0, 0xff};
         for (unsigned i = 0; i < sizeof(pixels); i += 4)
            if (memcmp(pixels + i, expected, 4)) ++mismatches;
      }
      bool pass = complete && glGetError() == GL_NO_ERROR && !mismatches;
      out("gl.mrt-partial-clear case=%s mismatches=%u result=%s\n",
          valid ? "fresh-valid" : "fresh-fresh", mismatches, pass ? "pass" : "fail");
      ok &= pass;
      glBindFramebuffer(GL_FRAMEBUFFER, original_fb);
      glDeleteTextures(2, targets);
      glDeleteFramebuffers(1, &fbo);
   }
   if (!ok) fail_reason("gl: native known-answer controls failed");
   return ok;
#else
   if (software_name(renderer) || !strstr(renderer, "Apple")) {
      fail_reason("gl: the renderer is not the Apple GPU");
      ok = false;
   }
   return ok && wayland;
#endif
}

#ifndef MESA_M3_GL_ONLY
/* ------------------------------------------------------------ Vulkan ---- */

static VkInstance vk_inst;
static VkDevice vk_dev;

static uint32_t
reference(uint32_t i, uint32_t seed)
{
   uint32_t x = seed, y = i * 2654435761u;
   for (uint32_t k = 0; k < ITERATIONS; ++k) {
      x = x * 1664525u + 1013904223u;
      y ^= x >> 13;
      y = (y << 5) | (y >> 27);
      x += y + k;
   }
   return x ^ y;
}

#define VKOK(x, what)                                                          \
   do {                                                                        \
      VkResult r_ = (x);                                                       \
      if (r_ != VK_SUCCESS) {                                                  \
         out("vk.error=%s:%d\n", what, r_);                                 \
         fail_reason("vulkan: " what " failed");                               \
         return false;                                                         \
      }                                                                        \
   } while (0)

static bool
vk_probe(void)
{
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "mesa-m3-probe",
      .apiVersion = VK_API_VERSION_1_2,
   };
   const VkInstanceCreateInfo ii = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VKOK(vkCreateInstance(&ii, NULL, &vk_inst), "vkCreateInstance");
   uint32_t n = 8;
   VkPhysicalDevice pdevs[8];
   VkResult er = vkEnumeratePhysicalDevices(vk_inst, &n, pdevs);
   if (er != VK_SUCCESS && er != VK_INCOMPLETE) {
      out("vk.error=vkEnumeratePhysicalDevices:%d\n", er);
      fail_reason("vulkan: no physical devices");
      return false;
   }
   out("vk.devices=%u\n", n);
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   VkPhysicalDeviceDriverProperties drv = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
   };
   VkPhysicalDeviceProperties2 props = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &drv,
   };
   for (uint32_t i = 0; i < n; ++i) {
      VkPhysicalDeviceDriverProperties d = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
      VkPhysicalDeviceProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &d};
      vkGetPhysicalDeviceProperties2(pdevs[i], &p);
      out("vk.device%u=%s type=%d driver=%s\n", i, p.properties.deviceName,
             p.properties.deviceType, d.driverName);
      if (!pdev && strstr(p.properties.deviceName, "Apple")) {
         pdev = pdevs[i];
         drv = d;
         props = p;
         props.pNext = NULL;
      }
   }
   if (!pdev) {
      fail_reason("vulkan: no Apple device");
      return false;
   }
   out("vk.device=%s\n", props.properties.deviceName);
   out("vk.device_type=%s\n",
          props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated-gpu" :
          props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "cpu" : "other");
   out("vk.driver=%s %s\n", drv.driverName, drv.driverInfo);
   out("vk.device_id=0x%x\n", props.properties.deviceID);
   if (props.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
      fail_reason("vulkan: the Apple device is not an integrated GPU");
      return false;
   }

   uint32_t nq = 16;
   VkQueueFamilyProperties qp[16];
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, qp);
   uint32_t family = UINT32_MAX;
   for (uint32_t i = 0; i < nq && family == UINT32_MAX; ++i)
      if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
         family = i;
   if (family == UINT32_MAX) {
      fail_reason("vulkan: no compute queue");
      return false;
   }
   const float prio = 1.0f;
   const VkDeviceQueueCreateInfo qi = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &prio,
   };
   const VkDeviceCreateInfo di = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
   };
   VKOK(vkCreateDevice(pdev, &di, NULL, &vk_dev), "vkCreateDevice");
   VkQueue queue;
   vkGetDeviceQueue(vk_dev, family, 0, &queue);

   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
   VkBuffer bufs[2];
   VkDeviceMemory mems[2];
   uint32_t *maps[2];
   for (int b = 0; b < 2; ++b) {
      const VkBufferCreateInfo bi = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = COUNT * sizeof(uint32_t),
         .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      };
      VKOK(vkCreateBuffer(vk_dev, &bi, NULL, &bufs[b]), "vkCreateBuffer");
      VkMemoryRequirements req;
      vkGetBufferMemoryRequirements(vk_dev, bufs[b], &req);
      uint32_t type = UINT32_MAX;
      const VkMemoryPropertyFlags want =
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; ++t)
         if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want)
            type = t;
      if (type == UINT32_MAX) {
         fail_reason("vulkan: no host-visible memory");
         return false;
      }
      const VkMemoryAllocateInfo ai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = req.size, .memoryTypeIndex = type,
      };
      VKOK(vkAllocateMemory(vk_dev, &ai, NULL, &mems[b]), "vkAllocateMemory");
      VKOK(vkBindBufferMemory(vk_dev, bufs[b], mems[b], 0), "vkBindBufferMemory");
      VKOK(vkMapMemory(vk_dev, mems[b], 0, VK_WHOLE_SIZE, 0, (void **)&maps[b]), "vkMapMemory");
   }
   for (uint32_t i = 0; i < COUNT; ++i) {
      maps[0][i] = i * 0x9e3779b9u + 0x7f4a7c15u;
      maps[1][i] = 0;
   }

   const VkDescriptorSetLayoutBinding binds[] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
   };
   const VkDescriptorSetLayoutCreateInfo dli = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = binds,
   };
   VkDescriptorSetLayout dsl;
   VKOK(vkCreateDescriptorSetLayout(vk_dev, &dli, NULL, &dsl), "vkCreateDescriptorSetLayout");
   const VkPipelineLayoutCreateInfo pli = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
   };
   VkPipelineLayout layout;
   VKOK(vkCreatePipelineLayout(vk_dev, &pli, NULL, &layout), "vkCreatePipelineLayout");
   const VkShaderModuleCreateInfo smi = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(compute_check_spv), .pCode = compute_check_spv,
   };
   VkShaderModule module;
   VKOK(vkCreateShaderModule(vk_dev, &smi, NULL, &module), "vkCreateShaderModule");
   const VkComputePipelineCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
      .layout = layout,
   };
   VkPipeline pipe;
   VKOK(vkCreateComputePipelines(vk_dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe), "vkCreateComputePipelines");
   const VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
   const VkDescriptorPoolCreateInfo dpi = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps,
   };
   VkDescriptorPool pool;
   VKOK(vkCreateDescriptorPool(vk_dev, &dpi, NULL, &pool), "vkCreateDescriptorPool");
   const VkDescriptorSetAllocateInfo dsi = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
      .descriptorSetCount = 1, .pSetLayouts = &dsl,
   };
   VkDescriptorSet set;
   VKOK(vkAllocateDescriptorSets(vk_dev, &dsi, &set), "vkAllocateDescriptorSets");
   const VkDescriptorBufferInfo dbi[] = {{bufs[0], 0, VK_WHOLE_SIZE}, {bufs[1], 0, VK_WHOLE_SIZE}};
   const VkWriteDescriptorSet w[] = {
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi[0], NULL},
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi[1], NULL},
   };
   vkUpdateDescriptorSets(vk_dev, 2, w, 0, NULL);
   const VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family,
   };
   VkCommandPool cpool;
   VKOK(vkCreateCommandPool(vk_dev, &cpci, NULL, &cpool), "vkCreateCommandPool");
   const VkCommandBufferAllocateInfo cbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
   };
   VkCommandBuffer cmd;
   VKOK(vkAllocateCommandBuffers(vk_dev, &cbi, &cmd), "vkAllocateCommandBuffers");
   const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   VKOK(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
   vkCmdDispatch(cmd, COUNT / 64, 1, 1);
   const VkMemoryBarrier mb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                        1, &mb, 0, NULL, 0, NULL);
   VKOK(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
   const VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   VKOK(vkCreateFence(vk_dev, &fci, NULL, &fence), "vkCreateFence");
   const VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd,
   };
   VKOK(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit");
   VKOK(vkWaitForFences(vk_dev, 1, &fence, VK_TRUE, 10ull * 1000000000ull), "vkWaitForFences");
   unsigned wrong = 0;
   for (uint32_t i = 0; i < COUNT; ++i)
      wrong += maps[1][i] != reference(i, maps[0][i]);
   out("vk.job=%s words=%u wrong=%u\n", wrong ? "fail" : "pass", COUNT, wrong);
   if (wrong)
      fail_reason("vulkan: the compute job returned wrong words");
   if (software_name(props.properties.deviceName)) {
      fail_reason("vulkan: the device is a software device");
      return false;
   }
   /* The device and its objects stay alive until the maps are reported. */
   return wrong == 0;
}

/* -------------------------------------------------------------- maps ---- */

/* The device number the maps lines use for PATH: that of the mount holding it
 * (on btrfs, stat() gives a per-subvolume device instead). */
#endif

static bool
mount_dev(const char *path, unsigned *mj, unsigned *mn)
{
   FILE *f = fopen("/proc/self/mountinfo", "r");
   if (!f)
      return false;
   char line[4096];
   size_t best = 0;
   bool found = false;
   while (fgets(line, sizeof(line), f)) {
      unsigned a, b;
      char point[2048];
      if (sscanf(line, "%*u %*u %u:%u %*s %2047s", &a, &b, point) != 3)
         continue;
      size_t l = strlen(point);
      bool under = strcmp(point, "/") == 0 ||
                   (strncmp(path, point, l) == 0 && (path[l] == '/' || path[l] == '\0'));
      if (under && (l >= best || !found)) {
         best = l;
         *mj = a;
         *mn = b;
         found = true;
      }
   }
   fclose(f);
   return found;
}

static bool
report_maps(void)
{
   regex_t impl, sys, any;
   regcomp(&impl, "/(libgallium-[^/]*\\.so|libEGL_mesa\\.so[^/]*|libGLX_mesa\\.so[^/]*|libgbm\\.so[^/]*|"
                  "gbm/dri_gbm\\.so|dri/[^/]*_dri\\.so|libvulkan_asahi\\.so)$", REG_EXTENDED | REG_NOSUB);
   regcomp(&sys, "/(libEGL\\.so|libGL\\.so|libGLX\\.so|libGLdispatch\\.so|libOpenGL\\.so|"
                 "libGLESv2\\.so|libGLESv1_CM\\.so|libvulkan\\.so|libdrm\\.so)[^/]*$",
           REG_EXTENDED | REG_NOSUB);
   regcomp(&any, "(EGL|GLX|libGL|GLES|OpenGL|gbm|_dri\\.so|gallium|vulkan|libdrm|LLVM|SPIRV|"
                 "zink|asahi|mesa)", REG_EXTENDED | REG_NOSUB);
   FILE *f = fopen("/proc/self/maps", "r");
   if (!f) {
      fail_reason("maps: /proc/self/maps unreadable");
      return false;
   }
   char line[4096], last[4096] = "";
   bool ok = true;
   unsigned impls = 0;
   while (fgets(line, sizeof(line), f)) {
      char range[64], perms[8], dev[16], path[4000] = "";
      unsigned long long off, ino;
      int n = sscanf(line, "%63s %7s %llx %15s %llu %3999[^\n]", range, perms, &off, dev, &ino, path);
      if (n < 6)
         continue;
      char *p = path;
      while (*p == ' ')
         ++p;
      if (p[0] != '/')
         continue;
      bool deleted = strstr(p, " (deleted)") != NULL;
      if (deleted)
         *strstr(p, " (deleted)") = '\0';
      if (regexec(&any, p, 0, NULL, 0) != 0 || strcmp(p, last) == 0)
         continue;
      snprintf(last, sizeof(last), "%s", p);
      const char *cls = regexec(&impl, p, 0, NULL, 0) == 0 ? "implementation" :
                        regexec(&sys, p, 0, NULL, 0) == 0 ? "system" : "other";

      /* The mapped object (maps device and inode; the map_files link can be
       * followed only with CAP_SYS_ADMIN) against the file now at its path. */
      unsigned mj = 0, mn = 0;
      sscanf(dev, "%x:%x", &mj, &mn);
      char mf[128];
      snprintf(mf, sizeof(mf), "/proc/self/map_files/%s", range);
      struct stat ms, ds;
      bool have_m = stat(mf, &ms) == 0, have_d = stat(p, &ds) == 0;
      unsigned fj = 0, fn = 0;
      bool have_f = have_d && mount_dev(p, &fj, &fn);
      char mdev[32] = "unreadable", mino[32] = "unreadable", ddev[32] = "missing",
           dino[32] = "missing", fdev[32] = "unknown";
      if (have_m) {
         snprintf(mdev, sizeof(mdev), "%u:%u", major(ms.st_dev), minor(ms.st_dev));
         snprintf(mino, sizeof(mino), "%llu", (unsigned long long)ms.st_ino);
      }
      if (have_d) {
         snprintf(ddev, sizeof(ddev), "%u:%u", major(ds.st_dev), minor(ds.st_dev));
         snprintf(dino, sizeof(dino), "%llu", (unsigned long long)ds.st_ino);
      }
      if (have_f)
         snprintf(fdev, sizeof(fdev), "%u:%u", fj, fn);
      bool same = !deleted && have_d &&
                  (have_m ? ms.st_dev == ds.st_dev && ms.st_ino == ds.st_ino
                          : have_f && fj == mj && fn == mn && ds.st_ino == ino);
      out("map class=%s path=%s range=%s maps_dev=%u:%u maps_ino=%llu deleted=%d mapped_dev=%s "
          "mapped_ino=%s disk_dev=%s disk_ino=%s disk_mount_dev=%s same_file=%d\n",
          cls, p, range, mj, mn, ino, deleted, mdev, mino, ddev, dino, fdev, same);
      if (strcmp(cls, "implementation") == 0) {
         impls++;
         if (deleted || strncmp(p, PREFIX, strlen(PREFIX)) != 0) {
            ok = false;
            fail_reason("maps: a Mesa implementation object is deleted or not under " PREFIX);
         } else if (!same) {
            ok = false;
            fail_reason("maps: a mapped Mesa implementation object is not the file now on disk");
         }
      }
   }
   fclose(f);
   if (!impls) {
      ok = false;
      fail_reason("maps: no Mesa implementation object is mapped");
   }
   out("maps.implementation_objects=%u\n", impls);
   regfree(&impl);
   regfree(&sys);
   regfree(&any);
   return ok;
}

/* The DRM nodes this process has open. */
static void
report_fds(void)
{
   for (int fd = 0; fd < 1024; ++fd) {
      char link[64], target[256];
      snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
      ssize_t n = readlink(link, target, sizeof(target) - 1);
      if (n <= 0)
         continue;
      target[n] = '\0';
      if (strncmp(target, "/dev/dri/", 9) == 0)
         out("fd.dri fd=%d node=%s\n", fd, target);
   }
}

static bool
env_wanted(const char *entry)
{
   for (size_t i = 0; i < sizeof(env_prefixes) / sizeof(env_prefixes[0]); ++i)
      if (strncmp(entry, env_prefixes[i], strlen(env_prefixes[i])) == 0)
         return true;
   for (size_t i = 0; i < sizeof(env_names) / sizeof(env_names[0]); ++i) {
      size_t l = strlen(env_names[i]);
      if (strncmp(entry, env_names[i], l) == 0 && entry[l] == '=')
         return true;
   }
   return false;
}

static void
read_line(const char *path, char *buf, size_t size)
{
   buf[0] = '\0';
   FILE *f = fopen(path, "r");
   if (!f)
      return;
   if (fgets(buf, size, f))
      buf[strcspn(buf, "\n")] = '\0';
   fclose(f);
}

int
main(int argc, char **argv)
{
   const char *out_path = NULL;
   char tmp_path[4096] = "";
   if (argc == 3 && strcmp(argv[1], "--out") == 0 && argv[2][0]) {
      out_path = argv[2];
   } else if (argc != 1) {
      fprintf(stderr, "usage: mesa-m3-probe [--out FILE]\n");
      return 2;
   }
   if (out_path) {
      snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", out_path);
      int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
      if (fd < 0 || !(out_file = fdopen(fd, "w"))) {
         fprintf(stderr, "mesa-m3-probe: cannot write %s\n", tmp_path);
         return 2;
      }
   }
   alarm(90);
   setvbuf(stdout, NULL, _IOLBF, 0);

   unsigned long long start = 0;
   char stat_line[2048];
   read_line("/proc/self/stat", stat_line, sizeof(stat_line));
   char *p = strrchr(stat_line, ')');
   /* Field 22 is starttime; fields 3.. follow the ')'. */
   for (int field = 3; p && field <= 22; ++field) {
      p = strchr(p + 1, ' ');
      if (field == 22 && p)
         start = strtoull(p + 1, NULL, 10);
   }
   char boot[64], cg[512], exe[512] = "";
   read_line("/proc/sys/kernel/random/boot_id", boot, sizeof(boot));
   read_line("/proc/self/cgroup", cg, sizeof(cg));
   ssize_t el = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
   if (el > 0)
      exe[el] = '\0';
   out("probe=mesa-m3-probe 1\n");
   out("pid=%d starttime=%llu boot_id=%s\n", getpid(), start, boot);
   out("cgroup=%s\n", cg);
   out("exe=%s\n", exe);
   for (char **e = environ; *e; ++e)
      if (env_wanted(*e))
         out("env.%s\n", *e);

   bool gl = gl_probe();
#ifdef MESA_M3_GL_ONLY
   bool vk = true;
   out("vk.capability=unavailable profile=j613-25g83\n");
#else
   bool vk = vk_probe();
#endif
   report_fds();
   bool maps = report_maps();
   bool pass = gl && vk && maps;
   out("result=%s%s%s\n", pass ? "pass" : "fail", reasons[0] ? " reasons=" : "", reasons);
   out("exit=%d\n", pass ? 0 : 1);

   fflush(stdout);
   if (out_file) {
      fflush(out_file);
      fsync(fileno(out_file));
      fclose(out_file);
      if (rename(tmp_path, out_path) != 0) {
         fprintf(stderr, "mesa-m3-probe: cannot rename %s\n", tmp_path);
         _exit(2);
      }
   }
   /* The process ends with its GL and Vulkan objects still alive; the kernel
    * releases them. Nothing after the report depends on a clean teardown. */
   _exit(pass ? 0 : 1);
}
