/*
 * [mali-g2] VK_KHR_maintenance5 / VK_KHR_maintenance6 for the Vortek client.
 *
 * DXVK 3.x (shipped with Proton Experimental ARM64) refuses devices without the
 * maintenance5 and maintenance6 features. The Mali driver has both, but Vortek's
 * wire protocol predates them. Instead of extending the generated serializer,
 * this file emulates the new entry points on top of commands Vortek already
 * forwards, and folds the new 64-bit flag structs into the legacy flag fields
 * (the low 32 bits are defined to be identical). The server enables the two
 * features on the real driver when the extensions are requested
 * (patches/winlator-0003-enable-maintenance5-6.patch).
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <vulkan/vulkan.h>

VkResult vt_call_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
void vt_call_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
void vt_call_vkGetPhysicalDeviceProperties2(VkPhysicalDevice, VkPhysicalDeviceProperties2*);
VkResult vt_call_vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
VkResult vt_call_vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
VkResult vt_call_vkCreateBuffer(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer*);
VkResult vt_call_vkCreateImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*);
void vt_call_vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks*);
void vt_call_vkGetImageSubresourceLayout(VkDevice, VkImage, const VkImageSubresource*, VkSubresourceLayout*);
void vt_call_vkCmdBindIndexBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType);
void vt_call_vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*);
void vt_call_vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*);
void vt_call_vkCmdPushDescriptorSetKHR(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkWriteDescriptorSet*);
void vt_call_vkCmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer, VkDescriptorUpdateTemplate, VkPipelineLayout, uint32_t, const void*);

static void* findInChain(const void* chain, VkStructureType type) {
    for (const VkBaseInStructure* s = chain; s; s = s->pNext) {
        if (s->sType == type) return (void*)s;
    }
    return NULL;
}

static bool deviceHasExtension(VkPhysicalDevice physicalDevice, const char* name) {
    uint32_t count = 0;
    if (vt_call_vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &count, NULL) != VK_SUCCESS || !count) return false;
    VkExtensionProperties* props = calloc(count, sizeof(*props));
    if (!props) return false;
    bool found = false;
    if (vt_call_vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &count, props) >= 0) {
        for (uint32_t i = 0; i < count && !found; i++) found = strcmp(props[i].extensionName, name) == 0;
    }
    free(props);
    return found;
}

/* ---- feature / property queries ---- */

static void dd_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures) {
    vt_call_vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
    VkPhysicalDeviceMaintenance5FeaturesKHR* m5 = findInChain(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR);
    VkPhysicalDeviceMaintenance6FeaturesKHR* m6 = findInChain(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR);
    if (m5) m5->maintenance5 = deviceHasExtension(physicalDevice, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    if (m6) m6->maintenance6 = deviceHasExtension(physicalDevice, VK_KHR_MAINTENANCE_6_EXTENSION_NAME);
}

static void dd_vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties) {
    vt_call_vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);
    /* Conservative values; on Mali-G2 the driver reports 1,1,1,0,0,0 and 1,1,1. */
    VkPhysicalDeviceMaintenance5PropertiesKHR* p5 = findInChain(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_PROPERTIES_KHR);
    if (p5) {
        p5->earlyFragmentMultisampleCoverageAfterSampleCounting = VK_TRUE;
        p5->earlyFragmentSampleMaskTestBeforeSampleCounting = VK_TRUE;
        p5->depthStencilSwizzleOneSupport = VK_FALSE;
        p5->polygonModePointSize = VK_FALSE;
        p5->nonStrictSinglePixelWideLinesUseParallelogram = VK_FALSE;
        p5->nonStrictWideLinesUseParallelogram = VK_FALSE;
    }
    VkPhysicalDeviceMaintenance6PropertiesKHR* p6 = findInChain(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_PROPERTIES_KHR);
    if (p6) {
        p6->blockTexelViewCompatibleMultipleLayers = VK_FALSE;
        p6->maxCombinedImageSamplerDescriptorCount = 1;
        p6->fragmentShadingRateClampCombinerInputs = VK_FALSE;
    }
}

/* ---- 64-bit flag structs folded into the legacy fields ---- */

