#!/bin/bash
# air-gpu-job.sh: run ONE short headless GPU compute job through a given Mesa prefix, and record
# its exit code and timing.
#
#   air-gpu-job.sh [--seconds N] <mesa-prefix>       N: 1 to 8 seconds of submissions (default 5)
#
# The job is a Vulkan compute shader (a 32-bit LCG per invocation, 65536 invocations per
# dispatch), submitted once every 10 ms until N seconds have passed. Each submission is waited
# for with a 2 s fence timeout, and a sample of every result is checked on the CPU. The work per
# dispatch is tiny, so the GPU is idle most of the time: this is a dispatch test, not a load.
# It uses only the prefix's Vulkan driver, opens no window and needs no desktop.
#
# The whole run, start-up included, is held to 10 s: past that the job is killed, and the script
# returns even if the kernel holds the process. Run it as root (sudo) or as the desktop user.
#
# The record goes to ~/air-gpu-runs/job-<time>.txt (the invoking user's home under sudo), with
# this boot's id, so air-gpu-collect.sh can find it. The record and a "start" line in the
# journal are written and synced before the job starts, so a hang still leaves them on disk.
#
# The record is written durably (fsync of the file and its directory) at the start and at the end;
# the result is also logged to the journal (tag air-gpu-job), naming the record.
#
# Exit status: 0 when every submission completed with correct results, otherwise non-zero
# (2 not run, 3 first submission never completed, 4 wrong results, 5 stalled later, 6 device
# lost, 7 killed at the time limit, 8 stuck in the kernel, 9 the result could not be saved to the
# record, 1 anything else).
set -euo pipefail

# ---- the Mesa prefix environment (keep in step with the prefix package) ----------------------
# The Vulkan loader sees only the prefix's driver; implicit layers stay off. The prefix is
# mesa-m3's (/opt/mesa-m3, from 12.4), which opens a G15G with ASAHI_M3_EXPERIMENTAL=1.
# ASAHI_M3_G15G=1 is what the earlier mesa-m3-g15g prefix also needed; mesa-m3 ignores it, and
# on any other chip it changes nothing. G15G shader binaries depend on the stall count, so no
# cached binary is reused.
mesa_env() {
  local p=$1 icd=$2
  printf '%s\n' \
    "LD_LIBRARY_PATH=$p/lib" \
    "VK_DRIVER_FILES=$icd" \
    "VK_ICD_FILENAMES=$icd" \
    "VK_LOADER_LAYERS_DISABLE=~implicit~" \
    "ASAHI_M3_EXPERIMENTAL=1" \
    "ASAHI_M3_G15G=1" \
    "AGX_MESA_DEBUG=nopromote" \
    "MESA_SHADER_CACHE_DISABLE=true"
}
# The render node must belong to this driver, and the Vulkan device's name must contain this.
RENDER_DRIVER=${AIR_GPU_JOB_RENDER_DRIVER:-asahi}
DEVICE_MATCH=${AIR_GPU_JOB_DEVICE_MATCH:-Apple}
# ----------------------------------------------------------------------------------------------

PROG=air-gpu-job
SYS=/sys
CMDLINE=/proc/cmdline
BOOT_ID=/proc/sys/kernel/random/boot_id
LIMIT_S=10
MAX_SECONDS=8
TAG=air_gpu.oneshot

# The record goes to this user's home (the invoking user's under sudo).
user_home() {
  local h
  h=$(getent passwd "$1" | cut -d: -f6)
  echo "${h:-$HOME}"
}
say() { printf '%s: %s\n' "$PROG" "$*"; }
log() { logger -t "$PROG" -- "$*" 2>/dev/null || true; }
now_ms() { date +%s%3N; }

usage() { sed -n '2,6p' "$0"; exit 2; }

# The job itself: Python with ctypes against the system Vulkan loader, so the Air needs no
# compiler. The shader is lcg.comp below, built with: glslc -O --target-env=vulkan1.0
# -fshader-stage=compute lcg.comp (the SPIR-V is embedded as base64).
#
#   #version 450
#   layout(local_size_x = 64) in;
#   layout(std430, binding = 0) buffer Out { uint value[]; };
#   layout(push_constant) uniform Push { uint seed; uint steps; } pc;
#   void main() {
#     uint i = gl_GlobalInvocationID.x;
#     uint x = i ^ pc.seed;
#     for (uint k = 0u; k < pc.steps; k++) { x = x * 1664525u + 1013904223u; }
#     value[i] = x;
#   }
read -r -d '' JOB_PY <<'PY' || true
import base64, ctypes as C, os, sys, time

