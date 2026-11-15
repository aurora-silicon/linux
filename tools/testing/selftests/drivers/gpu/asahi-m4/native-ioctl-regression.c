// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Opt-in live tests; uses the real kernel UAPI, never a copied ioctl ABI.
 * No shader or valid hardware command is submitted. The mixed stream must fail
 * structural admission. Run only through run-native-ioctl-regression.py on the
 * patched candidate while its independent rollback supervisor is armed.
 */
#define _GNU_SOURCE
#include <asahi_drm.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifndef ASAHI_M4_UAPI_SHA256
#error Build with the SHA256 of the actual asahi_drm.h being included
#endif

static const char *node;
static atomic_int failed;
static atomic_uint vm_cycles, timestamp_cycles;
static pthread_barrier_t start_barrier;

static int checked_ioctl(int fd, unsigned long request, void *data, const char *what)
{
	if (ioctl(fd, request, data) == 0)
		return 0;
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	return -1;
}

static int parameters(int fd, struct drm_asahi_params_global *params)
{
	struct drm_asahi_get_params get = {
		.pointer = (uintptr_t)params, .size = sizeof(*params),
	};
	memset(params, 0, sizeof(*params));
	if (checked_ioctl(fd, DRM_IOCTL_ASAHI_GET_PARAMS, &get, "GET_PARAMS"))
		return -1;
	if (params->gpu_generation != 16 || params->chip_id != 0x8132 ||
	    params->gpu_hal_generation != DRM_ASAHI_GPU_HAL_200 ||
	    !(params->features & DRM_ASAHI_FEATURE_VM_STATUS) ||
	    params->max_commands_per_submission < 2 ||
	    params->vm_end <= params->vm_start ||
	    params->vm_kernel_min_size >= params->vm_end - params->vm_start) {
		fprintf(stderr, "Unexpected native M4 GET_PARAMS identity/ranges\n");
		return -1;
	}
	return 0;
}

static int create_vm(int fd, const struct drm_asahi_params_global *p, uint32_t *id)
{
	struct drm_asahi_vm_create create = {
		.kernel_start = p->vm_end - p->vm_kernel_min_size,
		.kernel_end = p->vm_end,
	};
	if (checked_ioctl(fd, DRM_IOCTL_ASAHI_VM_CREATE, &create, "VM_CREATE"))
		return -1;
	*id = create.vm_id;
	return 0;
}

static int destroy_vm(int fd, uint32_t id)
{
	struct drm_asahi_vm_destroy destroy = { .vm_id = id };
	return checked_ioctl(fd, DRM_IOCTL_ASAHI_VM_DESTROY, &destroy, "VM_DESTROY");
}

static int healthy_vm(int fd, uint32_t id)
{
	struct drm_asahi_vm_status status = { .vm_id = id };
	struct drm_asahi_get_params get = {
		.param_group = DRM_ASAHI_PARAM_GROUP_VM_STATUS,
		.pointer = (uintptr_t)&status, .size = sizeof(status),
	};
	if (checked_ioctl(fd, DRM_IOCTL_ASAHI_GET_PARAMS, &get, "VM_STATUS"))
		return -1;
	if (status.error) {
		fprintf(stderr, "VM %u became sticky: %d\n", id, status.error);
		return -1;
	}
	return 0;
}

static int create_bo(int fd, uint64_t size, uint32_t *handle)
{
	struct drm_asahi_gem_create create = {
		.size = size, .flags = DRM_ASAHI_GEM_WRITEBACK,
	};
	if (checked_ioctl(fd, DRM_IOCTL_ASAHI_GEM_CREATE, &create, "GEM_CREATE"))
		return -1;
	*handle = create.handle;
	return 0;
}

static int close_bo(int fd, uint32_t handle)
{
	struct drm_gem_close close = { .handle = handle };
	return checked_ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close, "GEM_CLOSE");
}

static int bind_timestamp(int fd, uint32_t handle, uint64_t bytes, uint32_t *object)
{
	struct drm_asahi_gem_bind_object bind = {
		.op = DRM_ASAHI_BIND_OBJECT_OP_BIND,
		.flags = DRM_ASAHI_BIND_OBJECT_USAGE_TIMESTAMPS,
		.handle = handle, .range = bytes,
	};
	if (checked_ioctl(fd, DRM_IOCTL_ASAHI_GEM_BIND_OBJECT, &bind, "TIMESTAMP_BIND"))
		return -1;
	*object = bind.object_handle;
	return 0;
}

