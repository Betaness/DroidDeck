/* [mali-g2] Zero-copy acquire (headless_swapchain.c): vkAcquireNextImageKHR must not wait on the CPU
 * for the compositor's release fence; the fence goes to the GPU through the acquire semaphore/fence.
 * Presents ZC_FRAMES frames to an X window (Xvfb) through the Vortek client, with the server's
 * VORTEK_PRESENT_SOCKET pointing at tests/zc_mock_compositor.c (MOCK_RELEASE=early hands back
 * images with still-pending release fences). Each frame, like a game that acquires first:
 *   acquire (timed) -> submit ZC_GPU_MS of copies between two buffers (calibrated) -> submit a clear of
 *   the swapchain image to the frame number waiting on the acquire semaphore -> present.
 * ZC_GPU_MS default 16, ZC_INFLIGHT frames in flight (default 2), ZC_IMAGES swapchain images
 * (default 2: the image acquired was released with the previous frame's still-pending fence),
 * ZC_ACQUIRE_FENCE=1 also passes a fence to vkAcquireNextImageKHR (waited for when its frame slot
 * comes round again), ZC_ACQUIRE_FENCE=2 passes only a fence and waits for it right after acquire, like
 * a native program without an acquire semaphore. Every wait on an acquire fence is checked: afterwards
 * vkGetFenceStatus must say VK_SUCCESS and a second vkWaitForFences (100 ms) must not time out (the
 * server's vkWaitForFences exports a sync fd, which resets an imported payload unless it is restored).
 * Timestamps around each frame give the GPU idle share.
 * Reports acquire time (avg / p50 / p95 / max), how often acquire returned while the previous frame
 * was still on the GPU, GPU idle %, fps; the mock checks the frames' content and order.
 * Prints ZCACQ-PASS when the surface reports minImageCount ZC_EXPECT_MIN_IMAGES (default 3, 0 = any),
 * every acquire fence read as signalled after its wait and, with ZC_EXPECT_FAST=1 (default), at least
 * a quarter of the acquires returned while the previous frame was still rendering, which a CPU wait
 * for its release fence cannot do. Whether the image really waited for its release fence on the GPU is
 * checked by the mock with MOCK_RELEASE=gpu (see zc_acquire_test.sh).
 * Build: cc -O2 -o zc_acquire_test zc_acquire_test.c -lvulkan -lX11 */
#define VK_USE_PLATFORM_XLIB_KHR
#include <X11/Xlib.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r_ = (x); if (r_ < 0) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
#define MAX_INFLIGHT 4
#define COPY_SIZE (16u << 20)

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int envInt(const char* name, int def) {
    const char* v = getenv(name);
    return v && *v ? atoi(v) : def;
}

static int cmpDouble(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

/* waits for an acquire fence and checks it stays signalled until reset */
static int fenceWaits, fenceNotSignaled, fenceSecondWaitTimeout;
static void waitAcquireFence(VkDevice dev, VkFence fence) {
    CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    VkResult status = vkGetFenceStatus(dev, fence);
    if (status != VK_SUCCESS && fenceNotSignaled++ < 3) printf("acquire fence after vkWaitForFences: status %d\n", status);
    VkResult again = vkWaitForFences(dev, 1, &fence, VK_TRUE, 100000000ull);
    if (again == VK_TIMEOUT) fenceSecondWaitTimeout++;
    else CK(again);
    CK(vkResetFences(dev, 1, &fence));
    fenceWaits++;
}

static VkPhysicalDeviceMemoryProperties mp;
static uint32_t findType(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}

static VkBuffer makeBuffer(VkDevice dev, VkDeviceSize size) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer b;
    CK(vkCreateBuffer(dev, &bi, 0, &b));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, b, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, findType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory m;
    CK(vkAllocateMemory(dev, &ai, 0, &m));
    CK(vkBindBufferMemory(dev, b, m, 0));
    return b;
}

