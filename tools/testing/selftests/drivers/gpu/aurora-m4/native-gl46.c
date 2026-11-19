/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Build: cc -O2 -Wall -Wextra -Werror native-gl46.c -o native-gl46
 *           $(pkg-config --cflags --libs egl gbm gl)
 * Run using the paired native Mesa and the candidate render node.
 * M4_TEST_ROUNDS=128 exercises retirement and persistent resource reuse.
 * M4_TEST_STRESS_SECONDS=60 adds bounded compute load under installed policy.
 */
#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <gbm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void fail(const char *format, ...)
{
   va_list args;
   va_start(args, format);
   fputs("FAIL: ", stderr);
   vfprintf(stderr, format, args);
   fputc('\n', stderr);
   va_end(args);
   exit(EXIT_FAILURE);
}

static void gl_check(const char *stage)
{
   GLenum error = glGetError();
   if (error != GL_NO_ERROR)
      fail("%s: GL error %#x", stage, error);
}

static GLuint shader(GLenum type, const char *text)
{
   GLuint object = glCreateShader(type);
   if (!object)
      fail("glCreateShader(%#x) returned zero", type);
   glShaderSource(object, 1, &text, NULL);
   glCompileShader(object);
   GLint okay = 0;
   glGetShaderiv(object, GL_COMPILE_STATUS, &okay);
   if (!okay) {
      char log[16384] = {0};
      glGetShaderInfoLog(object, sizeof(log), NULL, log);
      fail("shader compilation (%#x): %s", type, log);
   }
   return object;
}

static GLuint program(const GLuint *shaders, unsigned count)
{
   GLuint object = glCreateProgram();
   if (!object)
      fail("glCreateProgram returned zero");
   for (unsigned i = 0; i < count; i++)
      glAttachShader(object, shaders[i]);
   glLinkProgram(object);
   GLint okay = 0;
   glGetProgramiv(object, GL_LINK_STATUS, &okay);
   if (!okay) {
      char log[16384] = {0};
      glGetProgramInfoLog(object, sizeof(log), NULL, log);
      fail("program link: %s", log);
   }
   for (unsigned i = 0; i < count; i++)
      glDeleteShader(shaders[i]);
   return object;
}

static bool rgba(const uint8_t *p, unsigned r, unsigned g, unsigned b, unsigned a)
{
   return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

static void test_triangle(void)
{
   static const char vs[] =
      "#version 460 core\n"
      "const vec2 p[3] = vec2[3](vec2(-0.8,-0.8),"
      "vec2(0.8,-0.8),vec2(0.0,0.8));\n"
      "void main() { gl_Position = vec4(p[gl_VertexID],0.0,1.0); }\n";
   static const char fs[] =
      "#version 460 core\n"
      "layout(location=0) out vec4 color;\n"
      "void main() { color = vec4(0.0,1.0,0.0,1.0); }\n";
   GLuint shaders[] = {shader(GL_VERTEX_SHADER, vs), shader(GL_FRAGMENT_SHADER, fs)};
   GLuint pipeline = program(shaders, 2);
   GLuint texture = 0, fbo = 0, vao = 0;
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 64, 64);
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          texture, 0);
   glDrawBuffer(GL_COLOR_ATTACHMENT0);
   glReadBuffer(GL_COLOR_ATTACHMENT0);
   GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
   if (status != GL_FRAMEBUFFER_COMPLETE)
      fail("RGBA8 framebuffer status %#x", status);
   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);
   glViewport(0, 0, 64, 64);
   glDisable(GL_DITHER);
   glDisable(GL_BLEND);
   glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glUseProgram(pipeline);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   uint8_t pixels[64 * 64 * 4];
   glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   gl_check("triangle draw and FBO readback");
   const uint8_t *center = pixels + (32 * 64 + 32) * 4;
   const uint8_t *corner = pixels + (61 * 64 + 2) * 4;
   if (!rgba(center, 0, 255, 0, 255) || !rgba(corner, 255, 0, 0, 255))
      fail("triangle pixels: center=(%u,%u,%u,%u), outside=(%u,%u,%u,%u)",
           center[0], center[1], center[2], center[3],
           corner[0], corner[1], corner[2], corner[3]);
   unsigned green = 0, red = 0;
   for (unsigned i = 0; i < 64 * 64; i++) {
      green += rgba(pixels + i * 4, 0, 255, 0, 255);
      red += rgba(pixels + i * 4, 255, 0, 0, 255);
   }
   if (green < 1000 || green > 1500 || green + red != 64 * 64)
      fail("triangle coverage: %u green, %u red of 4096 pixels", green, red);
   printf("PASS: GLSL 4.60 triangle, RGBA8 FBO readback (%u green, %u red)\n",
          green, red);
   glUseProgram(0);
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
   glDeleteVertexArrays(1, &vao);
   glDeleteFramebuffers(1, &fbo);
   glDeleteTextures(1, &texture);
   glDeleteProgram(pipeline);
}

