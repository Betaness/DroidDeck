/*
 * Headless replacement for vortekrenderer/src/xwindow_swapchain.c.
 *
 * Upstream asks the Java X server for an AHardwareBuffer per X window and
 * imports it as the swapchain image. Here the server owns plain device-local
 * VkImages, presents are counted and acknowledged, and (optionally) the last
 * presented frame is written to a PPM file. Implements the API declared in
 * the unmodified upstream xwindow_swapchain.h so it can be swapped back in or
 * replaced by a real presenter (e.g. DroidDeck's surface) later.
 *
 * Environment:
 *   VORTEK_HEADLESS_SIZE   window size reported to clients, "WxH" (default 1280x720)
 *   VORTEK_DUMP_PPM        if set, path of a PPM file receiving the presented frame
 *   VORTEK_DUMP_EVERY      dump every N-th present (default 1)
 *   VORTEK_PRESENT_LOG     log every N-th present (default 60, 0 = never)
 *   VORTEK_AHB_SWAPCHAIN   back swapchain images with AHardwareBuffers the server allocates
 *                          (default 1); with $VORTEK_PRESENT_SOCKET they are shown zero-copy
 *                          by DroidDeck's compositor (zero_copy.c), VORTEK_ZERO_COPY=0 disables it
 *
 * Copyright (C) 2026 mali-v15-bringup contributors.
 * Based on Winlator's vortekrenderer (C) brunodev85, LGPL-2.1.
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include <android/hardware_buffer.h>
#include <poll.h>
#include <pthread.h>
#include "xwindow_swapchain.h"
#include "vulkan_helper.h"
#include "zero_copy.h"
#include "droiddeck_requests.h"

typedef struct HeadlessSwapchain {
    XWindowSwapchain base; /* must be first: request_handler.c only sees this */
    VkDevice device;
    uint32_t nextImage;
    uint32_t lastAcquired;
    uint64_t presentCount;
    VkCommandPool commandPool;
    VkCommandBuffer commandBuffer;
    VkBuffer dumpBuffer;
    VkDeviceMemory dumpMemory;
    VkDeviceSize dumpSize;
    AHardwareBuffer** hardwareBuffers;
    /* zero-copy: who holds each image, guarded by zcLock */
    uint32_t chainId;
    bool zeroCopy;
    bool shown;
    uint8_t* owner;
    int* releaseFd;
    bool stealLogged;
} HeadlessSwapchain;

enum { OWNER_FREE = 0, OWNER_APP = 1, OWNER_COMPOSITOR = 2 };

#define MAX_ZERO_COPY_CHAINS 64
static pthread_mutex_t zcLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t zcCond = PTHREAD_COND_INITIALIZER;
static HeadlessSwapchain* zcChains[MAX_ZERO_COPY_CHAINS];
static uint32_t zcNextChain = 1;

static void releaseImageLocked(HeadlessSwapchain* headless, uint32_t index, int fd) {
    if (index >= (uint32_t)headless->base.imageCount) {
        if (fd >= 0) close(fd);
        return;
    }
    if (headless->releaseFd[index] >= 0) close(headless->releaseFd[index]);
    headless->releaseFd[index] = fd;
    if (headless->owner[index] == OWNER_COMPOSITOR) headless->owner[index] = OWNER_FREE;
}

static void onRelease(uint32_t chain, uint32_t index, int fd) {
    pthread_mutex_lock(&zcLock);
    for (int i = 0; i < MAX_ZERO_COPY_CHAINS; i++) {
        HeadlessSwapchain* headless = zcChains[i];
        if (!headless) continue;
        if (index == ZC_RELEASE_ALL) {
            for (int j = 0; j < headless->base.imageCount; j++) releaseImageLocked(headless, j, -1);
            headless->zeroCopy = false;
            headless->shown = false;
        }
        else if (headless->chainId == chain) {
            releaseImageLocked(headless, index, fd);
            fd = -1;
        }
    }
    pthread_cond_broadcast(&zcCond);
    pthread_mutex_unlock(&zcLock);
    if (fd >= 0) close(fd);
}