int main(void) {
    int frames = envInt("ZC_FRAMES", 300), inflight = envInt("ZC_INFLIGHT", 2), gpuMs = envInt("ZC_GPU_MS", 16);
    int useFence = envInt("ZC_ACQUIRE_FENCE", 0), expectFast = envInt("ZC_EXPECT_FAST", 1), expectMin = envInt("ZC_EXPECT_MIN_IMAGES", 3);
    bool fenceOnly = useFence == 2;
    if (inflight < 1) inflight = 1;
    if (inflight > MAX_INFLIGHT) inflight = MAX_INFLIGHT;

    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) {
        printf("FAIL no X display\n");
        return 1;
    }
    Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 640, 360, 0, 0, 0);
    XMapWindow(dpy, win);
    XSync(dpy, False);

    const char* iext[] = {"VK_KHR_surface", "VK_KHR_xlib_surface"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "zcacq", 1, "zcacq", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app, 0, 0, 2, iext};
    VkInstance inst;
    CK(vkCreateInstance(&ii, 0, &inst));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    VkQueueFamilyProperties qf[8];
    n = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qf);
    bool timestamps = qf[0].timestampValidBits > 0 && props.limits.timestampPeriod > 0;

    VkXlibSurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, 0, 0, dpy, win};
    VkSurfaceKHR surface;
    CK(vkCreateXlibSurfaceKHR(inst, &si, 0, &surface));

    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio};
    const char* dext[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qi, 0, 0, 1, dext};
    VkDevice dev;
    CK(vkCreateDevice(pd, &di, 0, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps));
    uint32_t images = envInt("ZC_IMAGES", 2);
    VkExtent2D extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){640, 360};
    printf("surface: minImageCount %u maxImageCount %u extent %ux%u; using %u images, %d frames in flight, acquire %s\n",
           caps.minImageCount, caps.maxImageCount, extent.width, extent.height, images, inflight,
           fenceOnly ? "fence only" : useFence ? "semaphore + fence" : "semaphore");

    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = images;
    sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    VkSwapchainKHR swapchain;
    CK(vkCreateSwapchainKHR(dev, &sci, 0, &swapchain));
    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(dev, swapchain, &imageCount, NULL);
    VkImage* swapImages = calloc(imageCount, sizeof(VkImage));
    vkGetSwapchainImagesKHR(dev, swapchain, &imageCount, swapImages);

    VkBuffer src = makeBuffer(dev, COPY_SIZE), dst = makeBuffer(dev, COPY_SIZE);
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    VkCommandPool pool;
    CK(vkCreateCommandPool(dev, &pi, 0, &pool));
    VkCommandBuffer heavyCmd[MAX_INFLIGHT], clearCmd[MAX_INFLIGHT];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, MAX_INFLIGHT};
    CK(vkAllocateCommandBuffers(dev, &cai, heavyCmd));
    CK(vkAllocateCommandBuffers(dev, &cai, clearCmd));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFenceCreateInfo fciSignaled = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, VK_FENCE_CREATE_SIGNALED_BIT};
    VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFence frameFence[MAX_INFLIGHT], acquireFence[MAX_INFLIGHT];
    VkSemaphore acquireSem[MAX_INFLIGHT], renderSem[MAX_INFLIGHT];
    bool acquireFencePending[MAX_INFLIGHT] = {0};
    for (int i = 0; i < MAX_INFLIGHT; i++) {
        CK(vkCreateFence(dev, &fciSignaled, 0, &frameFence[i]));
        CK(vkCreateFence(dev, &fci, 0, &acquireFence[i]));
        CK(vkCreateSemaphore(dev, &semi, 0, &acquireSem[i]));
        CK(vkCreateSemaphore(dev, &semi, 0, &renderSem[i]));
    }
    VkQueryPool queryPool = VK_NULL_HANDLE;
    if (timestamps) {
        VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, 0, 0, VK_QUERY_TYPE_TIMESTAMP, 2 * MAX_INFLIGHT};
        if (vkCreateQueryPool(dev, &qpi, 0, &queryPool) != VK_SUCCESS) timestamps = false;
    }

    /* calibrate: copies per frame for ZC_GPU_MS of GPU time */
    VkBufferCopy region = {0, 0, COPY_SIZE};
    int copies = 1;
    double perCopy = 0;
    for (int pass = 0; pass < 3; pass++) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CK(vkBeginCommandBuffer(heavyCmd[0], &bi));
        for (int i = 0; i < 8; i++) vkCmdCopyBuffer(heavyCmd[0], src, dst, 1, &region);
        CK(vkEndCommandBuffer(heavyCmd[0]));
        VkSubmitInfo s = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &heavyCmd[0]};
        VkFence f = acquireFence[0];
        double t = now();
        CK(vkQueueSubmit(queue, 1, &s, f));
        CK(vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX));
        perCopy = (now() - t) / 8;
        CK(vkResetFences(dev, 1, &f));
    }
    copies = (int)ceil(gpuMs / 1000.0 / perCopy);
    if (copies < 1) copies = 1;
    if (gpuMs <= 0) copies = 0;
    printf("calibration: %.3f ms per 16 MiB copy, %d copies per frame (~%d ms GPU)\n", perCopy * 1000, copies, gpuMs);

    double* acquireTimes = calloc(frames, sizeof(double));
    double gpuBusy = 0, gpuIdle = 0, firstStart = 0, lastEnd = 0;
    uint64_t prevEnd = 0;
    int early = 0, acquireFenceUnsignaled = 0, suboptimal = 0, gpuFrames = 0;
    double t0 = now(), tSteady = 0;
    int steadyFrom = frames > 20 ? 10 : 0;

    for (int f = 0; f < frames; f++) {
        int slot = f % inflight, prevSlot = (f + inflight - 1) % inflight;
        if (f == steadyFrom) tSteady = now();
        CK(vkWaitForFences(dev, 1, &frameFence[slot], VK_TRUE, UINT64_MAX));
        if (acquireFencePending[slot]) {
            waitAcquireFence(dev, acquireFence[slot]);
            acquireFencePending[slot] = false;
        }
        if (timestamps && f >= inflight) {
            uint64_t ts[2];
            if (vkGetQueryPoolResults(dev, queryPool, 2 * slot, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                double period = props.limits.timestampPeriod * 1e-9;
                if (f - inflight >= steadyFrom) {
                    if (!firstStart) firstStart = ts[0] * period;
                    gpuBusy += (ts[1] - ts[0]) * period;
                    if (prevEnd && ts[0] > prevEnd) gpuIdle += (ts[0] - prevEnd) * period;
                    lastEnd = ts[1] * period;
                    gpuFrames++;
                }
                prevEnd = ts[1];
            }
        }
        CK(vkResetFences(dev, 1, &frameFence[slot]));

        uint32_t index = 0;
        double ta = now();
        VkResult ar = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, fenceOnly ? VK_NULL_HANDLE : acquireSem[slot], useFence ? acquireFence[slot] : VK_NULL_HANDLE, &index);
        acquireTimes[f] = now() - ta;
        if (ar == VK_SUBOPTIMAL_KHR) suboptimal++;
        else CK(ar);
        if (index >= imageCount) {
            printf("FAIL acquired index %u of %u\n", index, imageCount);
            return 1;
        }
        if (useFence) {
            acquireFencePending[slot] = true;
            if (vkGetFenceStatus(dev, acquireFence[slot]) == VK_NOT_READY) acquireFenceUnsignaled++;
        }
        if (f > 0 && vkGetFenceStatus(dev, frameFence[prevSlot]) == VK_NOT_READY) early++;
        if (fenceOnly) {
            waitAcquireFence(dev, acquireFence[slot]);
            acquireFencePending[slot] = false;
        }

        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CK(vkBeginCommandBuffer(heavyCmd[slot], &bi));
        if (timestamps) {
            vkCmdResetQueryPool(heavyCmd[slot], queryPool, 2 * slot, 2);
            vkCmdWriteTimestamp(heavyCmd[slot], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 2 * slot);
        }
        for (int i = 0; i < copies; i++) vkCmdCopyBuffer(heavyCmd[slot], i & 1 ? dst : src, i & 1 ? src : dst, 1, &region);
        CK(vkEndCommandBuffer(heavyCmd[slot]));
        VkSubmitInfo hs = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &heavyCmd[slot]};
        CK(vkQueueSubmit(queue, 1, &hs, VK_NULL_HANDLE));

        CK(vkBeginCommandBuffer(clearCmd[slot], &bi));
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, swapImages[index],
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(clearCmd[slot], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b);
        VkClearColorValue color = {.float32 = {(f & 0xff) / 255.0f, ((f >> 8) & 0xff) / 255.0f, 0xa5 / 255.0f, 1.0f}};
        vkCmdClearColorImage(clearCmd[slot], swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &b.subresourceRange);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(clearCmd[slot], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &b);
        if (timestamps) vkCmdWriteTimestamp(clearCmd[slot], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 2 * slot + 1);
        CK(vkEndCommandBuffer(clearCmd[slot]));
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo cs = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, fenceOnly ? 0 : 1, &acquireSem[slot], &stage, 1, &clearCmd[slot], 1, &renderSem[slot]};
        CK(vkQueueSubmit(queue, 1, &cs, frameFence[slot]));

        VkPresentInfoKHR pinfo = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 1, &renderSem[slot], 1, &swapchain, &index};
        VkResult pr = vkQueuePresentKHR(queue, &pinfo);
        if (pr == VK_SUBOPTIMAL_KHR) suboptimal++;
        else CK(pr);
    }
    CK(vkDeviceWaitIdle(dev));
    double total = now() - t0, steady = now() - tSteady;

    int m = frames - steadyFrom;
    double sum = 0;
    for (int i = steadyFrom; i < frames; i++) sum += acquireTimes[i];
    qsort(acquireTimes + steadyFrom, m, sizeof(double), cmpDouble);
    double avg = sum / m, p50 = acquireTimes[steadyFrom + m / 2], p95 = acquireTimes[steadyFrom + m * 95 / 100], mx = acquireTimes[frames - 1];
    double frameGpu = gpuFrames ? gpuBusy / gpuFrames : gpuMs / 1000.0;
    printf("frames %d in %.2f s; steady %d frames at %.1f fps\n", frames, total, m, m / steady);
    printf("acquire: avg %.3f ms, p50 %.3f ms, p95 %.3f ms, max %.3f ms\n", avg * 1000, p50 * 1000, p95 * 1000, mx * 1000);
    printf("acquire returned while the previous frame was still on the GPU: %d of %d\n", early, frames - 1);
    if (useFence) {
        printf("acquire fence still unsignalled when acquire returned: %d of %d\n", acquireFenceUnsignaled, frames);
        printf("acquire fence waits %d: not VK_SUCCESS after the wait %d, second wait timed out %d\n", fenceWaits,
               fenceNotSignaled, fenceSecondWaitTimeout);
    }
    if (gpuFrames > 1 && lastEnd > firstStart)
        printf("GPU: %.2f ms per frame busy, idle %.1f%% of %.1f ms (%d frames)\n", frameGpu * 1000, gpuIdle * 100 / (lastEnd - firstStart),
               (lastEnd - firstStart) * 1000, gpuFrames);
    else
        printf("GPU: no timestamps\n");
    if (suboptimal) printf("suboptimal: %d\n", suboptimal);

    vkDestroySwapchainKHR(dev, swapchain, 0);
    vkDestroyDevice(dev, 0);
    vkDestroySurfaceKHR(inst, surface, 0);
    vkDestroyInstance(inst, 0);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    if (expectMin && caps.minImageCount != (uint32_t)expectMin) {
        printf("FAIL surface minImageCount %u, expected %d\n", caps.minImageCount, expectMin);
        return 1;
    }
    if (fenceNotSignaled || fenceSecondWaitTimeout) {
        printf("FAIL acquire fence read as unsignalled after its wait (%d) or timed out on a second wait (%d)\n",
               fenceNotSignaled, fenceSecondWaitTimeout);
        return 1;
    }
    if (expectFast && early * 4 < frames - 1) {
        printf("FAIL acquire returned before the previous frame finished only %d times of %d\n", early, frames - 1);
        return 1;
    }
    printf("ZCACQ-PASS\n");
    return 0;
}
