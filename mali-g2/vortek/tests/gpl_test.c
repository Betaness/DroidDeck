/* [mali-g2] VK_EXT_graphics_pipeline_library through Vortek, the way DXVK 3.x uses it.
 *
 * Checks that the GPL features/properties come through, then builds the four library parts
 * (vertex input, pre-rasterization with a vertex shader, fragment shader, fragment output)
 * twice and links them into two pipelines:
 *   A: DXVK style - VkPipelineCreateFlags2CreateInfoKHR, SPIR-V inline in the stage pNext,
 *      fast link (no LINK_TIME_OPTIMIZATION). The create infos also carry dangling pointers
 *      and handles in the state the spec ignores for the subsets they do not provide.
 *   B: legacy flags, VkShaderModule objects, libraries with RETAIN_LINK_TIME_OPTIMIZATION_INFO,
 *      linked with LINK_TIME_OPTIMIZATION.
 * The layout uses INDEPENDENT_SETS with a VK_NULL_HANDLE set layout, like DXVK's. Each pipeline
 * draws a triangle into one half of a 128x64 image (dynamic rendering), which is read back and
 * checked. B's libraries stay alive until the end, as in DXVK. A's libraries are destroyed right
 * after linking (A is linked twice and the second copy is destroyed before the draw): the spec
 * allows this, but the Mali driver (also natively, without Vortek) crashes or draws nothing then,
 * so the Vortek server keeps libraries alive while fast-linked pipelines use them
 * (winlator-0010). GPL_KEEP_LIBS=1 keeps A's libraries too. Also creates a fragment library from
 * a shader module identifier with FAIL_ON_PIPELINE_COMPILE_REQUIRED (DXVK's second-compile
 * path). Prints GPL-PASS.
 *
 * Build: cc -O2 -o gpl_test gpl_test.c -lvulkan   (shaders: gpl.vert/gpl.frag -> gpl_spv.h) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "gpl_spv.h"
#define W 128
#define H 64
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); return 1; } } while (0)

typedef struct { float x, y; uint8_t c[4]; } Vertex;

static VkDevice dev;
static VkPipelineLayout layout;
static VkPipelineRenderingCreateInfo rtLib = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
static VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
#define BOGUS ((void*)(uintptr_t)0x10) /* a dangling pointer/handle in state the spec ignores */

/* style 0: flags2 + inline SPIR-V (DXVK); style 1: legacy flags + modules + RETAIN */
static VkResult createLibrary(int style, VkGraphicsPipelineLibraryFlagsEXT part, VkShaderModule vs, VkShaderModule fs, VkPipeline* out) {
    VkPipelineCreateFlags2CreateInfoKHR f2 = {VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, &rtLib, VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR};
    VkGraphicsPipelineLibraryCreateInfoEXT lib = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, style == 0 ? (void*)&f2 : (void*)&rtLib, part};
    VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &lib};
    info.flags = style == 0 ? 0 : VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
    info.basePipelineIndex = -1;

    VkVertexInputBindingDescription vb = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 8}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, 0, 0, 1, &vb, 2, va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 2, dyn};

    VkShaderModuleCreateInfo code = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.pName = "main";

    if (part == VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT) {
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        if (style == 0) { info.pRasterizationState = BOGUS; info.pColorBlendState = BOGUS; info.layout = (VkPipelineLayout)BOGUS; info.renderPass = (VkRenderPass)BOGUS; info.subpass = 3; }
    }
    else if (part == VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT) {
        stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        if (style == 0) { code.codeSize = sizeof(gpl_vert_spv); code.pCode = gpl_vert_spv; stage.pNext = &code; }
        else stage.module = vs;
        info.stageCount = 1; info.pStages = &stage;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pDynamicState = &dy;
        info.layout = layout;
    }
    else if (part == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT) {
        stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        if (style == 0) { code.codeSize = sizeof(gpl_frag_spv); code.pCode = gpl_frag_spv; stage.pNext = &code; }
        else stage.module = fs;
        info.stageCount = 1; info.pStages = &stage;
        info.pDepthStencilState = &ds;
        info.layout = layout;
    }
    else {
        info.pMultisampleState = &ms;
        info.pColorBlendState = &cb;
        if (style == 0) { info.pVertexInputState = BOGUS; info.pDepthStencilState = BOGUS; info.stageCount = 1; info.pStages = BOGUS; }
    }
    *out = VK_NULL_HANDLE;
    return vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, 0, out);
}

