/*
 * The memory behind VK_EXT_map_memory_placed (src/placed.c) as WoW64 winevulkan uses it: every
 * allocation placed-mapped below 4 GB, unmapped with VK_MEMORY_UNMAP_RESERVE_BIT_EXT before it is
 * freed. Checks, for each host-visible memory type of the device:
 *   heap      which dma-buf heap backs it (/proc/self/fdinfo): HOST_COHERENT types system-uncached,
 *             HOST_CACHED non-coherent ones the cached system heap (system-uncached with
 *             DROIDDECK_HYBRIS_PLACED_CACHED=0); one dma-buf per allocation, none after the free
 *   to GPU    the CPU reads the memory (so it is in its cache), writes a pattern and flushes only a
 *             range in the middle (none for coherent types); the GPU copies the memory and the
 *             range must arrive (outside it: reported, may be stale on cached memory)
 *   to CPU    the CPU reads the memory, the GPU fills it, the CPU invalidates only a range and
 *             must read the GPU's values there
 *   images    a dedicated allocation for an optimal image is the driver's (no dma-buf) and a placed
 *             map of it fails (with DROIDDECK_HYBRIS_PLACED_OPTIMAL_IMAGES=1: a dma-buf, and the
 *             map succeeds); the GPU clears the image and copies it into placed memory; one for a
 *             linear image is a dma-buf and can be placed-mapped
 *   fds       300 rounds of allocate, place, unmap, free leave no descriptor behind
 *   no dma-buf  with no descriptors left (RLIMIT_NOFILE) the allocation fails with
 *             VK_ERROR_OUT_OF_DEVICE_MEMORY, which applications handle, rather than succeeding
 *             with memory a placed map then fails on
 * PM_BENCH=1 also times CPU reads and writes through each placed mapping, flush and invalidate by
 * size, and allocation (only when nothing else uses the device). Prints PM-PASS or PM-FAIL.
 *
 *   cc -O2 -o out/tests/placed_memory_test tests/placed_memory_test.c -lvulkan
 *   sh tests/run.sh out/tests/placed_memory_test
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#ifndef VK_EXT_map_memory_placed
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT ((VkStructureType)1000272000)
#define VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT ((VkStructureType)1000272002)
#define VK_MEMORY_MAP_PLACED_BIT_EXT 0x00000001
#define VK_MEMORY_UNMAP_RESERVE_BIT_EXT 0x00000001
typedef struct VkPhysicalDeviceMapMemoryPlacedFeaturesEXT {
    VkStructureType sType;
    void* pNext;
    VkBool32 memoryMapPlaced;
    VkBool32 memoryMapRangePlaced;
    VkBool32 memoryUnmapReserve;
} VkPhysicalDeviceMapMemoryPlacedFeaturesEXT;
typedef struct VkMemoryMapPlacedInfoEXT {
    VkStructureType sType;
    const void* pNext;
    void* pPlacedAddress;
} VkMemoryMapPlacedInfoEXT;
#endif

#define SIZE (4u << 20)
#define IMG_SIZE (16u << 20)
static int failures;
static VkDevice dev;
static VkQueue queue;
static VkCommandBuffer cmd;
static VkPhysicalDeviceMemoryProperties mp;
static PFN_vkMapMemory2KHR map2;
static PFN_vkUnmapMemory2KHR unmap2;

#define FAIL(...) do { printf(__VA_ARGS__); printf("  <-- FAIL\n"); failures++; } while (0)

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Open dma-bufs of this process from heap `name` (NULL: any), and all open descriptors. */
static int dmabufs(const char* name, int* all) {
    DIR* d = opendir("/proc/self/fdinfo");
    int count = 0, fds = 0;
    struct dirent* e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        fds++;
        char path[300], line[256];
        snprintf(path, sizeof(path), "/proc/self/fdinfo/%s", e->d_name);
        FILE* f = fopen(path, "r");
        while (f && fgets(line, sizeof(line), f)) {
            char exp[64];
            if (sscanf(line, "exp_name: %63s", exp) == 1 && (!name || !strcmp(exp, name))) count++;
        }
        if (f) fclose(f);
    }
    if (d) closedir(d);
    if (all) *all = fds;
    return count;
}

