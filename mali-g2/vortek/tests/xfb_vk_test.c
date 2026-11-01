/* [mali-g2] Stream output through a real driver, for the counts of src/xfb_stats.h: with the in-process
 * ICD (../../hybris/tests/run.sh ./xfb_vk_test) or the Vortek client against a server. Builds
 *   - a transform feedback pipeline from a VkShaderModule (xfb.vert),
 *   - the same from inline code (VkShaderModuleCreateInfo in the stage's pNext, VK_KHR_maintenance5),
 *   - a pipeline with a geometry shader (xfb_plain.vert + xfb.geom) when the device has them,
 * all with rasterizer discard in a render pass instance without attachments (dynamic rendering).
 * Captures a triangle with each transform feedback pipeline (vkCmdBeginTransformFeedbackEXT x2) and
 * checks the buffer holds 2 x each vertex; then, when the device has transformFeedbackDraw, draws the
 * first capture again with vkCmdDrawIndirectByteCountEXT into a third buffer and checks that too.
 * Destroys the device (the ICD's counts are logged then): the caller checks the log line
 *   "<tag>: xfb stats: pid P, pipelines 2, gs G, begins 3, drawauto D, ..." (G 1 with geometry
 *   shaders, else 0; with transformFeedbackDraw begins 3 and D 1, else begins 2 and D 0)
 * against the "XFB-VK expect" line this prints. Prints XFB-VK-PASS.
 *
 * Build: cc -O2 -o xfb_vk_test xfb_vk_test.c -lvulkan   (shaders: xfb_stats_gen.sh -> xfb_stats_spv.h) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "xfb_stats_spv.h"

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); return 1; } } while (0)
#define NEED(c) do { if (!(c)) { printf("FAIL %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static VkDevice dev;
static VkPhysicalDevice phys;

static int32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return (int32_t)i;
    return -1;
}

typedef struct { VkBuffer buffer; VkDeviceMemory memory; void* map; } Buf;

static int makeBuffer(Buf* b, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    CK(vkCreateBuffer(dev, &bi, NULL, &b->buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b->buffer, &req);
    int32_t type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    NEED(type >= 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type};
    CK(vkAllocateMemory(dev, &ai, NULL, &b->memory));
    CK(vkBindBufferMemory(dev, b->buffer, b->memory, 0));
    CK(vkMapMemory(dev, b->memory, 0, VK_WHOLE_SIZE, 0, &b->map));
    memset(b->map, 0, size);
    return 0;
}

static void freeBuffer(Buf* b) {
    vkDestroyBuffer(dev, b->buffer, NULL);
    vkFreeMemory(dev, b->memory, NULL);
}

static VkPipelineLayout layout;

/* A pipeline with rasterizer discard for a render pass without attachments. */
static int makePipeline(const VkPipelineShaderStageCreateInfo* stages, uint32_t stageCount, VkPipeline* out) {
    VkVertexInputBindingDescription binding = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
                                               .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attr};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .rasterizerDiscardEnable = VK_TRUE, .polygonMode = VK_POLYGON_MODE_FILL,
                                                 .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
                                       .stageCount = stageCount, .pStages = stages, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pDynamicState = &ds, .layout = layout};
    CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, NULL, out));
    return 0;
}

static int hasExtension(const VkExtensionProperties* list, uint32_t n, const char* name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(list[i].extensionName, name)) return 1;
    return 0;
}

static int check(const Buf* b, const float* in, const char* what) {
    const float* got = b->map;
    for (int i = 0; i < 12; i++) {
        if (got[i] != in[i] * 2.0f) {
            printf("FAIL %s: float %d is %g, not %g\n", what, i, got[i], in[i] * 2.0f);
            return 1;
        }
    }
    return 0;
}

