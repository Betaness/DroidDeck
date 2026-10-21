/*
 * Software X11 presentation for the standalone Vortek client (glibc side).
 *
 * The server keeps rendering into its own swapchain images (headless_swapchain.c).
 * On vkQueuePresentKHR the client records, through the normal Vortek forwarding,
 * a copy of the presented image into a host-visible buffer whose memory is
 * already shared with this process (vkMapMemory hands us the DMA-BUF / memfd),
 * submits it waiting on the app's present semaphores, waits for the fence and
 * writes the pixels into the X window: MIT-SHM (memfd via ShmAttachFd, or SysV)
 * with xcb_shm_put_image when available, otherwise xcb_put_image in large bands.
 * Pixels cross the CPU once (GPU-shared buffer -> SHM segment, swizzling R/B for
 * RGBA swapchains); with plain PutImage they are written straight from the
 * mapping into the socket.
 *
 * No protocol change: only existing Vulkan requests are used. The server just
 * has to accept any swapchain extent (patches/winlator-0002-x11-present.patch).
 *
 * Environment:
 *   VORTEK_X11_SHM=0     disable MIT-SHM (use PutImage)
 *   VORTEK_X11_STATS=N   print presented frames / fps every N seconds to stderr
 *
 * Copyright (C) 2026 mali-v15-bringup contributors.
 * Based on Vortek (C) brunodev85, LGPL-2.1.
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>
#include <xcb/xcb.h>
#include <xcb/shm.h>

#include "vortek.h"
#include "client_x11_present.h"

#define SHM_BUFFER_COUNT 2

extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName);

typedef struct X11Surface {
    xcb_connection_t* conn;
    xcb_window_t window;
} X11Surface;

typedef struct ShmBuffer {
    uint8_t* data;
    uint32_t seg;
    int shmId;           /* SysV id, or -1 for memfd */
    bool pending;
    xcb_get_input_focus_cookie_t sync;
} ShmBuffer;

typedef struct X11Swapchain {
    X11Surface* surface;
    VkDevice device;
    VkSwapchainKHR handle;
    VkFormat format;
    VkExtent2D extent;
    bool swapRB;
    bool ready;
    bool failed;

    uint32_t imageCount;
    VkImage* images;
    VkCommandPool commandPool;
    VkCommandBuffer* commandBuffers;
    VkFence fence;
    VkBuffer buffer;
    VkDeviceMemory memory;
    bool coherent;
    uint8_t* pixels;
    size_t frameSize;

    xcb_gcontext_t gc;
    bool useShm;
    ShmBuffer shm[SHM_BUFFER_COUNT];
    uint32_t shmIndex;
    uint32_t maxRowsPerPut;
    uint8_t* swizzleRow;

    uint64_t frames, statFrames;
    double statStart;
} X11Swapchain;

static struct {
    bool loaded;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges;
} vk;

static void loadFunctions() {
    if (vk.loaded) return;
#define LOAD(name) vk.name = (PFN_vk##name)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vk" #name)
    LOAD(GetPhysicalDeviceMemoryProperties);
    LOAD(GetSwapchainImagesKHR);
    LOAD(CreateCommandPool);
    LOAD(DestroyCommandPool);
    LOAD(AllocateCommandBuffers);
    LOAD(BeginCommandBuffer);
    LOAD(EndCommandBuffer);
    LOAD(CmdPipelineBarrier);
    LOAD(CmdCopyImageToBuffer);
    LOAD(CreateFence);
    LOAD(DestroyFence);
    LOAD(WaitForFences);
    LOAD(ResetFences);
    LOAD(QueueSubmit);
    LOAD(CreateBuffer);
    LOAD(DestroyBuffer);
    LOAD(GetBufferMemoryRequirements);
    LOAD(AllocateMemory);
    LOAD(FreeMemory);
    LOAD(BindBufferMemory);
    LOAD(MapMemory);
    LOAD(UnmapMemory);
    LOAD(InvalidateMappedMemoryRanges);
#undef LOAD
    vk.loaded = true;
}

