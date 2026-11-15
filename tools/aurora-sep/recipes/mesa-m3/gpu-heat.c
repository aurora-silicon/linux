/* SPDX-License-Identifier: MIT */
/*
 * gpu-heat: a sustained, GPU-bound GLES load for the M3 thermal-limit tests.
 *
 *   gpu-heat SECS [W H ITER]
 *
 * Draws a full-screen triangle into a WxH RGBA8 framebuffer object (default 2048x2048) for
 * SECS seconds (1..1800), with a fragment shader that runs ITER iterations of complex-number
 * and trigonometric arithmetic per pixel (default 256, 1..512). Nothing is read back except
 * one pixel at the end. It uses a surfaceless EGL display, never the desktop, and refuses to
 * run on anything but zink on the M3 (run it with the prefix environment, env.sh).
 *
 * Bounds: W and H are 64..4096 and W*H*ITER is at most 2048*2048*512, so one frame stays in
 * the tens of milliseconds even at the lowest performance state. The draw loop calls
 * glFinish() every 4 frames, so at most a few frames are in flight.
 *
 * Progress (frames, fps) is printed every 5 s. Exit status 0 on completion, 1 on a GL error.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x)                                                                            \
   do {                                                                                     \
      if (!(x)) {                                                                           \
         fprintf(stderr, "FAIL line %d: %s (EGL=0x%x GL=0x%x)\n", __LINE__, #x,             \
                 eglGetError(), glGetError());                                             \
         exit(1);                                                                           \
      }                                                                                     \
   } while (0)

static volatile sig_atomic_t stop;

static void
on_signal(int sig)
{
   (void)sig;
   stop = 1;
}

static double
now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static GLuint
shader(GLenum stage, const char *src)
{
   GLuint s = glCreateShader(stage);
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   GLint ok = 0;
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[2048];
      glGetShaderInfoLog(s, sizeof(log), NULL, log);
      fprintf(stderr, "shader compile failed: %s\n", log);
      exit(1);
   }
   return s;
}

static GLuint
program(int iter)
{
   const char *vs = "#version 300 es\n"
                    "void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
                    "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
   char fs[1024];
   snprintf(fs, sizeof(fs),
            "#version 300 es\n"
            "precision highp float;\n"
            "uniform float t;\n"
            "out vec4 o;\n"
            "void main(){\n"
            "  vec2 p=gl_FragCoord.xy*0.0005-vec2(0.5);\n"
            "  vec2 c=vec2(0.3*sin(t),0.3*cos(t));\n"
            "  float a=0.0;\n"
            "  for(int i=0;i<%d;i++){\n"
            "    p=vec2(p.x*p.x-p.y*p.y,2.0*p.x*p.y)*0.7+c+0.01*vec2(sin(p.y+float(i)),cos(p.x));\n"
            "    a+=fract(p.x*3.1+p.y*1.7);\n"
            "  }\n"
            "  o=vec4(fract(a*0.013),fract(a*0.029),fract(a*0.047),1.0);\n"
            "}\n",
            iter);
   GLuint prog = glCreateProgram();
   glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
   glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
   glLinkProgram(prog);
   GLint ok = 0;
   glGetProgramiv(prog, GL_LINK_STATUS, &ok);
   CHECK(ok);
   return prog;
}

int
main(int argc, char **argv)
{
   if (argc != 2 && argc != 5) {
      fprintf(stderr, "usage: %s SECS [W H ITER]\n", argv[0]);
      return 2;
   }
   const double secs = atof(argv[1]);
   const long w = argc > 2 ? atol(argv[2]) : 2048, h = argc > 2 ? atol(argv[3]) : 2048;
   const long iter = argc > 2 ? atol(argv[4]) : 256;
   if (!(secs >= 1 && secs <= 1800) || w < 64 || w > 4096 || h < 64 || h > 4096 || iter < 1 ||
       iter > 512 || w * h * iter > 2048L * 2048 * 512) {
      fprintf(stderr, "out of bounds: SECS 1..1800, W/H 64..4096, ITER 1..512, W*H*ITER <= 2048*2048*512\n");
      return 2;
   }
   signal(SIGINT, on_signal);
   signal(SIGTERM, on_signal);

   EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   CHECK(dpy != EGL_NO_DISPLAY);
   EGLint maj, min;
   CHECK(eglInitialize(dpy, &maj, &min));
   CHECK(eglBindAPI(EGL_OPENGL_ES_API));
   EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
   EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
   CHECK(ctx != EGL_NO_CONTEXT);
   CHECK(eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx));
   const char *r = (const char *)glGetString(GL_RENDERER);
   printf("Renderer: %s\n", r);
   CHECK(r && strstr(r, "zink") && strstr(r, "M3"));

   GLuint tex, fb;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
   glViewport(0, 0, w, h);

   GLuint prog = program(iter);
   glUseProgram(prog);
   GLint t_loc = glGetUniformLocation(prog, "t");
   CHECK(glGetError() == GL_NO_ERROR);
   printf("gpu-heat: %ldx%ld, %ld iterations per pixel, %.0f s\n", w, h, iter, secs);
   fflush(stdout);

   const double t0 = now();
   double last = t0;
   unsigned long frames = 0;
   while (!stop && now() - t0 < secs) {
      glUniform1f(t_loc, (float)(frames % 100000) * 0.01f);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      if (++frames % 4 == 0) {
         glFinish();
         CHECK(glGetError() == GL_NO_ERROR);
      }
      if (now() - last >= 5.0) {
         last = now();
         printf("progress t=%.1fs frames=%lu fps=%.1f\n", last - t0, frames,
                frames / (last - t0));
         fflush(stdout);
      }
   }
   glFinish();
   uint8_t px[4] = {0};
   glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   CHECK(glGetError() == GL_NO_ERROR);
   const double dt = now() - t0;
   printf("DONE: %lu frames in %.1f s (%.1f fps)%s; centre pixel %u %u %u %u\n", frames, dt,
          frames / dt, stop ? " (stopped by signal)" : "", px[0], px[1], px[2], px[3]);
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(dpy, ctx);
   eglTerminate(dpy);
   return 0;
}