SPIRV = base64.b64decode(
    "AwIjBwAAAQALAA0AOwAAAAAAAAARAAIAAQAAAAsABgABAAAAR0xTTC5zdGQuNDUwAAAAAA4AAwAAAAAAAQAAAA8ABgAF"
    "AAAABAAAAG1haW4AAAAACwAAABAABgAEAAAAEQAAAEAAAAABAAAAAQAAAEcABAALAAAACwAAABwAAABHAAMAEgAAAAIA"
    "AABIAAUAEgAAAAAAAAAjAAAAAAAAAEgABQASAAAAAQAAACMAAAAEAAAARwAEAC4AAAAGAAAABAAAAEcAAwAvAAAAAwAA"
    "AEgABQAvAAAAAAAAACMAAAAAAAAARwAEADEAAAAhAAAAAAAAAEcABAAxAAAAIgAAAAAAAABHAAQAOAAAAAsAAAAZAAAA"
    "EwACAAIAAAAhAAMAAwAAAAIAAAAVAAQABgAAACAAAAAAAAAAFwAEAAkAAAAGAAAAAwAAACAABAAKAAAAAQAAAAkAAAA7"
    "AAQACgAAAAsAAAABAAAAKwAEAAYAAAAMAAAAAAAAACAABAANAAAAAQAAAAYAAAAeAAQAEgAAAAYAAAAGAAAAIAAEABMA"
    "AAAJAAAAEgAAADsABAATAAAAFAAAAAkAAAAVAAQAFQAAACAAAAABAAAAKwAEABUAAAAWAAAAAAAAACAABAAXAAAACQAA"
    "AAYAAAArAAQAFQAAACIAAAABAAAAFAACACUAAAArAAQABgAAACgAAAANZhkAKwAEAAYAAAAqAAAAX/NuPB0AAwAuAAAA"
    "BgAAAB4AAwAvAAAALgAAACAABAAwAAAAAgAAAC8AAAA7AAQAMAAAADEAAAACAAAAIAAEADQAAAACAAAABgAAACsABAAG"
    "AAAANgAAAEAAAAArAAQABgAAADcAAAABAAAALAAGAAkAAAA4AAAANgAAADcAAAA3AAAANgAFAAIAAAAEAAAAAAAAAAMA"
    "AAD4AAIABQAAAEEABQANAAAADgAAAAsAAAAMAAAAPQAEAAYAAAAPAAAADgAAAEEABQAXAAAAGAAAABQAAAAWAAAAPQAE"
    "AAYAAAAZAAAAGAAAAMYABQAGAAAAGgAAAA8AAAAZAAAA+QACABwAAAD4AAIAHAAAAPUABwAGAAAAOgAAABoAAAAFAAAA"
    "KwAAAB0AAAD1AAcABgAAADkAAAAMAAAABQAAAC0AAAAdAAAAQQAFABcAAAAjAAAAFAAAACIAAAA9AAQABgAAACQAAAAj"
    "AAAAsAAFACUAAAAmAAAAOQAAACQAAAD2AAQAHgAAAB0AAAAAAAAA+gAEACYAAAAdAAAAHgAAAPgAAgAdAAAAhAAFAAYA"
    "AAApAAAAOgAAACgAAACAAAUABgAAACsAAAApAAAAKgAAAIAABQAGAAAALQAAADkAAAAiAAAA+QACABwAAAD4AAIAHgAA"
    "AEEABgA0AAAANQAAADEAAAAWAAAADwAAAD4AAwA1AAAAOgAAAP0AAQA4AAEA")
N = 65536            # invocations per dispatch (1024 groups of 64)
STEPS = 256          # LCG steps per invocation
FENCE_NS = 2_000_000_000
# The seconds of submissions; the time left of the wrapper's budget, which no step outlives;
# the device name to look for; the pause between submissions.
seconds, budget, match, interval = float(sys.argv[1]), float(sys.argv[2]), sys.argv[3], float(sys.argv[4])
t_start = time.monotonic()
deadline = t_start + budget


def out(k, v):
    print(f"{k}={v}", flush=True)


def finish(code, result):
    out("result", result)
    out("elapsed_ms", int((time.monotonic() - t_start) * 1000))
    sys.stdout.flush()
    os._exit(code)  # no teardown: after a GPU fault it can block


vk = C.CDLL("libvulkan.so.1")
P, U32, U64, I32 = C.c_void_p, C.c_uint32, C.c_uint64, C.c_int32


def fn(name, *args, res=I32):
    f = getattr(vk, name)
    f.argtypes, f.restype = list(args), res
    return f


def S(*fields):
    return type("S", (C.Structure,), {"_fields_": list(fields)})