static int envInt(const char* name, int defaultValue) {
    const char* value = getenv(name);
    return value && *value ? atoi(value) : defaultValue;
}

static double nowSeconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- surfaces ---- */

void X11Present_initXlibSurface(VkObject* surfaceObject, Display* dpy, Window window) {
    static xcb_connection_t* (*getXCBConnection)(Display*) = NULL;
    if (!dpy) return; /* headless: nothing to show (tests/present_test.c) */
    if (!getXCBConnection) {
        void* lib = dlopen("libX11-xcb.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (lib) getXCBConnection = dlsym(lib, "XGetXCBConnection");
        if (!getXCBConnection) {
            fprintf(stderr, "vortek-x11: libX11-xcb.so.1 not found, Xlib surface will not be shown\n");
            return;
        }
    }
    X11Surface* surface = calloc(1, sizeof(X11Surface));
    surface->conn = getXCBConnection(dpy);
    surface->window = (xcb_window_t)window;
    surfaceObject->tag = surface;
}

VkResult vt_call_vkCreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
    VkObject* surfaceObject = VkObject_create(VK_OBJECT_TYPE_SURFACE_KHR, (uint64_t)pCreateInfo->window);
    if (pCreateInfo->connection) {
        X11Surface* surface = calloc(1, sizeof(X11Surface));
        surface->conn = pCreateInfo->connection;
        surface->window = pCreateInfo->window;
        surfaceObject->tag = surface;
    }
    *pSurface = VkObject_toHandle(surfaceObject);
    return VK_SUCCESS;
}

VkBool32 vt_call_vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, xcb_connection_t* connection, xcb_visualid_t visual_id) {
    return VK_TRUE;
}

void X11Present_destroySurface(VkObject* surfaceObject) {
    if (!VKOBJECT_IS_NULL(surfaceObject)) MEMFREE(surfaceObject->tag);
}

static bool getWindowGeometry(X11Surface* surface, uint32_t* width, uint32_t* height, uint8_t* depth) {
    xcb_get_geometry_reply_t* reply = xcb_get_geometry_reply(surface->conn, xcb_get_geometry(surface->conn, surface->window), NULL);
    if (!reply) return false;
    *width = reply->width;
    *height = reply->height;
    if (depth) *depth = reply->depth;
    free(reply);
    return true;
}

void X11Present_fixSurfaceCapabilities(VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* capabilities) {
    X11Surface* x11Surface = VkObject_fromHandle(surface)->tag;
    uint32_t width, height;
    if (!x11Surface || !getWindowGeometry(x11Surface, &width, &height, NULL)) return;
    capabilities->currentExtent.width = width;
    capabilities->currentExtent.height = height;
    capabilities->minImageExtent.width = 1;
    capabilities->minImageExtent.height = 1;
    capabilities->maxImageExtent.width = MAX(width, 16384);
    capabilities->maxImageExtent.height = MAX(height, 16384);
    capabilities->supportedCompositeAlpha |= VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

/* ---- bookkeeping ---- */

void X11Present_onCreateDevice(VkPhysicalDevice physicalDevice, VkDevice device) {
    VkObject* deviceObject = VkObject_fromHandle(device);
    if (!VKOBJECT_IS_NULL(deviceObject)) deviceObject->tag = physicalDevice;
}

void X11Present_onGetDeviceQueue(VkQueue queue, uint32_t queueFamilyIndex) {
    VkObject* queueObject = VkObject_fromHandle(queue);
    if (!VKOBJECT_IS_NULL(queueObject)) queueObject->tag = (void*)(uintptr_t)(queueFamilyIndex + 1);
}

/* ---- swapchains ---- */

void X11Present_onCreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* createInfo, VkSwapchainKHR swapchain) {
    X11Surface* surface = VkObject_fromHandle(createInfo->surface)->tag;
    VkObject* swapchainObject = VkObject_fromHandle(swapchain);
    if (!surface || VKOBJECT_IS_NULL(swapchainObject)) return;

    X11Swapchain* sc = calloc(1, sizeof(X11Swapchain));
    sc->surface = surface;
    sc->device = device;
    sc->handle = swapchain;
    sc->format = createInfo->imageFormat;
    sc->extent = createInfo->imageExtent;
    for (int i = 0; i < SHM_BUFFER_COUNT; i++) sc->shm[i].shmId = -1;

    switch (sc->format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            sc->swapRB = false;
            break;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            sc->swapRB = true;
            break;
        default:
            fprintf(stderr, "vortek-x11: unsupported swapchain format %d, not presenting\n", sc->format);
            sc->failed = true;
            break;
    }
    swapchainObject->tag = sc;
}

static uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, bool* coherent) {
    VkPhysicalDeviceMemoryProperties props;
    vk.GetPhysicalDeviceMemoryProperties(physicalDevice, &props);
    const VkMemoryPropertyFlags wanted[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
    };
    for (int w = 0; w < ARRAY_SIZE(wanted); w++) {
        for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
            if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted[w]) == wanted[w]) {
                *coherent = (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
                return i;
            }
        }
    }
    return UINT32_MAX;
}

static void recordCopy(X11Swapchain* sc, VkCommandBuffer cmd, VkImage image) {
    VkCommandBufferBeginInfo beginInfo = {0};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vk.BeginCommandBuffer(cmd, &beginInfo);

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
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = sc->extent.width;
    region.imageExtent.height = sc->extent.height;
    region.imageExtent.depth = 1;
    vk.CmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->buffer, 1, &region);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkBufferMemoryBarrier bufferBarrier = {0};
    bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bufferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bufferBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.buffer = sc->buffer;
    bufferBarrier.size = VK_WHOLE_SIZE;
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          0, 0, NULL, 1, &bufferBarrier, 1, &barrier);
    vk.EndCommandBuffer(cmd);
}

static bool attachShm(X11Swapchain* sc, ShmBuffer* shm, bool useFd) {
    xcb_connection_t* conn = sc->surface->conn;
    shm->seg = xcb_generate_id(conn);
    xcb_generic_error_t* error;

    if (useFd) {
        int fd = memfd_create("vortek-x11", MFD_CLOEXEC);
        if (fd < 0) return false;
        if (ftruncate(fd, sc->frameSize) != 0) { close(fd); return false; }
        void* data = mmap(NULL, sc->frameSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) { close(fd); return false; }
        /* xcb takes ownership of the fd */
        error = xcb_request_check(conn, xcb_shm_attach_fd_checked(conn, shm->seg, fd, 1));
        if (error) {
            free(error);
            munmap(data, sc->frameSize);
            return false;
        }
        shm->data = data;
        shm->shmId = -1;
        return true;
    }

    int shmId = shmget(IPC_PRIVATE, sc->frameSize, IPC_CREAT | 0600);
    if (shmId < 0) return false;
    void* data = shmat(shmId, NULL, 0);
    if (data == (void*)-1) { shmctl(shmId, IPC_RMID, NULL); return false; }
    error = xcb_request_check(conn, xcb_shm_attach_checked(conn, shm->seg, shmId, 1));
    shmctl(shmId, IPC_RMID, NULL);
    if (error) {
        free(error);
        shmdt(data);
        return false;
    }
    shm->data = data;
    shm->shmId = shmId;
    return true;
}

static void detachShm(X11Swapchain* sc, ShmBuffer* shm) {
    if (!shm->data) return;
    xcb_connection_t* conn = sc->surface->conn;
    if (shm->pending) free(xcb_get_input_focus_reply(conn, shm->sync, NULL));
    xcb_shm_detach(conn, shm->seg);
    if (shm->shmId >= 0) shmdt(shm->data);
    else munmap(shm->data, sc->frameSize);
    shm->data = NULL;
}