static VkResult linkLibraries(const VkPipeline* libs, VkPipelineCreateFlags flags, VkPipeline* out) {
    VkPipelineLibraryCreateInfoKHR li = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, 0, 4, libs};
    VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &li};
    info.flags = flags;
    info.layout = layout;
    info.basePipelineIndex = -1;
    if (!flags) { info.pVertexInputState = BOGUS; info.pViewportState = BOGUS; info.pMultisampleState = BOGUS; info.stageCount = 2; info.pStages = BOGUS; info.renderPass = (VkRenderPass)BOGUS; }
    *out = VK_NULL_HANDLE;
    return vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, 0, out);
}

static int hasExtension(VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0; vkEnumerateDeviceExtensionProperties(pd, 0, &n, 0);
    VkExtensionProperties props[n ? n : 1]; vkEnumerateDeviceExtensionProperties(pd, 0, &n, props);
    for (uint32_t i = 0; i < n; i++) if (strcmp(props[i].extensionName, name) == 0) return 1;
    return 0;
}

static const VkGraphicsPipelineLibraryFlagsEXT parts[4] = {
    VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
    VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};

int main(void) {
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "gpl", 1, "gpl", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai};
    VkInstance inst; CK(vkCreateInstance(&ii, 0, &inst));
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd);

    int hasGpl = hasExtension(pd, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
    int hasLib = hasExtension(pd, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    int hasSmi = hasExtension(pd, VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME);
    printf("extensions: %s=%d %s=%d %s=%d\n", VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, hasGpl,
           VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, hasLib, VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME, hasSmi);

    VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT fsmi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT};
    VkPhysicalDeviceMaintenance5FeaturesKHR f5 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, &fsmi};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f5};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fgpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, &f13};
    VkPhysicalDeviceFeatures2 ff = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &fgpl};
    vkGetPhysicalDeviceFeatures2(pd, &ff);
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT pgpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 pp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pgpl};
    vkGetPhysicalDeviceProperties2(pd, &pp);
    printf("features: graphicsPipelineLibrary=%u dynamicRendering=%u maintenance5=%u shaderModuleIdentifier=%u pipelineCreationCacheControl=%u\n",
           fgpl.graphicsPipelineLibrary, f13.dynamicRendering, f5.maintenance5, fsmi.shaderModuleIdentifier, f13.pipelineCreationCacheControl);
    printf("properties: graphicsPipelineLibraryFastLinking=%u graphicsPipelineLibraryIndependentInterpolationDecoration=%u\n",
           pgpl.graphicsPipelineLibraryFastLinking, pgpl.graphicsPipelineLibraryIndependentInterpolationDecoration);
    if (!hasGpl || !hasLib || !fgpl.graphicsPipelineLibrary || !f13.dynamicRendering || !f5.maintenance5) { printf("GPL-FAIL features\n"); return 1; }
    int testIdentifier = hasSmi && fsmi.shaderModuleIdentifier && f13.pipelineCreationCacheControl;

    /* enable only what the test uses, like DXVK does */
    VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT esmi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT, 0, VK_TRUE};
    VkPhysicalDeviceMaintenance5FeaturesKHR e5 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, testIdentifier ? &esmi : 0, VK_TRUE};
    VkPhysicalDeviceVulkan13Features e13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &e5};
    e13.dynamicRendering = VK_TRUE; e13.pipelineCreationCacheControl = f13.pipelineCreationCacheControl;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT egpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, &e13, VK_TRUE};
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio};
    const char* exts[] = {VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
                          VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &egpl, 0, 1, &qi, 0, 0, testIdentifier ? 4 : 3, exts};
    CK(vkCreateDevice(pd, &di, 0, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

    /* INDEPENDENT_SETS layout with a null set layout in it, like DXVK's "independent" layouts */
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &dli, 0, &dsl));
    VkDescriptorSetLayout setLayouts[2] = {dsl, VK_NULL_HANDLE};
    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, 2, setLayouts, 1, &pcr};
    CK(vkCreatePipelineLayout(dev, &pli, 0, &layout));

    VkShaderModuleCreateInfo vsi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(gpl_vert_spv), gpl_vert_spv};
    VkShaderModuleCreateInfo fsi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(gpl_frag_spv), gpl_frag_spv};
    VkShaderModule vs, fs; CK(vkCreateShaderModule(dev, &vsi, 0, &vs)); CK(vkCreateShaderModule(dev, &fsi, 0, &fs));
    rtLib.colorAttachmentCount = 1; rtLib.pColorAttachmentFormats = &colorFormat;

    VkPipeline libsA[4], libsB[4], pipeA, pipeB;
    for (int i = 0; i < 4; i++) {
        CK(createLibrary(0, parts[i], vs, fs, &libsA[i]));
        CK(createLibrary(1, parts[i], vs, fs, &libsB[i]));
        if (!libsA[i] || !libsB[i]) { printf("GPL-FAIL null library handle\n"); return 1; }
    }
    printf("libraries: 2 x 4 parts created\n");
    CK(linkLibraries(libsA, 0, &pipeA));
    CK(linkLibraries(libsB, VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT, &pipeB));
    if (!pipeA || !pipeB) { printf("GPL-FAIL null linked pipeline\n"); return 1; }
    printf("linked: fast-link (A) and LINK_TIME_OPTIMIZATION (B)\n");
    const char* keepEnv = getenv("GPL_KEEP_LIBS");
    int keepLibsA = keepEnv && atoi(keepEnv);
    if (!keepLibsA) {
        VkPipeline pipeA2; CK(linkLibraries(libsA, 0, &pipeA2));
        for (int i = 0; i < 4; i++) vkDestroyPipeline(dev, libsA[i], 0);
        vkDestroyPipeline(dev, pipeA2, 0);
        printf("A's libraries destroyed after linking (second fast-linked copy destroyed too)\n");
    }

    int identifierOk = 1;
    if (testIdentifier) {
        PFN_vkGetShaderModuleCreateInfoIdentifierEXT getId = (void*)vkGetDeviceProcAddr(dev, "vkGetShaderModuleCreateInfoIdentifierEXT");
        VkShaderModuleIdentifierEXT id = {VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT};
        getId(dev, &fsi, &id);
        VkPipelineShaderStageModuleIdentifierCreateInfoEXT idi = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT, 0, id.identifierSize, id.identifier};
        VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &idi, 0, VK_SHADER_STAGE_FRAGMENT_BIT, VK_NULL_HANDLE, "main"};
        VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkGraphicsPipelineLibraryCreateInfoEXT lib = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, &rtLib, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT};
        VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &lib,
            VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT, 1, &stage};
        info.pDepthStencilState = &ds; info.layout = layout; info.basePipelineIndex = -1;
        VkPipeline p = (VkPipeline)(uintptr_t)0x1;
        VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, 0, &p);
        identifierOk = (r == VK_PIPELINE_COMPILE_REQUIRED && p == VK_NULL_HANDLE) || (r == VK_SUCCESS && p != VK_NULL_HANDLE);
        printf("module identifier (%u bytes) + FAIL_ON_PIPELINE_COMPILE_REQUIRED: result %d handle %s -> %s\n", id.identifierSize, r,
               p ? "non-null" : "null", identifierOk ? "ok" : "WRONG");
        if (r == VK_SUCCESS) vkDestroyPipeline(dev, p, 0);
    }
    else printf("module identifier: not supported, skipped\n");

    /* render target, vertex buffer, readback buffer */
    VkImageCreateInfo imi = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, colorFormat, {W, H, 1}, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage img; CK(vkCreateImage(dev, &imi, 0, &img));
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    uint32_t mt = 0; for (; mt < mp.memoryTypeCount; mt++) if ((mr.memoryTypeBits >> mt & 1) && (mp.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) break;
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mt};
    VkDeviceMemory imem; CK(vkAllocateMemory(dev, &ma, 0, &imem)); CK(vkBindImageMemory(dev, img, imem, 0));
    VkImageViewCreateInfo ivi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, img, VK_IMAGE_VIEW_TYPE_2D, colorFormat, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view; CK(vkCreateImageView(dev, &ivi, 0, &view));

    const VkMemoryPropertyFlags hostFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, W * H * 4 + 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    VkBuffer buf; CK(vkCreateBuffer(dev, &bi, 0, &buf));
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    for (mt = 0; mt < mp.memoryTypeCount; mt++) if ((mr.memoryTypeBits >> mt & 1) && (mp.memoryTypes[mt].propertyFlags & hostFlags) == hostFlags) break;
    VkMemoryAllocateInfo mb = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mt};
    VkDeviceMemory bmem; CK(vkAllocateMemory(dev, &mb, 0, &bmem)); CK(vkBindBufferMemory(dev, buf, bmem, 0));
    uint8_t* map; CK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void**)&map));
    /* triangle over the top-left half of the viewport; readback goes after the vertices */
    const VkDeviceSize readOffset = 256;
    Vertex verts[3] = {{-1, -1, {0, 128, 0, 255}}, {1, -1, {0, 128, 0, 255}}, {-1, 1, {0, 128, 0, 255}}};
    memset(map, 0, W * H * 4 + 256); memcpy(map, verts, sizeof(verts));

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, 0};
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpi, 0, &cp));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CK(vkBeginCommandBuffer(cb, &cbi));
    VkImageMemoryBarrier toColor = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, 0, 0, 0, 1, &toColor);
    VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = (VkClearColorValue){{0.25f, 0.25f, 0.25f, 1.0f}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, {{0, 0}, {W, H}}, 1, 0, 1, &att};
    vkCmdBeginRendering(cb, &ri);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &buf, &zero);
    VkPipeline pipes[2] = {pipeA, pipeB};
    float adds[2][4] = {{1, 0, 0, 0}, {0, 0, 1, 0}};
    for (int i = 0; i < 2; i++) {
        VkViewport v = {i * (W / 2), 0, W / 2, H, 0, 1};
        VkRect2D s = {{i * (W / 2), 0}, {W / 2, H}};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[i]);
        vkCmdSetViewportWithCount(cb, 1, &v);
        vkCmdSetScissorWithCount(cb, 1, &s);
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, adds[i]);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    vkCmdEndRendering(cb);
    VkImageMemoryBarrier toSrc = toColor;
    toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &toSrc);
    VkBufferImageCopy copy = {readOffset, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
    VkMemoryBarrier toHost = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, 0, 0, 0);
    CK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; CK(vkCreateFence(dev, &fci, 0, &fence));
    VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb};
    CK(vkQueueSubmit(q, 1, &sub, fence)); CK(vkWaitForFences(dev, 1, &fence, 1, 5000000000ull));

    /* inside: vertex green 128 + push constant (A: red 1.0, B: blue 1.0); outside: clear 0.25 */
    struct { const char* what; int x, y; uint8_t want[4]; } probes[] = {
        {"A inside ", 4, 4, {255, 128, 0, 255}}, {"A inside ", 20, 30, {255, 128, 0, 255}}, {"A outside", 60, 60, {64, 64, 64, 255}},
        {"B inside ", 68, 4, {0, 128, 255, 255}}, {"B inside ", 84, 30, {0, 128, 255, 255}}, {"B outside", 124, 60, {64, 64, 64, 255}},
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        const uint8_t* px = map + readOffset + (probes[i].y * W + probes[i].x) * 4;
        int ok = 1;
        for (int c = 0; c < 4; c++) { int d = (int)px[c] - probes[i].want[c]; if (d < -2 || d > 2) ok = 0; }
        printf("pixel %s (%3d,%2d) = %3u %3u %3u %3u  want %3u %3u %3u %3u  %s\n", probes[i].what, probes[i].x, probes[i].y,
               px[0], px[1], px[2], px[3], probes[i].want[0], probes[i].want[1], probes[i].want[2], probes[i].want[3], ok ? "ok" : "WRONG");
        bad += !ok;
    }
    int covered[2] = {0, 0};
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (map[readOffset + (y * W + x) * 4 + 1] > 100) covered[x >= W / 2]++;
    printf("covered pixels: A %d B %d (each half %d, triangle ~%d)\n", covered[0], covered[1], (W / 2) * H, (W / 2) * H / 2);
    if (covered[0] < 1900 || covered[0] > 2300 || covered[1] < 1900 || covered[1] > 2300) bad++;

    vkDeviceWaitIdle(dev);
    for (int i = 0; i < 4; i++) { if (keepLibsA) vkDestroyPipeline(dev, libsA[i], 0); vkDestroyPipeline(dev, libsB[i], 0); }
    vkDestroyPipeline(dev, pipeA, 0); vkDestroyPipeline(dev, pipeB, 0);
    vkDestroyDevice(dev, 0); vkDestroyInstance(inst, 0);
    if (bad || !identifierOk) { printf("GPL-FAIL\n"); return 1; }
    printf("GPL-PASS\n");
    return 0;
}