static void registerZeroCopy(HeadlessSwapchain* headless) {
    XWindowSwapchain* swapchain = &headless->base;
    if (swapchain->imageCount < 2) return;
    for (int i = 0; i < swapchain->imageCount; i++) if (!headless->hardwareBuffers[i]) return;
    if (!ZeroCopy_available()) return;
    ZeroCopy_setReleaseHandler(onRelease);

    headless->owner = calloc(swapchain->imageCount, sizeof(uint8_t));
    headless->releaseFd = malloc(swapchain->imageCount * sizeof(int));
    for (int i = 0; i < swapchain->imageCount; i++) headless->releaseFd[i] = -1;

    pthread_mutex_lock(&zcLock);
    int slot = -1;
    for (int i = 0; i < MAX_ZERO_COPY_CHAINS && slot < 0; i++) if (!zcChains[i]) slot = i;
    if (slot >= 0) {
        headless->chainId = zcNextChain++;
        zcChains[slot] = headless;
    }
    pthread_mutex_unlock(&zcLock);
    if (slot < 0) return;

    headless->zeroCopy = ZeroCopy_createChain(headless->chainId, swapchain->imageExtent.width, swapchain->imageExtent.height,
                                              headless->hardwareBuffers, swapchain->imageCount);
}

static void unregisterZeroCopy(HeadlessSwapchain* headless) {
    if (!headless->owner) return;
    pthread_mutex_lock(&zcLock);
    for (int i = 0; i < MAX_ZERO_COPY_CHAINS; i++) if (zcChains[i] == headless) zcChains[i] = NULL;
    pthread_mutex_unlock(&zcLock);
    if (headless->chainId) ZeroCopy_destroyChain(headless->chainId);
    for (int i = 0; i < headless->base.imageCount; i++) if (headless->releaseFd[i] >= 0) close(headless->releaseFd[i]);
    MEMFREE(headless->owner);
    MEMFREE(headless->releaseFd);
}

static int envInt(const char* name, int defaultValue) {
    const char* value = getenv(name);
    return value && *value ? atoi(value) : defaultValue;
}

void getWindowExtent(JMethods* jmethods, int windowId, VkExtent2D* extent) {
    (void)jmethods;
    (void)windowId;
    unsigned width = 1280, height = 720;
    const char* value = getenv("VORTEK_HEADLESS_SIZE");
    if (value) sscanf(value, "%ux%u", &width, &height);
    extent->width = width;
    extent->height = height;
}

int getSurfaceMinImageCount() {
    /* Zero-copy keeps one image on screen; with fewer than two the app could never acquire. */
    return envInt("VORTEK_AHB_SWAPCHAIN", 1) ? 2 : 1;
}

VkSurfaceFormatKHR* getSurfaceFormats(uint32_t* formatCount) {
    static const VkFormat supportedFormats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    int supportedFormatCount = ARRAY_SIZE(supportedFormats);
    VkSurfaceFormatKHR* surfaceFormats = calloc(supportedFormatCount, sizeof(VkSurfaceFormatKHR));

    if (formatCount) *formatCount = supportedFormatCount;

    for (int i = 0; i < supportedFormatCount; i++) {
        surfaceFormats[i].format = supportedFormats[i];
        surfaceFormats[i].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }

    return surfaceFormats;
}

static uint32_t ahbFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB: return 5; /* HAL_PIXEL_FORMAT_BGRA_8888 */
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB: return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT;
        default: return 0;
    }
}

/* The image is created for the AHB and bound to it as dedicated imported memory, so the
 * driver renders straight into a buffer the compositor can later scan out. */
