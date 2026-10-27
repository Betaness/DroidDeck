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
 *   linear  vkGetImageSubresourceLayout of LINEAR images of one row (Mali reports rowPitch 0); with
 *           MQ_INLINE=1 (maintenance5) also vkGetImageSubresourceLayout2KHR and
 *           vkGetDeviceImageSubresourceLayoutKHR
 *   dmabuf  images created with VkExternalMemoryImageCreateInfo(DMA_BUF), which Mali refuses (-1000158000):
 *           passes when the format query refuses them too
 *   wire    a triangle covering a 16x16 target drawn with VK_POLYGON_MODE_LINE (fillModeNonSolid,
 *           which Mali lacks and Vortek reports): counts the lit pixels, an INFO line (r56p0 fills it
 *           solid, as with VK_POLYGON_MODE_POINT; MQ_WIRE_MODE=1 point, 2 fill, which must pass)
 * MQ_PARTS (default all) picks parts, comma-separated; MQ_INLINE=1 hands the shaders to the pipelines
 * as VkShaderModuleCreateInfo in the stage's pNext (maintenance5) instead of modules, MQ_INLINE=2
 * the same behind a VkPipelineRobustnessCreateInfoEXT (device defaults) in the chain, which is valid
 * in any order (needs VK_EXT_pipeline_robustness; through Vortek such a stage chain fails with -4,
 * which it did before the client looked past the first structure); MQ_ENGINE sets
 * the engine name; MQ_FIX=1 applies ../src/spirv_spec_composite.h and ../src/linear_row_pitch.h
 * itself (to check them on the bare driver, as the in-process path uses them).
 * Prints "MQ <part> PASS|FAIL ..." per part and exits 1 when one failed.
 * Build: cc -O2 -o mali_quirks_test mali_quirks_test.c -lvulkan
 *        clang --target=aarch64-linux-android30 -O2 -o mali_quirks_test mali_quirks_test.c -lvulkan */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "mali_quirks_spv.h"
#include "../src/spirv_spec_composite.h"
#include "../src/linear_row_pitch.h"

#define CK(x) do { VkResult r_ = (x); if (r_ < 0) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkCommandPool pool;
static bool inlineStages, fixShaders; /* MQ_INLINE, MQ_FIX */
static bool inlineSecond;             /* MQ_INLINE=2 */
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

/* A stage of `code`: a module, or with MQ_INLINE=1 the create info chained to the stage (=2: second
 * in the chain). */
static VkShaderModuleCreateInfo inlineInfos[4];
static VkPipelineRobustnessCreateInfoEXT inlineAhead[4];
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
        if (inlineSecond) {
            inlineAhead[inlineUsed] = (VkPipelineRobustnessCreateInfoEXT){VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT,
                                                                         &inlineInfos[inlineUsed]};
            st.pNext = &inlineAhead[inlineUsed];
        }
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

