// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Queue churn on a G13/G14 (M1/M2) GPU: how much memory the driver keeps
 * for destroyed queues while the GPU never powers off.
 *
 * The driver keeps the firmware queue structures of every destroyed queue
 * until the firmware reports a PowerOff, and at most MAX_POWERED_CONTEXTS
 * (1024) of them. A child process keeps the GPU busy with back-to-back
 * compute dispatches, so no PowerOff comes, while this process creates a
 * Vulkan device, runs one fill on it and destroys it, CONTEXTS times. Each
 * device is a new GPU context. Shmem grows by about 60 KiB per held context:
 * without the bound by about CONTEXTS x 60 KiB, with it by at most the
 * bound's worth. Once the load stops and the GPU powers off, the rest goes.
 *
 * Needs the Vulkan loader and Mesa's Asahi driver. Hardware only, so it is
 * not in the selftests' TARGETS. Run it with nothing else using the GPU.
 *
 * Build: cc -O2 -Wall -o queue-churn queue-churn.c -lvulkan
 * Run:   ./queue-churn [contexts]   (default 6144; exit 0 = PASS, 1 = FAIL)
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define BOUND 1024			/* MAX_POWERED_CONTEXTS in gpu.rs */
#define HELD_KIB 64			/* a little over one held context */
#define LIMIT_KIB (2 * BOUND * HELD_KIB)	/* the bound, with room for noise */

/*
 * A bounded ALU loop, `glslc --target-env=vulkan1.0 -O -mfmt=c` of:
 *   layout(local_size_x = 1) in;
 *   layout(push_constant) uniform Params { uint n; } params;
 *   layout(std430, set = 0, binding = 0) buffer Result { uint value; } result;
 *   void main() { uint acc = 1u;
 *     for (uint i = 0u; i < params.n; i++) acc = acc * 1664525u + 1013904223u;
 *     result.value = acc; }
 */
static const uint32_t spin_spv[] = {
	0x07230203, 0x00010000, 0x000d000b, 0x0000002e, 0x00000000, 0x00020011,
	0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
	0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0005000f, 0x00000005,
	0x00000004, 0x6e69616d, 0x00000000, 0x00060010, 0x00000004, 0x00000011,
	0x00000001, 0x00000001, 0x00000001, 0x00030047, 0x00000012, 0x00000002,
	0x00050048, 0x00000012, 0x00000000, 0x00000023, 0x00000000, 0x00030047,
	0x00000024, 0x00000003, 0x00050048, 0x00000024, 0x00000000, 0x00000023,
	0x00000000, 0x00040047, 0x00000026, 0x00000021, 0x00000000, 0x00040047,
	0x00000026, 0x00000022, 0x00000000, 0x00040047, 0x0000002b, 0x0000000b,
	0x00000019, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
	0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x0004002b, 0x00000006,
	0x00000009, 0x00000001, 0x0004002b, 0x00000006, 0x0000000b, 0x00000000,
	0x0003001e, 0x00000012, 0x00000006, 0x00040020, 0x00000013, 0x00000009,
	0x00000012, 0x0004003b, 0x00000013, 0x00000014, 0x00000009, 0x00040015,
	0x00000015, 0x00000020, 0x00000001, 0x0004002b, 0x00000015, 0x00000016,
	0x00000000, 0x00040020, 0x00000017, 0x00000009, 0x00000006, 0x00020014,
	0x0000001a, 0x0004002b, 0x00000006, 0x0000001d, 0x0019660d, 0x0004002b,
	0x00000006, 0x0000001f, 0x3c6ef35f, 0x0004002b, 0x00000015, 0x00000022,
	0x00000001, 0x0003001e, 0x00000024, 0x00000006, 0x00040020, 0x00000025,
	0x00000002, 0x00000024, 0x0004003b, 0x00000025, 0x00000026, 0x00000002,
	0x00040020, 0x00000028, 0x00000002, 0x00000006, 0x00040017, 0x0000002a,
	0x00000006, 0x00000003, 0x0006002c, 0x0000002a, 0x0000002b, 0x00000009,
	0x00000009, 0x00000009, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
	0x00000003, 0x000200f8, 0x00000005, 0x000200f9, 0x0000000c, 0x000200f8,
	0x0000000c, 0x000700f5, 0x00000006, 0x0000002d, 0x00000009, 0x00000005,
	0x00000020, 0x0000000d, 0x000700f5, 0x00000006, 0x0000002c, 0x0000000b,
	0x00000005, 0x00000023, 0x0000000d, 0x00050041, 0x00000017, 0x00000018,
	0x00000014, 0x00000016, 0x0004003d, 0x00000006, 0x00000019, 0x00000018,
	0x000500b0, 0x0000001a, 0x0000001b, 0x0000002c, 0x00000019, 0x000400f6,
	0x0000000e, 0x0000000d, 0x00000000, 0x000400fa, 0x0000001b, 0x0000000d,
	0x0000000e, 0x000200f8, 0x0000000d, 0x00050084, 0x00000006, 0x0000001e,
	0x0000002d, 0x0000001d, 0x00050080, 0x00000006, 0x00000020, 0x0000001e,
	0x0000001f, 0x00050080, 0x00000006, 0x00000023, 0x0000002c, 0x00000022,
	0x000200f9, 0x0000000c, 0x000200f8, 0x0000000e, 0x00050041, 0x00000028,
	0x00000029, 0x00000026, 0x00000016, 0x0003003e, 0x00000029, 0x0000002d,
	0x000100fd, 0x00010038,
};

