/*
 * Renders FRAMES frames into a swapchain of an X window through the Vulkan loader (run it with
 * tests/run.sh): each frame clears the image to R = frame & 0xff, G = frame >> 8, B = 0xa5, the
 * marker tests/mock_compositor.c checks on the zero-copy path. Without a compositor the frames are
 * copied into the window, and the window's centre pixel is compared with the last frame at the end.
 *   SC_FRAMES (default 120), SC_SIZE=WxH (default 640x360), SC_POS=X,Y (default 20,30), SC_FORMAT=bgra|rgba, SC_RESIZE=1 resizes
 *   the window halfway and expects VK_SUBOPTIMAL_KHR, then recreates the swapchain, SC_COVER=partly|fully
 *   maps another window over this one for the middle third of the frames, SC_QUEUES=N takes N queues,
 *   SC_SUBMIT_THREAD=1 submits from a second thread all along.
 * Prints SC-PASS / SC-FAIL and the mean acquire, record+submit and present times.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#define VK_USE_PLATFORM_XCB_KHR
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ < 0) { printf("SC-FAIL %s -> %d\n", #x, r_); exit(1); } } while (0)

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static VkDevice device;
static VkPhysicalDevice physical;
static VkQueue queue;
/* The application's own queue lock: SC_SUBMIT_THREAD=1 submits from a second thread meanwhile, as an
 * engine with a submission thread does; acquire is not under it (it uses no application queue). */
static pthread_mutex_t queueLock = PTHREAD_MUTEX_INITIALIZER;
static volatile int stopSubmitting;
static unsigned threadSubmits;

static void* submitThread(void* arg) {
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    if (vkCreateFence(device, &fci, NULL, &fence) != VK_SUCCESS) return NULL;
    while (!stopSubmitting) {
        pthread_mutex_lock(&queueLock);
        VkResult r = vkQueueSubmit(queue, 0, NULL, fence);
        pthread_mutex_unlock(&queueLock);
        if (r != VK_SUCCESS) break;
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &fence);
        threadSubmits++;
    }
    vkDestroyFence(device, fence, NULL);
    return NULL;
}
static VkSurfaceKHR surface;
static VkFormat format;
static VkSwapchainKHR swapchain;
static uint32_t imageCount;
static VkImage images[8];
static VkExtent2D extent;

static void createSwapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    extent = caps.currentExtent;
    VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = caps.minImageCount;
    info.imageFormat = format;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain;
    VkSwapchainKHR created;
    CHECK(vkCreateSwapchainKHR(device, &info, NULL, &created));
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, NULL);
    swapchain = created;
    imageCount = 8;
    CHECK(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images));
    printf("swapchain %ux%u, %u images (minImageCount %u)\n", extent.width, extent.height, imageCount, caps.minImageCount);
}

