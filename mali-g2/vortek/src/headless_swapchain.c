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
 *
 * Copyright (C) 2026 mali-v15-bringup contributors.
 * Based on Winlator's vortekrenderer (C) brunodev85, LGPL-2.1.
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include "xwindow_swapchain.h"
#include "vulkan_helper.h"

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
} HeadlessSwapchain;

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
    return 1;
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

static VkResult createImage(VkDevice device, XWindowSwapchain* swapchain, XWindowSwapchain_Image* swapchainImage) {
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
    swapchain->imageFormat = swapchainInfo->imageFormat;
    swapchain->imageUsage = swapchainInfo->imageUsage;
    memcpy(&swapchain->imageExtent, &swapchainInfo->imageExtent, sizeof(VkExtent2D));
    swapchain->jmethods = jmethods;

    for (int i = 0; i < swapchain->imageCount; i++) {
        if (createImage(device, swapchain, &swapchain->images[i]) != VK_SUCCESS) {
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

    println("vortek-server: headless swapchain %dx%d format=%d images=%d window=%d",
            swapchain->imageExtent.width, swapchain->imageExtent.height, swapchain->imageFormat, swapchain->imageCount, windowId);
    return swapchain;
}

void XWindowSwapchain_destroy(VkDevice device, XWindowSwapchain* swapchain) {
    if (!swapchain) return;
    HeadlessSwapchain* headless = (HeadlessSwapchain*)swapchain;

    if (swapchain->queue) vulkanWrapper.vkQueueWaitIdle(swapchain->queue);

    for (int i = 0; i < swapchain->imageCount; i++) {
        if (swapchain->images[i].image) vulkanWrapper.vkDestroyImage(device, swapchain->images[i].image, NULL);
        if (swapchain->images[i].memory) vulkanWrapper.vkFreeMemory(device, swapchain->images[i].memory, NULL);
    }

    if (headless->dumpBuffer) vulkanWrapper.vkDestroyBuffer(device, headless->dumpBuffer, NULL);
    if (headless->dumpMemory) vulkanWrapper.vkFreeMemory(device, headless->dumpMemory, NULL);
    if (headless->commandPool) vulkanWrapper.vkDestroyCommandPool(device, headless->commandPool, NULL);

    println("vortek-server: headless swapchain destroyed after %llu presents", (unsigned long long)headless->presentCount);
    MEMFREE(swapchain->images);
    free(headless);
}

VkResult XWindowSwapchain_acquireNextImage(XWindowSwapchain* swapchain, uint64_t timeout, VkSemaphore signalSemaphore, VkFence fence, uint32_t* imageIndex) {
    HeadlessSwapchain* headless = (HeadlessSwapchain*)swapchain;
    (void)timeout;

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

    headless->lastAcquired = headless->nextImage;
    headless->nextImage = (headless->nextImage + 1) % swapchain->imageCount;
    *imageIndex = headless->lastAcquired;
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

    int logEvery = envInt("VORTEK_PRESENT_LOG", 60);
    if (logEvery > 0 && (headless->presentCount == 1 || headless->presentCount % logEvery == 0))
        println("vortek-server: present #%llu (image %u, window %d)", (unsigned long long)headless->presentCount, headless->lastAcquired, swapchain->windowId);

    const char* dumpPath = getenv("VORTEK_DUMP_PPM");
    int dumpEvery = MAX(envInt("VORTEK_DUMP_EVERY", 1), 1);
    if (dumpPath && *dumpPath && headless->presentCount % dumpEvery == 0) dumpImage(headless, dumpPath);
}