static VkResult createHardwareBufferImage(VkDevice device, XWindowSwapchain* swapchain, VkImageCreateInfo* imageInfo,
                                          XWindowSwapchain_Image* swapchainImage, AHardwareBuffer** pHardwareBuffer) {
    uint32_t format = ahbFormat(swapchain->imageFormat);
    if (!format || !vulkanWrapper.vkGetAndroidHardwareBufferPropertiesANDROID) return VK_ERROR_FORMAT_NOT_SUPPORTED;

    AHardwareBuffer_Desc desc = {0};
    desc.width = swapchain->imageExtent.width;
    desc.height = swapchain->imageExtent.height;
    desc.layers = 1;
    desc.format = format;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    AHardwareBuffer* hardwareBuffer = NULL;
    if (AHardwareBuffer_allocate(&desc, &hardwareBuffer) != 0 || !hardwareBuffer) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    VkExternalMemoryImageCreateInfo externalInfo = {0};
    externalInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    externalInfo.pNext = (void*)imageInfo->pNext;
    VkImageCreateInfo info = *imageInfo;
    info.pNext = &externalInfo;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult result = vulkanWrapper.vkCreateImage(device, &info, NULL, &image);
    if (result == VK_SUCCESS) {
        VkAndroidHardwareBufferPropertiesANDROID properties = {0};
        properties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
        result = vulkanWrapper.vkGetAndroidHardwareBufferPropertiesANDROID(device, hardwareBuffer, &properties);
        if (result == VK_SUCCESS) {
            VkImportAndroidHardwareBufferInfoANDROID importInfo = {0};
            importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
            importInfo.buffer = hardwareBuffer;
            VkMemoryDedicatedAllocateInfo dedicatedInfo = {0};
            dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            dedicatedInfo.pNext = &importInfo;
            dedicatedInfo.image = image;
            VkMemoryAllocateInfo memoryInfo = {0};
            memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            memoryInfo.pNext = &dedicatedInfo;
            memoryInfo.allocationSize = properties.allocationSize;
            memoryInfo.memoryTypeIndex = getMemoryTypeIndex(properties.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            result = vulkanWrapper.vkAllocateMemory(device, &memoryInfo, NULL, &memory);
        }
        if (result == VK_SUCCESS) result = vulkanWrapper.vkBindImageMemory(device, image, memory, 0);
    }
    if (result != VK_SUCCESS) {
        if (memory) vulkanWrapper.vkFreeMemory(device, memory, NULL);
        if (image) vulkanWrapper.vkDestroyImage(device, image, NULL);
        AHardwareBuffer_release(hardwareBuffer);
        return result;
    }

    swapchainImage->image = image;
    swapchainImage->memory = memory;
    *pHardwareBuffer = hardwareBuffer;
    return VK_SUCCESS;
}

static VkResult createImage(VkDevice device, XWindowSwapchain* swapchain, XWindowSwapchain_Image* swapchainImage, AHardwareBuffer** pHardwareBuffer) {
    VkImageCreateInfo imageInfo = {0};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = swapchain->imageFormat;
    imageInfo.extent.width = swapchain->imageExtent.width;
    imageInfo.extent.height = swapchain->imageExtent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = swapchain->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (envInt("VORTEK_AHB_SWAPCHAIN", 1)) {
        VkResult result = createHardwareBufferImage(device, swapchain, &imageInfo, swapchainImage, pHardwareBuffer);
        if (result == VK_SUCCESS) return result;
        println("vortek-server: AHardwareBuffer swapchain image failed (%d), using a plain image", result);
    }

    VkImage image;
    VkResult result = vulkanWrapper.vkCreateImage(device, &imageInfo, NULL, &image);
    if (result != VK_SUCCESS) return result;

    VkMemoryRequirements requirements;
    vulkanWrapper.vkGetImageMemoryRequirements(device, image, &requirements);

    VkMemoryAllocateInfo memoryInfo = {0};
    memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memoryInfo.allocationSize = requirements.size;
    memoryInfo.memoryTypeIndex = getMemoryTypeIndex(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory memory;
    result = vulkanWrapper.vkAllocateMemory(device, &memoryInfo, NULL, &memory);
    if (result != VK_SUCCESS) {
        vulkanWrapper.vkDestroyImage(device, image, NULL);
        return result;
    }

    result = vulkanWrapper.vkBindImageMemory(device, image, memory, 0);
    if (result != VK_SUCCESS) {
        vulkanWrapper.vkDestroyImage(device, image, NULL);
        vulkanWrapper.vkFreeMemory(device, memory, NULL);
        return result;
    }

    swapchainImage->image = image;
    swapchainImage->memory = memory;
    return VK_SUCCESS;
}

XWindowSwapchain* XWindowSwapchain_create(VkDevice device, uint32_t graphicsQueueIndex, VkSwapchainCreateInfoKHR* swapchainInfo, JMethods* jmethods, int windowId) {
    HeadlessSwapchain* headless = calloc(1, sizeof(HeadlessSwapchain));
    XWindowSwapchain* swapchain = &headless->base;
    headless->device = device;
    swapchain->windowId = windowId;
    swapchain->imageCount = MAX(swapchainInfo->minImageCount, 1);
    swapchain->images = calloc(swapchain->imageCount, sizeof(XWindowSwapchain_Image));
    headless->hardwareBuffers = calloc(swapchain->imageCount, sizeof(AHardwareBuffer*));
    swapchain->imageFormat = swapchainInfo->imageFormat;
    swapchain->imageUsage = swapchainInfo->imageUsage;
    memcpy(&swapchain->imageExtent, &swapchainInfo->imageExtent, sizeof(VkExtent2D));
    swapchain->jmethods = jmethods;

    for (int i = 0; i < swapchain->imageCount; i++) {
        if (createImage(device, swapchain, &swapchain->images[i], &headless->hardwareBuffers[i]) != VK_SUCCESS) {
            XWindowSwapchain_destroy(device, swapchain);
            return NULL;
        }
    }

    vulkanWrapper.vkGetDeviceQueue(device, graphicsQueueIndex, 0, &swapchain->queue);

    VkCommandPoolCreateInfo poolInfo = {0};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueIndex;
    vulkanWrapper.vkCreateCommandPool(device, &poolInfo, NULL, &headless->commandPool);

    registerZeroCopy(headless);
    println("vortek-server: headless swapchain %dx%d format=%d images=%d window=%d%s",
            swapchain->imageExtent.width, swapchain->imageExtent.height, swapchain->imageFormat, swapchain->imageCount, windowId,
            headless->zeroCopy ? " (zero-copy)" : headless->hardwareBuffers[0] ? " (AHardwareBuffer)" : "");
    return swapchain;
}

void XWindowSwapchain_destroy(VkDevice device, XWindowSwapchain* swapchain) {
    if (!swapchain) return;
    HeadlessSwapchain* headless = (HeadlessSwapchain*)swapchain;

    if (swapchain->queue) vulkanWrapper.vkQueueWaitIdle(swapchain->queue);
    unregisterZeroCopy(headless);

    for (int i = 0; i < swapchain->imageCount; i++) {
        if (swapchain->images[i].image) vulkanWrapper.vkDestroyImage(device, swapchain->images[i].image, NULL);
        if (swapchain->images[i].memory) vulkanWrapper.vkFreeMemory(device, swapchain->images[i].memory, NULL);
        if (headless->hardwareBuffers && headless->hardwareBuffers[i]) AHardwareBuffer_release(headless->hardwareBuffers[i]);
    }
    MEMFREE(headless->hardwareBuffers);

    if (headless->dumpBuffer) vulkanWrapper.vkDestroyBuffer(device, headless->dumpBuffer, NULL);
    if (headless->dumpMemory) vulkanWrapper.vkFreeMemory(device, headless->dumpMemory, NULL);
    if (headless->commandPool) vulkanWrapper.vkDestroyCommandPool(device, headless->commandPool, NULL);

    println("vortek-server: headless swapchain destroyed after %llu presents", (unsigned long long)headless->presentCount);
    MEMFREE(swapchain->images);
    free(headless);
}

/* Zero-copy: an image the compositor still shows cannot be handed out. Waits for a free one and
 * for its release fence; never fails, since a negative or timeout result would read as an index. */
static uint32_t acquireZeroCopy(HeadlessSwapchain* headless) {
    XWindowSwapchain* swapchain = &headless->base;
    pthread_mutex_lock(&zcLock);
    int found = -1;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;
    while (found < 0) {
        for (int k = 0; k < swapchain->imageCount && found < 0; k++) {
            int i = (headless->nextImage + k) % swapchain->imageCount;
            if (headless->owner[i] == OWNER_FREE) found = i;
        }
        if (found >= 0 || !headless->zeroCopy) break;
        if (pthread_cond_timedwait(&zcCond, &zcLock, &deadline) != 0) break;
    }
    if (found < 0) {
        found = headless->nextImage;
        if (!headless->stealLogged) println("vortek-server: zero-copy swapchain %u: no image released in 2 s, reusing one", headless->chainId);
        headless->stealLogged = true;
    }
    int fd = headless->releaseFd[found];
    headless->releaseFd[found] = -1;
    headless->owner[found] = OWNER_APP;
    pthread_mutex_unlock(&zcLock);
    if (fd >= 0) {
        struct pollfd p = {fd, POLLIN, 0};
        poll(&p, 1, 2000);
        close(fd);
    }
    return found;
}

VkResult XWindowSwapchain_acquireNextImage(XWindowSwapchain* swapchain, uint64_t timeout, VkSemaphore signalSemaphore, VkFence fence, uint32_t* imageIndex) {
    HeadlessSwapchain* headless = (HeadlessSwapchain*)swapchain;
    (void)timeout;
    uint32_t index = headless->owner ? acquireZeroCopy(headless) : headless->nextImage;

    if (signalSemaphore || fence) {
        VkSubmitInfo submitInfo = {0};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        if (signalSemaphore) {
            submitInfo.pSignalSemaphores = &signalSemaphore;
            submitInfo.signalSemaphoreCount = 1;
        }

        VkResult result = vulkanWrapper.vkQueueSubmit(swapchain->queue, 1, &submitInfo, fence);
        if (result == VK_ERROR_DEVICE_LOST) return result;
    }

    headless->lastAcquired = index;
    headless->nextImage = (index + 1) % swapchain->imageCount;
    *imageIndex = headless->lastAcquired;
    return VK_SUCCESS;
}

/* The app's present: wait for its rendering on the GPU, export that as a sync fence and give the
 * image to the compositor. VK_ERROR_FEATURE_NOT_PRESENT leaves the semaphores untouched so the
 * client can present through X11 instead. */
VkResult XWindowSwapchain_presentZeroCopy(XWindowSwapchain* swapchain, uint32_t index, const VkSemaphore* waitSemaphores,
                                          uint32_t waitCount, int32_t x, int32_t y, int32_t w, int32_t h) {
    HeadlessSwapchain* headless = (HeadlessSwapchain*)swapchain;
    if (!headless || !headless->owner || !headless->zeroCopy || index >= (uint32_t)swapchain->imageCount ||
        !vulkanWrapper.vkGetFenceFd)
        return VK_ERROR_FEATURE_NOT_PRESENT;

    VkExportFenceCreateInfo exportInfo = {0};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
    exportInfo.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    VkFenceCreateInfo fenceInfo = {0};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.pNext = &exportInfo;
    VkFence fence = VK_NULL_HANDLE;
    if (vulkanWrapper.vkCreateFence(headless->device, &fenceInfo, NULL, &fence) != VK_SUCCESS) return VK_ERROR_FEATURE_NOT_PRESENT;

    VkPipelineStageFlags stages[waitCount > 0 ? waitCount : 1];
    for (uint32_t i = 0; i < waitCount; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = waitCount;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = stages;
    VkResult result = vulkanWrapper.vkQueueSubmit(swapchain->queue, 1, &submitInfo, fence);
    if (result != VK_SUCCESS) {
        vulkanWrapper.vkDestroyFence(headless->device, fence, NULL);
        return result;
    }

    int fd = -1;
    VkFenceGetFdInfoKHR fdInfo = {0};
    fdInfo.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
    fdInfo.fence = fence;
    fdInfo.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    if (vulkanWrapper.vkGetFenceFd(headless->device, &fdInfo, &fd) != VK_SUCCESS) {
        vulkanWrapper.vkWaitForFences(headless->device, 1, &fence, VK_TRUE, 1000000000ull);
        fd = -1;
    }
    vulkanWrapper.vkDestroyFence(headless->device, fence, NULL);

    pthread_mutex_lock(&zcLock);
    headless->owner[index] = OWNER_COMPOSITOR;
    pthread_mutex_unlock(&zcLock);
    if (ZeroCopy_present(headless->chainId, index, x, y, w, h, fd)) {
        headless->shown = true;
    }
    else {
        /* The semaphores are consumed: report success and drop this frame, later ones use X11. */
        pthread_mutex_lock(&zcLock);
        headless->owner[index] = OWNER_FREE;
        headless->zeroCopy = false;
        pthread_cond_broadcast(&zcCond);
        pthread_mutex_unlock(&zcLock);
    }
    return VK_SUCCESS;
}

static bool isDumpableFormat(VkFormat format, bool* bgr) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            *bgr = true;
            return true;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            *bgr = false;
            return true;
        default:
            return false;
    }
}

static void dumpImage(HeadlessSwapchain* headless, const char* path) {
    XWindowSwapchain* swapchain = &headless->base;
    VkDevice device = headless->device;
    bool bgr;
    if (!headless->commandPool || !isDumpableFormat(swapchain->imageFormat, &bgr)) return;

    uint32_t width = swapchain->imageExtent.width, height = swapchain->imageExtent.height;
    VkDeviceSize size = (VkDeviceSize)width * height * 4;

    if (!headless->dumpBuffer) {
        VkBufferCreateInfo bufferInfo = {0};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vulkanWrapper.vkCreateBuffer(device, &bufferInfo, NULL, &headless->dumpBuffer) != VK_SUCCESS) return;

        VkMemoryRequirements requirements;
        vulkanWrapper.vkGetBufferMemoryRequirements(device, headless->dumpBuffer, &requirements);

        VkMemoryAllocateInfo memoryInfo = {0};
        memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memoryInfo.allocationSize = requirements.size;
        memoryInfo.memoryTypeIndex = getMemoryTypeIndex(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        if (!(getMemoryPropertyFlags(memoryInfo.memoryTypeIndex) & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
            vulkanWrapper.vkAllocateMemory(device, &memoryInfo, NULL, &headless->dumpMemory) != VK_SUCCESS) {
            vulkanWrapper.vkDestroyBuffer(device, headless->dumpBuffer, NULL);
            headless->dumpBuffer = VK_NULL_HANDLE;
            return;
        }
        vulkanWrapper.vkBindBufferMemory(device, headless->dumpBuffer, headless->dumpMemory, 0);
        headless->dumpSize = size;

        VkCommandBufferAllocateInfo allocInfo = {0};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = headless->commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        vulkanWrapper.vkAllocateCommandBuffers(device, &allocInfo, &headless->commandBuffer);
    }

    VkImage image = swapchain->images[headless->lastAcquired].image;
    VkCommandBuffer cmd = headless->commandBuffer;

    /* Presents carry no server-side wait here (upstream also ignores the wait
     * semaphores); for a debug dump just drain the queue. */
    vulkanWrapper.vkQueueWaitIdle(swapchain->queue);
    vulkanWrapper.vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo = {0};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vulkanWrapper.vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vulkanWrapper.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = width;
    region.imageExtent.height = height;
    region.imageExtent.depth = 1;
    vulkanWrapper.vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, headless->dumpBuffer, 1, &region);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vulkanWrapper.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    vulkanWrapper.vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    if (vulkanWrapper.vkQueueSubmit(swapchain->queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) return;
    vulkanWrapper.vkQueueWaitIdle(swapchain->queue);

    uint8_t* pixels = NULL;
    if (vulkanWrapper.vkMapMemory(device, headless->dumpMemory, 0, VK_WHOLE_SIZE, 0, (void**)&pixels) != VK_SUCCESS) return;
    VkMappedMemoryRange range = {0};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = headless->dumpMemory;
    range.size = VK_WHOLE_SIZE;
    vulkanWrapper.vkInvalidateMappedMemoryRanges(device, 1, &range);

    char tmpPath[PATH_MAX];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    FILE* file = fopen(tmpPath, "wb");
    if (file) {
        fprintf(file, "P6\n%u %u\n255\n", width, height);
        uint8_t* row = malloc(width * 3);
        for (uint32_t y = 0; y < height; y++) {
            const uint8_t* src = pixels + (size_t)y * width * 4;
            for (uint32_t x = 0; x < width; x++) {
                row[x * 3 + 0] = src[x * 4 + (bgr ? 2 : 0)];
                row[x * 3 + 1] = src[x * 4 + 1];
                row[x * 3 + 2] = src[x * 4 + (bgr ? 0 : 2)];
            }
            fwrite(row, 1, width * 3, file);
        }
        free(row);
        fclose(file);
        rename(tmpPath, path);
    }
    vulkanWrapper.vkUnmapMemory(device, headless->dumpMemory);
}

void XWindowSwapchain_presentImage(XWindowSwapchain* swapchain) {
    if (!swapchain) return;
    HeadlessSwapchain* headless = (HeadlessSwapchain*)swapchain;
    headless->presentCount++;
    if (headless->owner) {
        /* An image the app still holds was presented through X11: free it, and take the layer down. */
        bool hide = false;
        pthread_mutex_lock(&zcLock);
        if (headless->owner[headless->lastAcquired] == OWNER_APP) {
            headless->owner[headless->lastAcquired] = OWNER_FREE;
            hide = headless->shown;
            headless->shown = false;
            pthread_cond_broadcast(&zcCond);
        }
        pthread_mutex_unlock(&zcLock);
        if (hide) ZeroCopy_hide(headless->chainId);
    }

    int logEvery = envInt("VORTEK_PRESENT_LOG", 60);
    if (logEvery > 0 && (headless->presentCount == 1 || headless->presentCount % logEvery == 0))
        println("vortek-server: present #%llu (image %u, window %d)", (unsigned long long)headless->presentCount, headless->lastAcquired, swapchain->windowId);

    const char* dumpPath = getenv("VORTEK_DUMP_PPM");
    int dumpEvery = MAX(envInt("VORTEK_DUMP_EVERY", 1), 1);
    if (dumpPath && *dumpPath && headless->presentCount % dumpEvery == 0) dumpImage(headless, dumpPath);
}

/* REQUEST_CODE_DD_PRESENT_ZERO_COPY, dispatched by the patched getHandleRequestFunc. */
void droiddeck_handlePresentZeroCopy(VkContext* context) {
    VkResult result = VK_ERROR_FEATURE_NOT_PRESENT;
    DDPresentZeroCopy request;
    if (context->inputBufferSize >= (int)sizeof(request)) {
        memcpy(&request, context->inputBuffer, sizeof(request));
        uint32_t count = request.waitCount;
        if (count <= 16 && context->inputBufferSize >= (int)(sizeof(request) + count * sizeof(uint64_t))) {
            VkSemaphore semaphores[16];
            for (uint32_t i = 0; i < count; i++) {
                uint64_t id;
                memcpy(&id, context->inputBuffer + sizeof(request) + i * sizeof(uint64_t), sizeof(id));
                semaphores[i] = VkObject_fromId(id);
            }
            XWindowSwapchain* swapchain = VkObject_fromId(request.swapchainId);
            result = XWindowSwapchain_presentZeroCopy(swapchain, request.imageIndex, semaphores, count,
                                                      request.x, request.y, request.w, request.h);
            if (result == VK_ERROR_DEVICE_LOST) context->status = result;
        }
    }
    vt_send(context->clientRing, result, NULL, 0);
}

