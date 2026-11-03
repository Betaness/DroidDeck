/*
 * [mali-g2] A game in an emulated display mode under Proton, as win32u (dlls/win32u/vulkan.c) drives the
 * host WSI, against the Vortek client or the libhybris ICD (X11 WSI): the game's 1024x768 fullscreen
 * window is a full-screen top-level X window with the client window inside it, scaled to the screen's
 * height and centred (black bars left and right). Proton's fullscreen hack then creates the host
 * swapchain with the surface's minImageExtent, B8G8R8A8_UNORM, STORAGE usage only, and writes each frame
 * into it with a compute shader (fshack.comp: an image without format, ceil(size / 8) groups of 8x8). On
 * VK_SUBOPTIMAL_KHR / VK_ERROR_OUT_OF_DATE_KHR the swapchain is recreated, as DXVK does through Wine.
 * With a minImageExtent of 1x1 every present was SUBOPTIMAL and the swapchain was recreated every frame
 * (A.V.A. in fullscreen: 31671 swapchains of 1x1, the screen flickering).
 *
 * Each frame writes the marker of the zero-copy mock compositors (R = frame & 0xff, G = frame >> 8,
 * B = 0xa5), so on the zero-copy path the mock checks that the compute writes reached the image.
 *
 *   FS_MODE=fshack (default)  the fullscreen hack: minImageExtent and currentExtent must be the client
 *                             window's size, the host swapchain too, and it must never be recreated
 *   FS_MODE=plain             no fullscreen hack (a windowed game): Wine creates max(game's size,
 *                             minImageExtent), the game's size being the window's; never recreated
 *   FS_MODE=fixed             an application that recreates the same swapchain (FS_APP's size, not the
 *                             window's) on every SUBOPTIMAL: the WSI must stop the loop after
 *                             FS_LIMIT (default 10, VORTEK_RECREATE_LIMIT) recreations within a second;
 *                             with FS_RESIZE=1 the window then changes size at two thirds of the frames
 *                             and the swapchain must report SUBOPTIMAL again, and the loop stop again
 *   FS_LAYOUT=fullscreen (default) | offscreen  offscreen: the windowed A.V.A. layout, the client window
 *                             (FS_APP's size) inside a 1x1 parent (clipped: the X11 path)
 *   FS_APP=WxH                the game's size (default 1280x1024: 900x720 on a 1280x720 screen,
 *                             not a multiple of 8), FS_FRAMES (default 240)
 * Prints FS-PASS / FS-FAIL.
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
#include <time.h>

#include "fshack_spv.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ < 0) { printf("FS-FAIL %s -> %d\n", #x, r_); exit(1); } } while (0)
#define MAXI(a, b) ((a) > (b) ? (a) : (b))

enum { MODE_FSHACK, MODE_PLAIN, MODE_FIXED };

static int mode;
static xcb_connection_t* conn;
static xcb_window_t client;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkExtent2D extent, appSize;
static uint32_t imageCount;
static VkImage images[8];
static VkImageView views[8];
static VkDescriptorSet sets[8];
static VkDescriptorSetLayout setLayout;
static VkDescriptorPool descriptorPool;
static VkPipelineLayout pipelineLayout;
static VkPipeline pipeline;
static int creates;
static bool failed;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void windowSize(uint32_t* w, uint32_t* h) {
    xcb_get_geometry_reply_t* g = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, client), NULL);
    *w = g ? g->width : 0;
    *h = g ? g->height : 0;
    free(g);
}

static void destroyViews(void) {
    for (uint32_t i = 0; i < imageCount; i++) {
        if (views[i]) vkDestroyImageView(device, views[i], NULL);
        views[i] = VK_NULL_HANDLE;
    }
    if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, NULL);
    descriptorPool = VK_NULL_HANDLE;
}

/* What win32u_vkCreateSwapchainKHR hands the host driver for DXVK's swapchain of appSize. */
static void createSwapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    uint32_t w, h;
    windowSize(&w, &h);
    VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = MAXI(caps.minImageCount, 3);
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageArrayLayers = 1;
    info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain;
    if (mode == MODE_FSHACK) {
        /* swapchain->host_extents = capabilities.minImageExtent */
        info.imageExtent = caps.minImageExtent;
        info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        info.imageUsage = VK_IMAGE_USAGE_STORAGE_BIT;
    }
    else {
        /* the game asks for its client rect (Wine's adjusted capabilities), Wine for at least minImageExtent */
        VkExtent2D game = mode == MODE_PLAIN ? (VkExtent2D){w, h} : appSize;
        info.imageExtent.width = MAXI(game.width, caps.minImageExtent.width);
        info.imageExtent.height = MAXI(game.height, caps.minImageExtent.height);
        info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (creates < 3) /* a loop is in the summary */
        printf("window %ux%u: currentExtent %ux%u, minImageExtent %ux%u, maxImageExtent %ux%u -> swapchain %ux%u\n", w, h,
               caps.currentExtent.width, caps.currentExtent.height, caps.minImageExtent.width, caps.minImageExtent.height,
               caps.maxImageExtent.width, caps.maxImageExtent.height, info.imageExtent.width, info.imageExtent.height);
    if (mode == MODE_FSHACK && (caps.minImageExtent.width != w || caps.minImageExtent.height != h || caps.currentExtent.width != w ||
                                caps.currentExtent.height != h)) {
        if (!failed) printf("FS-FAIL minImageExtent / currentExtent are not the window's size\n");
        failed = true;
    }
    VkSwapchainKHR created;
    CHECK(vkCreateSwapchainKHR(device, &info, NULL, &created));
    creates++;
    destroyViews();
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, NULL);
    swapchain = created;
    extent = info.imageExtent;
    /* count first, as DXVK and Wine do (Vortek hands back the array's size as the count otherwise) */
    CHECK(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, NULL));
    if (imageCount > 8) imageCount = 8;
    CHECK(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images));

    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, imageCount};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, imageCount, 1, &size};
    CHECK(vkCreateDescriptorPool(device, &dpi, NULL, &descriptorPool));
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_B8G8R8A8_UNORM;
        vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CHECK(vkCreateImageView(device, &vi, NULL, &views[i]));
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, descriptorPool, 1, &setLayout};
        CHECK(vkAllocateDescriptorSets(device, &ai, &sets[i]));
        VkDescriptorImageInfo ii = {VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii};
        vkUpdateDescriptorSets(device, 1, &wr, 0, NULL);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char* m = getenv("FS_MODE");
    mode = !m || !strcmp(m, "fshack") ? MODE_FSHACK : !strcmp(m, "plain") ? MODE_PLAIN : MODE_FIXED;
    int frames = getenv("FS_FRAMES") ? atoi(getenv("FS_FRAMES")) : 240;
    int limit = getenv("FS_LIMIT") ? atoi(getenv("FS_LIMIT")) : 10;
    bool resize = getenv("FS_RESIZE") && atoi(getenv("FS_RESIZE"));
    bool offscreen = getenv("FS_LAYOUT") && !strcmp(getenv("FS_LAYOUT"), "offscreen");
    appSize = (VkExtent2D){1280, 1024};
    if (getenv("FS_APP")) sscanf(getenv("FS_APP"), "%ux%u", &appSize.width, &appSize.height);

    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        printf("FS-FAIL no X display\n");
        return 1;
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    uint32_t sw = screen->width_in_pixels, sh = screen->height_in_pixels;
    xcb_window_t top = xcb_generate_id(conn);
    client = xcb_generate_id(conn);
    uint32_t black[] = {screen->black_pixel};
    int cx, cy;
    uint32_t cw, ch;
    if (offscreen) {
        /* winex11's offscreen rendering: a 1x1 parent clips the client window */
        xcb_create_window(conn, XCB_COPY_FROM_PARENT, top, screen->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                          XCB_CW_BACK_PIXEL, black);
        cx = cy = 0;
        cw = appSize.width;
        ch = appSize.height;
    }
    else {
        /* the fullscreen top-level window, the client window scaled to the screen and centred (fshack) */
        xcb_create_window(conn, XCB_COPY_FROM_PARENT, top, screen->root, 0, 0, (uint16_t)sw, (uint16_t)sh, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual, XCB_CW_BACK_PIXEL, black);
        double scale = (double)sh / appSize.height < (double)sw / appSize.width ? (double)sh / appSize.height : (double)sw / appSize.width;
        cw = (uint32_t)(appSize.width * scale + 0.5);
        ch = (uint32_t)(appSize.height * scale + 0.5);
        cx = (int)(sw - cw) / 2;
        cy = (int)(sh - ch) / 2;
    }
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, client, top, (int16_t)cx, (int16_t)cy, (uint16_t)cw, (uint16_t)ch, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, NULL);
    xcb_map_window(conn, client);
    xcb_map_window(conn, top);
    free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL));
    printf("screen %ux%u, game %ux%u, client window %ux%u+%d+%d%s\n", sw, sh, appSize.width, appSize.height, cw, ch, cx, cy,
           offscreen ? " in a 1x1 parent" : "");

    const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "fshack_test", 1, "test", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, instanceExtensions};
    VkInstance instance;
    CHECK(vkCreateInstance(&ii, NULL, &instance));
    uint32_t n = 1;
    CHECK(vkEnumeratePhysicalDevices(instance, &n, &physical));
    VkXcbSurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR, NULL, 0, conn, client};
    CHECK(vkCreateXcbSurfaceKHR(instance, &si, NULL, &surface));

    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_B8G8R8A8_UNORM, &fp);
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(physical, &supported);
    printf("B8G8R8A8_UNORM optimal storage image: %s, shaderStorageImageWriteWithoutFormat: %s\n",
           fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT ? "yes" : "no",
           supported.shaderStorageImageWriteWithoutFormat ? "yes" : "no");
    /* win32u enables it for the fullscreen hack */
    VkPhysicalDeviceFeatures features = {0};
    features.shaderStorageImageWriteWithoutFormat = supported.shaderStorageImageWriteWithoutFormat;
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &priority};
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qi, 0, NULL, 1, deviceExtensions, &features};
    CHECK(vkCreateDevice(physical, &di, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);

    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &binding};
    CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &setLayout));
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &setLayout, 1, &push};
    CHECK(vkCreatePipelineLayout(device, &pli, NULL, &pipelineLayout));
    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(fshack_comp), fshack_comp};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(device, &smi, NULL, &module));
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                                                  module, "main", NULL};
    cpi.layout = pipelineLayout;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, NULL, &pipeline));
    vkDestroyShaderModule(device, module, NULL);

    createSwapchain();

    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pi, NULL, &pool));
    enum { SLOTS = 3 };
    VkCommandBuffer cmds[SLOTS];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, SLOTS};
    CHECK(vkAllocateCommandBuffers(device, &cai, cmds));
    VkSemaphore acquired[SLOTS], rendered[SLOTS];
    VkFence done[SLOTS];
    for (int i = 0; i < SLOTS; i++) {
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
        CHECK(vkCreateSemaphore(device, &sci, NULL, &acquired[i]));
        CHECK(vkCreateSemaphore(device, &sci, NULL, &rendered[i]));
        CHECK(vkCreateFence(device, &fci, NULL, &done[i]));
    }

    /* phase 0: before the resize (all frames without FS_RESIZE), phase 1: after it */
    int recreated[2] = {0, 0}, suboptimal[2] = {0, 0}, lateSuboptimal[2] = {0, 0}, fails = 0;
    int resizeAt = resize ? 2 * frames / 3 : frames;
    double t0 = now();
    for (int f = 0; f < frames; f++) {
        int phase = f >= resizeAt;
        if (f == resizeAt) {
            uint32_t values[] = {cw - 100, ch - 60};
            xcb_configure_window(conn, client, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
            free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL));
            printf("frame %d: client window now %ux%u\n", f, values[0], values[1]);
        }
        int slot = f % SLOTS;
        CHECK(vkWaitForFences(device, 1, &done[slot], VK_TRUE, UINT64_MAX));
        uint32_t index;
        VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired[slot], VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            CHECK(vkDeviceWaitIdle(device));
            createSwapchain();
            recreated[phase]++;
            f--;
            continue;
        }
        CHECK(r);
        CHECK(vkResetFences(device, 1, &done[slot]));
        VkCommandBuffer cmd = cmds[slot];
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CHECK(vkBeginCommandBuffer(cmd, &bi));
        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[index];
        barrier.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        float color[4] = {(f & 0xff) / 255.0f, ((f >> 8) & 0xff) / 255.0f, 0xa5 / 255.0f, 1.0f};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &sets[index], 0, NULL);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(color), color);
        vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        CHECK(vkEndCommandBuffer(cmd));
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &acquired[slot], &stage, 1, &cmd, 1, &rendered[slot]};
        CHECK(vkQueueSubmit(queue, 1, &submit, done[slot]));
        VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &rendered[slot], 1, &swapchain, &index, NULL};
        r = vkQueuePresentKHR(queue, &present);
        if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) {
            suboptimal[phase]++;
            /* the last tenth of each phase must be quiet */
            if (f >= (phase ? frames : resizeAt) - frames / 10) lateSuboptimal[phase]++;
            CHECK(vkDeviceWaitIdle(device));
            createSwapchain();
            recreated[phase]++;
        }
        else if (r != VK_SUCCESS) {
            printf("present %d -> %d\n", f, r);
            fails++;
        }
    }
    CHECK(vkDeviceWaitIdle(device));
    double seconds = now() - t0;
    uint32_t w, h;
    windowSize(&w, &h);
    printf("%d frames in %.2f s: swapchain %ux%u, window %ux%u; %d / %d suboptimal (%d / %d in the last tenth), %d / %d recreated "
           "(before / after the resize), %d swapchains\n",
           frames, seconds, extent.width, extent.height, w, h, suboptimal[0], suboptimal[1], lateSuboptimal[0], lateSuboptimal[1],
           recreated[0], recreated[1], creates);

    bool ok = !failed && !fails;
    if (mode != MODE_FIXED) {
        if (recreated[0] || recreated[1]) {
            printf("FS-FAIL recreated %d times\n", recreated[0] + recreated[1]);
            ok = false;
        }
        if (extent.width != w || extent.height != h) {
            printf("FS-FAIL swapchain %ux%u, window %ux%u\n", extent.width, extent.height, w, h);
            ok = false;
        }
    }
    else {
        /* the first swapchain counts too: more than limit within a second makes the next one quiet */
        for (int p = 0; p < (resize ? 2 : 1); p++) {
            if (!recreated[p] || recreated[p] > limit + 1 || lateSuboptimal[p]) {
                printf("FS-FAIL %s the resize: %d recreations (expected 1..%d), %d suboptimal in the last tenth\n", p ? "after" : "before",
                       recreated[p], limit + 1, lateSuboptimal[p]);
                ok = false;
            }
        }
    }

    destroyViews();
    vkDestroySwapchainKHR(device, swapchain, NULL);
    for (int i = 0; i < SLOTS; i++) {
        vkDestroySemaphore(device, acquired[i], NULL);
        vkDestroySemaphore(device, rendered[i], NULL);
        vkDestroyFence(device, done[i], NULL);
    }
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipelineLayout, NULL);
    vkDestroyDescriptorSetLayout(device, setLayout, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroyInstance(instance, NULL);
    xcb_disconnect(conn);
    printf(ok ? "FS-PASS\n" : "FS-FAIL\n");
    return ok ? 0 : 1;
}