static int unbind_timestamp(int fd, uint32_t object)
{
	struct drm_asahi_gem_bind_object unbind = {
		.op = DRM_ASAHI_BIND_OBJECT_OP_UNBIND, .object_handle = object,
	};
	return checked_ioctl(fd, DRM_IOCTL_ASAHI_GEM_BIND_OBJECT, &unbind, "TIMESTAMP_UNBIND");
}

static int create_queue(int fd, uint32_t vm, uint64_t usc, uint32_t *id)
{
	struct drm_asahi_queue_create create = {
		.vm_id = vm, .priority = DRM_ASAHI_PRIORITY_MEDIUM, .usc_exec_base = usc,
	};
	if (checked_ioctl(fd, DRM_IOCTL_ASAHI_QUEUE_CREATE, &create, "QUEUE_CREATE"))
		return -1;
	*id = create.queue_id;
	return 0;
}

static int destroy_queue(int fd, uint32_t id)
{
	struct drm_asahi_queue_destroy destroy = { .queue_id = id };
	return checked_ioctl(fd, DRM_IOCTL_ASAHI_QUEUE_DESTROY, &destroy, "QUEUE_DESTROY");
}

static int bind_normal(int fd, uint32_t vm, uint32_t bo, uint64_t bytes,
		       uint64_t address, bool unbind)
{
	struct drm_asahi_gem_bind_op op = {
		.flags = unbind ? DRM_ASAHI_BIND_UNBIND : DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_WRITE,
		.handle = unbind ? 0 : bo, .range = bytes, .addr = address,
	};
	struct drm_asahi_vm_bind bind = {
		.vm_id = vm, .num_binds = 1, .stride = sizeof(op), .userptr = (uintptr_t)&op,
	};
	return checked_ioctl(fd, DRM_IOCTL_ASAHI_VM_BIND, &bind, unbind ? "VM_UNBIND" : "VM_BIND");
}

struct compute_stats {
	uint64_t count, prefixes, mixed_count, mixed_prefixes;
	uint64_t compute_published, render_published;
};

static int compute_stats(struct compute_stats *s)
{
	/* Kernels before the Aurora rename used "asahi-g16". */
	FILE *file = fopen("/sys/kernel/debug/aurora-g16/stats", "re");
	if (!file)
		file = fopen("/sys/kernel/debug/asahi-g16/stats", "re");
	char *line = NULL;
	size_t size = 0;
	bool compute = false, mixed = false, publications = false;
	if (!file)
		return -1;
	memset(s, 0, sizeof(*s));
	while (getline(&line, &size, file) >= 0) {
		if (sscanf(line, "compute: count %" SCNu64 " prefixes %" SCNu64,
			   &s->count, &s->prefixes) == 2)
			compute = true;
		if (sscanf(line, "mixed: count %" SCNu64 " prefixes %" SCNu64,
			   &s->mixed_count, &s->mixed_prefixes) == 2)
			mixed = true;
		if (sscanf(line, "publications: compute %" SCNu64 " render %" SCNu64,
			   &s->compute_published, &s->render_published) == 2)
			publications = true;
	}
	free(line);
	fclose(file);
	return compute && mixed && publications ? 0 : -1;
}

/* All payload sizes/offsets/ioctls come from the included production UAPI. */
struct mixed_stream {
	struct drm_asahi_cmd_header compute_header;
	struct drm_asahi_cmd_compute compute;
	struct drm_asahi_cmd_header render_header;
	struct drm_asahi_cmd_render render;
};
_Static_assert(sizeof(struct mixed_stream) == 2 * sizeof(struct drm_asahi_cmd_header) +
	       sizeof(struct drm_asahi_cmd_compute) + sizeof(struct drm_asahi_cmd_render),
	       "Unexpected inter-command padding");

