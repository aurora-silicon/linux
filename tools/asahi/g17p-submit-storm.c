// SPDX-License-Identifier: MIT
// Small native render submits; interpose libdrm's ioctl to measure kernel CPU.
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_SAMPLES 100000

static struct sample {
	uint64_t wall, cpu;
	int rc, error;
} samples[MAX_SAMPLES];
static atomic_uint used;
static atomic_bool recording;
static int (*real_ioctl)(int, unsigned long, ...);

static uint64_t ns(clockid_t clock)
{
	struct timespec t;

	clock_gettime(clock, &t);
	return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

int ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (!real_ioctl)
		real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	/* DRM_COMMAND_BASE + DRM_ASAHI_SUBMIT (10), fixed by public UAPI. */
	if (atomic_load_explicit(&recording, memory_order_relaxed) &&
	    _IOC_TYPE(request) == 'd' && _IOC_NR(request) == 0x4a) {
		uint64_t wall = ns(CLOCK_MONOTONIC), cpu = ns(CLOCK_THREAD_CPUTIME_ID);
		int rc, error;
		unsigned int index;

		rc = real_ioctl(fd, request, arg);
		error = errno;
		cpu = ns(CLOCK_THREAD_CPUTIME_ID) - cpu;
		wall = ns(CLOCK_MONOTONIC) - wall;
		index = atomic_fetch_add_explicit(&used, 1, memory_order_relaxed);
		if (index < MAX_SAMPLES)
			samples[index] = (struct sample){wall, cpu, rc, rc < 0 ? error : 0};
		errno = error;
		return rc;
	}
	return real_ioctl(fd, request, arg);
}

static void require(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "%s EGL=%x GL=%x\n", what, eglGetError(), glGetError());
		exit(1);
	}
}

static GLuint shader(GLenum type, const char *text)
{
	GLuint s = glCreateShader(type);
	GLint ok;

	glShaderSource(s, 1, &text, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	require(ok, "shader compile");
	return s;
}

int main(int argc, char **argv)
{
	unsigned int count = argc > 1 ? strtoul(argv[1], NULL, 10) : 10000;
	unsigned int depth = argc > 2 ? strtoul(argv[2], NULL, 10) : 64;
	EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
		      EGL_OPENGL_ES3_BIT, EGL_NONE};
	EGLint ctxa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
	EGLint sa[] = {EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE};
	EGLint configs;
	EGLDisplay d;
	EGLConfig config;
	EGLContext ctx;
	EGLSurface surface;
	GLuint p;
	GLint ok;
	uint64_t start;

	real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	require(count > 0 && count <= 50000 && depth > 0 && depth <= 256, "count/depth range");
	d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	require(eglInitialize(d, NULL, NULL), "eglInitialize");
	require(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI");
	require(eglChooseConfig(d, ca, &config, 1, &configs) && configs, "eglChooseConfig");
	ctx = eglCreateContext(d, config, EGL_NO_CONTEXT, ctxa);
	surface = eglCreatePbufferSurface(d, config, sa);
	require(eglMakeCurrent(d, surface, surface, ctx), "eglMakeCurrent");
	fprintf(stderr, "renderer=%s count=%u depth=%u\n", glGetString(GL_RENDERER), count, depth);
	p = glCreateProgram();
	glAttachShader(p, shader(GL_VERTEX_SHADER,
		"#version 300 es\nvoid main(){vec2 v[3]=vec2[3](vec2(-1,-1),"
		"vec2(3,-1),vec2(-1,3));gl_Position=vec4(v[gl_VertexID],0,1);}"));
	glAttachShader(p, shader(GL_FRAGMENT_SHADER,
		"#version 300 es\nprecision highp float;out vec4 c;"
		"void main(){c=vec4(.25,.5,.75,1);}"));
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	require(ok, "program link");
	glUseProgram(p);
	glViewport(0, 0, 32, 32);
	for (unsigned int i = 0; i < 64; i++) {
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glFlush();
	}
	glFinish();
	start = ns(CLOCK_MONOTONIC);
	atomic_store_explicit(&recording, 1, memory_order_relaxed);
	for (unsigned int i = 0; i < count; i++) {
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glFlush();
		if ((i + 1) % depth == 0)
			glFinish();
	}
	glFinish();
	atomic_store_explicit(&recording, 0, memory_order_relaxed);
	require(glGetError() == GL_NO_ERROR, "draw loop");
	require(used <= MAX_SAMPLES, "sample buffer exceeded");
	fprintf(stderr, "seconds=%.6f submits=%u\n", (ns(CLOCK_MONOTONIC)-start)/1e9, used);
	puts("submit,wall_ns,cpu_ns,rc,errno");
	for (unsigned int i = 0; i < used; i++)
		printf("%u,%lu,%lu,%d,%d\n", i, samples[i].wall, samples[i].cpu,
		       samples[i].rc, samples[i].error);
	eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(d, surface);
	eglDestroyContext(d, ctx);
	eglTerminate(d);
	return used >= count ? 0 : 2;
}