static const char* setupX11(X11Swapchain* sc) {
    xcb_connection_t* conn = sc->surface->conn;
    sc->gc = xcb_generate_id(conn);
    xcb_create_gc(conn, sc->gc, sc->surface->window, 0, NULL);

    uint32_t maxBytes = xcb_get_maximum_request_length(conn) * 4;
    sc->maxRowsPerPut = MAX((maxBytes - 64) / (sc->extent.width * 4), 1);

    if (!envInt("VORTEK_X11_SHM", 1)) return "PutImage (VORTEK_X11_SHM=0)";

    xcb_shm_query_version_reply_t* version = xcb_shm_query_version_reply(conn, xcb_shm_query_version(conn), NULL);
    if (!version) return "PutImage (no MIT-SHM)";
    bool hasFd = version->major_version > 1 || (version->major_version == 1 && version->minor_version >= 2);
    free(version);

    for (int pass = hasFd ? 0 : 1; pass < 2; pass++) {
        bool ok = true;
        for (int i = 0; i < SHM_BUFFER_COUNT && ok; i++) ok = attachShm(sc, &sc->shm[i], pass == 0);
        if (ok) {
            sc->useShm = true;
            return pass == 0 ? "MIT-SHM (memfd)" : "MIT-SHM (SysV)";
        }
        for (int i = 0; i < SHM_BUFFER_COUNT; i++) detachShm(sc, &sc->shm[i]);
    }
    return "PutImage (MIT-SHM attach failed)";
}

static bool setupSwapchain(X11Swapchain* sc, VkQueue queue) {
    loadFunctions();
    VkDevice device = sc->device;
    VkPhysicalDevice physicalDevice = VkObject_fromHandle(device)->tag;
    if (!physicalDevice) {
        fprintf(stderr, "vortek-x11: unknown physical device for VkDevice\n");
        return false;
    }

    if (vk.GetSwapchainImagesKHR(device, sc->handle, &sc->imageCount, NULL) != VK_SUCCESS || sc->imageCount == 0) return false;
    sc->images = calloc(sc->imageCount, sizeof(VkImage));
    if (vk.GetSwapchainImagesKHR(device, sc->handle, &sc->imageCount, sc->images) != VK_SUCCESS) return false;

    sc->frameSize = (size_t)sc->extent.width * sc->extent.height * 4;

    VkBufferCreateInfo bufferInfo = {0};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sc->frameSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk.CreateBuffer(device, &bufferInfo, NULL, &sc->buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements requirements;
    vk.GetBufferMemoryRequirements(device, sc->buffer, &requirements);
    VkMemoryAllocateInfo memoryInfo = {0};
    memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memoryInfo.allocationSize = requirements.size;
    memoryInfo.memoryTypeIndex = findMemoryType(physicalDevice, requirements.memoryTypeBits, &sc->coherent);
    if (memoryInfo.memoryTypeIndex == UINT32_MAX) return false;
    if (vk.AllocateMemory(device, &memoryInfo, NULL, &sc->memory) != VK_SUCCESS) return false;
    if (vk.BindBufferMemory(device, sc->buffer, sc->memory, 0) != VK_SUCCESS) return false;
    if (vk.MapMemory(device, sc->memory, 0, VK_WHOLE_SIZE, 0, (void**)&sc->pixels) != VK_SUCCESS) return false;

    uintptr_t family = (uintptr_t)VkObject_fromHandle(queue)->tag;
    VkCommandPoolCreateInfo poolInfo = {0};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = family ? family - 1 : 0;
    if (vk.CreateCommandPool(device, &poolInfo, NULL, &sc->commandPool) != VK_SUCCESS) return false;

    sc->commandBuffers = calloc(sc->imageCount, sizeof(VkCommandBuffer));
    VkCommandBufferAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = sc->commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = sc->imageCount;
    if (vk.AllocateCommandBuffers(device, &allocInfo, sc->commandBuffers) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < sc->imageCount; i++) recordCopy(sc, sc->commandBuffers[i], sc->images[i]);

    VkFenceCreateInfo fenceInfo = {0};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vk.CreateFence(device, &fenceInfo, NULL, &sc->fence) != VK_SUCCESS) return false;

    const char* mode = setupX11(sc);
    if (sc->swapRB && !sc->useShm) sc->swizzleRow = malloc((size_t)sc->extent.width * 4 * MIN(sc->maxRowsPerPut, sc->extent.height));
    fprintf(stderr, "vortek-x11: window 0x%x %ux%u format %d, %u images, %s%s\n", sc->surface->window,
            sc->extent.width, sc->extent.height, sc->format, sc->imageCount, mode, sc->swapRB ? ", R/B swizzle" : "");
    sc->statStart = nowSeconds();
    return true;
}