int main(void) {
    int frames = getenv("SC_FRAMES") ? atoi(getenv("SC_FRAMES")) : 120;
    unsigned width = 640, height = 360;
    if (getenv("SC_SIZE")) sscanf(getenv("SC_SIZE"), "%ux%u", &width, &height);
    bool rgba = getenv("SC_FORMAT") && !strcmp(getenv("SC_FORMAT"), "rgba");
    bool resize = getenv("SC_RESIZE") && atoi(getenv("SC_RESIZE"));
    const char* cover = getenv("SC_COVER");
    xcb_window_t coverWindow = 0;
    format = rgba ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;

    xcb_connection_t* conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) { printf("SC-FAIL no X display\n"); return 1; }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    xcb_window_t window = xcb_generate_id(conn);
    int posX = 20, posY = 30;
    if (getenv("SC_POS")) sscanf(getenv("SC_POS"), "%d,%d", &posX, &posY);
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, window, screen->root, (int16_t)posX, (int16_t)posY, (uint16_t)width, (uint16_t)height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, NULL);
    xcb_map_window(conn, window);
    xcb_flush(conn);

    const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "swapchain_test", 1, "test", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, instanceExtensions};
    VkInstance instance;
    CHECK(vkCreateInstance(&ii, NULL, &instance));
    uint32_t n = 1;
    CHECK(vkEnumeratePhysicalDevices(instance, &n, &physical));
    VkXcbSurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR, NULL, 0, conn, window};
    CHECK(vkCreateXcbSurfaceKHR(instance, &si, NULL, &surface));
    VkBool32 supported = VK_FALSE;
    CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(physical, 0, surface, &supported));
    if (!supported) { printf("SC-FAIL family 0 cannot present\n"); return 1; }

    /* SC_QUEUES=N asks for N queues of family 0; with all of them taken the ICD shares one for acquire. */
    float priorities[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    uint32_t queues = getenv("SC_QUEUES") ? (uint32_t)atoi(getenv("SC_QUEUES")) : 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, queues < 1 ? 1 : queues > 16 ? 16 : queues, priorities};
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qi, 0, NULL, 1, deviceExtensions};
    CHECK(vkCreateDevice(physical, &di, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    pthread_t submitter;
    bool threaded = getenv("SC_SUBMIT_THREAD") && atoi(getenv("SC_SUBMIT_THREAD"));
    if (threaded) pthread_create(&submitter, NULL, submitThread, NULL);
    createSwapchain();

    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pi, NULL, &pool));
    enum { SLOTS = 3 };
    VkCommandBuffer cmds[SLOTS];
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, SLOTS};
    CHECK(vkAllocateCommandBuffers(device, &ai, cmds));
    VkSemaphore acquired[SLOTS], rendered[SLOTS];
    VkFence done[SLOTS];
    for (int i = 0; i < SLOTS; i++) {
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
        CHECK(vkCreateSemaphore(device, &sci, NULL, &acquired[i]));
        CHECK(vkCreateSemaphore(device, &sci, NULL, &rendered[i]));
        CHECK(vkCreateFence(device, &fci, NULL, &done[i]));
    }

    double tAcquire = 0, tSubmit = 0, tPresent = 0;
    int suboptimal = 0, recreated = 0, fails = 0;
    VkClearColorValue last = {{0}};
    for (int f = 0; f < frames; f++) {
        if (resize && f == frames / 2) {
            uint32_t values[] = {width / 2, height / 2};
            xcb_configure_window(conn, window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
            free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL));
        }
        /* SC_COVER=partly|fully: another window over this one for the middle third of the frames. */
        if (cover && f == frames / 3) {
            bool fully = !strcmp(cover, "fully");
            coverWindow = xcb_generate_id(conn);
            xcb_create_window(conn, XCB_COPY_FROM_PARENT, coverWindow, screen->root, fully ? 0 : 200, fully ? 0 : 200,
                              fully ? 1280 : 200, fully ? 720 : 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, NULL);
            xcb_map_window(conn, coverWindow);
            free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL));
        }
        if (cover && f == 2 * frames / 3) {
            xcb_unmap_window(conn, coverWindow);
            free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL));
        }
        int slot = f % SLOTS;
        CHECK(vkWaitForFences(device, 1, &done[slot], VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(device, 1, &done[slot]));
        double t0 = now();
        uint32_t index;
        CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired[slot], VK_NULL_HANDLE, &index));
        double t1 = now();
        VkCommandBuffer cmd = cmds[slot];
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CHECK(vkBeginCommandBuffer(cmd, &bi));
        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[index];
        barrier.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        VkClearColorValue color = {{(f & 0xff) / 255.0f, ((f >> 8) & 0xff) / 255.0f, 0xa5 / 255.0f, 1.0f}};
        last = color;
        vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        CHECK(vkEndCommandBuffer(cmd));
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &acquired[slot], &stage, 1, &cmd, 1, &rendered[slot]};
        pthread_mutex_lock(&queueLock);
        CHECK(vkQueueSubmit(queue, 1, &submit, done[slot]));
        pthread_mutex_unlock(&queueLock);
        double t2 = now();
        VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &rendered[slot], 1, &swapchain, &index, NULL};
        pthread_mutex_lock(&queueLock);
        VkResult r = vkQueuePresentKHR(queue, &present);
        pthread_mutex_unlock(&queueLock);
        double t3 = now();
        if (r == VK_SUBOPTIMAL_KHR) {
            suboptimal++;
            pthread_mutex_lock(&queueLock);
            CHECK(vkDeviceWaitIdle(device));
            pthread_mutex_unlock(&queueLock);
            createSwapchain();
            recreated++;
        }
        else if (r != VK_SUCCESS) {
            printf("present %d -> %d\n", f, r);
            fails++;
        }
        tAcquire += t1 - t0;
        tSubmit += t2 - t1;
        tPresent += t3 - t2;
    }
    if (threaded) {
        stopSubmitting = 1;
        pthread_join(submitter, NULL);
        printf("submission thread: %u submits meanwhile\n", threadSubmits);
    }
    CHECK(vkDeviceWaitIdle(device));
    printf("%d frames: acquire %.3f ms, record+submit %.3f ms, present %.3f ms (means); %d suboptimal, %d recreated\n", frames,
           tAcquire / frames * 1000, tSubmit / frames * 1000, tPresent / frames * 1000, suboptimal, recreated);

    /* Copied frames end up in the window; a zero-copy one is on the compositor's layer instead. */
    int ok = !fails && (!resize || recreated > 0);
    xcb_get_geometry_reply_t* geo = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, window), NULL);
    xcb_get_image_reply_t* img = geo ? xcb_get_image_reply(conn, xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, window, geo->width / 2,
                                                                               geo->height / 2, 1, 1, ~0u), NULL) : NULL;
    if (img && xcb_get_image_data_length(img) >= 4) {
        const uint8_t* p = xcb_get_image_data(img);
        unsigned want[3] = {(unsigned)(last.float32[0] * 255 + 0.5f), (unsigned)(last.float32[1] * 255 + 0.5f), 0xa5};
        bool copied = p[2] == want[0] && p[1] == want[1] && p[0] == want[2];
        printf("window centre pixel R %u G %u B %u (last frame R %u G %u B %u): %s\n", p[2], p[1], p[0], want[0], want[1], want[2],
               copied ? "the last frame was copied into the window" : "not the last frame (zero-copy, or a copy failed)");
        if (getenv("SC_EXPECT_COPY") && atoi(getenv("SC_EXPECT_COPY")) && !copied) ok = 0;
    }
    else if (getenv("SC_EXPECT_COPY") && atoi(getenv("SC_EXPECT_COPY"))) ok = 0;
    free(img);
    free(geo);
    vkDestroySwapchainKHR(device, swapchain, NULL);
    for (int i = 0; i < SLOTS; i++) {
        vkDestroySemaphore(device, acquired[i], NULL);
        vkDestroySemaphore(device, rendered[i], NULL);
        vkDestroyFence(device, done[i], NULL);
    }
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroyInstance(instance, NULL);
    xcb_disconnect(conn);
    printf(ok ? "SC-PASS\n" : "SC-FAIL\n");
    return ok ? 0 : 1;
}
