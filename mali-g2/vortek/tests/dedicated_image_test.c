/* [mali-g2] Dedicated-allocation render target the way DXVK allocates large render targets:
 * an OPTIMAL-tiling colour image whose memory is allocated with VkMemoryDedicatedAllocateInfo
 * (image != 0) and never mapped. Exercises the server's VORTEK_NATIVE_IMAGES path (winlator-0011):
 *   - vkMapMemory on the dedicated memory must not crash or hang (VK_ERROR_MEMORY_MAP_FAILED with
 *     VORTEK_NATIVE_IMAGES=1, a mapping without it);
 *   - render pass clear + vkCmdClearAttachments + vkCmdClearColorImage into the image, copy to a
 *     host-visible buffer and check the pixels;
 *   - 70 more dedicated images allocated/bound/freed (count log every 64th, free path);
 *   - a transient image on LAZILY_ALLOCATED memory if the device has it (plain, not host visible).
 * Prints DEDICATED-PASS. Build: gcc -O1 -o dedicated_image_test dedicated_image_test.c -lvulkan */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define W 256
#define H 256
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); return 1; } } while (0)

static VkPhysicalDeviceMemoryProperties mp;

static uint32_t findType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want && !(mp.memoryTypes[i].propertyFlags & avoid)) return i;
    return UINT32_MAX;
}

static VkResult allocDedicated(VkDevice dev, VkImage img, VkDeviceMemory* mem, VkDeviceSize* size, uint32_t* type, int verbose) {
    VkMemoryDedicatedRequirements dr = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 mr2 = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dr};
    VkImageMemoryRequirementsInfo2 ri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, 0, img};
    vkGetImageMemoryRequirements2(dev, &ri, &mr2);
    uint32_t mt = findType(mr2.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
    if (mt == UINT32_MAX) mt = findType(mr2.memoryRequirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
    if (verbose)
        printf("image requirements: size %llu align %llu typeBits 0x%x prefersDedicated %u requiresDedicated %u -> type %u flags 0x%x\n",
               (unsigned long long)mr2.memoryRequirements.size, (unsigned long long)mr2.memoryRequirements.alignment,
               mr2.memoryRequirements.memoryTypeBits, dr.prefersDedicatedAllocation, dr.requiresDedicatedAllocation,
               mt, mt != UINT32_MAX ? mp.memoryTypes[mt].propertyFlags : 0);
    VkMemoryDedicatedAllocateInfo dai = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, 0, img, VK_NULL_HANDLE};
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dai, mr2.memoryRequirements.size, mt};
    if (size) *size = mr2.memoryRequirements.size;
    if (type) *type = mt;
    return vkAllocateMemory(dev, &ma, 0, mem);
}