AppInfo = S(("sType", U32), ("pNext", P), ("pApplicationName", C.c_char_p), ("applicationVersion", U32),
            ("pEngineName", C.c_char_p), ("engineVersion", U32), ("apiVersion", U32))
InstanceCI = S(("sType", U32), ("pNext", P), ("flags", U32), ("pApplicationInfo", C.POINTER(AppInfo)),
               ("enabledLayerCount", U32), ("ppEnabledLayerNames", P), ("enabledExtensionCount", U32),
               ("ppEnabledExtensionNames", P))
QueueFamily = S(("queueFlags", U32), ("queueCount", U32), ("timestampValidBits", U32), ("gran", U32 * 3))
QueueCI = S(("sType", U32), ("pNext", P), ("flags", U32), ("queueFamilyIndex", U32), ("queueCount", U32),
            ("pQueuePriorities", C.POINTER(C.c_float)))
DeviceCI = S(("sType", U32), ("pNext", P), ("flags", U32), ("queueCreateInfoCount", U32),
             ("pQueueCreateInfos", C.POINTER(QueueCI)), ("enabledLayerCount", U32), ("ppEnabledLayerNames", P),
             ("enabledExtensionCount", U32), ("ppEnabledExtensionNames", P), ("pEnabledFeatures", P))
BufferCI = S(("sType", U32), ("pNext", P), ("flags", U32), ("size", U64), ("usage", U32), ("sharingMode", U32),
             ("queueFamilyIndexCount", U32), ("pQueueFamilyIndices", P))
MemReq = S(("size", U64), ("alignment", U64), ("memoryTypeBits", U32))
MemType = S(("propertyFlags", U32), ("heapIndex", U32))
MemHeap = S(("size", U64), ("flags", U32))
MemProps = S(("memoryTypeCount", U32), ("memoryTypes", MemType * 32), ("memoryHeapCount", U32),
             ("memoryHeaps", MemHeap * 16))
MemAI = S(("sType", U32), ("pNext", P), ("allocationSize", U64), ("memoryTypeIndex", U32))
ShaderCI = S(("sType", U32), ("pNext", P), ("flags", U32), ("codeSize", C.c_size_t), ("pCode", P))
Binding = S(("binding", U32), ("descriptorType", U32), ("descriptorCount", U32), ("stageFlags", U32),
            ("pImmutableSamplers", P))
SetLayoutCI = S(("sType", U32), ("pNext", P), ("flags", U32), ("bindingCount", U32), ("pBindings", C.POINTER(Binding)))
PushRange = S(("stageFlags", U32), ("offset", U32), ("size", U32))
LayoutCI = S(("sType", U32), ("pNext", P), ("flags", U32), ("setLayoutCount", U32), ("pSetLayouts", C.POINTER(U64)),
             ("pushConstantRangeCount", U32), ("pPushConstantRanges", C.POINTER(PushRange)))
StageCI = S(("sType", U32), ("pNext", P), ("flags", U32), ("stage", U32), ("module", U64), ("pName", C.c_char_p),
            ("pSpecializationInfo", P))
ComputeCI = S(("sType", U32), ("pNext", P), ("flags", U32), ("stage", StageCI), ("layout", U64),
              ("basePipelineHandle", U64), ("basePipelineIndex", I32))
PoolSize = S(("type", U32), ("descriptorCount", U32))
PoolCI = S(("sType", U32), ("pNext", P), ("flags", U32), ("maxSets", U32), ("poolSizeCount", U32),
           ("pPoolSizes", C.POINTER(PoolSize)))
SetAI = S(("sType", U32), ("pNext", P), ("descriptorPool", U64), ("descriptorSetCount", U32),
          ("pSetLayouts", C.POINTER(U64)))
BufInfo = S(("buffer", U64), ("offset", U64), ("range", U64))
Write = S(("sType", U32), ("pNext", P), ("dstSet", U64), ("dstBinding", U32), ("dstArrayElement", U32),
          ("descriptorCount", U32), ("descriptorType", U32), ("pImageInfo", P), ("pBufferInfo", C.POINTER(BufInfo)),
          ("pTexelBufferView", P))
CmdPoolCI = S(("sType", U32), ("pNext", P), ("flags", U32), ("queueFamilyIndex", U32))
CmdAI = S(("sType", U32), ("pNext", P), ("commandPool", U64), ("level", U32), ("commandBufferCount", U32))
BeginI = S(("sType", U32), ("pNext", P), ("flags", U32), ("pInheritanceInfo", P))
MemBarrier = S(("sType", U32), ("pNext", P), ("srcAccessMask", U32), ("dstAccessMask", U32))
SubmitI = S(("sType", U32), ("pNext", P), ("waitSemaphoreCount", U32), ("pWaitSemaphores", P),
            ("pWaitDstStageMask", P), ("commandBufferCount", U32), ("pCommandBuffers", C.POINTER(P)),
            ("signalSemaphoreCount", U32), ("pSignalSemaphores", P))