static int mixed_rejection(uint64_t *render_publication_delta)
{
	struct drm_asahi_params_global p;
	struct drm_asahi_gem_mmap_offset offset = {0};
	uint32_t vm = 0, queue = 0, bo = 0, object = 0;
	uint64_t bytes = (uint64_t)sysconf(_SC_PAGESIZE);
	int fd = open(node, O_RDWR | O_CLOEXEC), result = -1;
	unsigned char *mapped = MAP_FAILED;
	if (fd < 0 || parameters(fd, &p) || create_vm(fd, &p, &vm))
		goto out;
	/* An aligned USC window is required even though this test runs no shader. */
	uint64_t usc = (p.vm_start + UINT64_C(0xffffffff)) & ~UINT64_C(0xffffffff);
	uint64_t unmapped = p.vm_start + bytes * 16;
	if (usc + UINT64_C(0x100000000) > p.vm_end - p.vm_kernel_min_size ||
	    create_queue(fd, vm, usc, &queue) || create_bo(fd, bytes, &bo))
		goto out;
	offset.handle = bo;
	if (checked_ioctl(fd, DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET, &offset, "GEM_MMAP_OFFSET"))
		goto out;
	mapped = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset.offset);
	if (mapped == MAP_FAILED || bind_timestamp(fd, bo, bytes, &object))
		goto out;
	memset(mapped, 0xa7, bytes);
	for (unsigned int variant = 0; variant < 4; variant++) {
		struct mixed_stream stream = {
			.compute_header = { .cmd_type = DRM_ASAHI_CMD_COMPUTE,
				.size = sizeof(stream.compute), .vdm_barrier = DRM_ASAHI_BARRIER_NONE,
				.cdm_barrier = DRM_ASAHI_BARRIER_NONE },
			.compute = { .cdm_ctrl_stream_base = unmapped,
				.cdm_ctrl_stream_end = unmapped + 4 },
			.render_header = { .cmd_type = DRM_ASAHI_CMD_RENDER,
				.size = sizeof(stream.render), .vdm_barrier = DRM_ASAHI_BARRIER_NONE,
				.cdm_barrier = 1 },
			.render = { .width_px = 16384, .height_px = 16384,
				.layers = variant < 2 ? 7 : 2048, .samples = 1,
				.utile_width_px = 32, .utile_height_px = 32, .sample_size_B = 4 },
		};
		/* Odd variants name real sentinel storage; even variants name no object.
		 * E2BIG must beat both timestamp ENOENT and unmapped CDM-span ENOENT. */
		stream.compute.ts.start.handle = variant & 1 ? object : UINT32_MAX;
		stream.compute.ts.end = stream.compute.ts.start;
		stream.compute.ts.end.offset = 8;
		struct drm_asahi_submit submit = {
			.queue_id = queue, .cmdbuf = (uintptr_t)&stream, .cmdbuf_size = sizeof(stream),
		};
		struct compute_stats before, after;
		if (compute_stats(&before))
			goto out;
		errno = 0;
		int ret = ioctl(fd, DRM_IOCTL_ASAHI_SUBMIT, &submit), error = errno;
		if (ret != -1 || error != E2BIG) {
			fprintf(stderr, "mixed variant %u: expected E2BIG, returned %d errno %d\n",
				variant, ret, error);
			goto out;
		}
		if (compute_stats(&after) || before.compute_published != after.compute_published ||
		    before.count != after.count || before.prefixes != after.prefixes ||
		    before.mixed_count != after.mixed_count || before.mixed_prefixes != after.mixed_prefixes) {
			fprintf(stderr, "Compute publication/retirement counters changed; no zero-publication proof\n");
			goto out;
		}
		/* The desktop may publish render commands on unrelated VMs. Record that
		 * delta without attributing it to this structurally refused stream. */
		*render_publication_delta += after.render_published - before.render_published;
		for (uint64_t i = 0; i < bytes; i++) {
			if (((volatile unsigned char *)mapped)[i] != 0xa7) {
				fprintf(stderr, "Timestamp sentinel modified after refused SUBMIT\n");
				goto out;
			}
		}
		if (healthy_vm(fd, vm))
			goto out;
	}
	/* Same VM still accepts mapping changes and a fresh logical queue. */
	uint32_t second_queue = 0;
	if (create_queue(fd, vm, usc, &second_queue) || destroy_queue(fd, second_queue) ||
	    bind_normal(fd, vm, bo, bytes, unmapped, false) ||
	    bind_normal(fd, vm, 0, bytes, unmapped, true) || healthy_vm(fd, vm))
		goto out;
	result = 0;