int main(void) {
    const char* nativeEnv = getenv("VORTEK_NATIVE_IMAGES");
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "dedicated", 1, "dedicated", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai};
    VkInstance inst; CK(vkCreateInstance(&ii, 0, &inst));
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s\n", props.deviceName);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        printf("memory type %u: flags 0x%x heap %u\n", i, mp.memoryTypes[i].propertyFlags, mp.memoryTypes[i].heapIndex);

    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qi};
    VkDevice dev; CK(vkCreateDevice(pd, &di, 0, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;

    /* the render target */
    VkImageCreateInfo imi = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, fmt, {W, H, 1}, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkImage img; CK(vkCreateImage(dev, &imi, 0, &img));
    VkDeviceMemory imem; VkDeviceSize isize; uint32_t itype;
    CK(allocDedicated(dev, img, &imem, &isize, &itype, 1));
    CK(vkBindImageMemory(dev, img, imem, 0));

    /* an app must not map memory of an optimal image it renders to, but it must not hang or crash either */
    void* p = NULL;
    VkResult mapResult = vkMapMemory(dev, imem, 0, VK_WHOLE_SIZE, 0, &p);
    printf("vkMapMemory(dedicated image memory) = %d (%s)\n", mapResult,
           mapResult == VK_SUCCESS ? "mapped" : mapResult == VK_ERROR_MEMORY_MAP_FAILED ? "VK_ERROR_MEMORY_MAP_FAILED" : "?");
    if (mapResult == VK_SUCCESS) vkUnmapMemory(dev, imem);
    int nativeOn = nativeEnv && atoi(nativeEnv) != 0;
    int mapOk = nativeOn ? mapResult == VK_ERROR_MEMORY_MAP_FAILED : (mapResult == VK_SUCCESS || mapResult == VK_ERROR_MEMORY_MAP_FAILED);

    VkImageViewCreateInfo ivi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, img, VK_IMAGE_VIEW_TYPE_2D, fmt, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view; CK(vkCreateImageView(dev, &ivi, 0, &view));
    VkAttachmentDescription ad = {0, fmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 1, &ar};
    VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0};
    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, 1, &ad, 1, &sd, 1, &dep};
    VkRenderPass rp; CK(vkCreateRenderPass(dev, &rpi, 0, &rp));
    VkFramebufferCreateInfo fbi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0, 0, rp, 1, &view, W, H, 1};
    VkFramebuffer fb; CK(vkCreateFramebuffer(dev, &fbi, 0, &fb));

    /* readback buffer */
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buf; CK(vkCreateBuffer(dev, &bi, 0, &buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
    const VkMemoryPropertyFlags hostFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryAllocateInfo mb = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, findType(mr.memoryTypeBits, hostFlags, 0)};
    VkDeviceMemory bmem; CK(vkAllocateMemory(dev, &mb, 0, &bmem)); CK(vkBindBufferMemory(dev, buf, bmem, 0));
    uint8_t* map; CK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void**)&map));
    memset(map, 0xcd, W * H * 4);

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, 0};
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpi, 0, &cp));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CK(vkBeginCommandBuffer(cb, &cbi));
    /* 1. render pass: clear whole image to blue, clear a 64x64 rect at (32,32) to green */
    VkClearValue cv = {.color = {{0.0f, 0.0f, 1.0f, 1.0f}}};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, rp, fb, {{0, 0}, {W, H}}, 1, &cv};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0.0f, 1.0f, 0.0f, 1.0f}}}};
    VkClearRect cr = {{{32, 32}, {64, 64}}, 0, 1};
    vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
    vkCmdEndRenderPass(cb);
    /* 2. transfer clear of the bottom-right quarter via a second, region-limited copy source:
     *    vkCmdClearColorImage clears whole subresources, so clear a second dedicated image red and
     *    copy a 128x128 block of it into the render target. */
    VkImage img2; CK(vkCreateImage(dev, &imi, 0, &img2));
    VkDeviceMemory imem2; CK(allocDedicated(dev, img2, &imem2, 0, 0, 0)); CK(vkBindImageMemory(dev, img2, imem2, 0));
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier toDst2 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img2, range};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &toDst2);
    VkClearColorValue red = {{1.0f, 0.0f, 0.0f, 1.0f}};
    vkCmdClearColorImage(cb, img2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &range);
    VkImageMemoryBarrier toSrc2 = toDst2;
    toSrc2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; toSrc2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; toSrc2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &toSrc2);
    VkImageCopy ic = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {128, 128, 0}, {128, 128, 1}};
    vkCmdCopyImage(cb, img2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    /* 3. read back */
    VkImageMemoryBarrier toSrc = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, range};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &toSrc);
    VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
    VkBufferMemoryBarrier toHost = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, buf, 0, VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, 0, 1, &toHost, 0, 0);
    CK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; CK(vkCreateFence(dev, &fci, 0, &fence));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb};
    CK(vkQueueSubmit(q, 1, &si, fence));
    CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
    VkMappedMemoryRange range2 = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, 0, bmem, 0, VK_WHOLE_SIZE};
    vkInvalidateMappedMemoryRanges(dev, 1, &range2);

    struct { int x, y; uint8_t want[4]; const char* what; } checks[] = {
        {0, 0, {0, 0, 255, 255}, "blue (render pass clear)"},
        {200, 10, {0, 0, 255, 255}, "blue (render pass clear)"},
        {32, 32, {0, 255, 0, 255}, "green (vkCmdClearAttachments)"},
        {95, 95, {0, 255, 0, 255}, "green (vkCmdClearAttachments)"},
        {96, 96, {0, 0, 255, 255}, "blue (outside clear rect)"},
        {128, 128, {255, 0, 0, 255}, "red (copied from 2nd dedicated image)"},
        {255, 255, {255, 0, 0, 255}, "red (copied from 2nd dedicated image)"},
        {127, 200, {0, 0, 255, 255}, "blue (outside copy)"},
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        const uint8_t* px = map + (checks[i].y * W + checks[i].x) * 4;
        int ok = !memcmp(px, checks[i].want, 4);
        bad += !ok;
        printf("pixel (%3d,%3d) = %3u %3u %3u %3u  want %3u %3u %3u %3u  %-38s %s\n", checks[i].x, checks[i].y, px[0], px[1], px[2], px[3],
               checks[i].want[0], checks[i].want[1], checks[i].want[2], checks[i].want[3], checks[i].what, ok ? "ok" : "WRONG");
    }
    int counts[4] = {0}; /* blue green red other */
    for (int i = 0; i < W * H; i++) {
        const uint8_t* px = map + i * 4;
        if (px[0] == 0 && px[1] == 0 && px[2] == 255) counts[0]++;
        else if (px[0] == 0 && px[1] == 255 && px[2] == 0) counts[1]++;
        else if (px[0] == 255 && px[1] == 0 && px[2] == 0) counts[2]++;
        else counts[3]++;
    }
    int countsOk = counts[1] == 64 * 64 && counts[2] == 128 * 128 && counts[0] == W * H - 64 * 64 - 128 * 128 && counts[3] == 0;
    printf("pixel counts: blue %d green %d red %d other %d -> %s\n", counts[0], counts[1], counts[2], counts[3], countsOk ? "ok" : "WRONG");

    /* many dedicated optimal images: allocate, bind, free (exercises the every-64th log and free) */
    enum { MANY = 70 };
    VkImageCreateInfo smi = imi; smi.extent = (VkExtent3D){512, 512, 1};
    VkImage many[MANY]; VkDeviceMemory manyMem[MANY];
    VkDeviceSize manyBytes = 0;
    for (int i = 0; i < MANY; i++) {
        CK(vkCreateImage(dev, &smi, 0, &many[i]));
        VkDeviceSize s;
        CK(allocDedicated(dev, many[i], &manyMem[i], &s, 0, 0));
        CK(vkBindImageMemory(dev, many[i], manyMem[i], 0));
        manyBytes += s;
    }
    for (int i = 0; i < MANY; i++) { vkDestroyImage(dev, many[i], 0); vkFreeMemory(dev, manyMem[i], 0); }
    printf("%d dedicated 512x512 images allocated, bound, freed (%.1f MiB)\n", MANY, manyBytes / 1048576.0);

    /* a non-dedicated optimal image on the same kind of memory: must keep the host-visible path */
    VkImage plainImg; CK(vkCreateImage(dev, &imi, 0, &plainImg));
    vkGetImageMemoryRequirements(dev, plainImg, &mr);
    VkMemoryAllocateInfo mpi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, itype};
    VkDeviceMemory plainMem; CK(vkAllocateMemory(dev, &mpi, 0, &plainMem)); CK(vkBindImageMemory(dev, plainImg, plainMem, 0));
    void* p2 = NULL;
    VkResult map2 = vkMapMemory(dev, plainMem, 0, VK_WHOLE_SIZE, 0, &p2);
    printf("vkMapMemory(non-dedicated image memory, type %u) = %d\n", itype, map2);
    if (map2 == VK_SUCCESS) vkUnmapMemory(dev, plainMem);
    vkDestroyImage(dev, plainImg, 0); vkFreeMemory(dev, plainMem, 0);

    /* transient attachment on lazily allocated memory, if any (the not-host-visible path) */
    VkImageCreateInfo ti = imi;
    ti.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    VkImage timg; CK(vkCreateImage(dev, &ti, 0, &timg));
    vkGetImageMemoryRequirements(dev, timg, &mr);
    uint32_t lazy = findType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, 0);
    if (lazy != UINT32_MAX) {
        VkMemoryAllocateInfo mti = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, lazy};
        VkDeviceMemory tmem; CK(vkAllocateMemory(dev, &mti, 0, &tmem)); CK(vkBindImageMemory(dev, timg, tmem, 0));
        printf("transient image on lazily allocated type %u (flags 0x%x): ok\n", lazy, mp.memoryTypes[lazy].propertyFlags);
        vkDestroyImage(dev, timg, 0); vkFreeMemory(dev, tmem, 0);
    }
    else { printf("no lazily allocated memory type for a transient image\n"); vkDestroyImage(dev, timg, 0); }

    vkDestroyFence(dev, fence, 0);
    vkDestroyCommandPool(dev, cp, 0);
    vkUnmapMemory(dev, bmem);
    vkDestroyBuffer(dev, buf, 0); vkFreeMemory(dev, bmem, 0);
    vkDestroyFramebuffer(dev, fb, 0); vkDestroyRenderPass(dev, rp, 0); vkDestroyImageView(dev, view, 0);
    vkDestroyImage(dev, img2, 0); vkFreeMemory(dev, imem2, 0);
    vkDestroyImage(dev, img, 0); vkFreeMemory(dev, imem, 0);
    vkDestroyDevice(dev, 0);
    vkDestroyInstance(inst, 0);

    if (bad || !countsOk || !mapOk) {
        printf("DEDICATED-FAIL (pixels wrong %d, counts %s, map %s)\n", bad, countsOk ? "ok" : "wrong", mapOk ? "ok" : "unexpected");
        return 1;
    }
    printf("DEDICATED-PASS (VORTEK_NATIVE_IMAGES=%s)\n", nativeEnv ? nativeEnv : "unset");
    return 0;
}
