/* [mali-g2] Checks for Mali driver quirks that a third-party probe (MaliBridge's startup probe on the
 * same tablet, r56p0) reported as failing, run through Vortek or, built as a bionic program, straight
 * on the device's driver:
 *   spec    a compute shader reads OpConstantComposite values built from spec constants (a bvec4
 *           condition of OpSelect, an ivec4, an array holding that ivec4) specialized away from their
 *           defaults; Mali folds such composites at the defaults (mq_spec.spvasm)
 *   bound   a fragment shader shaped like DXVK's texture reads: location 0 behind a bound check
 *           (OpSelect on a composite of a spec constant, specialized to true), location 1 behind an
 *           ordinary runtime condition that is false (mq_bound.spvasm); with MQ_ENGINE=DXVK
 *           (default) Winlator's removeImageBoundCheck sees it too
 *   linear  vkGetImageSubresourceLayout of LINEAR images of one row (Mali reports rowPitch 0)
 *   dmabuf  a LINEAR image created with VkExternalMemoryImageCreateInfo(DMA_BUF), which Mali refuses
 * MQ_PARTS (default all) picks parts, comma-separated; MQ_INLINE=1 hands the shaders to the pipelines
 * as VkShaderModuleCreateInfo in the stage's pNext (maintenance5) instead of modules; MQ_ENGINE sets
 * the engine name; MQ_FIX=1 applies ../src/spirv_spec_composite.h to the shaders itself (to check
 * the rewrite on the bare driver, as the in-process path uses it). Prints "MQ <part> PASS|FAIL ..." per part and exits 1 when one failed.
 * Build: cc -O2 -o mali_quirks_test mali_quirks_test.c -lvulkan
 *        clang --target=aarch64-linux-android30 -O2 -o mali_quirks_test mali_quirks_test.c -lvulkan */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "mali_quirks_spv.h"
#include "../src/spirv_spec_composite.h"

#define CK(x) do { VkResult r_ = (x); if (r_ < 0) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkCommandPool pool;
static bool inlineStages, fixShaders;
static int failures;

static bool part(const char* name) {
    const char* v = getenv("MQ_PARTS");
    if (!v || !*v || strcmp(v, "all") == 0) return true;
    size_t n = strlen(name);
    for (const char* p = v; (p = strstr(p, name)); p += n)
        if ((p == v || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return true;
    return false;
}

static void result(const char* name, bool ok, const char* detail) {
    printf("MQ %s %s %s\n", name, ok ? "PASS" : "FAIL", detail);
    if (!ok) failures++;
}

static uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    printf("FAIL no memory type 0x%x for 0x%x\n", bits, want);
    exit(1);
}

typedef struct { VkBuffer buf; VkDeviceMemory mem; void* map; } HostBuf;

static HostBuf hostBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    HostBuf b = {0};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE};
    CK(vkCreateBuffer(dev, &bi, NULL, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b.buf, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                               memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CK(vkAllocateMemory(dev, &ai, NULL, &b.mem));
    CK(vkBindBufferMemory(dev, b.buf, b.mem, 0));
    CK(vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map));
    memset(b.map, 0xcd, size);
    return b;
}

static void freeHostBuffer(HostBuf* b) {
    vkUnmapMemory(dev, b->mem);
    vkDestroyBuffer(dev, b->buf, NULL);
    vkFreeMemory(dev, b->mem, NULL);
}

static VkCommandBuffer beginCmd(void) {
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd;
    CK(vkAllocateCommandBuffers(dev, &ai, &cmd));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CK(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}

static void submitWait(VkCommandBuffer cmd) {
    CK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

/* A stage of `code`: a module, or with MQ_INLINE=1 the create info chained to the stage. */
static VkShaderModuleCreateInfo inlineInfos[4];
static int inlineUsed;
static VkPipelineShaderStageCreateInfo stage(VkShaderStageFlagBits s, const uint32_t* code, size_t size, const VkSpecializationInfo* spec) {
    VkPipelineShaderStageCreateInfo st = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, s, VK_NULL_HANDLE, "main", spec};
    static uint32_t fixed[4][1024];
    if (fixShaders && size <= sizeof(fixed[0])) {
        memcpy(fixed[inlineUsed], code, size);
        dd_spec_composites(fixed[inlineUsed], size / 4, fixed[inlineUsed]);
        code = fixed[inlineUsed];
    }
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    if (inlineStages) {
        inlineInfos[inlineUsed] = mi;
        st.pNext = &inlineInfos[inlineUsed];
    } else CK(vkCreateShaderModule(dev, &mi, NULL, &st.module));
    inlineUsed++;
    return st;
}

static void dropStages(VkPipelineShaderStageCreateInfo* st, int n) {
    for (int i = 0; i < n; i++) if (st[i].module) vkDestroyShaderModule(dev, st[i].module, NULL);
    inlineUsed = 0;
}

static VkDescriptorSetLayout setLayout(const VkDescriptorSetLayoutBinding* b, uint32_t n) {
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, n, b};
    VkDescriptorSetLayout l;
    CK(vkCreateDescriptorSetLayout(dev, &ci, NULL, &l));
    return l;
}