static void testWire(void) {
    const uint32_t W = 16;
    const char* m = getenv("MQ_WIRE_MODE");
    VkPolygonMode mode = m && *m == '1' ? VK_POLYGON_MODE_POINT : m && *m == '2' ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE;
    Img rt = image2d(W, W, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    HostBuf down = hostBuffer(W * W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout;
    CK(vkCreatePipelineLayout(dev, &lci, NULL, &layout));
    VkAttachmentDescription att = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref};
    VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 1, &dep};
    VkRenderPass rp;
    CK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &rt.view, W, W, 1};
    VkFramebuffer fb;
    CK(vkCreateFramebuffer(dev, &fci, NULL, &fb));
    VkPipelineShaderStageCreateInfo st[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, mq_quad_spv, sizeof(mq_quad_spv), NULL),
                                             stage(VK_SHADER_STAGE_FRAGMENT_BIT, mq_white_spv, sizeof(mq_white_spv), NULL)};
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp = {0, 0, W, W, 0, 1};
    VkRect2D sc = {{0, 0}, {W, W}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = mode;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cba};
    VkGraphicsPipelineCreateInfo gci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vin, &ia, NULL, &vps, &rs, &ms, NULL, &cb, NULL,
                                        layout, rp, 0};
    VkPipeline pipe;
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe);
    dropStages(st, 2);
    char d[160];
    if (r == VK_SUCCESS) {
        VkCommandBuffer cmd = beginCmd();
        VkClearValue clear = {.color = {.float32 = {0, 0, 0, 0}}};
        VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {W, W}}, 1, &clear};
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, W, 1}};
        vkCmdCopyImageToBuffer(cmd, rt.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.buf, 1, &c);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        submitWait(cmd);
        const uint8_t* px = down.map;
        unsigned lit = 0;
        for (uint32_t i = 0; i < W * W; i++) lit += px[i * 4] != 0;
        /* fill lights all 256; the triangle's two edges inside the target, a row and a column, 31 */
        unsigned want = mode == VK_POLYGON_MODE_FILL ? W * W : mode == VK_POLYGON_MODE_LINE ? 2 * W - 1 : 0;
        snprintf(d, sizeof(d), "polygon mode %d: %u of %u pixels lit (lines: %u, fill: %u)", mode, lit, W * W, 2 * W - 1, W * W);
        if (mode == VK_POLYGON_MODE_FILL) result("wire", lit == want, d);
        else printf("MQ wire INFO %s: %s\n", d, lit == W * W ? "drawn solid" : lit == want ? "drawn as lines" : "other");
        vkDestroyPipeline(dev, pipe, NULL);
    } else {
        snprintf(d, sizeof(d), "polygon mode %d: vkCreateGraphicsPipelines %d", mode, r);
        result("wire", false, d);
    }
    vkDestroyFramebuffer(dev, fb, NULL);
    vkDestroyRenderPass(dev, rp, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    freeHostBuffer(&down);
    freeImage(&rt);
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
        {"3D 16x1x4 RGBA8", VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 4, 16, 1, 4, 1, 1, 0},
        {"3D 16x2x4 RGBA8", VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 4, 16, 2, 4, 1, 1, 0},
        {"2D 16x2 RGBA8 mip 1 (8x1)", VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 4, 16, 2, 1, 2, 1, 1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const LinearCase* c = &cases[i];
        VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, c->mip, 0};
        uint32_t w = c->w >> c->mip ? c->w >> c->mip : 1, h = c->h >> c->mip ? c->h >> c->mip : 1;
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, c->type, c->format, {c->w, c->h, c->d}, c->mips, c->layers,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_SHARING_MODE_EXCLUSIVE};
        VkImage img;
        VkResult r = vkCreateImage(dev, &ci, NULL, &img);
        char d[240];
        if (r != VK_SUCCESS) {
            snprintf(d, sizeof(d), "%s: vkCreateImage %d (skipped)", c->name, r);
            printf("MQ linear SKIP %s\n", d);
            continue;
        }
        VkSubresourceLayout l;
        memset(&l, 0, sizeof(l));
        vkGetImageSubresourceLayout(dev, img, &sub, &l);
        if (fixShaders) dd_fix_linear_row_pitch(&l);
        uint32_t rowBytes = w * c->bpp;
        VkDeviceSize slice = c->type == VK_IMAGE_TYPE_3D ? l.depthPitch : l.size;
        bool ok = l.rowPitch >= rowBytes && slice >= l.rowPitch * (h - 1) + rowBytes && l.size >= slice;
        snprintf(d, sizeof(d), "%s: rowPitch %llu depthPitch %llu arrayPitch %llu size %llu offset %llu (a row is %u bytes)", c->name,
                 (unsigned long long)l.rowPitch, (unsigned long long)l.depthPitch, (unsigned long long)l.arrayPitch, (unsigned long long)l.size,
                 (unsigned long long)l.offset, rowBytes);
        result("linear", ok, d);
        PFN_vkGetImageSubresourceLayout2KHR layout2 =
            inlineStages ? (PFN_vkGetImageSubresourceLayout2KHR)vkGetDeviceProcAddr(dev, "vkGetImageSubresourceLayout2KHR") : NULL;
        if (layout2) {
            VkImageSubresource2KHR sub2 = {VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2_KHR, NULL, sub};
            VkSubresourceLayout2KHR l2 = {VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2_KHR};
            layout2(dev, img, &sub2, &l2);
            if (fixShaders) dd_fix_linear_row_pitch(&l2.subresourceLayout);
            snprintf(d, sizeof(d), "%s through vkGetImageSubresourceLayout2KHR: rowPitch %llu", c->name, (unsigned long long)l2.subresourceLayout.rowPitch);
            result("linear", l2.subresourceLayout.rowPitch == l.rowPitch && l.rowPitch, d);
        }
        PFN_vkGetDeviceImageSubresourceLayoutKHR deviceLayout =
            inlineStages ? (PFN_vkGetDeviceImageSubresourceLayoutKHR)vkGetDeviceProcAddr(dev, "vkGetDeviceImageSubresourceLayoutKHR") : NULL;
        if (deviceLayout) {
            VkImageSubresource2KHR sub2 = {VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2_KHR, NULL, sub};
            VkDeviceImageSubresourceInfoKHR info = {VK_STRUCTURE_TYPE_DEVICE_IMAGE_SUBRESOURCE_INFO_KHR, NULL, &ci, &sub2};
            VkSubresourceLayout2KHR l3 = {VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2_KHR};
            deviceLayout(dev, &info, &l3);
            if (fixShaders) dd_fix_linear_row_pitch(&l3.subresourceLayout);
            snprintf(d, sizeof(d), "%s through vkGetDeviceImageSubresourceLayoutKHR: rowPitch %llu", c->name,
                     (unsigned long long)l3.subresourceLayout.rowPitch);
            result("linear", l3.subresourceLayout.rowPitch == l.rowPitch && l.rowPitch, d);
        }
        vkDestroyImage(dev, img, NULL);
    }
}

