/* SPDX-License-Identifier: MIT */
/*
 * compute-check: a headless Vulkan compute job that checks its own results.
 *
 *   compute-check SECS
 *
 * Runs the compute-check.comp kernel (65536 invocations, each a 256-step
 * dependent integer chain with a loop, a device load and a device store) on
 * the first Apple GPU, over and over for SECS seconds (1..600), and compares
 * every result word with the same chain computed on the CPU after each
 * submission. One line per check failure; exit status 0 only when every
 * submission completed and every word matched, 1 on a mismatch, 2 on a
 * Vulkan error or timeout.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "compute-check.spv.h"

#define COUNT 65536u
#define ITERATIONS 256u
#define FENCE_TIMEOUT_NS (10ull * 1000000000ull)

#define VK(x)                                                                  \
   do {                                                                        \
      VkResult r_ = (x);                                                       \
      if (r_ != VK_SUCCESS) {                                                  \
         fprintf(stderr, "compute-check: %s failed: %d (line %d)\n", #x, r_,   \
                 __LINE__);                                                    \
         exit(2);                                                              \
      }                                                                        \
   } while (0)

static double
now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec * 1e-9;
}

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

static uint32_t
memory_type(VkPhysicalDevice pdev, uint32_t bits)
{
   VkPhysicalDeviceMemoryProperties props;
   vkGetPhysicalDeviceMemoryProperties(pdev, &props);
   const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) &&
          (props.memoryTypes[i].propertyFlags & want) == want)
         return i;
   }
   fprintf(stderr, "compute-check: no host-visible coherent memory\n");
   exit(2);
}

static void
make_buffer(VkPhysicalDevice pdev, VkDevice dev, VkBuffer *buf,
            VkDeviceMemory *mem, void **map)
{
   const VkBufferCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = COUNT * sizeof(uint32_t),
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VK(vkCreateBuffer(dev, &info, NULL, buf));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(dev, *buf, &req);
   const VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = memory_type(pdev, req.memoryTypeBits),
   };
   VK(vkAllocateMemory(dev, &alloc, NULL, mem));
   VK(vkBindBufferMemory(dev, *buf, *mem, 0));
   VK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, map));
}

int
main(int argc, char **argv)
{
   const double secs = argc == 2 ? atof(argv[1]) : 0;
   if (!(secs >= 1 && secs <= 600)) {
      fprintf(stderr, "usage: %s SECS (1..600)\n", argv[0]);
      return 2;
   }

   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "compute-check",
      .apiVersion = VK_API_VERSION_1_1,
   };
   const VkInstanceCreateInfo inst_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance inst;
   VK(vkCreateInstance(&inst_info, NULL, &inst));

   uint32_t npdev = 0;
   VK(vkEnumeratePhysicalDevices(inst, &npdev, NULL));
   VkPhysicalDevice pdevs[8];
   npdev = npdev > 8 ? 8 : npdev;
   VK(vkEnumeratePhysicalDevices(inst, &npdev, pdevs));
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   VkPhysicalDeviceProperties props;
   for (uint32_t i = 0; i < npdev && !pdev; ++i) {
      vkGetPhysicalDeviceProperties(pdevs[i], &props);
      if (strstr(props.deviceName, "Apple"))
         pdev = pdevs[i];
   }
   if (!pdev) {
      fprintf(stderr, "compute-check: no Apple GPU among %u devices\n", npdev);
      return 2;
   }
   printf("Device: %s (deviceID 0x%x)\n", props.deviceName, props.deviceID);

   uint32_t nq = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, NULL);
   VkQueueFamilyProperties qprops[16];
   nq = nq > 16 ? 16 : nq;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, qprops);
   uint32_t family = UINT32_MAX;
   for (uint32_t i = 0; i < nq && family == UINT32_MAX; ++i) {
      if (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
         family = i;
   }
   if (family == UINT32_MAX) {
      fprintf(stderr, "compute-check: no compute queue\n");
      return 2;
   }

   const float priority = 1.0f;
   const VkDeviceQueueCreateInfo qinfo = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   const VkDeviceCreateInfo dinfo = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qinfo,
   };
   VkDevice dev;
   VK(vkCreateDevice(pdev, &dinfo, NULL, &dev));
   VkQueue queue;
   vkGetDeviceQueue(dev, family, 0, &queue);

   VkBuffer seeds_buf, results_buf;
   VkDeviceMemory seeds_mem, results_mem;
   uint32_t *seeds, *results;
   make_buffer(pdev, dev, &seeds_buf, &seeds_mem, (void **)&seeds);
   make_buffer(pdev, dev, &results_buf, &results_mem, (void **)&results);
   for (uint32_t i = 0; i < COUNT; ++i)
      seeds[i] = i * 0x9e3779b9u + 0x7f4a7c15u;

   uint32_t *expected = malloc(COUNT * sizeof(uint32_t));
   for (uint32_t i = 0; i < COUNT; ++i)
      expected[i] = reference(i, seeds[i]);

   const VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       NULL},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       NULL},
   };
   const VkDescriptorSetLayoutCreateInfo dsl_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = bindings,
   };
   VkDescriptorSetLayout dsl;
   VK(vkCreateDescriptorSetLayout(dev, &dsl_info, NULL, &dsl));
   const VkPipelineLayoutCreateInfo pl_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &dsl,
   };
   VkPipelineLayout layout;
   VK(vkCreatePipelineLayout(dev, &pl_info, NULL, &layout));

   const VkShaderModuleCreateInfo sm_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(compute_check_spv),
      .pCode = compute_check_spv,
   };
   VkShaderModule module;
   VK(vkCreateShaderModule(dev, &sm_info, NULL, &module));
   const VkComputePipelineCreateInfo cp_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = module,
         .pName = "main",
      },
      .layout = layout,
   };
   VkPipeline pipeline;
   VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp_info, NULL,
                               &pipeline));

   const VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
   const VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   VkDescriptorPool pool;
   VK(vkCreateDescriptorPool(dev, &pool_info, NULL, &pool));
   const VkDescriptorSetAllocateInfo set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &dsl,
   };
   VkDescriptorSet set;
   VK(vkAllocateDescriptorSets(dev, &set_info, &set));
   const VkDescriptorBufferInfo buf_info[] = {
      {seeds_buf, 0, VK_WHOLE_SIZE},
      {results_buf, 0, VK_WHOLE_SIZE},
   };
   const VkWriteDescriptorSet writes[] = {
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1,
       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &buf_info[0], NULL},
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1,
       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &buf_info[1], NULL},
   };
   vkUpdateDescriptorSets(dev, 2, writes, 0, NULL);

   const VkCommandPoolCreateInfo cpool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = family,
   };
   VkCommandPool cpool;
   VK(vkCreateCommandPool(dev, &cpool_info, NULL, &cpool));
   const VkCommandBufferAllocateInfo cb_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = cpool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd;
   VK(vkAllocateCommandBuffers(dev, &cb_info, &cmd));
   const VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   VK(vkBeginCommandBuffer(cmd, &begin));
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
                           &set, 0, NULL);
   vkCmdDispatch(cmd, COUNT / 64, 1, 1);
   const VkMemoryBarrier to_host = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, NULL, 0,
                        NULL);
   VK(vkEndCommandBuffer(cmd));

   const VkFenceCreateInfo fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   VkFence fence;
   VK(vkCreateFence(dev, &fence_info, NULL, &fence));
   const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };

   printf("compute-check: %u invocations x %u steps, %.0f s\n", COUNT,
          ITERATIONS, secs);
   fflush(stdout);
   const double t0 = now();
   double last = t0;
   unsigned long runs = 0, bad_runs = 0;
   int status = 0;
   while (now() - t0 < secs) {
      memset(results, 0, COUNT * sizeof(uint32_t));
      VK(vkResetFences(dev, 1, &fence));
      VK(vkQueueSubmit(queue, 1, &submit, fence));
      const VkResult wait = vkWaitForFences(dev, 1, &fence, VK_TRUE,
                                            FENCE_TIMEOUT_NS);
      if (wait != VK_SUCCESS) {
         fprintf(stderr, "compute-check: run %lu did not complete: %d\n",
                 runs, wait);
         status = 2;
         break;
      }
      unsigned mismatches = 0, first = UINT32_MAX;
      for (uint32_t i = 0; i < COUNT; ++i) {
         if (results[i] != expected[i]) {
            if (first == UINT32_MAX)
               first = i;
            mismatches++;
         }
      }
      runs++;
      if (mismatches) {
         bad_runs++;
         if (bad_runs <= 8)
            printf("MISMATCH run %lu: %u of %u words wrong; first at %u: "
                   "got 0x%08x want 0x%08x\n",
                   runs, mismatches, COUNT, first, results[first],
                   expected[first]);
         status = status ? status : 1;
      }
      if (now() - last >= 1.0) {
         last = now();
         printf("progress t=%.1fs runs=%lu bad=%lu\n", last - t0, runs,
                bad_runs);
         fflush(stdout);
      }
   }
   const double dt = now() - t0;
   printf("%s: %lu runs in %.1f s, %lu with wrong results\n",
          status == 0 ? "PASS" : "FAIL", runs, dt, bad_runs);
   vkDeviceWaitIdle(dev);
   vkDestroyFence(dev, fence, NULL);
   vkDestroyCommandPool(dev, cpool, NULL);
   vkDestroyDescriptorPool(dev, pool, NULL);
   vkDestroyPipeline(dev, pipeline, NULL);
   vkDestroyShaderModule(dev, module, NULL);
   vkDestroyPipelineLayout(dev, layout, NULL);
   vkDestroyDescriptorSetLayout(dev, dsl, NULL);
   vkDestroyBuffer(dev, results_buf, NULL);
   vkDestroyBuffer(dev, seeds_buf, NULL);
   vkFreeMemory(dev, results_mem, NULL);
   vkFreeMemory(dev, seeds_mem, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   free(expected);
   return status;
}