static VkResult dd_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* alloc, VkPipeline* pipelines) {
    VkGraphicsPipelineCreateInfo* copy = malloc(count * sizeof(*copy));
    if (!copy) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(copy, infos, count * sizeof(*copy));
    for (uint32_t i = 0; i < count; i++) {
        const VkPipelineCreateFlags2CreateInfoKHR* f2 = findInChain(infos[i].pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR);
        if (f2) copy[i].flags = (VkPipelineCreateFlags)(f2->flags & 0xffffffffull);
    }
    VkResult result = vt_call_vkCreateGraphicsPipelines(device, cache, count, copy, alloc, pipelines);
    free(copy);
    return result;
}

static VkResult dd_vkCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* alloc, VkPipeline* pipelines) {
    VkComputePipelineCreateInfo* copy = malloc(count * sizeof(*copy));
    if (!copy) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(copy, infos, count * sizeof(*copy));
    for (uint32_t i = 0; i < count; i++) {
        const VkPipelineCreateFlags2CreateInfoKHR* f2 = findInChain(infos[i].pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR);
        if (f2) copy[i].flags = (VkPipelineCreateFlags)(f2->flags & 0xffffffffull);
    }
    VkResult result = vt_call_vkCreateComputePipelines(device, cache, count, copy, alloc, pipelines);
    free(copy);
    return result;
}

static VkResult dd_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* info, const VkAllocationCallbacks* alloc, VkBuffer* buffer) {
    const VkBufferUsageFlags2CreateInfoKHR* u2 = findInChain(info->pNext, VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR);
    if (!u2) return vt_call_vkCreateBuffer(device, info, alloc, buffer);
    VkBufferCreateInfo copy = *info;
    copy.usage = (VkBufferUsageFlags)(u2->usage & 0xffffffffull);
    return vt_call_vkCreateBuffer(device, &copy, alloc, buffer);
}

/* ---- maintenance5 commands ---- */

static void dd_vkCmdBindIndexBuffer2KHR(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, VkIndexType indexType) {
    (void)size; /* a range limit only; the legacy command binds to the end of the buffer */
    vt_call_vkCmdBindIndexBuffer(cmd, buffer, offset, indexType);
}

static void dd_vkGetRenderingAreaGranularityKHR(VkDevice device, const VkRenderingAreaInfoKHR* info, VkExtent2D* granularity) {
    (void)device; (void)info;
    granularity->width = 1;
    granularity->height = 1;
}

static void dd_vkGetImageSubresourceLayout2KHR(VkDevice device, VkImage image, const VkImageSubresource2KHR* sub, VkSubresourceLayout2KHR* layout) {
    vt_call_vkGetImageSubresourceLayout(device, image, &sub->imageSubresource, &layout->subresourceLayout);
}

static void dd_vkGetDeviceImageSubresourceLayoutKHR(VkDevice device, const VkDeviceImageSubresourceInfoKHR* info, VkSubresourceLayout2KHR* layout) {
    memset(&layout->subresourceLayout, 0, sizeof(layout->subresourceLayout));
    VkImage image = VK_NULL_HANDLE;
    if (vt_call_vkCreateImage(device, info->pCreateInfo, NULL, &image) != VK_SUCCESS) return;
    vt_call_vkGetImageSubresourceLayout(device, image, &info->pSubresource->imageSubresource, &layout->subresourceLayout);
    vt_call_vkDestroyImage(device, image, NULL);
}

/* ---- maintenance6 commands ---- */

#define DD_GRAPHICS_STAGES (VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT)

static void dd_vkCmdBindDescriptorSets2KHR(VkCommandBuffer cmd, const VkBindDescriptorSetsInfoKHR* info) {
    if (info->stageFlags & DD_GRAPHICS_STAGES)
        vt_call_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, info->firstSet, info->descriptorSetCount,
                                        info->pDescriptorSets, info->dynamicOffsetCount, info->pDynamicOffsets);
    if (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
        vt_call_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, info->layout, info->firstSet, info->descriptorSetCount,
                                        info->pDescriptorSets, info->dynamicOffsetCount, info->pDynamicOffsets);
}