out:
	if (object && unbind_timestamp(fd, object)) result = -1;
	if (mapped != MAP_FAILED) munmap(mapped, bytes);
	if (bo && close_bo(fd, bo)) result = -1;
	if (queue && destroy_queue(fd, queue)) result = -1;
	if (vm && destroy_vm(fd, vm)) result = -1;
	if (fd >= 0) close(fd);
	return result;
}

/* Timestamp destinations are CPU-owned mappings. One 16 KiB GEM object can
 * have more simultaneous timestamp handles than the old 64 MiB firmware arena
 * could contain. This exercises real handle/mapping lifetime without shaders
 * or allocating 4097 distinct backing objects. */
static int timestamp_aliases(void)
{
	enum { aliases = 4097 };
	struct drm_asahi_params_global params;
	uint32_t bo = 0, vm = 0, bound = 0;
	uint32_t *objects = calloc(aliases, sizeof(*objects));
	int fd = open(node, O_RDWR | O_CLOEXEC), result = -1;

	if (!objects || fd < 0 || parameters(fd, &params) ||
	    create_vm(fd, &params, &vm) || create_bo(fd, 16384, &bo))
		goto out;
	for (; bound < aliases; bound++) {
		if (bind_timestamp(fd, bo, 16384, &objects[bound])) {
			fprintf(stderr, "Timestamp alias %u/%u rejected\n", bound + 1, aliases);
			goto out;
		}
	}
	if (healthy_vm(fd, vm))
		goto out;
	result = 0;
out:
	while (bound) {
		if (unbind_timestamp(fd, objects[--bound]))
			result = -1;
	}
	if (bo && close_bo(fd, bo)) result = -1;
	if (vm && destroy_vm(fd, vm)) result = -1;
	if (fd >= 0) close(fd);
	free(objects);
	return result;
}

struct worker { unsigned int index, loops; };

static void *stress_worker(void *opaque)
{
	struct worker *worker = opaque;
	struct drm_asahi_params_global p;
	uint32_t bo = 0;
	uint64_t bytes = (uint64_t)sysconf(_SC_PAGESIZE);
	int fd = open(node, O_RDWR | O_CLOEXEC);
	bool timestamps = worker->index & 1;
	if (fd < 0 || parameters(fd, &p) || (timestamps && create_bo(fd, bytes, &bo)))
		atomic_store(&failed, 1);
	pthread_barrier_wait(&start_barrier);
	for (unsigned int i = 0; i < worker->loops && !atomic_load(&failed); i++) {
		uint32_t id = 0;
		if (timestamps) {
			if (bind_timestamp(fd, bo, bytes, &id) || unbind_timestamp(fd, id)) {
				atomic_store(&failed, 1);
				break;
			}
			atomic_fetch_add(&timestamp_cycles, 1);
		} else {
			if (create_vm(fd, &p, &id) || healthy_vm(fd, id) || destroy_vm(fd, id)) {
				atomic_store(&failed, 1);
				break;
			}
			atomic_fetch_add(&vm_cycles, 1);
		}
	}
	if (bo && close_bo(fd, bo)) atomic_store(&failed, 1);
	if (fd >= 0) close(fd);
	return NULL;
}

static int read_text(const char *path, char *text, size_t size)
{
	FILE *file = fopen(path, "re");
	if (!file) return -1;
	size_t n = fread(text, 1, size - 1, file);
	text[n] = 0;
	bool ok = !ferror(file) && feof(file);
	fclose(file);
	return ok ? 0 : -1;
}

static bool command_word(const char *line, const char *key, const char *value)
{
	char expected[256], *copy = strdup(line), *save = NULL;
	unsigned int matches = 0;
	if (!copy || snprintf(expected, sizeof(expected), "%s=%s", key, value) >= (int)sizeof(expected)) {
		free(copy);
		return false;
	}
	for (char *word = strtok_r(copy, " \n\t", &save); word; word = strtok_r(NULL, " \n\t", &save))
		if (!strncmp(word, key, strlen(key)) && word[strlen(key)] == '=') {
			if (strcmp(word, expected)) { free(copy); return false; }
			matches++;
		}
	free(copy);
	return matches == 1;
}