FenceCI = S(("sType", U32), ("pNext", P), ("flags", U32))

PP, PU32, PU64 = C.POINTER(P), C.POINTER(U32), C.POINTER(U64)
vkCreateInstance = fn("vkCreateInstance", C.POINTER(InstanceCI), P, PP)
vkEnumeratePhysicalDevices = fn("vkEnumeratePhysicalDevices", P, PU32, PP)
vkGetPhysicalDeviceProperties = fn("vkGetPhysicalDeviceProperties", P, P, res=None)
vkGetPhysicalDeviceQueueFamilyProperties = fn("vkGetPhysicalDeviceQueueFamilyProperties", P, PU32, P, res=None)
vkGetPhysicalDeviceMemoryProperties = fn("vkGetPhysicalDeviceMemoryProperties", P, C.POINTER(MemProps), res=None)
vkCreateDevice = fn("vkCreateDevice", P, C.POINTER(DeviceCI), P, PP)
vkGetDeviceQueue = fn("vkGetDeviceQueue", P, U32, U32, PP, res=None)
vkCreateBuffer = fn("vkCreateBuffer", P, C.POINTER(BufferCI), P, PU64)
vkGetBufferMemoryRequirements = fn("vkGetBufferMemoryRequirements", P, U64, C.POINTER(MemReq), res=None)
vkAllocateMemory = fn("vkAllocateMemory", P, C.POINTER(MemAI), P, PU64)
vkBindBufferMemory = fn("vkBindBufferMemory", P, U64, U64, U64)
vkMapMemory = fn("vkMapMemory", P, U64, U64, U64, U32, PP)
vkCreateShaderModule = fn("vkCreateShaderModule", P, C.POINTER(ShaderCI), P, PU64)
vkCreateDescriptorSetLayout = fn("vkCreateDescriptorSetLayout", P, C.POINTER(SetLayoutCI), P, PU64)
vkCreatePipelineLayout = fn("vkCreatePipelineLayout", P, C.POINTER(LayoutCI), P, PU64)
vkCreateComputePipelines = fn("vkCreateComputePipelines", P, U64, U32, C.POINTER(ComputeCI), P, PU64)
vkCreateDescriptorPool = fn("vkCreateDescriptorPool", P, C.POINTER(PoolCI), P, PU64)
vkAllocateDescriptorSets = fn("vkAllocateDescriptorSets", P, C.POINTER(SetAI), PU64)
vkUpdateDescriptorSets = fn("vkUpdateDescriptorSets", P, U32, C.POINTER(Write), U32, P, res=None)
vkCreateCommandPool = fn("vkCreateCommandPool", P, C.POINTER(CmdPoolCI), P, PU64)
vkAllocateCommandBuffers = fn("vkAllocateCommandBuffers", P, C.POINTER(CmdAI), PP)
vkBeginCommandBuffer = fn("vkBeginCommandBuffer", P, C.POINTER(BeginI))
vkEndCommandBuffer = fn("vkEndCommandBuffer", P)
vkResetCommandBuffer = fn("vkResetCommandBuffer", P, U32)
vkCmdBindPipeline = fn("vkCmdBindPipeline", P, U32, U64, res=None)
vkCmdBindDescriptorSets = fn("vkCmdBindDescriptorSets", P, U32, U64, U32, U32, PU64, U32, P, res=None)
vkCmdPushConstants = fn("vkCmdPushConstants", P, U64, U32, U32, U32, P, res=None)
vkCmdDispatch = fn("vkCmdDispatch", P, U32, U32, U32, res=None)
vkCmdPipelineBarrier = fn("vkCmdPipelineBarrier", P, U32, U32, U32, U32, C.POINTER(MemBarrier), U32, P, U32, P,
                          res=None)
vkCreateFence = fn("vkCreateFence", P, C.POINTER(FenceCI), P, PU64)
vkResetFences = fn("vkResetFences", P, U32, PU64)
vkWaitForFences = fn("vkWaitForFences", P, U32, PU64, U32, U64)
vkQueueSubmit = fn("vkQueueSubmit", P, U32, C.POINTER(SubmitI), U64)


def check(r, what, code=2, result="setup-failed"):
    if r != 0:
        out("error", f"{what} returned {r}")
        finish(6 if r == -4 else code, "device-lost" if r == -4 else result)