static void dd_vkCmdPushConstants2KHR(VkCommandBuffer cmd, const VkPushConstantsInfoKHR* info) {
    vt_call_vkCmdPushConstants(cmd, info->layout, info->stageFlags, info->offset, info->size, info->pValues);
}

static void dd_vkCmdPushDescriptorSet2KHR(VkCommandBuffer cmd, const VkPushDescriptorSetInfoKHR* info) {
    if (info->stageFlags & DD_GRAPHICS_STAGES)
        vt_call_vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, info->set, info->descriptorWriteCount, info->pDescriptorWrites);
    if (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
        vt_call_vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, info->layout, info->set, info->descriptorWriteCount, info->pDescriptorWrites);
}

static void dd_vkCmdPushDescriptorSetWithTemplate2KHR(VkCommandBuffer cmd, const VkPushDescriptorSetWithTemplateInfoKHR* info) {
    vt_call_vkCmdPushDescriptorSetWithTemplateKHR(cmd, info->descriptorUpdateTemplate, info->layout, info->set, info->pData);
}

/* ---- lookup, consulted before Vortek's own dispatch table ---- */

static const struct { const char* name; void* func; } ddOverrides[] = {
    {"vkGetPhysicalDeviceFeatures2", dd_vkGetPhysicalDeviceFeatures2},
    {"vkGetPhysicalDeviceFeatures2KHR", dd_vkGetPhysicalDeviceFeatures2},
    {"vkGetPhysicalDeviceProperties2", dd_vkGetPhysicalDeviceProperties2},
    {"vkGetPhysicalDeviceProperties2KHR", dd_vkGetPhysicalDeviceProperties2},
    {"vkCreateGraphicsPipelines", dd_vkCreateGraphicsPipelines},
    {"vkCreateComputePipelines", dd_vkCreateComputePipelines},
    {"vkCreateBuffer", dd_vkCreateBuffer},
    {"vkCmdBindIndexBuffer2KHR", dd_vkCmdBindIndexBuffer2KHR},
    {"vkCmdBindIndexBuffer2", dd_vkCmdBindIndexBuffer2KHR},
    {"vkGetRenderingAreaGranularityKHR", dd_vkGetRenderingAreaGranularityKHR},
    {"vkGetRenderingAreaGranularity", dd_vkGetRenderingAreaGranularityKHR},
    {"vkGetImageSubresourceLayout2KHR", dd_vkGetImageSubresourceLayout2KHR},
    {"vkGetImageSubresourceLayout2EXT", dd_vkGetImageSubresourceLayout2KHR},
    {"vkGetImageSubresourceLayout2", dd_vkGetImageSubresourceLayout2KHR},
    {"vkGetDeviceImageSubresourceLayoutKHR", dd_vkGetDeviceImageSubresourceLayoutKHR},
    {"vkGetDeviceImageSubresourceLayout", dd_vkGetDeviceImageSubresourceLayoutKHR},
    {"vkCmdBindDescriptorSets2KHR", dd_vkCmdBindDescriptorSets2KHR},
    {"vkCmdBindDescriptorSets2", dd_vkCmdBindDescriptorSets2KHR},
    {"vkCmdPushConstants2KHR", dd_vkCmdPushConstants2KHR},
    {"vkCmdPushConstants2", dd_vkCmdPushConstants2KHR},
    {"vkCmdPushDescriptorSet2KHR", dd_vkCmdPushDescriptorSet2KHR},
    {"vkCmdPushDescriptorSet2", dd_vkCmdPushDescriptorSet2KHR},
    {"vkCmdPushDescriptorSetWithTemplate2KHR", dd_vkCmdPushDescriptorSetWithTemplate2KHR},
    {"vkCmdPushDescriptorSetWithTemplate2", dd_vkCmdPushDescriptorSetWithTemplate2KHR},
};

void* droiddeck_find_override(const char* name) {
    for (size_t i = 0; i < sizeof(ddOverrides) / sizeof(ddOverrides[0]); i++) {
        if (strcmp(name, ddOverrides[i].name) == 0) return ddOverrides[i].func;
    }
    return NULL;
}
