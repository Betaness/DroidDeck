/*
 * The Mali workarounds of the Vortek client (mali-g2/vortek/src/droiddeck_maint.c) that are about the
 * driver rather than Vortek's wire protocol; with the driver in this process, maintenance5/6,
 * graphics pipeline libraries and the extension filter (ext_commands.h) need nothing.
 *   - ClipDistance / CullDistance stores are removed from shaders: the Mali shader compiler crashes
 *     on them (spirv_clip_cull.h). VORTEK_KEEP_CLIP_DISTANCE=1 keeps them.
 *   - vkd3d-proton is told storage texel buffers need single-texel alignment only
 *     (VORTEK_STRICT_TEXEL_ALIGNMENT=1 reports the driver's value).
 *   - VK_EXT_vertex_attribute_divisor stands in for the driver's KHR extension (icd.c); its
 *     properties structure is filled from the KHR one here.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include "icd.h"

#include <stdlib.h>
#include <string.h>

#include "spirv_clip_cull.h"

static void* findInChain(const void* chain, VkStructureType type) {
    for (const VkBaseInStructure* s = chain; s; s = s->pNext)
        if (s->sType == type) return (void*)s;
    return NULL;
}

static void VKAPI_CALL GetPhysicalDeviceProperties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2* properties) {
    Instance* instance = icd_instance_of(physical);
    if (!instance) return;
    /* The driver does not know the EXT divisor structure: it is unlinked for the call. */
    VkBaseOutStructure* prev = (VkBaseOutStructure*)properties;
    VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT* divExt = NULL;
    while (prev->pNext && prev->pNext->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT) prev = prev->pNext;
    if (prev->pNext) {
        divExt = (VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT*)prev->pNext;
        prev->pNext = divExt->pNext;
    }
    instance->GetPhysicalDeviceProperties2(physical, properties);
    if (divExt) {
        prev->pNext = (VkBaseOutStructure*)divExt;
        VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR khr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &khr};
        instance->GetPhysicalDeviceProperties2(physical, &query);
        divExt->maxVertexAttribDivisor = khr.maxVertexAttribDivisor;
    }
    /* vkd3d-proton refuses a device without single-texel alignment for storage texel buffers; the
     * Mali asks for 64-byte offsets. D3D12 games create typed UAV buffer views at element offsets
     * that are aligned in practice, so vkd3d-proton (only) is told the Mali has it. */
    if (instance->vkd3d && !icd_env_int("VORTEK_STRICT_TEXEL_ALIGNMENT", 0)) {
        VkPhysicalDeviceVulkan13Properties* p13 = findInChain(properties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES);
        if (p13) p13->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
        VkPhysicalDeviceTexelBufferAlignmentProperties* tba =
            findInChain(properties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES);
        if (tba) tba->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
    }
}

static bool clipStripOn(void) {
    static int on = -1;
    if (on < 0) on = !icd_env_int("VORTEK_KEEP_CLIP_DISTANCE", 0);
    return on;
}

/* A rewritten copy of `info` in *out (code to free in *freeCode), or false to use `info`. */
static bool stripClipCull(const VkShaderModuleCreateInfo* info, VkShaderModuleCreateInfo* out, uint32_t** freeCode) {
    if (!clipStripOn() || !info->pCode || info->codeSize < 20) return false;
    size_t words = 0;
    uint32_t* code = dd_strip_clip_cull(info->pCode, info->codeSize / 4, &words);
    if (!code) return false;
    static int logged;
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "hybris-icd: clip/cull distance writes removed from a shader (the Mali compiler crashes on them)\n");
    *out = *info;
    out->pCode = code;
    out->codeSize = words * 4;
    *freeCode = code;
    return true;
}

static VkResult VKAPI_CALL CreateShaderModule(VkDevice handle, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* allocator,
                                              VkShaderModule* module) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    VkShaderModuleCreateInfo stripped;
    uint32_t* code = NULL;
    VkResult result = device->CreateShaderModule(handle, stripClipCull(info, &stripped, &code) ? &stripped : info, allocator, module);
    free(code);
    return result;
}

/* Inline stage code (DXVK 3.x, pipeline libraries): a stage whose pNext chain starts with its
 * VkShaderModuleCreateInfo is copied with the rewritten code. */
static VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice handle, VkPipelineCache cache, uint32_t count,
                                                   const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
                                                   VkPipeline* pipelines) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    if (!clipStripOn()) return device->CreateGraphicsPipelines(handle, cache, count, infos, allocator, pipelines);
    uint32_t totalStages = 0;
    bool inline_ = false;
    for (uint32_t i = 0; i < count; i++) {
        totalStages += infos[i].stageCount;
        for (uint32_t s = 0; s < infos[i].stageCount; s++) {
            const VkBaseInStructure* next = infos[i].pStages[s].pNext;
            if (next && next->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) inline_ = true;
        }
    }
    if (!inline_) return device->CreateGraphicsPipelines(handle, cache, count, infos, allocator, pipelines);
    VkGraphicsPipelineCreateInfo* copy = malloc(count * sizeof(*copy));
    VkPipelineShaderStageCreateInfo* stages = calloc(totalStages, sizeof(*stages));
    VkShaderModuleCreateInfo* modules = calloc(totalStages, sizeof(*modules));
    uint32_t** codes = calloc(totalStages, sizeof(*codes));
    uint32_t used = 0;
    bool any = false;
    if (!copy || !stages || !modules || !codes) goto out;
    memcpy(copy, infos, count * sizeof(*copy));
    for (uint32_t i = 0; i < count; i++) {
        bool rewritten = false;
        uint32_t base = used;
        for (uint32_t s = 0; s < infos[i].stageCount; s++, used++) {
            stages[used] = infos[i].pStages[s];
            const VkShaderModuleCreateInfo* inl = infos[i].pStages[s].pNext;
            if (inl && inl->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO && stripClipCull(inl, &modules[used], &codes[used])) {
                stages[used].pNext = &modules[used];
                rewritten = true;
            }
        }
        if (rewritten) {
            copy[i].pStages = &stages[base];
            any = true;
        }
    }
out:;
    VkResult result = device->CreateGraphicsPipelines(handle, cache, count, any ? copy : infos, allocator, pipelines);
    for (uint32_t i = 0; codes && i < used; i++) free(codes[i]);
    free(codes);
    free(modules);
    free(stages);
    free(copy);
    return result;
}

PFN_vkVoidFunction mali_instance_proc(const char* name) {
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2") || !strcmp(name, "vkGetPhysicalDeviceProperties2KHR"))
        return (PFN_vkVoidFunction)GetPhysicalDeviceProperties2;
    return NULL;
}

PFN_vkVoidFunction mali_device_proc(const char* name) {
    if (!strcmp(name, "vkCreateShaderModule")) return (PFN_vkVoidFunction)CreateShaderModule;
    if (!strcmp(name, "vkCreateGraphicsPipelines")) return (PFN_vkVoidFunction)CreateGraphicsPipelines;
    return NULL;
}