inst = P()
app = AppInfo(0, None, b"air-gpu-job", 1, b"none", 0, 1 << 22)
check(vkCreateInstance(C.byref(InstanceCI(1, None, 0, C.pointer(app), 0, None, 0, None)), None, C.byref(inst)),
      "vkCreateInstance", result="no-device")
n = U32(0)
check(vkEnumeratePhysicalDevices(inst, C.byref(n), None), "vkEnumeratePhysicalDevices", result="no-device")
devs = (P * max(n.value, 1))()
if n.value:
    check(vkEnumeratePhysicalDevices(inst, C.byref(n), devs), "vkEnumeratePhysicalDevices", result="no-device")
phys, family, name = None, None, None
for i in range(n.value):
    props = C.create_string_buffer(4096)
    vkGetPhysicalDeviceProperties(devs[i], props)
    dname = props.raw[20:276].split(b"\0")[0].decode(errors="replace")
    out("seen_device", dname)
    if match not in dname:
        continue
    qn = U32(0)
    vkGetPhysicalDeviceQueueFamilyProperties(devs[i], C.byref(qn), None)
    qf = (QueueFamily * qn.value)()
    vkGetPhysicalDeviceQueueFamilyProperties(devs[i], C.byref(qn), qf)
    for j in range(qn.value):
        if qf[j].queueFlags & 0x2:
            phys, family, name = devs[i], j, dname
            break
    if phys:
        break
if phys is None:
    out("error", f"no Vulkan device named like '{match}' with a compute queue ({n.value} seen)")
    finish(2, "no-device")
out("device", name)

prio = C.c_float(1.0)
qci = QueueCI(2, None, 0, family, 1, C.pointer(prio))
dev = P()
# vkCreateDevice opens a GPU context through the firmware; if that hangs, the wrapper records how
# far we got. A hang here is a GPU-side hang, never a clean FW-RUNNING (collect reads this).
out("opened_device", "attempting")
check(vkCreateDevice(phys, C.byref(DeviceCI(3, None, 0, 1, C.pointer(qci), 0, None, 0, None, None)), None,
                     C.byref(dev)), "vkCreateDevice")
queue = P()
vkGetDeviceQueue(dev, family, 0, C.byref(queue))
out("opened_device", "yes")

size = N * 4
buf = U64()
check(vkCreateBuffer(dev, C.byref(BufferCI(12, None, 0, size, 0x20, 0, 0, None)), None, C.byref(buf)), "vkCreateBuffer")
req = MemReq()
vkGetBufferMemoryRequirements(dev, buf, C.byref(req))
mp = MemProps()
vkGetPhysicalDeviceMemoryProperties(phys, C.byref(mp))
mtype = next((t for t in range(mp.memoryTypeCount)
              if req.memoryTypeBits & (1 << t) and mp.memoryTypes[t].propertyFlags & 0x6 == 0x6), None)
if mtype is None:
    out("error", "no host-visible, host-coherent memory for the buffer")
    finish(2, "setup-failed")
mem = U64()
check(vkAllocateMemory(dev, C.byref(MemAI(5, None, req.size, mtype)), None, C.byref(mem)), "vkAllocateMemory")
check(vkBindBufferMemory(dev, buf, mem, 0), "vkBindBufferMemory")
ptr = P()
check(vkMapMemory(dev, mem, 0, size, 0, C.byref(ptr)), "vkMapMemory")
data = (U32 * N).from_address(ptr.value)

code = C.create_string_buffer(SPIRV, len(SPIRV))
module = U64()
check(vkCreateShaderModule(dev, C.byref(ShaderCI(16, None, 0, len(SPIRV), C.cast(code, P))), None, C.byref(module)),
      "vkCreateShaderModule")
binding = Binding(0, 7, 1, 0x20, None)
set_layout = U64()
check(vkCreateDescriptorSetLayout(dev, C.byref(SetLayoutCI(32, None, 0, 1, C.pointer(binding))), None,
                                  C.byref(set_layout)), "vkCreateDescriptorSetLayout")
push = PushRange(0x20, 0, 8)
layout = U64()
check(vkCreatePipelineLayout(dev, C.byref(LayoutCI(30, None, 0, 1, C.pointer(set_layout), 1, C.pointer(push))), None,
                             C.byref(layout)), "vkCreatePipelineLayout")
pipeline = U64()
stage = StageCI(18, None, 0, 0x20, module, b"main", None)
check(vkCreateComputePipelines(dev, 0, 1, C.byref(ComputeCI(29, None, 0, stage, layout, 0, -1)), None,
                               C.byref(pipeline)), "vkCreateComputePipelines")