static VkDescriptorSet allocSet(VkDescriptorPool dp, VkDescriptorSetLayout l) {
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &l};
    VkDescriptorSet s;
    CK(vkAllocateDescriptorSets(dev, &ai, &s));
    return s;
}

static void testSpec(void) {
    HostBuf out = hostBuffer(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayout sl = setLayout(&b, 1);
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &sl};
    VkPipelineLayout layout;
    CK(vkCreatePipelineLayout(dev, &lci, NULL, &layout));

    /* defaults: false true false true, 1 2 3 4; specialized to the opposite */
    uint32_t values[8] = {1, 0, 1, 0, 5, 6, 7, 8};
    VkSpecializationMapEntry entries[8];
    for (uint32_t i = 0; i < 8; i++) entries[i] = (VkSpecializationMapEntry){i, i * 4, 4};
    VkSpecializationInfo spec = {8, entries, sizeof(values), values};
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = stage(VK_SHADER_STAGE_COMPUTE_BIT, mq_spec_spv, sizeof(mq_spec_spv), &spec);
    ci.layout = layout;
    VkPipeline pipe;
    CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe));
    dropStages(&ci.stage, 1);

    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    VkDescriptorPool dp;
    CK(vkCreateDescriptorPool(dev, &pci, NULL, &dp));
    VkDescriptorSet set = allocSet(dp, sl);
    VkDescriptorBufferInfo dbi = {out.buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi};
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

    VkCommandBuffer cmd = beginCmd();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cmd, 1, 1, 1);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    submitWait(cmd);

    const int32_t* o = out.map;
    static const int32_t want[16] = {11, 0, 33, 0, 5, 6, 7, 8, 5, 6, 7, 8, 99, 99, 99, 99};
    static const char* what[4] = {"select(bvec4 composite)", "ivec4 composite", "array of it", "select(scalar)"};
    for (int r = 0; r < 4; r++) {
        char d[160];
        bool ok = memcmp(o + r * 4, want + r * 4, 16) == 0;
        snprintf(d, sizeof(d), "%s: %d %d %d %d (want %d %d %d %d)", what[r], o[r * 4], o[r * 4 + 1], o[r * 4 + 2], o[r * 4 + 3],
                 want[r * 4], want[r * 4 + 1], want[r * 4 + 2], want[r * 4 + 3]);
        result("spec", ok, d);
    }

    vkDestroyDescriptorPool(dev, dp, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, sl, NULL);
    freeHostBuffer(&out);
}

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } Img;