static void testDmabuf(void) {
    static const VkImageTiling tilings[2] = {VK_IMAGE_TILING_LINEAR, VK_IMAGE_TILING_OPTIMAL};
    for (int t = 0; t < 2; t++) {
        VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VkPhysicalDeviceExternalImageFormatInfo extInfo = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, NULL,
                                                           VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkPhysicalDeviceImageFormatInfo2 fi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &extInfo, VK_FORMAT_R8G8B8A8_UNORM,
                                               VK_IMAGE_TYPE_2D, tilings[t], usage};
        VkExternalImageFormatProperties extProps = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 fp = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &extProps};
        VkResult q = vkGetPhysicalDeviceImageFormatProperties2(pd, &fi, &fp);
        VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {8, 8, 1}, 1, 1,
                                VK_SAMPLE_COUNT_1_BIT, tilings[t], usage, VK_SHARING_MODE_EXCLUSIVE};
        VkImage img;
        VkResult r = vkCreateImage(dev, &ci, NULL, &img);
        if (r == VK_SUCCESS) vkDestroyImage(dev, img, NULL);
        /* refusing a combination the format query also refuses is allowed; apps ask first */
        char d[200];
        snprintf(d, sizeof(d), "%s 8x8 RGBA8 with the DMA_BUF handle type: format query %d (features 0x%x), vkCreateImage %d",
                 t ? "OPTIMAL" : "LINEAR", q, extProps.externalMemoryProperties.externalMemoryFeatures, r);
        result("dmabuf", q != VK_SUCCESS || r == VK_SUCCESS, d);
    }
}

int main(void) {
    const char* engine = getenv("MQ_ENGINE");
    const char* inl = getenv("MQ_INLINE");
    inlineStages = inl && (*inl == '1' || *inl == '2');
    inlineSecond = inl && *inl == '2';
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
           inlineSecond ? ", inline stages second in the chain" : inlineStages ? ", inline stages" : "", fixShaders ? ", fixes applied by the test" : "");

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties qp[16];
    if (qn > 16) qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qp);
    for (qfam = 0; qfam < qn && (qp[qfam].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT); qfam++) {}
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    const char* exts[2];
    uint32_t extCount = 0;
    if (inlineStages) exts[extCount++] = "VK_KHR_maintenance5";
    if (inlineSecond) {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(pd, NULL, &count, NULL);
        VkExtensionProperties* list = calloc(count, sizeof(*list));
        vkEnumerateDeviceExtensionProperties(pd, NULL, &count, list);
        bool found = false;
        for (uint32_t i = 0; list && i < count; i++) found = found || !strcmp(list[i].extensionName, "VK_EXT_pipeline_robustness");
        free(list);
        if (!found) { printf("FAIL MQ_INLINE=2 needs VK_EXT_pipeline_robustness\n"); return 1; }
        exts[extCount++] = "VK_EXT_pipeline_robustness";
    }
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
    if (part("wire")) testWire();

    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("MQ done: %d failed\n", failures);
    return failures ? 1 : 0;
}
