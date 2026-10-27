/*
 * What Wine's WoW64 winevulkan (win32u/vulkan.c, Wine 10/11) asks of the driver, in a 64-bit
 * process: 32-bit games need every mapping below 4 GB.
 *   - With VK_EXT_map_memory_placed (memoryMapPlaced + memoryUnmapReserve) and VK_KHR_map_memory2,
 *     vkMapMemory2KHR(VK_MEMORY_MAP_PLACED_BIT_EXT) of the whole allocation onto an address Wine
 *     reserved below 4 GB, and vkUnmapMemory2KHR(VK_MEMORY_UNMAP_RESERVE_BIT_EXT).
 *   - Otherwise, with VK_EXT_external_memory_host: every vkCreateImage / vkCreateBuffer gets
 *     VkExternalMemory{Image,Buffer}CreateInfo{HOST_ALLOCATION_BIT_EXT}, and every host-visible
 *     allocation imports anonymous memory from below 4 GB (vkGetMemoryHostPointerPropertiesEXT
 *     picks the memory type). A host-visible allocation that cannot be imported is mapped by the
 *     driver, and a mapping above 4 GB is VK_ERROR_OUT_OF_HOST_MEMORY.
 * Prints what the device reports and what each step returns.
 *
 *   cc -o out/tests/wow64_memory_test tests/wow64_memory_test.c -lvulkan
 *   sh tests/run.sh out/tests/wow64_memory_test
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <vulkan/vulkan.h>

#ifndef VK_EXT_map_memory_placed
#define VK_EXT_map_memory_placed 1
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT ((VkStructureType)1000272000)
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT ((VkStructureType)1000272001)
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
typedef struct VkPhysicalDeviceMapMemoryPlacedPropertiesEXT {
    VkStructureType sType;
    void* pNext;
    VkDeviceSize minPlacedMemoryMapAlignment;
} VkPhysicalDeviceMapMemoryPlacedPropertiesEXT;
typedef struct VkMemoryMapPlacedInfoEXT {
    VkStructureType sType;
    const void* pNext;
    void* pPlacedAddress;
} VkMemoryMapPlacedInfoEXT;
#endif

static int failures;
#define CHECK(what, expr)                                                                      \
    do {                                                                                       \
        VkResult r_ = (expr);                                                                  \
        printf("%-64s %d%s\n", what, r_, r_ ? "  <-- FAIL" : "");                            \
        if (r_) failures++;                                                                    \
    } while (0)

static bool hasExt(VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties* p = calloc(n + 1, sizeof(*p));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, p);
    bool found = false;
    for (uint32_t i = 0; i < n; i++) found |= !strcmp(p[i].extensionName, name);
    free(p);
    return found;
}

/* Anonymous memory below 4 GB, as NtAllocateVirtualMemory with zero_bits gives a WoW64 process. */
static void* low(size_t size) {
    for (uintptr_t at = 0x20000000; at + size < 0x100000000ull; at += 0x01000000) {
        void* p = mmap((void*)at, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) return p;
    }
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) { puts("no instance"); return 2; }
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { puts("no device"); return 2; }

    bool placed = hasExt(pd, "VK_EXT_map_memory_placed"), map2 = hasExt(pd, "VK_KHR_map_memory2"),
         host = hasExt(pd, "VK_EXT_external_memory_host");
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placedFeatures = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = placed ? &placedFeatures : NULL};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    VkPhysicalDeviceMapMemoryPlacedPropertiesEXT placedProps = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT,
                                                                 .pNext = placed ? &placedProps : NULL};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hostProps};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    printf("device %s\n", p2.properties.deviceName);
    printf("VK_EXT_map_memory_placed %d (memoryMapPlaced %u, memoryUnmapReserve %u, alignment %llu), VK_KHR_map_memory2 %d\n",
           placed, placedFeatures.memoryMapPlaced, placedFeatures.memoryUnmapReserve,
           (unsigned long long)placedProps.minPlacedMemoryMapAlignment, map2);
    printf("VK_EXT_external_memory_host %d (minImportedHostPointerAlignment %llu)\n", host,
           (unsigned long long)hostProps.minImportedHostPointerAlignment);
    bool wineUsesPlaced = placed && map2 && placedFeatures.memoryMapPlaced && placedFeatures.memoryUnmapReserve;
    bool wineUsesHost = !wineUsesPlaced && host && hostProps.minImportedHostPointerAlignment;
    printf("=> WoW64 winevulkan maps with: %s\n", wineUsesPlaced ? "placed maps" : wineUsesHost ? "imported host memory"
                                                                                : "the driver's own (fails above 4 GB)");

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    /* What the driver says about HOST_ALLOCATION images, then the image DXVK's D3D9 backbuffer is. */
    VkPhysicalDeviceExternalImageFormatInfo extFmt = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
                                                      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkPhysicalDeviceImageFormatInfo2 fi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &extFmt,
                                           .format = VK_FORMAT_B8G8R8A8_UNORM, .type = VK_IMAGE_TYPE_2D,
                                           .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                                           .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT};
    VkExternalImageFormatProperties extProps = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 ifp = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &extProps};
    VkResult r = vkGetPhysicalDeviceImageFormatProperties2(pd, &fi, &ifp);
    printf("%-64s %d (externalMemoryFeatures 0x%x)\n", "image format properties, B8G8R8A8 optimal, HOST_ALLOCATION", r,
           extProps.externalMemoryProperties.externalMemoryFeatures);
    fi.tiling = VK_IMAGE_TILING_LINEAR;
    r = vkGetPhysicalDeviceImageFormatProperties2(pd, &fi, &ifp);
    printf("%-64s %d (externalMemoryFeatures 0x%x)\n", "image format properties, B8G8R8A8 linear, HOST_ALLOCATION", r,
           extProps.externalMemoryProperties.externalMemoryFeatures);

    float prio = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    const char* exts[4];
    uint32_t extCount = 0;
    if (host) exts[extCount++] = "VK_EXT_external_memory_host";
    if (map2) exts[extCount++] = "VK_KHR_map_memory2";
    if (wineUsesPlaced) exts[extCount++] = "VK_EXT_map_memory_placed";
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = wineUsesPlaced ? &placedFeatures : NULL,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                              .enabledExtensionCount = extCount, .ppEnabledExtensionNames = exts};
    placedFeatures.memoryMapRangePlaced = VK_FALSE;
    VkDevice dev;
    CHECK("vkCreateDevice", vkCreateDevice(pd, &dci, NULL, &dev));
    if (failures) return 1;
    PFN_vkGetMemoryHostPointerPropertiesEXT getHostProps =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT");
    PFN_vkMapMemory2KHR map2KHR = (PFN_vkMapMemory2KHR)vkGetDeviceProcAddr(dev, "vkMapMemory2KHR");
    PFN_vkUnmapMemory2KHR unmap2KHR = (PFN_vkUnmapMemory2KHR)vkGetDeviceProcAddr(dev, "vkUnmapMemory2KHR");

    /* The D3D9 backbuffer, plain and as winevulkan creates it on the imported-host-memory path. */
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = {1024, 768, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image;
    CHECK("vkCreateImage 1024x768 B8G8R8A8 optimal", vkCreateImage(dev, &ii, NULL, &image));
    vkDestroyImage(dev, image, NULL);
    /* What follows is winevulkan's imported-host-memory path: it counts only when winevulkan takes it. */
    int failuresBefore = failures;
    VkExternalMemoryImageCreateInfo extImage = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    ii.pNext = &extImage;
    r = vkCreateImage(dev, &ii, NULL, &image);
    printf("%-64s %d%s\n", "  same + VkExternalMemoryImageCreateInfo{HOST_ALLOCATION}", r,
           r == -1000158000 ? "  (= DXVK's 'Failed to create image: -1000158000')" : "");
    if (r) failures++;
    if (r == VK_SUCCESS) vkDestroyImage(dev, image, NULL);
    ii.tiling = VK_IMAGE_TILING_LINEAR;
    r = vkCreateImage(dev, &ii, NULL, &image);
    printf("%-64s %d\n", "  linear + HOST_ALLOCATION", r);
    if (r == VK_SUCCESS) vkDestroyImage(dev, image, NULL);

    VkExternalMemoryBufferCreateInfo extBuffer = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                                  .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &extBuffer, .size = 4 << 20,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VkBuffer buffer;
    CHECK("vkCreateBuffer 4 MB + HOST_ALLOCATION", vkCreateBuffer(dev, &bi, NULL, &buffer));
    VkMemoryRequirements req = {0};
    vkGetBufferMemoryRequirements(dev, buffer, &req);
    printf("  buffer memoryTypeBits 0x%x\n", req.memoryTypeBits);

    /* Host-visible allocations of 4 MB, imported from below 4 GB, as winevulkan does them. */
    size_t size = 4 << 20;
    void* lowMem = low(size);
    printf("low memory at %p\n", lowMem);
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (getHostProps && lowMem) {
        CHECK("vkGetMemoryHostPointerPropertiesEXT", getHostProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, lowMem, &hp));
        printf("  importable memory types 0x%x\n", hp.memoryTypeBits);
    }
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
        VkMemoryPropertyFlags flags = mp.memoryTypes[t].propertyFlags;
        if (!(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        char what[96];
        /* allocate_external_host_memory(): a type that cannot import falls back to an importable one
         * with the same flags but DEVICE_LOCAL; when there is none, the allocation is not imported. */
        uint32_t use = t;
        if (!(hp.memoryTypeBits & (1u << t))) {
            uint32_t mask = flags & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            use = UINT32_MAX;
            for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
                if ((hp.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & mask) == mask) { use = i; break; }
        }
        if (use != UINT32_MAX) {
            VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                                    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                    .pHostPointer = lowMem};
            VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                                       .memoryTypeIndex = use};
            VkDeviceMemory mem;
            snprintf(what, sizeof(what), "type %u (0x%x): import low memory as type %u", t, flags, use);
            r = vkAllocateMemory(dev, &ai, NULL, &mem);
            printf("%-64s %d%s\n", what, r, r ? "  <-- FAIL" : "");
            if (r) failures++;
            else {
                if (buffer && (req.memoryTypeBits & (1u << use))) {
                    snprintf(what, sizeof(what), "  bind the 4 MB HOST_ALLOCATION buffer");
                    CHECK(what, vkBindBufferMemory(dev, buffer, mem, 0));
                    vkDestroyBuffer(dev, buffer, NULL);
                    CHECK("  vkCreateBuffer again", vkCreateBuffer(dev, &bi, NULL, &buffer));
                }
                vkFreeMemory(dev, mem, NULL);
            }
            continue;
        }
        /* Not imported: winevulkan maps it through the driver and needs the pointer below 4 GB. */
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = size, .memoryTypeIndex = t};
        VkDeviceMemory mem;
        snprintf(what, sizeof(what), "type %u (0x%x): no importable type, plain allocation", t, flags);
        CHECK(what, vkAllocateMemory(dev, &ai, NULL, &mem));
        void* p = NULL;
        r = vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &p);
        printf("  vkMapMemory %d at %p%s\n", r, p, (uintptr_t)p >> 32 ? "  <-- above 4 GB: WoW64 returns VK_ERROR_OUT_OF_HOST_MEMORY" : "");
        if ((uintptr_t)p >> 32) failures++;
        vkFreeMemory(dev, mem, NULL);
    }
    vkDestroyBuffer(dev, buffer, NULL);
    if (!wineUsesHost) failures = failuresBefore;

    /* The placed path, when the device has it: every host-visible type is mapped onto low memory,
     * written there, copied and filled by the GPU, read back there, and unmapped keeping the range
     * reserved, as winevulkan maps, unmaps and frees it. */
    if (wineUsesPlaced && map2KHR && unmap2KHR) {
        VkQueue queue;
        vkGetDeviceQueue(dev, 0, 0, &queue);
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
        VkCommandPool pool;
        CHECK("vkCreateCommandPool", vkCreateCommandPool(dev, &pci, NULL, &pool));
        VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkCommandBuffer cmd;
        CHECK("vkAllocateCommandBuffers", vkAllocateCommandBuffers(dev, &cai, &cmd));
        VkBufferCreateInfo plain = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        void* lowB = low(size);
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
            if (!(mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
            char what[96];
            VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = size, .memoryTypeIndex = t};
            VkDeviceMemory mem[2];
            void* at[2] = {lowMem, lowB};
            bool mapped = true;
            for (int i = 0; i < 2; i++) {
                snprintf(what, sizeof(what), "type %u (0x%x): allocate", t, mp.memoryTypes[t].propertyFlags);
                CHECK(what, vkAllocateMemory(dev, &ai, NULL, &mem[i]));
                VkMemoryMapPlacedInfoEXT pi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = at[i]};
                VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pi, .flags = VK_MEMORY_MAP_PLACED_BIT_EXT,
                                         .memory = mem[i], .offset = 0, .size = VK_WHOLE_SIZE};
                void* p = NULL;
                r = map2KHR(dev, &mi, &p);
                printf("%-64s %d at %p%s\n", "  vkMapMemory2KHR placed below 4 GB", r, p, r || p != at[i] ? "  <-- FAIL" : "");
                if (r || p != at[i]) { failures++; mapped = false; }
            }
            /* GPU round trip through the placed mappings, for the types a transfer buffer can use. */
            VkBuffer buf[2];
            vkCreateBuffer(dev, &plain, NULL, &buf[0]);
            vkCreateBuffer(dev, &plain, NULL, &buf[1]);
            vkGetBufferMemoryRequirements(dev, buf[0], &req);
            if (mapped && (req.memoryTypeBits & (1u << t))) {
                uint32_t* a = lowMem;
                uint32_t* b = lowB;
                for (size_t i = 0; i < size / 4; i++) { a[i] = (uint32_t)(i * 2654435761u) ^ t; b[i] = 0; }
                VkMappedMemoryRange ranges[2] = {{.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = mem[0], .size = VK_WHOLE_SIZE},
                                                 {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = mem[1], .size = VK_WHOLE_SIZE}};
                vkFlushMappedMemoryRanges(dev, 2, ranges);
                vkBindBufferMemory(dev, buf[0], mem[0], 0);
                vkBindBufferMemory(dev, buf[1], mem[1], 0);
                VkCommandBufferBeginInfo bi2 = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
                vkBeginCommandBuffer(cmd, &bi2);
                VkBufferCopy copy = {.size = size};
                vkCmdCopyBuffer(cmd, buf[0], buf[1], 1, &copy);
                VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
                vkCmdFillBuffer(cmd, buf[0], size / 2, size / 2, 0xc0ffee00u | t);
                mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
                vkEndCommandBuffer(cmd);
                VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
                CHECK("  GPU copy and fill", vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
                vkQueueWaitIdle(queue);
                vkInvalidateMappedMemoryRanges(dev, 2, ranges);
                size_t bad = 0;
                for (size_t i = 0; i < size / 4; i++) {
                    if (b[i] != ((uint32_t)(i * 2654435761u) ^ t)) bad++;
                    if (i >= size / 8 && a[i] != (0xc0ffee00u | t)) bad++;
                }
                printf("  GPU saw the CPU's writes and the CPU the GPU's: %s\n", bad ? "NO  <-- FAIL" : "yes");
                if (bad) failures++;
                vkResetCommandBuffer(cmd, 0);
            }
            vkDestroyBuffer(dev, buf[0], NULL);
            vkDestroyBuffer(dev, buf[1], NULL);
            for (int i = 0; i < 2; i++) {
                VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = mem[i],
                                           .flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT};
                CHECK("  vkUnmapMemory2KHR(RESERVE)", unmap2KHR(dev, &ui));
                /* The range stays reserved (nothing else can be mapped there) until the caller frees it. */
                void* again = mmap(at[i], size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                printf("  range still reserved after unmap: %s\n", again == MAP_FAILED ? "yes" : "NO  <-- FAIL");
                if (again != MAP_FAILED) { failures++; munmap(again, size); }
                vkFreeMemory(dev, mem[i], NULL);
                if (mmap(at[i], size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != at[i]) failures++;
            }
        }
        /* An optimal image (a D3D9 backbuffer) in the host-visible memory images get on the Mali,
         * cleared by the GPU and copied into a buffer mapped below 4 GB. */
        VkImageCreateInfo oi = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = {1024, 768, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage img;
        CHECK("optimal image: vkCreateImage 1024x768", vkCreateImage(dev, &oi, NULL, &img));
        VkMemoryRequirements ir;
        vkGetImageMemoryRequirements(dev, img, &ir);
        uint32_t it = 0, bt = 0;
        while (it < mp.memoryTypeCount && !((ir.memoryTypeBits >> it) & 1)) it++;
        VkBuffer out;
        plain.size = 1024 * 768 * 4;
        vkCreateBuffer(dev, &plain, NULL, &out);
        vkGetBufferMemoryRequirements(dev, out, &req);
        while (bt < mp.memoryTypeCount && !(((req.memoryTypeBits >> bt) & 1) &&
                                            (mp.memoryTypes[bt].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) bt++;
        printf("  image memory type %u (0x%x), readback buffer type %u\n", it, mp.memoryTypes[it].propertyFlags, bt);
        VkMemoryAllocateInfo iai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = ir.size, .memoryTypeIndex = it};
        VkMemoryAllocateInfo bai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = bt};
        VkDeviceMemory imem, bmem;
        CHECK("  allocate image memory", vkAllocateMemory(dev, &iai, NULL, &imem));
        CHECK("  vkBindImageMemory", vkBindImageMemory(dev, img, imem, 0));
        CHECK("  allocate readback memory", vkAllocateMemory(dev, &bai, NULL, &bmem));
        CHECK("  vkBindBufferMemory", vkBindBufferMemory(dev, out, bmem, 0));
        VkMemoryMapPlacedInfoEXT pi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = lowMem};
        VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pi, .flags = VK_MEMORY_MAP_PLACED_BIT_EXT,
                                 .memory = bmem, .size = VK_WHOLE_SIZE};
        uint32_t* px = NULL;
        CHECK("  map the readback buffer below 4 GB", map2KHR(dev, &mi, (void**)&px));
        VkCommandBufferBeginInfo bi3 = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &bi3);
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = img, .subresourceRange = range};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
        VkClearColorValue color = {.float32 = {1.0f, 0.0f, 1.0f, 1.0f}};
        vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
        VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {1024, 768, 1}};
        vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, 1, &region);
        VkMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
        CHECK("  GPU clear and copy", vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        vkQueueWaitIdle(queue);
        VkMappedMemoryRange br = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = bmem, .size = VK_WHOLE_SIZE};
        vkInvalidateMappedMemoryRanges(dev, 1, &br);
        size_t wrong = 0;
        for (size_t i = 0; px && i < 1024 * 768; i++) wrong += px[i] != 0xffff00ffu;
        printf("  cleared image read back below 4 GB: %s (pixel 0 %08x)\n", wrong ? "NO  <-- FAIL" : "yes", px ? px[0] : 0);
        if (wrong) failures++;
        VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = bmem, .flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT};
        unmap2KHR(dev, &ui);
        vkDestroyBuffer(dev, out, NULL);
        vkDestroyImage(dev, img, NULL);
        vkFreeMemory(dev, bmem, NULL);
        vkFreeMemory(dev, imem, NULL);
        vkDestroyCommandPool(dev, pool, NULL);
    }

    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