static void test_compute(void)
{
   static const char cs[] =
      "#version 460 core\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430,binding=0) buffer Result { uint data[]; };\n"
      "void main() { uint i = gl_GlobalInvocationID.x; data[i] = i*17u+5u; }\n";
   GLuint object = shader(GL_COMPUTE_SHADER, cs);
   GLuint pipeline = program(&object, 1);
   uint32_t values[64];
   for (unsigned i = 0; i < 64; i++)
      values[i] = 0xdeadbeef;
   GLuint buffer = 0;
   glGenBuffers(1, &buffer);
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
   glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(values), values, GL_DYNAMIC_READ);
   glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
   glUseProgram(pipeline);
   glDispatchCompute(2, 1, 1);
   glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
   glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(values), values);
   gl_check("compute dispatch, barrier and SSBO readback");
   for (unsigned i = 0; i < 64; i++)
      if (values[i] != i * 17u + 5u)
         fail("compute SSBO[%u]: got %#x, expected %#x", i, values[i], i * 17u + 5u);
   puts("PASS: GLSL 4.60 compute, 64 SSBO writes verified after memory barrier");
   glUseProgram(0);
   glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
   glDeleteBuffers(1, &buffer);
   glDeleteProgram(pipeline);
}


/* Geometry processing uses the Mesa compute/render path on M4. Keep transform
 * feedback and rasterization in the same draw, then verify both GPU results. */
static void test_geometry_feedback(void)
{
   static const char vs[] =
      "#version 460 core\n"
      "const vec2 p[3] = vec2[3](vec2(-0.5,-0.5),"
      "vec2(0.5,-0.5),vec2(0.0,0.5));\n"
      "void main() { gl_Position = vec4(p[gl_VertexID],0.0,1.0); }\n";
   static const char gs[] =
      "#version 460 core\n"
      "layout(triangles) in;\n"
      "layout(triangle_strip,max_vertices=3) out;\n"
      "out vec4 captured;\n"
      "void main() { for (int i=0; i<3; ++i) {\n"
      "gl_Position=gl_in[i].gl_Position; captured=gl_Position;\n"
      "EmitVertex(); } EndPrimitive(); }\n";
   static const char fs[] =
      "#version 460 core\n"
      "layout(location=0) out vec4 color;\n"
      "void main() { color=vec4(0.0,0.0,1.0,1.0); }\n";
   GLuint shaders[] = {shader(GL_VERTEX_SHADER, vs),
                       shader(GL_GEOMETRY_SHADER, gs),
                       shader(GL_FRAGMENT_SHADER, fs)};
   GLuint pipeline = glCreateProgram();
   for (unsigned i = 0; i < 3; i++)
      glAttachShader(pipeline, shaders[i]);
   const char *varying = "captured";
   glTransformFeedbackVaryings(pipeline, 1, &varying, GL_INTERLEAVED_ATTRIBS);
   glLinkProgram(pipeline);
   GLint linked = 0;
   glGetProgramiv(pipeline, GL_LINK_STATUS, &linked);
   if (!linked) {
      char log[16384] = {0};
      glGetProgramInfoLog(pipeline, sizeof(log), NULL, log);
      fail("geometry/feedback program link: %s", log);
   }
   for (unsigned i = 0; i < 3; i++)
      glDeleteShader(shaders[i]);
   GLuint buffer = 0, texture = 0, fbo = 0, vao = 0, query = 0, feedback = 0;
   float vertices[12] = {0};
   glGenBuffers(1, &buffer);
   glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
   glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, sizeof(vertices), vertices,
                GL_DYNAMIC_READ);
   glGenTransformFeedbacks(1, &feedback);
   glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, feedback);
   glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 64, 64);
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         texture, 0);
   glDrawBuffer(GL_COLOR_ATTACHMENT0);
   glReadBuffer(GL_COLOR_ATTACHMENT0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      fail("geometry framebuffer incomplete");
   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);
   glViewport(0, 0, 64, 64);
   glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glUseProgram(pipeline);
   glGenQueries(1, &query);
   glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, query);
   glBeginTransformFeedback(GL_TRIANGLES);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glEndTransformFeedback();
   glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
   glMemoryBarrier(GL_TRANSFORM_FEEDBACK_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
   glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(vertices), vertices);
   GLuint written = 0;
   glGetQueryObjectuiv(query, GL_QUERY_RESULT, &written);
   const float expected[] = {-0.5f,-0.5f,0.0f,1.0f, 0.5f,-0.5f,0.0f,1.0f,
                             0.0f,0.5f,0.0f,1.0f};
   for (unsigned i = 0; i < 12; i++)
      if (vertices[i] != expected[i])
         fail("geometry feedback[%u]: %g, expected %g", i, vertices[i], expected[i]);
   if (written != 1)
      fail("geometry feedback wrote %u primitives, expected 1", written);
   uint8_t center[4], corner[4];
   glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
   glReadPixels(2, 61, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
   gl_check("geometry, transform feedback, query and rasterization");
   if (!rgba(center, 0, 0, 255, 255) || !rgba(corner, 255, 0, 0, 255))
      fail("geometry rasterization: center=(%u,%u,%u,%u), outside=(%u,%u,%u,%u)",
           center[0],center[1],center[2],center[3],
           corner[0],corner[1],corner[2],corner[3]);
   puts("PASS: geometry+transform feedback+render, captured vertices and query verified");
   glUseProgram(0);
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
   glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, 0);
   glDeleteTransformFeedbacks(1, &feedback);
   glDeleteQueries(1, &query);
   glDeleteVertexArrays(1, &vao);
   glDeleteBuffers(1, &buffer);
   glDeleteFramebuffers(1, &fbo);
   glDeleteTextures(1, &texture);
   glDeleteProgram(pipeline);
}

