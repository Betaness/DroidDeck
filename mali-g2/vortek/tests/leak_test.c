/* [mali-g2] A client that leaves everything behind, the way games and DXVK usually quit: the
 * server must release what the connection created once it is gone (winlator-0015).
 *
 *   leak_test exit    allocate, submit work that is still running, then exit() without destroying
 *                     anything (default)
 *   leak_test kill    the same, then print LEAK-READY and wait to be killed (SIGKILL from the runner)
 *   leak_test clean   destroy everything in order and exit: the server must not free anything twice
 *
 * What it creates (environment):
 *   LEAK_ALLOCS (64) x LEAK_SIZE_MB (16) MiB host-visible memory, every second allocation mapped and
 *     written, each with a buffer bound to it (the GPU fills the first ones);
 *   LEAK_IMAGES (8) 1024x1024 RGBA8 optimal images on dedicated device-local memory, cleared;
 *   a compute pipeline (with a pipeline cache, descriptor pool and set), vertex and fragment shader
 *     modules and a graphics pipeline built from them, a command pool with recorded buffers;
 *   a timeline semaphore that a second thread waits on forever (LEAK_TIMELINE_WAIT, default 1): the
 *     server's wait for it must not keep the connection from being cleaned up;
 *   with $DISPLAY (and LEAK_SWAPCHAIN, default 1) an X window, surface and swapchain, one frame presented
 *     (zero-copy when the server has a presenter, e.g. leak_test.sh LEAK_MOCK=zc_mock_compositor).
 * Queue work left behind that keeps the device busy after the client is gone (exit/kill only):
 *   LEAK_GPU_STUCK=1    a batch waiting for a timeline value only the client would have signalled
 *                       (and signalling another timeline semaphore): the server must signal it
 *   LEAK_GPU_BUSY_MS=N  fills that keep the queue busy for about N ms (calibrated): longer than the
 *                       server's 3 s idle wait, the device is released in the background later
 * Prints LEAK-READY (exit/kill) or LEAK-CLEAN-PASS (clean). Run many times against one server with
 * leak_test.sh, which records the server's memory after each run.
 * Build: cc -O2 -o leak_test leak_test.c -lvulkan -lX11 -lpthread */
#define VK_USE_PLATFORM_XLIB_KHR
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <vulkan/vulkan.h>
#include "m56_spv.h"
#include "cmd_stream_spv.h"

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); fflush(stdout); exit(1); } } while (0)
#define IMAGE_SIZE 1024

static VkPhysicalDeviceMemoryProperties mp;

static int envInt(const char* name, int defaultValue) {
    const char* value = getenv(name);
    return value && *value ? atoi(value) : defaultValue;
}

static uint32_t findType(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    printf("FAIL no memory type for bits 0x%x flags 0x%x\n", bits, want);
    exit(1);
}

typedef struct {
    VkDevice dev;
    VkSemaphore semaphore;
} TimelineWait;

static void* timelineWaitThread(void* param) {
    TimelineWait* wait = param;
    uint64_t value = 1;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, 0, 0, 1, &wait->semaphore, &value};
    VkResult result = vkWaitSemaphores(wait->dev, &wi, UINT64_MAX); /* nobody ever signals it */
    printf("timeline wait returned %d\n", result);
    return NULL;
}

static VkShaderModule makeModule(VkDevice dev, const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, size, code};
    VkShaderModule module;
    CK(vkCreateShaderModule(dev, &ci, 0, &module));
    return module;
}