static void releaseSwapchain(X11Swapchain* sc) {
    VkDevice device = sc->device;
    for (int i = 0; i < SHM_BUFFER_COUNT; i++) detachShm(sc, &sc->shm[i]);
    if (sc->gc) xcb_free_gc(sc->surface->conn, sc->gc);
    if (sc->gc) xcb_flush(sc->surface->conn);
    if (sc->fence) vk.DestroyFence(device, sc->fence, NULL);
    if (sc->commandPool) vk.DestroyCommandPool(device, sc->commandPool, NULL);
    if (sc->pixels) vk.UnmapMemory(device, sc->memory);
    if (sc->buffer) vk.DestroyBuffer(device, sc->buffer, NULL);
    if (sc->memory) vk.FreeMemory(device, sc->memory, NULL);
    MEMFREE(sc->commandBuffers);
    MEMFREE(sc->images);
    MEMFREE(sc->swizzleRow);
}

void X11Present_onDestroySwapchain(VkDevice device, VkSwapchainKHR swapchain) {
    VkObject* swapchainObject = VkObject_fromHandle(swapchain);
    X11Swapchain* sc = swapchainObject->tag;
    if (!sc) return;
    if (sc->ready) {
        fprintf(stderr, "vortek-x11: swapchain %ux%u destroyed after %llu frames\n", sc->extent.width, sc->extent.height, (unsigned long long)sc->frames);
    }
    if (vk.loaded) releaseSwapchain(sc);
    free(sc);
    swapchainObject->tag = NULL;
}

/* Copy rows from the (possibly uncached) GPU mapping, swapping R and B. */
static void swizzleRows(uint32_t* restrict dst, const uint32_t* restrict src, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint32_t v = src[i];
        dst[i] = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu);
    }
}

static void putFrame(X11Swapchain* sc, uint8_t depth) {
    xcb_connection_t* conn = sc->surface->conn;
    uint32_t width = sc->extent.width, height = sc->extent.height;
    size_t stride = (size_t)width * 4;

    if (sc->useShm) {
        ShmBuffer* shm = &sc->shm[sc->shmIndex];
        sc->shmIndex = (sc->shmIndex + 1) % SHM_BUFFER_COUNT;
        if (shm->pending) {
            free(xcb_get_input_focus_reply(conn, shm->sync, NULL));
            shm->pending = false;
        }
        if (sc->swapRB) swizzleRows((uint32_t*)shm->data, (const uint32_t*)sc->pixels, (size_t)width * height);
        else memcpy(shm->data, sc->pixels, sc->frameSize);

        xcb_shm_put_image(conn, sc->surface->window, sc->gc, width, height, 0, 0, width, height, 0, 0,
                          depth, XCB_IMAGE_FORMAT_Z_PIXMAP, 0, shm->seg, 0);
        shm->sync = xcb_get_input_focus(conn);
        shm->pending = true;
    }
    else {
        uint32_t rowsPerPut = MIN(sc->maxRowsPerPut, height);
        for (uint32_t y = 0; y < height; y += rowsPerPut) {
            uint32_t rows = MIN(rowsPerPut, height - y);
            const uint8_t* src = sc->pixels + y * stride;
            if (sc->swapRB) {
                swizzleRows((uint32_t*)sc->swizzleRow, (const uint32_t*)src, (size_t)width * rows);
                src = sc->swizzleRow;
            }
            xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, sc->surface->window, sc->gc, width, rows, 0, y, 0, depth, rows * stride, src);
        }
        /* PutImage data is copied into the socket, so the source can be reused at once;
         * a round trip keeps us from queueing frames faster than X consumes them. */
        free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL));
    }
    xcb_flush(conn);
}