psize = PoolSize(7, 1)
pool = U64()
check(vkCreateDescriptorPool(dev, C.byref(PoolCI(33, None, 0, 1, 1, C.pointer(psize))), None, C.byref(pool)),
      "vkCreateDescriptorPool")
dset = U64()
check(vkAllocateDescriptorSets(dev, C.byref(SetAI(34, None, pool, 1, C.pointer(set_layout))), C.byref(dset)),
      "vkAllocateDescriptorSets")
binfo = BufInfo(buf, 0, size)
vkUpdateDescriptorSets(dev, 1, C.byref(Write(35, None, dset, 0, 0, 1, 7, None, C.pointer(binfo), None)), 0, None)
cpool = U64()
check(vkCreateCommandPool(dev, C.byref(CmdPoolCI(39, None, 0x2, family)), None, C.byref(cpool)), "vkCreateCommandPool")
cb = P()
check(vkAllocateCommandBuffers(dev, C.byref(CmdAI(40, None, cpool, 0, 1)), C.byref(cb)), "vkAllocateCommandBuffers")
fence = U64()
check(vkCreateFence(dev, C.byref(FenceCI(8, None, 0)), None, C.byref(fence)), "vkCreateFence")
barrier = MemBarrier(46, None, 0x40, 0x2000)  # shader write -> host read
pc = (U32 * 2)()


def lcg(i, seed):
    x = (i ^ seed) & 0xFFFFFFFF
    for _ in range(STEPS):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
    return x