static long shmem_kib(void)
{
	char line[256];
	long kib = -1;
	FILE *f = fopen("/proc/meminfo", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "Shmem: %ld kB", &kib) == 1)
			break;
	fclose(f);
	return kib;
}

#define CHECK(expr)                                                      \
	do {                                                             \
		VkResult r_ = (expr);                                    \
		if (r_ != VK_SUCCESS) {                                  \
			fprintf(stderr, "%s failed: %d\n", #expr, r_); \
			return -1;                                       \
		}                                                        \
	} while (0)

static VkInstance instance;
static VkPhysicalDevice physical;

static int instance_init(void)
{
	uint32_t count = 1;
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.apiVersion = VK_API_VERSION_1_1,
	};

	CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
	}, NULL, &instance));
	if (vkEnumeratePhysicalDevices(instance, &count, &physical) < 0 || !count)
		return -1;
	return 0;
}

struct gpu {
	VkDevice device;
	VkQueue queue;
	VkCommandPool pool;
	VkBuffer buffer;
	VkDeviceMemory memory;
};

static int gpu_open(struct gpu *g, VkBufferUsageFlags usage)
{
	float priority = 1.0f;
	VkMemoryRequirements req;
	VkPhysicalDeviceMemoryProperties props;
	uint32_t type = 0;

	CHECK(vkCreateDevice(physical, &(VkDeviceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueCount = 1,
			.pQueuePriorities = &priority,
		},
	}, NULL, &g->device));
	vkGetDeviceQueue(g->device, 0, 0, &g->queue);
	CHECK(vkCreateCommandPool(g->device, &(VkCommandPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	}, NULL, &g->pool));
	CHECK(vkCreateBuffer(g->device, &(VkBufferCreateInfo){
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = 4096,
		.usage = usage,
	}, NULL, &g->buffer));
	vkGetBufferMemoryRequirements(g->device, g->buffer, &req);
	vkGetPhysicalDeviceMemoryProperties(physical, &props);
	while (type < props.memoryTypeCount && !(req.memoryTypeBits & (1u << type)))
		type++;
	CHECK(vkAllocateMemory(g->device, &(VkMemoryAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = type,
	}, NULL, &g->memory));
	CHECK(vkBindBufferMemory(g->device, g->buffer, g->memory, 0));
	return 0;
}

static void gpu_close(struct gpu *g)
{
	vkDeviceWaitIdle(g->device);
	vkDestroyCommandPool(g->device, g->pool, NULL);
	vkFreeMemory(g->device, g->memory, NULL);
	vkDestroyBuffer(g->device, g->buffer, NULL);
	vkDestroyDevice(g->device, NULL);
}

/* Records `record` into a new command buffer, submits it and waits for it. */
static int gpu_run(struct gpu *g, void (*record)(VkCommandBuffer, void *), void *data)
{
	VkCommandBuffer cmd;
	VkFence done;

	CHECK(vkAllocateCommandBuffers(g->device, &(VkCommandBufferAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = g->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	}, &cmd));
	CHECK(vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	}));
	record(cmd, data);
	CHECK(vkEndCommandBuffer(cmd));
	CHECK(vkCreateFence(g->device, &(VkFenceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }, NULL, &done));
	CHECK(vkQueueSubmit(g->queue, 1, &(VkSubmitInfo){
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmd,
	}, done));
	CHECK(vkWaitForFences(g->device, 1, &done, VK_TRUE, 5000000000ull));
	vkDestroyFence(g->device, done, NULL);
	vkFreeCommandBuffers(g->device, g->pool, 1, &cmd);
	return 0;
}