VkResult X11Present_present(VkQueue queue, const VkPresentInfoKHR* presentInfo, VkResult* results) {
    VkResult overall = VK_SUCCESS;
    bool waitedSemaphores = false;

    for (uint32_t i = 0; i < presentInfo->swapchainCount; i++) {
        VkResult result = VK_SUCCESS;
        X11Swapchain* sc = VkObject_fromHandle(presentInfo->pSwapchains[i])->tag;
        if (sc && !sc->ready && !sc->failed) {
            sc->ready = setupSwapchain(sc, queue);
            if (!sc->ready) {
                fprintf(stderr, "vortek-x11: swapchain setup failed, frames will not be shown\n");
                releaseSwapchain(sc);
                sc->failed = true;
            }
        }

        if (sc && sc->ready) {
            uint32_t imageIndex = presentInfo->pImageIndices[i];
            xcb_get_geometry_cookie_t geometryCookie = xcb_get_geometry(sc->surface->conn, sc->surface->window);
            xcb_flush(sc->surface->conn);

            VkPipelineStageFlags waitStages[presentInfo->waitSemaphoreCount + 1];
            for (uint32_t s = 0; s < presentInfo->waitSemaphoreCount; s++) waitStages[s] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo submitInfo = {0};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            if (!waitedSemaphores) {
                submitInfo.waitSemaphoreCount = presentInfo->waitSemaphoreCount;
                submitInfo.pWaitSemaphores = presentInfo->pWaitSemaphores;
                submitInfo.pWaitDstStageMask = waitStages;
                waitedSemaphores = true;
            }
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &sc->commandBuffers[imageIndex % sc->imageCount];

            result = vk.QueueSubmit(queue, 1, &submitInfo, sc->fence);
            if (result == VK_SUCCESS) result = vk.WaitForFences(sc->device, 1, &sc->fence, VK_TRUE, UINT64_MAX);
            if (result == VK_SUCCESS) vk.ResetFences(sc->device, 1, &sc->fence);

            xcb_get_geometry_reply_t* geometry = xcb_get_geometry_reply(sc->surface->conn, geometryCookie, NULL);
            if (!geometry) {
                if (result == VK_SUCCESS) result = VK_ERROR_SURFACE_LOST_KHR;
            }
            else if (result == VK_SUCCESS) {
                if (!sc->coherent) {
                    VkMappedMemoryRange range = {0};
                    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                    range.memory = sc->memory;
                    range.size = VK_WHOLE_SIZE;
                    vk.InvalidateMappedMemoryRanges(sc->device, 1, &range);
                }
                putFrame(sc, geometry->depth);
                if (geometry->width != sc->extent.width || geometry->height != sc->extent.height) result = VK_SUBOPTIMAL_KHR;
            }
            free(geometry);

            sc->frames++;
            sc->statFrames++;
            int statEvery = envInt("VORTEK_X11_STATS", 0);
            if (statEvery > 0) {
                double now = nowSeconds();
                if (now - sc->statStart >= statEvery) {
                    fprintf(stderr, "vortek-x11: %llu frames, %.1f fps\n", (unsigned long long)sc->frames, sc->statFrames / (now - sc->statStart));
                    sc->statStart = now;
                    sc->statFrames = 0;
                }
            }
        }

        if (results) results[i] = result;
        if (result < 0 || (overall == VK_SUCCESS && result != VK_SUCCESS)) {
            if (overall >= 0) overall = result;
        }
    }
    return overall;
}