SAMPLE = [0, 1, 63, 64, 1023, 4096, 32767, N - 1]
out("setup_ms", int((time.monotonic() - t_start) * 1000))
out("reached_gpu", "yes")
submits = 0
loop_start = time.monotonic()
end = min(loop_start + seconds, deadline)
while True:
    seed = (0x9E3779B9 * (submits + 1)) & 0xFFFFFFFF
    pc[0], pc[1] = seed, STEPS
    check(vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer", 1, "error")
    check(vkBeginCommandBuffer(cb, C.byref(BeginI(42, None, 0x1, None))), "vkBeginCommandBuffer", 1, "error")
    vkCmdBindPipeline(cb, 1, pipeline)
    vkCmdBindDescriptorSets(cb, 1, layout, 0, 1, C.byref(dset), 0, None)
    vkCmdPushConstants(cb, layout, 0x20, 0, 8, pc)
    vkCmdDispatch(cb, N // 64, 1, 1)
    vkCmdPipelineBarrier(cb, 0x800, 0x4000, 0, 1, C.byref(barrier), 0, None, 0, None)
    check(vkEndCommandBuffer(cb), "vkEndCommandBuffer", 1, "error")
    for k in SAMPLE:
        data[k] = 0xDEADBEEF
    cbs = (P * 1)(cb)
    t0 = time.monotonic()
    check(vkQueueSubmit(queue, 1, C.byref(SubmitI(4, None, 0, None, None, 1, cbs, 0, None)), fence),
          "vkQueueSubmit", 1, "error")
    r = vkWaitForFences(dev, 1, C.byref(fence), 1, FENCE_NS)
    wait_ms = (time.monotonic() - t0) * 1000
    if r == 2:  # VK_TIMEOUT
        out("submits", submits)
        out("error", f"submission {submits + 1} did not complete within {FENCE_NS // 1_000_000} ms")
        finish(3 if submits == 0 else 5, "fence-timeout" if submits == 0 else "stalled")
    check(r, "vkWaitForFences", 1, "error")
    submits += 1
    if submits == 1:
        out("first_submit_ms", f"{wait_ms:.2f}")
    bad = [k for k in SAMPLE if data[k] != lcg(k, seed)]
    if bad:
        out("submits", submits)
        out("error", f"submission {submits}: wrong values at {bad[:4]} (got {data[bad[0]]:#x}, want {lcg(bad[0], seed):#x})")
        finish(4, "wrong-result")
    check(vkResetFences(dev, 1, C.byref(fence)), "vkResetFences", 1, "error")
    if time.monotonic() + interval >= end:
        break
    time.sleep(interval)
out("submits", submits)
out("verified", submits)
out("loop_ms", int((time.monotonic() - loop_start) * 1000))
finish(0, "pass")
PY

# Run as the invoking user when we are root under sudo; otherwise run directly. User-side files
# (the record, its directory) are written this way, never as root, so a symlink a local user
# planted in their home can't redirect a root write or a chown.
runu() {
  if [[ $(id -u) == 0 && -n ${SUDO_USER:-} && $SUDO_USER != root ]]; then
    runuser -u "$SUDO_USER" -- "$@"
  else
    "$@"
  fi
}

main() {
  local seconds=5 prefix="" a icd icds start deadline out rec recdir home user boot oneshot pid rc result
  local reached elapsed submits first device err budget work stage
  start=$(now_ms)
  while (($#)); do
    case $1 in
      --seconds) seconds=${2:-}; shift ;;
      -h | --help) usage ;;
      -*) say "refused: unknown option $1."; exit 2 ;;
      *) [[ -z $prefix ]] || { say "refused: one Mesa prefix only."; exit 2; }; prefix=$1 ;;
    esac
    shift
  done
  [[ -n $prefix ]] || usage
  # One digit, so no leading zero reaches $(( )) as an octal number.
  if ! [[ $seconds =~ ^[1-9]$ ]] || ((seconds > MAX_SECONDS)); then
    say "refused: --seconds takes 1 to $MAX_SECONDS (the whole run is held to $LIMIT_S s)."
    exit 2
  fi
  # A control character (a newline, say) in the path could forge a key=value line in the record.
  if [[ $prefix == *[[:cntrl:]]* ]]; then say "refused: the Mesa prefix path has a control character."; exit 2; fi
  prefix=$(realpath -e -- "$prefix" 2>/dev/null) || { say "refused: no Mesa prefix at $prefix."; exit 2; }
  if [[ $prefix == *[[:cntrl:]]* ]]; then say "refused: the Mesa prefix path has a control character."; exit 2; fi
  icds=("$prefix"/share/vulkan/icd.d/*.json)
  [[ ${#icds[@]} == 1 && -f ${icds[0]} ]] ||
    { say "refused: $prefix/share/vulkan/icd.d must hold exactly one Vulkan driver file."; exit 2; }
  icd=${icds[0]}
  command -v python3 >/dev/null || { say "refused: python3 is not installed (pacman -S python)."; exit 2; }

  # The record, in the user's home: under sudo, the invoking user's.
  user=${SUDO_USER:-$(id -un)}
  home=$(user_home "$user")
  recdir=$home/air-gpu-runs
  # Created as the user, so a symlink there can only point where the user may already write.
  runu mkdir -p "$recdir" 2>/dev/null ||
    { say "refused: could not create $recdir as $user (is it a symlink?)."; exit 2; }
  rec=$recdir/job-$(date +%Y%m%d-%H%M%S-%N).txt
  boot=$(tr -d '-' <"$BOOT_ID")
  oneshot=$(tr ' ' '\n' <"$CMDLINE" | sed -n "s/^${TAG//./\\.}=//p" | head -1)
  work=$(mktemp -d)
  trap 'rm -rf "${work:-}"' EXIT
  out=$work/job.out
  # Writes the record (key=value lines on stdin) as the user and makes it durable before it
  # returns: a temp file next to the record, fsync it, rename it over the record, fsync the
  # directory. A hard reset right after (a GPU hang, a power cycle) still finds this version on
  # disk. Returns non-zero on any failure, with the reason in $work/rec.err; callers check it.
  write_record() {
    local pub
    cat >"$work/rec" || return 1
    # The user can't read the root-only work dir, so hand the content over through a
    # world-readable temp (the record has no secrets).
    pub=$(mktemp --tmpdir "air-gpu-job.XXXXXX") || return 1
    if ! { cp -f "$work/rec" "$pub" && chmod 0644 "$pub"; }; then rm -f "$pub"; return 1; fi
    # shellcheck disable=SC2016 # $1..$3 are expanded by that sh
    if ! runu sh -c 'cp -f -- "$1" "$2.tmp" && sync -- "$2.tmp" && mv -f -- "$2.tmp" "$2" && sync -- "$3"' \
      _ "$pub" "$rec" "$recdir" 2>"$work/rec.err"; then
      rm -f "$pub"
      return 1
    fi
    rm -f "$pub"
  }
  record_error() { # what failed
    local why
    why=$(head -c 300 "$work/rec.err" 2>/dev/null | tr '\n' ' ')
    say "error: $1: could not save the record $rec${why:+ ($why)}"
    logger -p user.err -t "$PROG" -- "error: $1: could not save the record $rec${why:+ ($why)}" 2>/dev/null || true
  }
  header() {
    printf '%s\n' "boot_id=$boot" "oneshot=${oneshot:-none}" "kernel=$(uname -r)" "started=$(date -Is)" \
      "prefix=$prefix" "icd=$icd" "seconds=$seconds" "cmdline=$(cat "$CMDLINE")"
  }

  if ! render_node_ok; then
    log "result: no-render-node (exit 2, 0 ms, 0 submissions) stage none record $rec"
    if ! { header; printf '%s\n' "reached_gpu=no" "result=no-render-node" "stage=none" "exit=2" \
      "elapsed_ms=$(($(now_ms) - start))"; } | write_record; then
      record_error "no render node"
    fi
    say "not run: no $RENDER_DRIVER render node on this boot (arm with m3_expose=1). Record: $rec"
    exit 2
  fi

  # No durable record, no job: collect could not judge it.
  if ! { header; echo "result=running"; } | write_record; then
    record_error "before the job"
    say "refused: the GPU job was not started because its record could not be saved."
    exit 2
  fi
  log "start: $seconds s through $prefix, oneshot ${oneshot:-none}, record $rec"
  # Everything logged so far onto the disk before the GPU is touched.
  if [[ $(id -u) == 0 ]]; then journalctl --sync 2>/dev/null || true; fi
  sync

  # The whole run ends within LIMIT_S: TERM at LIMIT_S - 2 s, KILL 1 s later, the verdict 1 s
  # after that. The job stops submitting 1 s before the TERM, leaving time to report.
  deadline=$((start + LIMIT_S * 1000 - 2000))
  budget=$((deadline - $(now_ms) - 1000))
  ((budget > 0)) || budget=0
  (
    while read -r a; do export "${a?}"; done < <(mesa_env "$prefix" "$icd")
    exec python3 -I -c "$JOB_PY" "$seconds" "$((budget / 1000)).$(printf '%03d' $((budget % 1000)))" \
      "$DEVICE_MATCH" 0.01
  ) >"$out" 2>&1 &
  pid=$!
  while kill -0 "$pid" 2>/dev/null; do
    if (($(now_ms) >= deadline)); then break; fi
    sleep 0.1
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid" 2>/dev/null || true
    sleep 1
    kill -KILL "$pid" 2>/dev/null || true
    sleep 1
    if kill -0 "$pid" 2>/dev/null; then result=stuck rc=8; else result=killed rc=7; fi
  else
    wait "$pid" && rc=0 || rc=$?
    result=$(sed -n 's/^result=//p' "$out" | tail -1)
    result=${result:-error}
  fi
  elapsed=$(($(now_ms) - start))
  reached=$(sed -n 's/^reached_gpu=//p' "$out" | tail -1)
  submits=$(sed -n 's/^submits=//p' "$out" | tail -1)
  first=$(sed -n 's/^first_submit_ms=//p' "$out" | tail -1)
  device=$(sed -n 's/^device=//p' "$out" | tail -1)
  err=$(sed -n 's/^error=//p' "$out" | tail -1)
  # How far the job got, for collect: a job that started but did not finish is never a clean
  # FW-RUNNING or JOB-COMPLETED.
  stage=none
  grep -q '^opened_device=' "$out" && stage="opening-device"
  grep -q '^opened_device=yes' "$out" && stage="opened-device"
  grep -q '^reached_gpu=yes' "$out" && stage="reached-gpu"
  grep -q '^first_submit_ms=' "$out" && stage="submitted"
  grep -q '^loop_ms=' "$out" && stage="finished"
  # The result goes to the journal first (a second, independent source collect reads: it names the
  # record), then to the record, durably. A failed record write is an error, never silent.
  log "result: $result (exit $rc, $elapsed ms, ${submits:-0} submissions) stage $stage record $rec"
  if ! {
    header
    printf '%s\n' "reached_gpu=${reached:-no}" "result=$result" "stage=$stage" "exit=$rc" \
      "elapsed_ms=$elapsed" "submits=${submits:-0}" "first_submit_ms=${first:--}" \
      "device=${device:--}" "error=${err:-}"
    echo "--- job output"
    head -c 16384 "$out"
  } | write_record; then
    record_error "after the job ($result)"
    say "$result (exit $rc), but the record was not saved; the journal line above has the result."
    exit 9
  fi
  rm -f "$out"
  say "$result (exit $rc, $((elapsed / 1000)).$(((elapsed % 1000) / 100)) s, ${submits:-0} submissions, first in ${first:--} ms, device ${device:--})${err:+: $err}. Record: $rec"
  exit "$rc"
}

# A render node of the expected driver exists on this boot.
render_node_ok() {
  local r
  for r in "$SYS"/class/drm/renderD*; do
    [[ -e $r/device/driver ]] || continue
    [[ $(basename "$(readlink -f "$r/device/driver")") == "$RENDER_DRIVER" ]] && return 0
  done
  return 1
}

if [[ ${AIR_GPU_SOURCE_ONLY:-} == 1 ]]; then return 0; fi
main "$@"