static void record_fill(VkCommandBuffer cmd, void *data)
{
	vkCmdFillBuffer(cmd, *(VkBuffer *)data, 0, 4096, 0x5a5a5a5a);
}

struct spin {
	VkPipeline pipeline;
	VkPipelineLayout layout;
	VkDescriptorSet set;
	uint32_t n;
};

static void record_spin(VkCommandBuffer cmd, void *data)
{
	struct spin *s = data;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->layout, 0, 1, &s->set, 0,
				NULL);
	vkCmdPushConstants(cmd, s->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &s->n);
	vkCmdDispatch(cmd, 1, 1, 1);
}

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* The load: back-to-back dispatches of about 20 ms each, until killed. */
static int load(void)
{
	struct gpu g;
	struct spin s = { .n = 1u << 16 };
	VkDescriptorSetLayout set_layout;
	VkDescriptorPool pool;
	VkShaderModule module;

	if (instance_init() || gpu_open(&g, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
		return 1;
	CHECK(vkCreateDescriptorSetLayout(g.device, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &(VkDescriptorSetLayoutBinding){
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		},
	}, NULL, &set_layout));
	CHECK(vkCreatePipelineLayout(g.device, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &set_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &(VkPushConstantRange){
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 4 },
	}, NULL, &s.layout));
	CHECK(vkCreateShaderModule(g.device, &(VkShaderModuleCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(spin_spv),
		.pCode = spin_spv,
	}, NULL, &module));
	CHECK(vkCreateComputePipelines(g.device, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = module,
			.pName = "main",
		},
		.layout = s.layout,
	}, NULL, &s.pipeline));
	CHECK(vkCreateDescriptorPool(g.device, &(VkDescriptorPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &(VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
	}, NULL, &pool));
	CHECK(vkAllocateDescriptorSets(g.device, &(VkDescriptorSetAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &set_layout,
	}, &s.set));
	vkUpdateDescriptorSets(g.device, 1, &(VkWriteDescriptorSet){
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = s.set,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &(VkDescriptorBufferInfo){ g.buffer, 0, VK_WHOLE_SIZE },
	}, 0, NULL);

	/* Grow the dispatch to about 20 ms, well inside any watchdog. */
	for (;;) {
		double start = now_ms();

		if (gpu_run(&g, record_spin, &s))
			return 1;
		if (now_ms() - start >= 20 || s.n >= (1u << 30))
			break;
		s.n *= 2;
	}
	for (;;)
		if (gpu_run(&g, record_spin, &s))
			return 1;
}

int main(int argc, char **argv)
{
	long contexts = argc > 1 ? atol(argv[1]) : 6 * BOUND;
	long base, held, idle;
	pid_t busy;

	busy = fork();
	if (busy == 0)
		_exit(load());
	sleep(2);
	if (instance_init()) {
		kill(busy, SIGKILL);
		return 2;
	}
	base = shmem_kib();
	printf("Shmem before: %ld KiB; churning %ld contexts with the GPU kept busy\n", base,
	       contexts);
	for (long i = 0; i < contexts; i++) {
		struct gpu g;

		if (gpu_open(&g, VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
		    gpu_run(&g, record_fill, &g.buffer)) {
			fprintf(stderr, "context %ld failed\n", i);
			kill(busy, SIGKILL);
			return 2;
		}
		gpu_close(&g);
		if ((i + 1) % BOUND == 0) {
			printf("  %6ld contexts: Shmem +%ld KiB\n", i + 1, shmem_kib() - base);
			fflush(stdout);
		}
	}
	held = shmem_kib() - base;
	kill(busy, SIGKILL);
	waitpid(busy, NULL, 0);
	sleep(5);
	idle = shmem_kib() - base;

	printf("held while busy: +%ld KiB (%.1f KiB per churned context), after idle: +%ld KiB\n",
	       held, (double)held / contexts, idle);
	if (held > LIMIT_KIB) {
		printf("FAIL: destroyed queues held more than the bound's %d KiB\n", LIMIT_KIB);
		return 1;
	}
	printf("PASS: held at most the bound's worth (%d KiB)\n", LIMIT_KIB);
	return 0;
}