static double monotonic_seconds(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts))
      fail("clock_gettime: %s", strerror(errno));
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Short, individually completed dispatches keep the workload bounded even
 * at S1. The installed kernel policy owns thermal regulation. No test-specific
 * temperature or power cutoff is introduced here.
 */
static void test_compute_load(unsigned seconds)
{
   static const char cs[] =
      "#version 460 core\n"
      "layout(local_size_x=64) in;\n"
      "layout(location=0) uniform uint iterations;\n"
      "layout(std430,binding=0) buffer Result { uint data[]; };\n"
      "void main() { uint i=gl_GlobalInvocationID.x; uint x=i+1u;\n"
      "for(uint n=0u;n<iterations;n++) x=(x^(x>>13u))*1664525u+1013904223u;\n"
      "data[i]=x; }\n";
   GLuint object = shader(GL_COMPUTE_SHADER, cs);
   GLuint pipeline = program(&object, 1), buffer;
   const unsigned invocations = 1024 * 64, iterations = 256;
   glGenBuffers(1, &buffer);
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
   glBufferData(GL_SHADER_STORAGE_BUFFER, invocations * sizeof(uint32_t),
                NULL, GL_DYNAMIC_READ);
   glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
   glUseProgram(pipeline);
   glUniform1ui(0, iterations);
   unsigned long dispatches = 0;
   double start = monotonic_seconds();
   do {
      glDispatchCompute(1024, 1, 1);
      glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
      glFinish();
      gl_check("bounded compute load");
      dispatches++;
   } while (monotonic_seconds() - start < seconds);
   for (unsigned index = 0; index < invocations; index += invocations - 1) {
      uint32_t actual, expected = index + 1;
      for (unsigned n = 0; n < iterations; n++)
         expected = (expected ^ (expected >> 13)) * 1664525u + 1013904223u;
      glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, index * sizeof(actual),
                         sizeof(actual), &actual);
      if (actual != expected)
         fail("compute load[%u]: %#x, expected %#x", index, actual, expected);
   }
   gl_check("compute load readback");
   printf("PASS: bounded compute load, %lu dispatches in %.3f seconds, readback verified\n",
          dispatches, monotonic_seconds() - start);
   glUseProgram(0);
   glDeleteBuffers(1, &buffer);
   glDeleteProgram(pipeline);
}

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   if (argc > 2)
      fail("usage: %s [/dev/dri/renderD128]", argv[0]);
   const char *overrides[] = {"MESA_GL_VERSION_OVERRIDE", "MESA_GLSL_VERSION_OVERRIDE",
                              "MESA_GLES_VERSION_OVERRIDE", "MESA_EXTENSION_OVERRIDE"};
   for (unsigned i = 0; i < sizeof(overrides) / sizeof(overrides[0]); i++) {
      const char *value = getenv(overrides[i]);
      if (value && *value)
         fail("%s is set; this test requires the driver's reported capabilities",
              overrides[i]);
   }
   const char *node = argc == 2 ? argv[1] : "/dev/dri/renderD128";
   int fd = open(node, O_RDWR | O_CLOEXEC);
   if (fd < 0)
      fail("open %s: %s", node, strerror(errno));
   struct gbm_device *gbm = gbm_create_device(fd);
   if (!gbm)
      fail("gbm_create_device(%s) failed", node);
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   if (!get_display)
      fail("EGL_EXT_platform_base entry point is unavailable");
   EGLDisplay display = get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
   EGLint major = 0, minor = 0;
   if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor))
      fail("EGL GBM initialization: error %#x", eglGetError());
   printf("Render node: %s\nEGL: %d.%d %s\n", node, major, minor,
          eglQueryString(display, EGL_VENDOR));
   if (!eglBindAPI(EGL_OPENGL_API))
      fail("eglBindAPI(EGL_OPENGL_API): error %#x", eglGetError());
   const EGLint config_attributes[] = {EGL_SURFACE_TYPE, 0,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
   EGLConfig config;
   EGLint count = 0;
   if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || count != 1)
      fail("no desktop OpenGL EGL configuration: error %#x", eglGetError());
   const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 4,
      EGL_CONTEXT_MINOR_VERSION_KHR, 6, EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
      EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
   EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                           context_attributes);
   if (context == EGL_NO_CONTEXT)
      fail("native OpenGL 4.6 core context creation: EGL error %#x", eglGetError());
   if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
      fail("surfaceless eglMakeCurrent: EGL error %#x", eglGetError());
   const char *renderer = (const char *)glGetString(GL_RENDERER);
   const char *version = (const char *)glGetString(GL_VERSION);
   const char *vendor = (const char *)glGetString(GL_VENDOR);
   const char *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
   if (!renderer || !version || !vendor || !glsl)
      fail("OpenGL context returned missing identity strings");
   printf("GL_VENDOR: %s\nGL_RENDERER: %s\nGL_VERSION: %s\nGLSL: %s\n",
          vendor, renderer, version, glsl);
   const char *blocked[] = {"zink", "llvmpipe", "softpipe", "swrast", "software",
                             "lavapipe", "swiftshader"};
   for (unsigned i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++)
      if (strcasestr(renderer, blocked[i]) || strcasestr(version, blocked[i]))
         fail("renderer/version identifies %s; native hardware rendering is required",
              blocked[i]);
   if (!strstr(version, "Mesa"))
      fail("GL_VERSION does not identify Mesa");
   GLint gl_major = 0, gl_minor = 0, profile = 0;
   glGetIntegerv(GL_MAJOR_VERSION, &gl_major);
   glGetIntegerv(GL_MINOR_VERSION, &gl_minor);
   glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
   if (gl_major < 4 || (gl_major == 4 && gl_minor < 6) ||
       !(profile & GL_CONTEXT_CORE_PROFILE_BIT))
      fail("context reports OpenGL %d.%d, profile %#x; 4.6 core is required",
           gl_major, gl_minor, profile);
   gl_check("context identity and capabilities");
   unsigned rounds = 1;
   const char *rounds_text = getenv("M4_TEST_ROUNDS");
   if (rounds_text) {
      char *end = NULL;
      errno = 0;
      unsigned long count = strtoul(rounds_text, &end, 10);
      if (errno || end == rounds_text || *end || count < 1 || count > 10000)
         fail("M4_TEST_ROUNDS must be 1..10000");
      rounds = count;
   }
   for (unsigned i = 0; i < rounds; i++) {
      printf("Round %u/%u\n", i + 1, rounds);
      test_triangle();
      test_compute();
      test_geometry_feedback();
   }
   const char *stress_text = getenv("M4_TEST_STRESS_SECONDS");
   if (stress_text) {
      char *end = NULL;
      errno = 0;
      unsigned long seconds = strtoul(stress_text, &end, 10);
      if (errno || end == stress_text || *end || seconds < 1 || seconds > 300)
         fail("M4_TEST_STRESS_SECONDS must be 1..300");
      test_compute_load(seconds);
   }
   glFinish();
   gl_check("final completion and cleanup");
   if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) ||
       !eglDestroyContext(display, context) || !eglTerminate(display))
      fail("EGL context cleanup: error %#x", eglGetError());
   gbm_device_destroy(gbm);
   close(fd);
   puts("PASS: native Mesa OpenGL 4.6 core rendering and compute");
   return EXIT_SUCCESS;
}