int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "xfb_vk_test", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    CK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1;
    VkResult er = vkEnumeratePhysicalDevices(inst, &n, &phys);
    NEED((er == VK_SUCCESS || er == VK_INCOMPLETE) && n == 1);

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &extCount, NULL);
    VkExtensionProperties* exts = calloc(extCount, sizeof(*exts));
    vkEnumerateDeviceExtensionProperties(phys, NULL, &extCount, exts);
    NEED(hasExtension(exts, extCount, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME));
    int m5 = hasExtension(exts, extCount, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    free(exts);

    VkPhysicalDeviceTransformFeedbackPropertiesEXT xfbProps = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &xfbProps};
    vkGetPhysicalDeviceProperties2(phys, &props);
    VkPhysicalDeviceMaintenance5FeaturesKHR m5f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfbF = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, m5 ? &m5f : NULL};
    VkPhysicalDeviceVulkan13Features v13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &xfbF};
    VkPhysicalDeviceFeatures2 feats = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v13};
    vkGetPhysicalDeviceFeatures2(phys, &feats);
    NEED(xfbF.transformFeedback && v13.dynamicRendering);
    int gs = feats.features.geometryShader, drawAuto = xfbProps.transformFeedbackDraw, inl = m5 && m5f.maintenance5;
    printf("device %s: geometry shaders %d, transformFeedbackDraw %d, maintenance5 %d\n", props.properties.deviceName, gs, drawAuto, inl);

    /* only what the test uses */
    VkPhysicalDeviceMaintenance5FeaturesKHR m5on = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, .maintenance5 = VK_TRUE};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfbOn = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, inl ? &m5on : NULL,
                                                          .transformFeedback = VK_TRUE};
    VkPhysicalDeviceVulkan13Features v13on = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &xfbOn, .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceFeatures2 on = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v13on};
    on.features.geometryShader = gs ? VK_TRUE : VK_FALSE;
    const char* devExts[] = {VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, VK_KHR_MAINTENANCE_5_EXTENSION_NAME};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &on, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = inl ? 2 : 1, .ppEnabledExtensionNames = devExts};
    CK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);
    PFN_vkCmdBindTransformFeedbackBuffersEXT bindXfb = (PFN_vkCmdBindTransformFeedbackBuffersEXT)vkGetDeviceProcAddr(dev, "vkCmdBindTransformFeedbackBuffersEXT");
    PFN_vkCmdBeginTransformFeedbackEXT beginXfb = (PFN_vkCmdBeginTransformFeedbackEXT)vkGetDeviceProcAddr(dev, "vkCmdBeginTransformFeedbackEXT");
    PFN_vkCmdEndTransformFeedbackEXT endXfb = (PFN_vkCmdEndTransformFeedbackEXT)vkGetDeviceProcAddr(dev, "vkCmdEndTransformFeedbackEXT");
    PFN_vkCmdDrawIndirectByteCountEXT drawByteCount = (PFN_vkCmdDrawIndirectByteCountEXT)vkGetDeviceProcAddr(dev, "vkCmdDrawIndirectByteCountEXT");
    NEED(bindXfb && beginXfb && endXfb && (!drawAuto || drawByteCount));

    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    CK(vkCreatePipelineLayout(dev, &pli, NULL, &layout));
    VkShaderModuleCreateInfo xfbCode = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(xfb_vert_spv), .pCode = xfb_vert_spv};
    VkShaderModuleCreateInfo plainCode = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(xfb_plain_vert_spv), .pCode = xfb_plain_vert_spv};
    VkShaderModuleCreateInfo geomCode = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(xfb_geom_spv), .pCode = xfb_geom_spv};
    VkShaderModule xfbModule, plainModule, geomModule;
    CK(vkCreateShaderModule(dev, &xfbCode, NULL, &xfbModule));
    CK(vkCreateShaderModule(dev, &plainCode, NULL, &plainModule));
    CK(vkCreateShaderModule(dev, &geomCode, NULL, &geomModule));

    VkPipeline byModule, byInline = VK_NULL_HANDLE, withGs = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                             .module = xfbModule, .pName = "main"};
    if (makePipeline(&stage, 1, &byModule)) return 1;
    if (inl) {
        VkPipelineShaderStageCreateInfo inlineStage = stage;
        inlineStage.pNext = &xfbCode;
        inlineStage.module = VK_NULL_HANDLE;
        if (makePipeline(&inlineStage, 1, &byInline)) return 1;
    } else {
        /* no inline code: a second module pipeline keeps the expected count */
        if (makePipeline(&stage, 1, &byInline)) return 1;
    }
    if (gs) {
        VkPipelineShaderStageCreateInfo two[2] = {stage, stage};
        two[0].module = plainModule;
        two[1].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
        two[1].module = geomModule;
        if (makePipeline(two, 2, &withGs)) return 1;
    }
    vkDestroyShaderModule(dev, xfbModule, NULL);
    vkDestroyShaderModule(dev, plainModule, NULL);
    vkDestroyShaderModule(dev, geomModule, NULL);

    static const float verts[12] = {-0.5f, -0.5f, 0.0f, 1.0f, 0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.5f, 0.25f, 1.0f};
    Buf vb, out[3], counter;
    if (makeBuffer(&vb, sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 1;
    memcpy(vb.map, verts, sizeof(verts));
    for (int i = 0; i < 3; i++)
        if (makeBuffer(&out[i], 256, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT)) return 1;
    if (makeBuffer(&counter, 64, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)) return 1;

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0};
    VkCommandPool pool;
    CK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CK(vkBeginCommandBuffer(cb, &bi));
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {64, 64}}, .layerCount = 1};
    VkViewport viewport = {0, 0, 64, 64, 0, 1};
    VkRect2D scissor = {{0, 0}, {64, 64}};
    VkDeviceSize zero = 0, whole = 256, counterOffset = 0;
    VkPipeline captures[2] = {byModule, byInline};
    for (int i = 0; i < 2; i++) {
        vkCmdBeginRendering(cb, &ri);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, captures[i]);
        vkCmdSetViewport(cb, 0, 1, &viewport);
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdBindVertexBuffers(cb, 0, 1, &vb.buffer, &zero);
        bindXfb(cb, 0, 1, &out[i].buffer, &zero, &whole);
        beginXfb(cb, 0, 0, NULL, NULL);
        vkCmdDraw(cb, 3, 1, 0, 0);
        /* the first capture's byte count, for the DrawAuto below */
        endXfb(cb, 0, i == 0 ? 1 : 0, i == 0 ? &counter.buffer : NULL, i == 0 ? &counterOffset : NULL);
        vkCmdEndRendering(cb);
    }
    if (drawAuto) {
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT |
                                                                                 VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
                              .dstAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        /* draw the vertex buffer again, as many vertices as the first capture wrote (3 x 16 bytes) */
        vkCmdBeginRendering(cb, &ri);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, byModule);
        vkCmdSetViewport(cb, 0, 1, &viewport);
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdBindVertexBuffers(cb, 0, 1, &vb.buffer, &zero);
        bindXfb(cb, 0, 1, &out[2].buffer, &zero, &whole);
        beginXfb(cb, 0, 0, NULL, NULL);
        drawByteCount(cb, 1, 0, counter.buffer, 0, 0, 16);
        endXfb(cb, 0, 0, NULL, NULL);
        vkCmdEndRendering(cb);
    }
    VkMemoryBarrier toHost = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
                              .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, NULL, 0, NULL);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CK(vkQueueWaitIdle(queue));

    int bad = check(&out[0], verts, "module pipeline capture") + check(&out[1], verts, "inline pipeline capture");
    if (drawAuto) bad += check(&out[2], verts, "DrawAuto capture");
    printf("XFB-VK expect pipelines 2, gs %d, begins %d, drawauto %d\n", gs ? 1 : 0, drawAuto ? 3 : 2, drawAuto ? 1 : 0);
    fflush(stdout);

    vkDestroyCommandPool(dev, pool, NULL);
    freeBuffer(&vb);
    for (int i = 0; i < 3; i++) freeBuffer(&out[i]);
    freeBuffer(&counter);
    vkDestroyPipeline(dev, byModule, NULL);
    vkDestroyPipeline(dev, byInline, NULL);
    if (withGs) vkDestroyPipeline(dev, withGs, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (bad) return 1;
    printf("XFB-VK-PASS\n");
    return 0;
}
