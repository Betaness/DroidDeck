/*
 * Fresh glibc threads entering the Mali driver (libhybris's TLS thunks, patches/libhybris-0003): the
 * first `mrs tpidr_el0` a thread executes in vendor code finds its bionic TLS unset and takes the
 * first-touch path, every later one the fast path. Three waves of 16 threads, each starting
 * straight in the driver: its own command pool, 2000 recorded commands, a submission, a fence wait
 * and a check of what the GPU wrote. The main thread then times recording (TT_BENCH=1: ns per
 * command, only when nothing else uses the device). Prints TT-PASS or TT-FAIL.
 *
 *   cc -O2 -o out/tests/thread_tls_test tests/thread_tls_test.c -lvulkan -lpthread
 *   sh tests/run.sh out/tests/thread_tls_test
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define THREADS 16
#define WAVES 3
#define COMMANDS 2000

static VkDevice dev;
static VkQueue queue;
static pthread_mutex_t queueLock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t family, hostType = UINT32_MAX;
static atomic_int failures;

typedef struct {
    int index;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint32_t* data;
} Job;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void record(VkCommandBuffer cmd, VkBuffer buffer, uint32_t value, int commands) {
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &bi);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    for (int i = 0; i < commands / 2; i++) {
        vkCmdFillBuffer(cmd, buffer, (VkDeviceSize)(i % 64) * 64, 64, value);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cmd);
}

static void* worker(void* arg) {
    Job* job = arg;
    uint32_t value = 0xa0000000u | (uint32_t)job->index;
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool pool;
    if (vkCreateCommandPool(dev, &pci, NULL, &pool)) { failures++; return NULL; }
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    record(cmd, job->buffer, value, COMMANDS);
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(dev, &fci, NULL, &fence);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    pthread_mutex_lock(&queueLock);
    VkResult r = vkQueueSubmit(queue, 1, &si, fence);
    pthread_mutex_unlock(&queueLock);
    if (r || vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull)) failures++;
    for (int i = 0; i < 64 * 16; i++)
        if (job->data[i] != value) { failures++; break; }
    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) { puts("TT-FAIL no instance"); return 2; }
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { puts("TT-FAIL no device"); return 2; }
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1,
                                  .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) { puts("TT-FAIL no device"); return 2; }
    vkGetDeviceQueue(dev, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    Job jobs[THREADS];
    for (int i = 0; i < THREADS; i++) {
        jobs[i].index = i;
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        vkCreateBuffer(dev, &bi, NULL, &jobs[i].buffer);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, jobs[i].buffer, &req);
        for (uint32_t t = 0; t < mp.memoryTypeCount && hostType == UINT32_MAX; t++)
            if ((req.memoryTypeBits & (1u << t)) &&
                (mp.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                hostType = t;
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = hostType};
        vkAllocateMemory(dev, &ai, NULL, &jobs[i].memory);
        vkBindBufferMemory(dev, jobs[i].buffer, jobs[i].memory, 0);
        vkMapMemory(dev, jobs[i].memory, 0, VK_WHOLE_SIZE, 0, (void**)&jobs[i].data);
    }
    for (int wave = 0; wave < WAVES; wave++) {
        pthread_t threads[THREADS];
        for (int i = 0; i < THREADS; i++) memset(jobs[i].data, 0, 4096);
        for (int i = 0; i < THREADS; i++) pthread_create(&threads[i], NULL, worker, &jobs[i]);
        for (int i = 0; i < THREADS; i++) pthread_join(threads[i], NULL);
        printf("wave %d: %d threads, each recorded %d commands, submitted and checked the GPU's writes: %s\n", wave, THREADS, COMMANDS,
               failures ? "FAILED" : "ok");
    }

    if (getenv("TT_BENCH") && atoi(getenv("TT_BENCH"))) {
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
        VkCommandPool pool;
        vkCreateCommandPool(dev, &pci, NULL, &pool);
        VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(dev, &cai, &cmd);
        double best = 1e9;
        for (int rep = 0; rep < 20; rep++) {
            double t0 = now();
            record(cmd, jobs[0].buffer, 1, 20000);
            double t = (now() - t0) / 20000 * 1e9;
            if (t < best) best = t;
            vkResetCommandPool(dev, pool, 0);
        }
        printf("bench recording: %.1f ns per command (best of 20 x 20000)\n", best);
        vkDestroyCommandPool(dev, pool, NULL);
    }
    for (int i = 0; i < THREADS; i++) {
        vkDestroyBuffer(dev, jobs[i].buffer, NULL);
        vkFreeMemory(dev, jobs[i].memory, NULL);
    }
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("%s (%d failures)\n", failures ? "TT-FAIL" : "TT-PASS", (int)failures);
    return failures ? 1 : 0;
}
