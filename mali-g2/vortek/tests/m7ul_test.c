/* [mali-g2] VK_KHR_maintenance7, VK_KHR_unified_image_layouts and VK_KHR_shader_float_controls2
 * through Vortek, the way DXVK 3.1 uses them (winlator-0014 / vortek-0010 forward their structs).
 *
 * - The instance says engine "DXVK" 3.1.1, so the server takes its DXVK branch (which hides
 *   VK_EXT_hdr_metadata); the three extensions must still be listed.
 * - Features2 must report maintenance7, unifiedImageLayouts and shaderFloatControls2, and
 *   Properties2 the maintenance7 properties (DXVK reads separateDepthStencilAttachmentAccess).
 * - The device is created with the three features enabled, the way DXVK enables them (the server
 *   logs "vortek: vkCreateDevice: maintenance7 1, unifiedImageLayouts 1, shaderFloatControls2 1").
 * - Depth/stencil with separate access, all in VK_IMAGE_LAYOUT_GENERAL (unified layouts, no
 *   per-use transitions): clear depth 0.25 / stencil 0x55; pass 1 writes depth 0.75 with the
 *   stencil aspect LOAD_OP_NONE / STORE_OP_NONE; pass 2 writes stencil 0xA3 on the left half with
 *   the depth aspect NONE / NONE. The readback must give depth 0.75 everywhere and stencil 0xA3
 *   left, 0x55 right.
 * - A compute shader with SPV_KHR_float_controls2 (FPFastMathDefault None: strict IEEE) computes
 *   x - x and x * 0 for inf, -inf, NaN, 1: NaN, NaN, NaN, 0 and NaN, NaN, NaN, 0.
 * Prints M7UL-PASS.
 *
 * Build: cc -O2 -o m7ul_test m7ul_test.c -lvulkan   (shaders: m7ul.vert, m7ul_fc2.spvasm -> m7ul_spv.h) */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "m7ul_spv.h"

/* The system headers may predate these extensions (values from Vulkan-Headers 1.4.341). */
#ifndef VK_KHR_maintenance7
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR ((VkStructureType)1000562000)
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR ((VkStructureType)1000562001)
typedef struct VkPhysicalDeviceMaintenance7FeaturesKHR { VkStructureType sType; void* pNext; VkBool32 maintenance7; } VkPhysicalDeviceMaintenance7FeaturesKHR;
typedef struct VkPhysicalDeviceMaintenance7PropertiesKHR {
    VkStructureType sType; void* pNext;
    VkBool32 robustFragmentShadingRateAttachmentAccess, separateDepthStencilAttachmentAccess;
    uint32_t maxDescriptorSetTotalUniformBuffersDynamic, maxDescriptorSetTotalStorageBuffersDynamic, maxDescriptorSetTotalBuffersDynamic;
    uint32_t maxDescriptorSetUpdateAfterBindTotalUniformBuffersDynamic, maxDescriptorSetUpdateAfterBindTotalStorageBuffersDynamic, maxDescriptorSetUpdateAfterBindTotalBuffersDynamic;
} VkPhysicalDeviceMaintenance7PropertiesKHR;
#endif
#ifndef VK_KHR_unified_image_layouts
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR ((VkStructureType)1000527000)
typedef struct VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR { VkStructureType sType; void* pNext; VkBool32 unifiedImageLayouts, unifiedImageLayoutsVideo; } VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR;
#endif
#ifndef VK_KHR_shader_float_controls2
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT_CONTROLS_2_FEATURES_KHR ((VkStructureType)1000528000)
typedef struct VkPhysicalDeviceShaderFloatControls2FeaturesKHR { VkStructureType sType; void* pNext; VkBool32 shaderFloatControls2; } VkPhysicalDeviceShaderFloatControls2FeaturesKHR;
#endif
#ifndef VK_ATTACHMENT_LOAD_OP_NONE_KHR
#define VK_ATTACHMENT_LOAD_OP_NONE_KHR VK_ATTACHMENT_LOAD_OP_NONE_EXT
#endif