static void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "exit";
    bool clean = strcmp(mode, "clean") == 0, kill = strcmp(mode, "kill") == 0;
    if (!clean && !kill && strcmp(mode, "exit") != 0) {
        printf("usage: %s [exit|kill|clean]\n", argv[0]);
        return 2;
    }
    int allocs = envInt("LEAK_ALLOCS", 64), sizeMb = envInt("LEAK_SIZE_MB", 16), imageCount = envInt("LEAK_IMAGES", 8);
    int timelineWait = envInt("LEAK_TIMELINE_WAIT", 1) && !clean;
    VkDeviceSize allocSize = (VkDeviceSize)sizeMb << 20;

    Display* dpy = envInt("LEAK_SWAPCHAIN", 1) && getenv("DISPLAY") ? XOpenDisplay(NULL) : NULL;
    Window win = 0;
    if (dpy) {
        win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 640, 360, 0, 0, 0);
        XMapWindow(dpy, win);
        XSync(dpy, False);
    }

    const char* iext[] = {"VK_KHR_surface", "VK_KHR_xlib_surface"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "leak_test", 1, "leak_test", 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app, 0, 0, dpy ? 2 : 0, iext};
    VkInstance inst;
    CK(vkCreateInstance(&ii, 0, &inst));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (dpy) {
        VkXlibSurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, 0, 0, dpy, win};
        CK(vkCreateXlibSurfaceKHR(inst, &si, 0, &surface));
    }

    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio};
    const char* dext[] = {"VK_KHR_swapchain"};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &qi, 0, 0, dpy ? 1 : 0, dext};
    VkDevice dev;
    CK(vkCreateDevice(pd, &di, 0, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    /* host-visible memory: every second allocation mapped and written, a buffer bound to each */
    VkDeviceMemory* memory = calloc(allocs, sizeof(VkDeviceMemory));
    VkBuffer* buffers = calloc(allocs, sizeof(VkBuffer));
    void** mapped = calloc(allocs, sizeof(void*));
    for (int i = 0; i < allocs; i++) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, allocSize,
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE};
        CK(vkCreateBuffer(dev, &bi, 0, &buffers[i]));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, buffers[i], &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, req.size,
                                   findType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        CK(vkAllocateMemory(dev, &ai, 0, &memory[i]));
        CK(vkBindBufferMemory(dev, buffers[i], memory[i], 0));
        if (i % 2 == 0) {
            CK(vkMapMemory(dev, memory[i], 0, VK_WHOLE_SIZE, 0, &mapped[i]));
            memset(mapped[i], 0x40 + (i & 15), allocSize);
        }
    }

    /* images on dedicated device-local memory */
    VkImage* images = calloc(imageCount ? imageCount : 1, sizeof(VkImage));
    VkDeviceMemory* imageMemory = calloc(imageCount ? imageCount : 1, sizeof(VkDeviceMemory));
    for (int i = 0; i < imageCount; i++) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = (VkExtent3D){IMAGE_SIZE, IMAGE_SIZE, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        CK(vkCreateImage(dev, &ci, 0, &images[i]));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, images[i], &req);
        VkMemoryDedicatedAllocateInfo dedicated = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, 0, images[i]};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated, req.size,
                                   findType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CK(vkAllocateMemory(dev, &ai, 0, &imageMemory[i]));
        CK(vkBindImageMemory(dev, images[i], imageMemory[i], 0));
    }

    /* compute pipeline writing into buffer 0 */
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dsli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &binding};
    VkDescriptorSetLayout dsl;
    CK(vkCreateDescriptorSetLayout(dev, &dsli, 0, &dsl));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl, 1, &pcr};
    VkPipelineLayout layout;
    CK(vkCreatePipelineLayout(dev, &pli, 0, &layout));
    VkPipelineCacheCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache cache;
    CK(vkCreatePipelineCache(dev, &pci, 0, &cache));
    VkShaderModule comp = makeModule(dev, m56_spv, sizeof(m56_spv));
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, comp, "main"};
    cpi.layout = layout;
    VkPipeline computePipeline;
    CK(vkCreateComputePipelines(dev, cache, 1, &cpi, 0, &computePipeline));
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, 4, 1, &poolSize};
    VkDescriptorPool descriptorPool;
    CK(vkCreateDescriptorPool(dev, &dpi, 0, &descriptorPool));
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, descriptorPool, 1, &dsl};
    VkDescriptorSet set;
    CK(vkAllocateDescriptorSets(dev, &dsai, &set));
    VkDescriptorBufferInfo dbi = {buffers[0], 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wds = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &dbi};
    vkUpdateDescriptorSets(dev, 1, &wds, 0, 0);

    /* graphics pipeline from vertex and fragment modules (the server keeps their SPIR-V) */
    VkAttachmentDescription att = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 1, &ref};
    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, 1, &att, 1, &sub};
    VkRenderPass renderPass;
    CK(vkCreateRenderPass(dev, &rpi, 0, &renderPass));
    VkShaderModule vert = makeModule(dev, cmd_stream_vert_spv, sizeof(cmd_stream_vert_spv));
    VkShaderModule frag = makeModule(dev, cmd_stream_frag_spv, sizeof(cmd_stream_frag_spv));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main"}};
    VkPipelineVertexInputStateCreateInfo vis = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ias = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {0, 0, IMAGE_SIZE, IMAGE_SIZE, 0, 1};
    VkRect2D scissor = {{0, 0}, {IMAGE_SIZE, IMAGE_SIZE}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, &viewport, 1, &scissor};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &cba};
    VkGraphicsPipelineCreateInfo gpi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, 0, 0, 2, stages, &vis, &ias, 0, &vps, &rs, &ms, 0, &cbs, 0, layout, renderPass, 0};
    VkPipeline graphicsPipeline;
    CK(vkCreateGraphicsPipelines(dev, cache, 1, &gpi, 0, &graphicsPipeline));
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (imageCount > 0) {
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, images[0], VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0},
                                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CK(vkCreateImageView(dev, &vci, 0, &view));
        VkFramebufferCreateInfo fbi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0, 0, renderPass, 1, &view, IMAGE_SIZE, IMAGE_SIZE, 1};
        CK(vkCreateFramebuffer(dev, &fbi, 0, &framebuffer));
    }

    /* command buffers: one waited for, one still running at exit */
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    VkCommandPool pool;
    CK(vkCreateCommandPool(dev, &cpci, 0, &pool));
    VkCommandBuffer cmd[2];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
    CK(vkAllocateCommandBuffers(dev, &cai, cmd));
    VkFence fence[2];
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CK(vkCreateFence(dev, &fci, 0, &fence[0]));
    CK(vkCreateFence(dev, &fci, 0, &fence[1]));
    for (int k = 0; k < 2; k++) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CK(vkBeginCommandBuffer(cmd[k], &bi));
        for (int i = 0; i < allocs && i < 8; i++) vkCmdFillBuffer(cmd[k], buffers[i], 0, VK_WHOLE_SIZE, 0x11111111u * (k + 1));
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT};
        vkCmdPipelineBarrier(cmd[k], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, 0, 0, 0);
        vkCmdBindPipeline(cmd[k], VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
        vkCmdBindDescriptorSets(cmd[k], VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, 0);
        uint32_t add = 7;
        vkCmdPushConstants(cmd[k], layout, pcr.stageFlags, 0, 4, &add);
        vkCmdDispatch(cmd[k], (uint32_t)(allocSize / 4 / 64 < 65535 ? allocSize / 4 / 64 : 65535), 1, 1);
        VkClearColorValue color = {{0.25f * k, 0.5f, 0.75f, 1}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (int i = 0; i < imageCount; i++) {
            imageBarrier(cmd[k], images[i], k == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cmd[k], images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
            imageBarrier(cmd[k], images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        if (framebuffer) {
            VkClearValue clear = {{{0, 0, 0, 1}}};
            VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, renderPass, framebuffer, scissor, 1, &clear};
            vkCmdBeginRenderPass(cmd[k], &rbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd[k], VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
            float pc[8] = {-1, -1, 1, 1, 1, 0, 0, 1};
            vkCmdPushConstants(cmd[k], layout, pcr.stageFlags, 0, 32, pc);
            vkCmdDraw(cmd[k], 6, 1, 0, 0);
            vkCmdEndRenderPass(cmd[k]);
        }
        CK(vkEndCommandBuffer(cmd[k]));
    }
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cmd[0]};
    CK(vkQueueSubmit(queue, 1, &submit, fence[0]));
    CK(vkWaitForFences(dev, 1, &fence[0], VK_TRUE, 5000000000ull));

    /* swapchain: one frame acquired and presented */
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSemaphore acquireSem = VK_NULL_HANDLE, renderSem = VK_NULL_HANDLE;
    VkCommandBuffer presentCmd = VK_NULL_HANDLE;
    if (surface) {
        VkSurfaceCapabilitiesKHR caps;
        CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps));
        VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sci.surface = surface;
        sci.minImageCount = caps.minImageCount;
        sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        sci.imageExtent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){640, 360};
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;
        CK(vkCreateSwapchainKHR(dev, &sci, 0, &swapchain));
        uint32_t count = 0;
        vkGetSwapchainImagesKHR(dev, swapchain, &count, NULL);
        VkImage* swapImages = calloc(count, sizeof(VkImage));
        vkGetSwapchainImagesKHR(dev, swapchain, &count, swapImages);
        VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        CK(vkCreateSemaphore(dev, &semi, 0, &acquireSem));
        CK(vkCreateSemaphore(dev, &semi, 0, &renderSem));
        uint32_t index;
        CK(vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, acquireSem, VK_NULL_HANDLE, &index));
        VkCommandBufferAllocateInfo pcai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        CK(vkAllocateCommandBuffers(dev, &pcai, &presentCmd));
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CK(vkBeginCommandBuffer(presentCmd, &bi));
        imageBarrier(presentCmd, swapImages[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        /* zc_mock_compositor's marker for frame 0 (blue 0xa5, frame number in red/green), so that
         * leak_test.sh LEAK_MOCK=... can require MOCK-RESULT ok */
        VkClearColorValue color = {{0, 0, 165.0f / 255, 1}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(presentCmd, swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        imageBarrier(presentCmd, swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        CK(vkEndCommandBuffer(presentCmd));
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo psubmit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 1, &acquireSem, &stage, 1, &presentCmd, 1, &renderSem};
        CK(vkQueueSubmit(queue, 1, &psubmit, VK_NULL_HANDLE));
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 1, &renderSem, 1, &swapchain, &index};
        VkResult presented = vkQueuePresentKHR(queue, &pi);
        if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR) printf("present returned %d\n", presented);
        free(swapImages);
    }

    VkSemaphoreTypeCreateInfo sti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, 0, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo tsi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &sti};
    VkSemaphore timeline;
    CK(vkCreateSemaphore(dev, &tsi, 0, &timeline));

    printf("leak_test %s: %d x %d MiB host-visible (%d mapped), %d images %dx%d, swapchain %s\n", mode, allocs, sizeMb,
           (allocs + 1) / 2, imageCount, IMAGE_SIZE, IMAGE_SIZE, swapchain ? "yes" : "no");

    if (!clean) {
        /* work still running when the client goes away */
        submit.pCommandBuffers = &cmd[1];
        CK(vkQueueSubmit(queue, 1, &submit, fence[1]));
        int busyMs = envInt("LEAK_GPU_BUSY_MS", 0);
        if (busyMs > 0 && allocs > 0) {
            /* calibrate: 16 fills of one buffer, then enough fills for busyMs */
            VkCommandBuffer busy[2];
            VkCommandBufferAllocateInfo bai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
            CK(vkAllocateCommandBuffers(dev, &bai, busy));
            VkFence calibrated;
            CK(vkCreateFence(dev, &fci, 0, &calibrated));
            VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            CK(vkBeginCommandBuffer(busy[0], &bi));
            for (int i = 0; i < 16; i++) vkCmdFillBuffer(busy[0], buffers[0], 0, VK_WHOLE_SIZE, i);
            CK(vkEndCommandBuffer(busy[0]));
            CK(vkWaitForFences(dev, 1, &fence[1], VK_TRUE, 5000000000ull));
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            VkSubmitInfo bsubmit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &busy[0]};
            CK(vkQueueSubmit(queue, 1, &bsubmit, calibrated));
            CK(vkWaitForFences(dev, 1, &calibrated, VK_TRUE, 5000000000ull));
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double ms16 = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
            long fills = (long)(busyMs / (ms16 > 0.01 ? ms16 : 0.01) * 16);
            if (fills > 1000000) fills = 1000000;
            CK(vkBeginCommandBuffer(busy[1], &bi));
            for (long i = 0; i < fills; i++) vkCmdFillBuffer(busy[1], buffers[0], 0, VK_WHOLE_SIZE, (uint32_t)i);
            CK(vkEndCommandBuffer(busy[1]));
            bsubmit.pCommandBuffers = &busy[1];
            CK(vkQueueSubmit(queue, 1, &bsubmit, VK_NULL_HANDLE));
            printf("leak_test: queue busy for about %d ms (%ld fills of %d MiB, 16 took %.2f ms)\n", busyMs, fills, sizeMb, ms16);
        }
        if (envInt("LEAK_GPU_STUCK", 0) && allocs > 0) {
            VkSemaphore gate, done;
            CK(vkCreateSemaphore(dev, &tsi, 0, &gate));
            CK(vkCreateSemaphore(dev, &tsi, 0, &done));
            VkCommandBuffer stuck;
            VkCommandBufferAllocateInfo sai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
            CK(vkAllocateCommandBuffers(dev, &sai, &stuck));
            VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            CK(vkBeginCommandBuffer(stuck, &bi));
            vkCmdFillBuffer(stuck, buffers[0], 0, VK_WHOLE_SIZE, 0x5a5a5a5au);
            CK(vkEndCommandBuffer(stuck));
            uint64_t one = 1;
            VkTimelineSemaphoreSubmitInfo tsub = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, 0, 1, &one, 1, &one};
            VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo ssubmit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &tsub, 1, &gate, &stage, 1, &stuck, 1, &done};
            CK(vkQueueSubmit(queue, 1, &ssubmit, VK_NULL_HANDLE));
            printf("leak_test: queue stuck on a timeline value nobody signals\n");
        }
        if (timelineWait) {
            static TimelineWait wait;
            wait.dev = dev;
            wait.semaphore = timeline;
            pthread_t thread;
            pthread_create(&thread, NULL, timelineWaitThread, &wait);
            usleep(200000);
        }
        printf("LEAK-READY\n");
        fflush(stdout);
        if (kill) for (;;) pause();
        exit(0);
    }

    /* clean: destroy everything, children first */
    CK(vkDeviceWaitIdle(dev));
    vkDestroySemaphore(dev, timeline, 0);
    if (swapchain) {
        vkFreeCommandBuffers(dev, pool, 1, &presentCmd);
        vkDestroySemaphore(dev, acquireSem, 0);
        vkDestroySemaphore(dev, renderSem, 0);
        vkDestroySwapchainKHR(dev, swapchain, 0);
    }
    vkDestroyFence(dev, fence[0], 0);
    vkDestroyFence(dev, fence[1], 0);
    vkFreeCommandBuffers(dev, pool, 2, cmd);
    vkDestroyCommandPool(dev, pool, 0);
    if (framebuffer) vkDestroyFramebuffer(dev, framebuffer, 0);
    if (view) vkDestroyImageView(dev, view, 0);
    vkDestroyPipeline(dev, graphicsPipeline, 0);
    vkDestroyPipeline(dev, computePipeline, 0);
    vkDestroyShaderModule(dev, vert, 0);
    vkDestroyShaderModule(dev, frag, 0);
    vkDestroyShaderModule(dev, comp, 0);
    vkDestroyRenderPass(dev, renderPass, 0);
    vkFreeDescriptorSets(dev, descriptorPool, 1, &set);
    vkDestroyDescriptorPool(dev, descriptorPool, 0);
    vkDestroyPipelineCache(dev, cache, 0);
    vkDestroyPipelineLayout(dev, layout, 0);
    vkDestroyDescriptorSetLayout(dev, dsl, 0);
    for (int i = 0; i < imageCount; i++) {
        vkDestroyImage(dev, images[i], 0);
        vkFreeMemory(dev, imageMemory[i], 0);
    }
    for (int i = 0; i < allocs; i++) {
        if (mapped[i]) vkUnmapMemory(dev, memory[i]);
        vkDestroyBuffer(dev, buffers[i], 0);
        vkFreeMemory(dev, memory[i], 0);
    }
    vkDestroyDevice(dev, 0);
    if (surface) vkDestroySurfaceKHR(inst, surface, 0);
    vkDestroyInstance(inst, 0);
    if (dpy) {
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
    }
    printf("LEAK-CLEAN-PASS\n");
    return 0;
}