static Img image2d(uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    Img i = {0};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {w, h, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE};
    CK(vkCreateImage(dev, &ci, NULL, &i.img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, i.img, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CK(vkAllocateMemory(dev, &ai, NULL, &i.mem));
    CK(vkBindImageMemory(dev, i.img, i.mem, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, i.img, VK_IMAGE_VIEW_TYPE_2D, ci.format};
    vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CK(vkCreateImageView(dev, &vi, NULL, &i.view));
    return i;
}

static void freeImage(Img* i) {
    vkDestroyImageView(dev, i->view, NULL);
    vkDestroyImage(dev, i->img, NULL);
    vkFreeMemory(dev, i->mem, NULL);
}

static void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                    VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, src, dst, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void testBound(void) {
    const uint32_t W = 4;
    const uint8_t texel[4] = {64, 128, 192, 255};
    Img tex = image2d(1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Img rt[2] = {image2d(W, W, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
                 image2d(W, W, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)};
    HostBuf up = hostBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    memcpy(up.map, texel, 4);
    HostBuf down = hostBuffer(2 * W * W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler;
    CK(vkCreateSampler(dev, &sci, NULL, &sampler));

    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
                                         {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayout sl = setLayout(b, 2);
    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &sl, 1, &pcr};
    VkPipelineLayout layout;
    CK(vkCreatePipelineLayout(dev, &lci, NULL, &layout));

    VkAttachmentDescription att[2];
    VkAttachmentReference ref[2];
    for (int i = 0; i < 2; i++) {
        att[i] = (VkAttachmentDescription){0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                           VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        ref[i] = (VkAttachmentReference){(uint32_t)i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 2, ref};
    VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 2, att, 1, &sub, 1, &dep};
    VkRenderPass rp;
    CK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
    VkImageView views[2] = {rt[0].view, rt[1].view};
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 2, views, W, W, 1};
    VkFramebuffer fb;
    CK(vkCreateFramebuffer(dev, &fci, NULL, &fb));

    uint32_t bound = 1; /* the bound check's spec constant: the texture is bound */
    VkSpecializationMapEntry e = {0, 0, 4};
    VkSpecializationInfo spec = {1, &e, 4, &bound};
    VkPipelineShaderStageCreateInfo st[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, mq_quad_spv, sizeof(mq_quad_spv), NULL),
                                             stage(VK_SHADER_STAGE_FRAGMENT_BIT, mq_bound_spv, sizeof(mq_bound_spv), &spec)};
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp = {0, 0, W, W, 0, 1};
    VkRect2D sc = {{0, 0}, {W, W}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba[2] = {{0}, {0}};
    cba[0].colorWriteMask = cba[1].colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 2, cba};
    VkGraphicsPipelineCreateInfo gci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vin, &ia, NULL, &vps, &rs, &ms, NULL, &cb, NULL,
                                        layout, rp, 0};
    VkPipeline pipe;
    CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe));
    dropStages(st, 2);

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 2, ps};
    VkDescriptorPool dp;
    CK(vkCreateDescriptorPool(dev, &pci, NULL, &dp));
    VkDescriptorSet set = allocSet(dp, sl);
    VkDescriptorImageInfo dii = {VK_NULL_HANDLE, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo dsi = {sampler};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &dii},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &dsi}};
    vkUpdateDescriptorSets(dev, 2, w, 0, NULL);

    VkCommandBuffer cmd = beginCmd();
    barrier(cmd, tex.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy upc = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}};
    vkCmdCopyBufferToImage(cmd, up.buf, tex.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upc);
    barrier(cmd, tex.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkClearValue clear[2] = {{.color = {.float32 = {1, 0, 1, 1}}}, {.color = {.float32 = {1, 0, 1, 1}}}};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {W, W}}, 2, clear};
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    uint32_t runtimeCond = 0; /* location 1: the texel only where this is non-zero, i.e. nowhere */
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &runtimeCond);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    for (int i = 0; i < 2; i++) {
        VkBufferImageCopy c = {(VkDeviceSize)i * W * W * 4, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, W, 1}};
        vkCmdCopyImageToBuffer(cmd, rt[i].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.buf, 1, &c);
    }
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    submitWait(cmd);

    const uint8_t* px = down.map;
    const uint8_t* p0 = px + (W + 1) * 4;
    const uint8_t* p1 = px + W * W * 4 + (W + 1) * 4;
    char d[160];
    snprintf(d, sizeof(d), "bound check (spec constant composite, specialized true): %u %u %u %u (want the texel %u %u %u %u)",
             p0[0], p0[1], p0[2], p0[3], texel[0], texel[1], texel[2], texel[3]);
    result("bound", memcmp(p0, texel, 4) == 0, d);
    snprintf(d, sizeof(d), "runtime condition false after a sample: %u %u %u %u (want 0 0 0 0)", p1[0], p1[1], p1[2], p1[3]);
    static const uint8_t zero[4];
    result("bound", memcmp(p1, zero, 4) == 0, d);

    vkDestroyDescriptorPool(dev, dp, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyFramebuffer(dev, fb, NULL);
    vkDestroyRenderPass(dev, rp, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, sl, NULL);
    vkDestroySampler(dev, sampler, NULL);
    freeHostBuffer(&up);
    freeHostBuffer(&down);
    freeImage(&tex);
    freeImage(&rt[0]);
    freeImage(&rt[1]);
}

typedef struct { const char* name; VkImageType type; VkFormat format; uint32_t bpp, w, h, d, mips, layers, mip; } LinearCase;