/* Anonymous memory below 4 GB, as NtAllocateVirtualMemory with zero_bits gives a WoW64 process. */
static void* low(size_t size) {
    for (uintptr_t at = 0x20000000; at + size < 0x100000000ull; at += 0x01000000) {
        void* p = mmap((void*)at, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) return p;
    }
    return NULL;
}

typedef struct {
    VkDeviceMemory memory;
    VkBuffer buffer;
    uint32_t* at;
} Placed;

/* Allocates `size` of type t, places it at `at` (reserved low memory), binds a transfer buffer. */
static bool place(Placed* p, uint32_t t, void* at, VkDeviceSize size) {
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = size, .memoryTypeIndex = t};
    VkResult r = vkAllocateMemory(dev, &ai, NULL, &p->memory);
    if (r) { FAIL("type %u: vkAllocateMemory %d", t, r); return false; }
    VkMemoryMapPlacedInfoEXT pi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = at};
    VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pi, .flags = VK_MEMORY_MAP_PLACED_BIT_EXT,
                             .memory = p->memory, .size = VK_WHOLE_SIZE};
    void* data = NULL;
    if ((r = map2(dev, &mi, &data)) || data != at) { FAIL("type %u: placed map %d at %p", t, r, data); return false; }
    p->at = data;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    vkCreateBuffer(dev, &bi, NULL, &p->buffer);
    vkBindBufferMemory(dev, p->buffer, p->memory, 0);
    return true;
}

static void release(Placed* p) {
    vkDestroyBuffer(dev, p->buffer, NULL);
    VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = p->memory,
                               .flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT};
    if (p->at) unmap2(dev, &ui);
    vkFreeMemory(dev, p->memory, NULL);
    *p = (Placed){0};
}

static void submitWait(void) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)) FAIL("vkQueueSubmit");
    vkQueueWaitIdle(queue);
    vkResetCommandBuffer(cmd, 0);
}

static void begin(void) {
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &bi);
}

static void barrier(VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags dstStage) {
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, dstStage, 0, 1, &mb, 0, NULL, 0, NULL);
}

static volatile uint32_t sink;
static void readAll(const uint32_t* p, size_t words) {
    uint32_t x = 0;
    for (size_t i = 0; i < words; i++) x += p[i];
    sink = x;
}

static void syncRange(VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, bool flush) {
    VkMappedMemoryRange r = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = memory, .offset = offset, .size = size};
    VkResult res = flush ? vkFlushMappedMemoryRanges(dev, 1, &r) : vkInvalidateMappedMemoryRanges(dev, 1, &r);
    if (res) FAIL("%s %d", flush ? "vkFlushMappedMemoryRanges" : "vkInvalidateMappedMemoryRanges", res);
}

/* to GPU and to CPU for type t; `check` is a coherent placed allocation the GPU copies into. */
static void coherency(uint32_t t, Placed* m, Placed* check) {
    bool coherent = mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const size_t words = SIZE / 4;
    const VkDeviceSize lo = 64 << 10, len = 1 << 20; /* the range synced: 64 KB .. 1088 KB */
    for (int round = 0; round < 3; round++) {
        uint32_t seed = (t << 24) ^ (round << 20) ^ 0x5a5a;
        /* to GPU */
        readAll(m->at, words);
        for (size_t i = 0; i < words; i++) m->at[i] = (uint32_t)(i * 2654435761u) ^ seed;
        if (!coherent) syncRange(m->memory, lo, len, true);
        begin();
        VkBufferCopy copy = {.size = SIZE};
        vkCmdCopyBuffer(cmd, m->buffer, check->buffer, 1, &copy);
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);
        submitWait();
        size_t bad = 0, outside = 0;
        for (size_t i = 0; i < words; i++) {
            bool want = check->at[i] == ((uint32_t)(i * 2654435761u) ^ seed);
            bool in = coherent || (i * 4 >= lo && i * 4 < lo + len);
            if (!want) { if (in) bad++; else outside++; }
        }
        if (bad) FAIL("type %u round %d: GPU read %zu stale words in the flushed range", t, round, bad);
        else if (round == 0) printf("type %u: to GPU ok%s (%zu stale words outside the flushed range)\n", t, coherent ? ", no flush" : "", outside);
        /* The rest is flushed too: host writes left unflushed, then written by the GPU and
         * invalidated, are undefined (a dirty line can be written back over the GPU's data). */
        if (!coherent) syncRange(m->memory, 0, VK_WHOLE_SIZE, true);
        /* to CPU */
        readAll(m->at, words);
        begin();
        barrier(VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdFillBuffer(cmd, m->buffer, 0, SIZE, 0xc0de0000u ^ seed);
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);
        submitWait();
        const VkDeviceSize ilo = 1 << 20, ilen = 512 << 10;
        if (!coherent) syncRange(m->memory, ilo, ilen, false);
        bad = outside = 0;
        for (size_t i = 0; i < words; i++) {
            bool want = m->at[i] == (0xc0de0000u ^ seed);
            bool in = coherent || (i * 4 >= ilo && i * 4 < ilo + ilen);
            if (!want) { if (in) bad++; else outside++; }
        }
        if (bad) FAIL("type %u round %d: CPU read %zu stale words in the invalidated range", t, round, bad);
        else if (round == 0) printf("type %u: to CPU ok%s (%zu stale words outside the invalidated range)\n", t, coherent ? ", no invalidate" : "", outside);
    }
}