static int identity(const char *release, const char *boot, const char *source, const char *image)
{
	struct utsname uts;
	struct stat info;
	char buffer[8192], path[PATH_MAX], target[PATH_MAX];
	if (geteuid() || uname(&uts) || strcmp(uts.release, release) ||
	    strlen(source) != 40 || strlen(image) != 64 ||
	    read_text("/proc/sys/kernel/random/boot_id", buffer, sizeof(buffer))) return -1;
	buffer[strcspn(buffer, "\n")] = 0;
	if (strcmp(buffer, boot) || read_text("/proc/cmdline", buffer, sizeof(buffer)) ||
	    !command_word(buffer, "m4_asahi_rust.source", source) ||
	    !command_word(buffer, "m4_asahi_rust.image", image) ||
	    !command_word(buffer, "m4_asahi_rust", "1") || stat(node, &info) || !S_ISCHR(info.st_mode)) return -1;
	snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/driver", major(info.st_rdev), minor(info.st_rdev));
	ssize_t n = readlink(path, target, sizeof(target) - 1);
	if (n < 0) return -1;
	target[n] = 0;
	const char *owner = strrchr(target, '/');
	if (!owner || (strcmp(owner + 1, "aurora-g16") && strcmp(owner + 1, "asahi-g16")) ||
	    access("/sys/module/gpu_m4_drm", F_OK) == 0 ||
	    access("/sys/module/apple_gpu_m4_power", F_OK) == 0) return -1;
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 10 || strcmp(argv[1], "--execute")) {
		puts("Dry run: 4 mixed E2BIG admission checks, 4097 simultaneous CPU timestamp aliases, then independent-fd VM/timestamp stress; no shaders. Use run-native-ioctl-regression.py --execute on the patched rollback-protected boot.");
		return argc == 1 ? 0 : 2;
	}
	node = argv[2];
	unsigned int threads = strtoul(argv[8], NULL, 10), loops = strtoul(argv[9], NULL, 10);
	if (threads < 2 || threads > 8 || threads % 2 || loops < 1 || loops > 256 ||
	    !strcmp(argv[5], "78d2519c82fca4c0403315e040d595628bc5a4d6") ||
	    !strcmp(argv[5], "df583754ae2b3607af4155ba0442bc93077bdce8") ||
	    identity(argv[3], argv[4], argv[5], argv[6])) {
		fprintf(stderr, "Refusing invalid bounds or unqualified kernel/boot/source/driver identity\n");
		return 2;
	}
	/* argv[7] is the compiled-UAPI identity, independently checked by the wrapper. */
	if (strcmp(argv[7], ASAHI_M4_UAPI_SHA256)) return 2;
	alarm(90);
	uint64_t render_publication_delta = 0;
	if (mixed_rejection(&render_publication_delta) || timestamp_aliases()) return 1;
	pthread_t ids[8];
	struct worker workers[8];
	if (pthread_barrier_init(&start_barrier, NULL, threads)) return 1;
	for (unsigned int i = 0; i < threads; i++) {
		workers[i] = (struct worker){ .index = i, .loops = loops };
		if (pthread_create(&ids[i], NULL, stress_worker, &workers[i])) {
			/* Do not leave successfully created workers at an incomplete barrier. */
			fprintf(stderr, "pthread_create failed\n");
			_exit(1);
		}
	}
	for (unsigned int i = 0; i < threads; i++) pthread_join(ids[i], NULL);
	pthread_barrier_destroy(&start_barrier);
	bool passed = !atomic_load(&failed) && atomic_load(&vm_cycles) == threads / 2 * loops &&
		atomic_load(&timestamp_cycles) == threads / 2 * loops;
	printf("{\"passed\":%s,\"mixed_e2big_rejections\":4,\"same_vm_healthy\":true,\"compute_publication_delta\":0,\"render_publication_delta_observed\":%" PRIu64 ",\"timestamp_sentinel_unchanged\":true,\"simultaneous_timestamp_aliases\":4097,\"vm_create_destroy_cycles\":%u,\"timestamp_bind_unbind_cycles\":%u,\"threads\":%u,\"loops_per_thread\":%u,\"shader_dispatches\":0,\"uapi_sha256\":\"%s\"}\n",
		passed ? "true" : "false", render_publication_delta, atomic_load(&vm_cycles), atomic_load(&timestamp_cycles),
		threads, loops, ASAHI_M4_UAPI_SHA256);
	return passed ? 0 : 1;
}