#define W 64
#define H 64
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); return 1; } } while (0)
#define FAIL(...) do { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)

static VkPhysicalDevice pd;
static VkDevice dev;

static uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buf, VkDeviceMemory* mem, void** map) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size, usage};
    CK(vkCreateBuffer(dev, &bi, 0, buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, *buf, &mr);
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size,
                               memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CK(vkAllocateMemory(dev, &ma, 0, mem));
    CK(vkBindBufferMemory(dev, *buf, *mem, 0));
    CK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, map));
    return 0;
}

static int hasExtension(VkExtensionProperties* exts, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; i++) if (!strcmp(exts[i].extensionName, name)) return 1;
    return 0;
}

static void generalBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = oldLayout;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}

int main(void) {
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "m7ul", 1, "DXVK", VK_MAKE_VERSION(3, 1, 1), VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai};
    VkInstance inst; CK(vkCreateInstance(&ii, 0, &inst));
    uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) FAIL("no physical device");

    /* Extensions, as DXVK sees them (engine DXVK: the server's DXVK branch) */
    uint32_t extCount = 0;
    CK(vkEnumerateDeviceExtensionProperties(pd, 0, &extCount, 0));
    VkExtensionProperties exts[512];
    if (extCount > 512) extCount = 512;
    CK(vkEnumerateDeviceExtensionProperties(pd, 0, &extCount, exts));
    int hasM7 = hasExtension(exts, extCount, "VK_KHR_maintenance7");
    int hasUnified = hasExtension(exts, extCount, "VK_KHR_unified_image_layouts");
    int hasFc2 = hasExtension(exts, extCount, "VK_KHR_shader_float_controls2");
    int hasLoadOpNone = hasExtension(exts, extCount, "VK_KHR_load_store_op_none");
    int hasHdr = hasExtension(exts, extCount, "VK_EXT_hdr_metadata");
    printf("extensions (%u): VK_KHR_maintenance7 %d, VK_KHR_unified_image_layouts %d, VK_KHR_shader_float_controls2 %d, VK_KHR_load_store_op_none %d, VK_EXT_hdr_metadata %d (hidden for DXVK)\n",
           extCount, hasM7, hasUnified, hasFc2, hasLoadOpNone, hasHdr);
    if (!hasM7 || !hasUnified || !hasFc2 || !hasLoadOpNone) FAIL("extension missing");

    /* Features and properties */
    VkPhysicalDeviceDynamicRenderingFeatures fDr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceShaderFloatControls2FeaturesKHR fFc2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT_CONTROLS_2_FEATURES_KHR, &fDr};
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR fUl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR, &fFc2};
    VkPhysicalDeviceMaintenance7FeaturesKHR fM7 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR, &fUl};
    VkPhysicalDeviceFeatures2 ff = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &fM7};
    vkGetPhysicalDeviceFeatures2(pd, &ff);
    printf("features: maintenance7 %u, unifiedImageLayouts %u (video %u), shaderFloatControls2 %u, dynamicRendering %u\n",
           fM7.maintenance7, fUl.unifiedImageLayouts, fUl.unifiedImageLayoutsVideo, fFc2.shaderFloatControls2, fDr.dynamicRendering);
    if (!fM7.maintenance7 || !fUl.unifiedImageLayouts || !fFc2.shaderFloatControls2 || !fDr.dynamicRendering) FAIL("features");

    VkPhysicalDeviceMaintenance7PropertiesKHR pM7 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 pp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pM7};
    vkGetPhysicalDeviceProperties2(pd, &pp);
    printf("maintenance7 properties: robustFragmentShadingRateAttachmentAccess %u, separateDepthStencilAttachmentAccess %u, "
           "maxDescriptorSetTotal{Uniform,Storage,}BuffersDynamic %u/%u/%u, UpdateAfterBind %u/%u/%u\n",
           pM7.robustFragmentShadingRateAttachmentAccess, pM7.separateDepthStencilAttachmentAccess,
           pM7.maxDescriptorSetTotalUniformBuffersDynamic, pM7.maxDescriptorSetTotalStorageBuffersDynamic, pM7.maxDescriptorSetTotalBuffersDynamic,
           pM7.maxDescriptorSetUpdateAfterBindTotalUniformBuffersDynamic, pM7.maxDescriptorSetUpdateAfterBindTotalStorageBuffersDynamic,
           pM7.maxDescriptorSetUpdateAfterBindTotalBuffersDynamic);
    if (!pM7.separateDepthStencilAttachmentAccess || !pM7.maxDescriptorSetTotalBuffersDynamic) FAIL("maintenance7 properties");

    /* Device, with the features enabled the way DXVK does it */
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio};
    const char* devExts[] = {"VK_KHR_maintenance7", "VK_KHR_unified_image_layouts", "VK_KHR_shader_float_controls2",
                             "VK_KHR_load_store_op_none", "VK_KHR_dynamic_rendering"};
    fUl.unifiedImageLayoutsVideo = VK_FALSE;
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &fM7, 0, 1, &qi, 0, 0, 5, devExts};
    CK(vkCreateDevice(pd, &di, 0, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, 0, 0, &queue);
    PFN_vkCmdBeginRenderingKHR beginRendering = (void*)vkGetDeviceProcAddr(dev, "vkCmdBeginRenderingKHR");
    PFN_vkCmdEndRenderingKHR endRendering = (void*)vkGetDeviceProcAddr(dev, "vkCmdEndRenderingKHR");
    if (!beginRendering || !endRendering) FAIL("no vkCmdBeginRenderingKHR");

    /* Depth/stencil image */
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
    for (int i = 0; i < 2 && fmt == VK_FORMAT_UNDEFINED; i++) {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, candidates[i], &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((fp.optimalTilingFeatures & need) == need) fmt = candidates[i];
    }
    if (fmt == VK_FORMAT_UNDEFINED) FAIL("no depth/stencil format");
    printf("depth/stencil format %s\n", fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32_SFLOAT_S8_UINT" : "D24_UNORM_S8_UINT");

    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt; ici.extent = (VkExtent3D){W, H, 1};
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage image; CK(vkCreateImage(dev, &ici, 0, &image));
    VkMemoryRequirements imr; vkGetImageMemoryRequirements(dev, image, &imr);
    VkMemoryAllocateInfo ima = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, imr.size, memType(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory imageMem; CK(vkAllocateMemory(dev, &ima, 0, &imageMem));
    CK(vkBindImageMemory(dev, image, imageMem, 0));
    VkImageViewCreateInfo ivi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, image, VK_IMAGE_VIEW_TYPE_2D, fmt};
    ivi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    VkImageView view; CK(vkCreateImageView(dev, &ivi, 0, &view));

    VkBuffer depthBuf, stencilBuf; VkDeviceMemory depthMem, stencilMem; void *depthMap, *stencilMap;
    if (makeBuffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &depthBuf, &depthMem, &depthMap)) return 1;
    if (makeBuffer(W * H, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &stencilBuf, &stencilMem, &stencilMap)) return 1;

    /* Depth-only pipeline: full-screen triangle at depth pc.depth */
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 0, 0, 1, &pcr};
    VkPipelineLayout gfxLayout; CK(vkCreatePipelineLayout(dev, &pli, 0, &gfxLayout));
    VkShaderModuleCreateInfo vsi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(m7ul_vert_spv), m7ul_vert_spv};
    VkShaderModule vs; CK(vkCreateShaderModule(dev, &vsi, 0, &vs));
    VkPipelineShaderStageCreateInfo vstage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vpFull = {0, 0, W, H, 0, 1};
    VkRect2D scFull = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, &vpFull, 1, &scFull};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 1, dyn};
    VkPipelineRenderingCreateInfo prci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, 0, 0, fmt, fmt};

    VkPipelineDepthStencilStateCreateInfo dsDepth = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dsDepth.depthTestEnable = VK_TRUE; dsDepth.depthWriteEnable = VK_TRUE; dsDepth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineDepthStencilStateCreateInfo dsStencil = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dsStencil.stencilTestEnable = VK_TRUE;
    dsStencil.front = dsStencil.back = (VkStencilOpState){VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 0xA3};

    VkGraphicsPipelineCreateInfo gpi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &prci};
    gpi.stageCount = 1; gpi.pStages = &vstage; gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms; gpi.pDynamicState = &dy;
    gpi.layout = gfxLayout; gpi.basePipelineIndex = -1;
    VkPipeline depthPipe, stencilPipe;
    gpi.pDepthStencilState = &dsDepth;
    CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, 0, &depthPipe));
    gpi.pDepthStencilState = &dsStencil;
    CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, 0, &stencilPipe));

    /* float_controls2 compute pipeline */
    VkDescriptorSetLayoutBinding binds[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dsli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 2, binds};
    VkDescriptorSetLayout setLayout; CK(vkCreateDescriptorSetLayout(dev, &dsli, 0, &setLayout));
    VkPipelineLayoutCreateInfo cpli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &setLayout};
    VkPipelineLayout compLayout; CK(vkCreatePipelineLayout(dev, &cpli, 0, &compLayout));
    VkShaderModuleCreateInfo csi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(m7ul_fc2_spv), m7ul_fc2_spv};
    VkShaderModule cs; CK(vkCreateShaderModule(dev, &csi, 0, &cs));
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main"};
    cpi.layout = compLayout; cpi.basePipelineIndex = -1;
    VkPipeline compPipe; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, 0, &compPipe));
    VkBuffer inBuf, outBuf; VkDeviceMemory inMem, outMem; void *inMap, *outMap;
    if (makeBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &inBuf, &inMem, &inMap)) return 1;
    if (makeBuffer(32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &outBuf, &outMem, &outMap)) return 1;
    float in[4] = {INFINITY, -INFINITY, NAN, 1.0f};
    memcpy(inMap, in, sizeof(in));
    memset(outMap, 0x7b, 32);
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 1, 1, &poolSize};
    VkDescriptorPool pool; CK(vkCreateDescriptorPool(dev, &dpi, 0, &pool));
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, pool, 1, &setLayout};
    VkDescriptorSet set; CK(vkAllocateDescriptorSets(dev, &dsai, &set));
    VkDescriptorBufferInfo dbi[2] = {{inBuf, 0, 16}, {outBuf, 0, 32}};
    VkWriteDescriptorSet wds[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &dbi[0]},
                                   {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &dbi[1]}};
    vkUpdateDescriptorSets(dev, 2, wds, 0, 0);

    /* Record */
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, 0};
    VkCommandPool cmdPool; CK(vkCreateCommandPool(dev, &cpci, 0, &cmdPool));
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cmdPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd; CK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CK(vkBeginCommandBuffer(cmd, &cbbi));

    /* One transition into GENERAL; everything after stays there (unified layouts). */
    generalBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue clear = {0.25f, 0x55};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
    generalBarrier(cmd, image, VK_IMAGE_LAYOUT_GENERAL);

    VkRenderingAttachmentInfo depthAtt = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, view, VK_IMAGE_LAYOUT_GENERAL};
    VkRenderingAttachmentInfo stencilAtt = depthAtt;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, {{0, 0}, {W, H}}, 1, 0, 0, 0, &depthAtt, &stencilAtt};
    float depthValue = 0.75f;

    /* Pass 1: write depth; stencil untouched (LOAD_OP_NONE / STORE_OP_NONE, separate access) */
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    stencilAtt.loadOp = VK_ATTACHMENT_LOAD_OP_NONE_KHR; stencilAtt.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    beginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depthPipe);
    vkCmdSetScissor(cmd, 0, 1, &scFull);
    vkCmdPushConstants(cmd, gfxLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &depthValue);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    endRendering(cmd);
    generalBarrier(cmd, image, VK_IMAGE_LAYOUT_GENERAL);

    /* Pass 2: write stencil on the left half; depth untouched */
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_NONE_KHR; depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    stencilAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; stencilAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    beginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, stencilPipe);
    VkRect2D scLeft = {{0, 0}, {W / 2, H}};
    vkCmdSetScissor(cmd, 0, 1, &scLeft);
    float otherDepth = 0.5f;
    vkCmdPushConstants(cmd, gfxLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &otherDepth);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    endRendering(cmd);
    generalBarrier(cmd, image, VK_IMAGE_LAYOUT_GENERAL);

    VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, depthBuf, 1, &copy);
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, stencilBuf, 1, &copy);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compLayout, 0, 1, &set, 0, 0);
    vkCmdDispatch(cmd, 1, 1, 1);

    VkMemoryBarrier hostBarrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostBarrier, 0, 0, 0, 0);
    CK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cmd};
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; CK(vkCreateFence(dev, &fci, 0, &fence));
    CK(vkQueueSubmit(queue, 1, &si, fence));
    CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));

    /* Check depth/stencil */
    int badDepth = 0, badStencil = 0;
    float firstBadDepth = 0; int firstBadStencil = -1, firstBadX = -1, firstBadY = -1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint32_t raw = ((uint32_t*)depthMap)[y * W + x];
            float d;
            if (fmt == VK_FORMAT_D32_SFLOAT_S8_UINT) memcpy(&d, &raw, 4);
            else d = (raw & 0xffffff) / 16777215.0f;
            if (fabsf(d - 0.75f) > 1e-3f) { if (!badDepth++) firstBadDepth = d; }
            uint8_t s = ((uint8_t*)stencilMap)[y * W + x];
            uint8_t want = x < W / 2 ? 0xA3 : 0x55;
            if (s != want) { if (!badStencil++) { firstBadStencil = s; firstBadX = x; firstBadY = y; } }
        }
    }
    printf("depth/stencil: depth(0,0) %.4f depth(63,63) %.4f stencil(0,0) 0x%02x stencil(63,0) 0x%02x; bad depth %d, bad stencil %d\n",
           fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? ((float*)depthMap)[0] : (((uint32_t*)depthMap)[0] & 0xffffff) / 16777215.0f,
           fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? ((float*)depthMap)[W * H - 1] : (((uint32_t*)depthMap)[W * H - 1] & 0xffffff) / 16777215.0f,
           ((uint8_t*)stencilMap)[0], ((uint8_t*)stencilMap)[W - 1], badDepth, badStencil);
    if (badDepth) FAIL("depth: %d texels wrong, first %.4f (want 0.75)", badDepth, firstBadDepth);
    if (badStencil) FAIL("stencil: %d texels wrong, first 0x%02x at %d,%d", badStencil, firstBadStencil, firstBadX, firstBadY);

    /* Check float_controls2 */
    float* out = outMap;
    printf("float_controls2: x-x = %g %g %g %g, x*0 = %g %g %g %g\n", out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
    for (int i = 0; i < 3; i++) if (!isnan(out[i]) || !isnan(out[4 + i])) FAIL("float_controls2: lane %d not NaN", i);
    if (out[3] != 0.0f || out[7] != 0.0f) FAIL("float_controls2: lane 3 not 0");

    vkDestroyDevice(dev, 0);
    vkDestroyInstance(inst, 0);
    printf("M7UL-PASS\n");
    return 0;
}