static void bench(uint32_t t, Placed* m) {
    const size_t words = SIZE / 4;
    double t0 = now();
    for (int i = 0; i < 8; i++) readAll(m->at, words);
    double rd = 8.0 * SIZE / (now() - t0) / 1e6;
    t0 = now();
    for (int i = 0; i < 8; i++) memset(m->at, i, SIZE);
    double wr = 8.0 * SIZE / (now() - t0) / 1e6;
    printf("bench type %2u (0x%x): CPU read %6.0f MB/s, write %6.0f MB/s", t, mp.memoryTypes[t].propertyFlags, rd, wr);
    if (!(mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        for (VkDeviceSize s = 64 << 10; s <= SIZE; s *= 8) {
            t0 = now();
            for (int i = 0; i < 16; i++) syncRange(m->memory, 0, s, true);
            double f = (now() - t0) / 16 * 1e6;
            t0 = now();
            for (int i = 0; i < 16; i++) syncRange(m->memory, 0, s, false);
            double v = (now() - t0) / 16 * 1e6;
            printf(", %llu KB flush %.1f us invalidate %.1f us", (unsigned long long)(s >> 10), f, v);
        }
    }
    printf("\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    bool doBench = getenv("PM_BENCH") && atoi(getenv("PM_BENCH"));
    bool cachedHeap = !getenv("DROIDDECK_HYBRIS_PLACED_CACHED") || atoi(getenv("DROIDDECK_HYBRIS_PLACED_CACHED"));
    bool optimalPlaced = getenv("DROIDDECK_HYBRIS_PLACED_OPTIMAL_IMAGES") && atoi(getenv("DROIDDECK_HYBRIS_PLACED_OPTIMAL_IMAGES"));
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) { puts("PM-FAIL no instance"); return 2; }
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { puts("PM-FAIL no device"); return 2; }
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT,
                                                         .memoryMapPlaced = VK_TRUE, .memoryUnmapReserve = VK_TRUE};
    const char* exts[] = {"VK_KHR_map_memory2", "VK_EXT_map_memory_placed"};
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &placed, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qi, .enabledExtensionCount = 2, .ppEnabledExtensionNames = exts};
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    if (r) { printf("PM-FAIL vkCreateDevice with VK_EXT_map_memory_placed: %d\n", r); return 1; }
    map2 = (PFN_vkMapMemory2KHR)vkGetDeviceProcAddr(dev, "vkMapMemory2KHR");
    unmap2 = (PFN_vkUnmapMemory2KHR)vkGetDeviceProcAddr(dev, "vkUnmapMemory2KHR");
    vkGetDeviceQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    void* lowA = low(SIZE);
    void* lowB = low(SIZE);
    /* For the 1920x1080 image (8 MB) and the buffer it is copied into. */
    void* lowImg = low(IMG_SIZE);
    /* A coherent type to check the GPU's copies through. */
    uint32_t coherentType = UINT32_MAX;
    for (uint32_t t = 0; t < mp.memoryTypeCount && coherentType == UINT32_MAX; t++)
        if ((mp.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            coherentType = t;
    Placed check;
    if (coherentType == UINT32_MAX || !place(&check, coherentType, lowB, SIZE)) { puts("PM-FAIL no coherent type to check with"); return 1; }
    /* After the first allocation: the ICD keeps the heap devices open from then on. */
    int fdsBefore;
    int bufsBefore = dmabufs(NULL, &fdsBefore);

    VkBufferCreateInfo probe = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = SIZE,
                                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer probeBuffer;
    vkCreateBuffer(dev, &probe, NULL, &probeBuffer);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, probeBuffer, &req);
    vkDestroyBuffer(dev, probeBuffer, NULL);

    for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
        VkMemoryPropertyFlags flags = mp.memoryTypes[t].propertyFlags;
        if (!(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        bool cachedType = (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) && !(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        const char* want = cachedType && cachedHeap ? "system" : "system-uncached";
        int sys0 = dmabufs("system", NULL), unc0 = dmabufs("system-uncached", NULL);
        Placed m;
        if (!place(&m, t, lowA, SIZE)) continue;
        int sys = dmabufs("system", NULL) - sys0, unc = dmabufs("system-uncached", NULL) - unc0;
        const char* got = sys == 1 && !unc ? "system" : unc == 1 && !sys ? "system-uncached" : "?";
        if (strcmp(got, want)) FAIL("type %u (0x%x): heap %s (system +%d, system-uncached +%d), want %s", t, flags, got, sys, unc, want);
        else printf("type %u (0x%x): one dma-buf from %s\n", t, flags, got);
        if (req.memoryTypeBits & (1u << t)) coherency(t, &m, &check);
        if (doBench) bench(t, &m);
        release(&m);
        if (mmap(lowA, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != lowA) FAIL("re-reserve");
    }
    int fdsAfter;
    int bufsAfter = dmabufs(NULL, &fdsAfter);
    if (bufsAfter != bufsBefore || fdsAfter != fdsBefore)
        FAIL("descriptors over the types: %d dma-bufs and %d fds before, %d and %d after", bufsBefore, fdsBefore, bufsAfter, fdsAfter);
    release(&check);
    bufsBefore--;
    fdsBefore--;
    if (mmap(lowB, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != lowB) FAIL("re-reserve");

    /* Descriptors: 300 rounds over the host-visible types, freeing some while still mapped. */
    double t0 = now();
    int rounds = 0;
    for (int i = 0; i < 300; i++) {
        uint32_t t = (uint32_t)i % mp.memoryTypeCount;
        if (!(mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        Placed m;
        if (!place(&m, t, lowA, 1 << 20)) break;
        if (i % 3 == 0) m.at = NULL; /* freed while mapped */
        release(&m);
        if (mmap(lowA, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != lowA) FAIL("re-reserve");
        rounds++;
    }
    double perRound = (now() - t0) / rounds * 1e6;
    bufsAfter = dmabufs(NULL, &fdsAfter);
    if (bufsAfter != bufsBefore || fdsAfter != fdsBefore)
        FAIL("descriptors: %d dma-bufs and %d fds before, %d and %d after", bufsBefore, fdsBefore, bufsAfter, fdsAfter);
    else printf("descriptors: %d rounds of allocate, place, free leave none open\n", rounds);
    if (doBench) printf("bench allocate+place+free of 1 MB: %.0f us\n", perRound);

    /* Dedicated allocations for images: optimal ones are the driver's, linear ones placed. */
    for (int tiling = 0; tiling < 2; tiling++) {
        bool optimal = tiling == 0;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {optimal ? 1920 : 256, optimal ? 1080 : 256, 1},
                                .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = optimal ? VK_IMAGE_TILING_OPTIMAL : VK_IMAGE_TILING_LINEAR,
                                .usage = optimal ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                                 : VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                .initialLayout = optimal ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_PREINITIALIZED};
        VkImage image;
        if ((r = vkCreateImage(dev, &ii, NULL, &image))) { FAIL("vkCreateImage %d", r); continue; }
        VkMemoryRequirements ir;
        vkGetImageMemoryRequirements(dev, image, &ir);
        uint32_t t = 0;
        while (t < mp.memoryTypeCount && !(((ir.memoryTypeBits >> t) & 1) &&
                                           (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) t++;
        VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = image};
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &dedicated, .allocationSize = ir.size,
                                   .memoryTypeIndex = t};
        int before = dmabufs(NULL, NULL);
        VkDeviceMemory memory;
        double t0 = now();
        if ((r = vkAllocateMemory(dev, &ai, NULL, &memory))) { FAIL("dedicated image memory: vkAllocateMemory %d", r); continue; }
        double took = (now() - t0) * 1e3;
        int added = dmabufs(NULL, NULL) - before;
        vkBindImageMemory(dev, image, memory, 0);
        if (optimal) {
            if (added != (optimalPlaced ? 1 : 0))
                FAIL("dedicated optimal image (type %u): %d dma-bufs, want %s", t, added, optimalPlaced ? "one" : "the driver's memory");
            else printf("dedicated optimal image (type %u): %s\n", t, optimalPlaced ? "a dma-buf" : "the driver's memory, no dma-buf");
            if (doBench) printf("bench dedicated 1920x1080 image memory, %llu KB: %.2f ms\n", (unsigned long long)(ir.size >> 10), took);
            Placed out;
            if (!lowImg) FAIL("no low reservation for the image");
            else if (place(&out, coherentType, lowImg, (VkDeviceSize)1920 * 1080 * 4)) {
                begin();
                VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                           .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .image = image, .subresourceRange = range};
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
                VkClearColorValue color = {.float32 = {1.0f, 0.0f, 1.0f, 1.0f}};
                vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
                ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
                VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {1920, 1080, 1}};
                vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.buffer, 1, &region);
                barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);
                submitWait();
                size_t wrong = 0;
                for (size_t i = 0; i < 1920 * 1080; i++) wrong += out.at[i] != 0xffff00ffu;
                if (wrong) FAIL("  cleared image read back through placed memory: %zu wrong pixels", wrong);
                else printf("  cleared it on the GPU and read it back through placed memory: ok\n");
                release(&out);
                /* winevulkan places every map; what an application that maps this memory gets. */
                VkMemoryMapPlacedInfoEXT pi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = lowImg};
                VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pi, .flags = VK_MEMORY_MAP_PLACED_BIT_EXT,
                                         .memory = memory, .size = VK_WHOLE_SIZE};
                void* data = NULL;
                r = map2(dev, &mi, &data);
                if (optimalPlaced ? (r || data != lowImg) : r != VK_ERROR_MEMORY_MAP_FAILED)
                    FAIL("  placed map of its memory: %d at %p", r, data);
                else printf("  placed map of its memory: %s\n", r ? "VK_ERROR_MEMORY_MAP_FAILED (the documented limit)" : "ok");
                VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = memory};
                if (!r) unmap2(dev, &ui);
                if (mmap(lowImg, IMG_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != lowImg) FAIL("re-reserve");
            }
        } else {
            VkMemoryMapPlacedInfoEXT pi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = lowA};
            VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pi, .flags = VK_MEMORY_MAP_PLACED_BIT_EXT,
                                     .memory = memory, .size = VK_WHOLE_SIZE};
            void* data = NULL;
            r = map2(dev, &mi, &data);
            if (added != 1 || r || data != lowA) FAIL("dedicated linear image (type %u): %d dma-bufs, placed map %d at %p", t, added, r, data);
            else printf("dedicated linear image (type %u): a dma-buf, placed-mapped\n", t);
            VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = memory, .flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT};
            if (!r) unmap2(dev, &ui);
        }
        vkDestroyImage(dev, image, NULL);
        vkFreeMemory(dev, memory, NULL);
    }

    /* No descriptor left for a dma-buf. */
    struct rlimit saved, none;
    getrlimit(RLIMIT_NOFILE, &saved);
    int highest = 0;
    for (int fd = 0; fd < 4096; fd++) if (fcntl(fd, F_GETFD) >= 0) highest = fd;
    none = saved;
    none.rlim_cur = highest + 1;
    setrlimit(RLIMIT_NOFILE, &none);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 1 << 20, .memoryTypeIndex = coherentType};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    r = vkAllocateMemory(dev, &ai, NULL, &memory);
    setrlimit(RLIMIT_NOFILE, &saved);
    if (r != VK_ERROR_OUT_OF_DEVICE_MEMORY) FAIL("no descriptors left: vkAllocateMemory %d, want VK_ERROR_OUT_OF_DEVICE_MEMORY", r);
    else printf("no descriptors left: vkAllocateMemory fails with VK_ERROR_OUT_OF_DEVICE_MEMORY\n");
    if (r == VK_SUCCESS) vkFreeMemory(dev, memory, NULL);

    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("%s (%d failures)\n", failures ? "PM-FAIL" : "PM-PASS", failures);
    return failures ? 1 : 0;
}