static void testLinear(void) {
    static const LinearCase cases[] = {
        {"2D 16x1 RGBA8", VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 4, 16, 1, 1, 1, 1, 0},
        {"2D 1x1 RGBA8", VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 4, 1, 1, 1, 1, 1, 0},
        {"2D 1x16 RGBA8", VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 4, 1, 16, 1, 1, 1, 0},
        {"2D 300x1 R8", VK_IMAGE_TYPE_2D, VK_FORMAT_R8_UNORM, 1, 300, 1, 1, 1, 1, 0},
        {"2D 300x2 R8", VK_IMAGE_TYPE_2D, VK_FORMAT_R8_UNORM, 1, 300, 2, 1, 1, 1, 0},
        {"1D 4096 R32F", VK_IMAGE_TYPE_1D, VK_FORMAT_R32_SFLOAT, 4, 4096, 1, 1, 1, 1, 0},
        {"2D 64x1 RGBA16F", VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 64, 1, 1, 1, 1, 0},
        {"2D 16x1 RGBA8 4 layers", VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 4, 16, 1, 1, 1, 4, 0},
    };
    VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const LinearCase* c = &cases[i];
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, c->type, c->format, {c->w, c->h, c->d}, c->mips, c->layers,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_SHARING_MODE_EXCLUSIVE};
        VkImage img;
        VkResult r = vkCreateImage(dev, &ci, NULL, &img);
        char d[200];
        if (r != VK_SUCCESS) {
            snprintf(d, sizeof(d), "%s: vkCreateImage %d (skipped)", c->name, r);
            printf("MQ linear SKIP %s\n", d);
            continue;
        }
        VkSubresourceLayout l;
        memset(&l, 0, sizeof(l));
        vkGetImageSubresourceLayout(dev, img, &sub, &l);
        uint32_t rowBytes = c->w * c->bpp;
        bool ok = l.rowPitch >= rowBytes && l.size >= rowBytes && l.offset + l.rowPitch * (c->h - 1) + rowBytes <= l.offset + l.size;
        snprintf(d, sizeof(d), "%s: rowPitch %llu arrayPitch %llu size %llu offset %llu (a row is %u bytes)", c->name,
                 (unsigned long long)l.rowPitch, (unsigned long long)l.arrayPitch, (unsigned long long)l.size, (unsigned long long)l.offset, rowBytes);
        result("linear", ok, d);
        vkDestroyImage(dev, img, NULL);
    }
}

static void testDmabuf(void) {
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {8, 8, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_SHARING_MODE_EXCLUSIVE};
    VkImage img;
    VkResult r = vkCreateImage(dev, &ci, NULL, &img);
    char d[160];
    snprintf(d, sizeof(d), "LINEAR 8x8 RGBA8 with DMA_BUF handle type: vkCreateImage %d", r);
    result("dmabuf", r == VK_SUCCESS, d);
    if (r == VK_SUCCESS) {
        VkSubresourceLayout l;
        VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        vkGetImageSubresourceLayout(dev, img, &sub, &l);
        printf("MQ dmabuf layout rowPitch %llu size %llu\n", (unsigned long long)l.rowPitch, (unsigned long long)l.size);
        vkDestroyImage(dev, img, NULL);
    }
}

int main(void) {
    const char* engine = getenv("MQ_ENGINE");
    const char* inl = getenv("MQ_INLINE");
    inlineStages = inl && *inl == '1';
    const char* fix = getenv("MQ_FIX");
    fixShaders = fix && *fix == '1';
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "mali_quirks_test", 1, engine ? engine : "DXVK",
                             VK_MAKE_VERSION(3, 1, 1), VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    CK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1;
    VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (er < 0 || n == 0) { printf("FAIL no physical device (%d)\n", er); return 1; }
    VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
    vkGetPhysicalDeviceProperties2(pd, &props);
    printf("MQ device %s, driver %u %s, engine %s%s%s\n", props.properties.deviceName, drv.driverID, drv.driverInfo, app.pEngineName,
           inlineStages ? ", inline stages" : "", fixShaders ? ", shaders rewritten by the test" : "");

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties qp[16];
    if (qn > 16) qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qp);
    for (qfam = 0; qfam < qn && (qp[qfam].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT); qfam++) {}
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    const char* exts[1];
    uint32_t extCount = 0;
    if (inlineStages) exts[extCount++] = "VK_KHR_maintenance5";
    VkPhysicalDeviceMaintenance5FeaturesKHR m5 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, NULL, VK_TRUE};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, inlineStages ? &m5 : NULL, 0, 1, &qci, 0, NULL, extCount, exts};
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    CK(vkCreateCommandPool(dev, &pci, NULL, &pool));

    if (part("spec")) testSpec();
    if (part("bound")) testBound();
    if (part("linear")) testLinear();
    if (part("dmabuf")) testDmabuf();

    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("MQ done: %d failed\n", failures);
    return failures ? 1 : 0;
}
