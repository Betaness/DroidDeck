/*
 * [mali-g2] Inline shader code of a pipeline stage (VK_KHR_maintenance5: a VkShaderModuleCreateInfo
 * in the pNext chain of a VkPipelineShaderStageCreateInfo) wherever it is in the chain, and a copy
 * of that chain with the code replaced. Shared by the Vortek client (droiddeck_maint.c) and the
 * in-process libhybris path (mali-g2/hybris/src/mali_fixes.c), which rewrite the code (clip/cull
 * distance stores, spec-constant composites).
 *
 * DXVK puts the module first, but any order is valid: a rewrite that only looked at the first
 * structure handed the Mali compiler the original code when, say, a VkPipelineRobustnessCreateInfo
 * came first (and ClipDistance writes crash it). The structures ahead of the module are copied
 * (they are const), which needs their sizes: the stage structures known here; a chain with another
 * one ahead of the module is left alone (the original code is used, as before).
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef DROIDDECK_STAGE_INLINE_MODULE_H
#define DROIDDECK_STAGE_INLINE_MODULE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define DD_STAGE_CHAIN_MAX 6
#define DD_STAGE_NODE_BYTES 64

/* Room for the copies of the structures ahead of the module in one stage's chain. */
typedef struct {
    uint64_t node[DD_STAGE_CHAIN_MAX][DD_STAGE_NODE_BYTES / 8];
} DdStageChain;

/* The size of a VkPipelineShaderStageCreateInfo extension structure, 0 for one not known here. */
static inline size_t dd_stage_struct_size(VkStructureType type) {
    switch (type) {
    case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO:
        return sizeof(VkPipelineShaderStageRequiredSubgroupSizeCreateInfo);
    case VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT:
        return sizeof(VkPipelineRobustnessCreateInfoEXT);
    case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT:
        return sizeof(VkPipelineShaderStageModuleIdentifierCreateInfoEXT);
    case VK_STRUCTURE_TYPE_SHADER_MODULE_VALIDATION_CACHE_CREATE_INFO_EXT:
        return sizeof(VkShaderModuleValidationCacheCreateInfoEXT);
    case VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT:
        return sizeof(VkDebugUtilsObjectNameInfoEXT);
    default:
        return 0;
    }
}

_Static_assert(sizeof(VkPipelineShaderStageRequiredSubgroupSizeCreateInfo) <= DD_STAGE_NODE_BYTES &&
                   sizeof(VkPipelineRobustnessCreateInfoEXT) <= DD_STAGE_NODE_BYTES &&
                   sizeof(VkPipelineShaderStageModuleIdentifierCreateInfoEXT) <= DD_STAGE_NODE_BYTES &&
                   sizeof(VkShaderModuleValidationCacheCreateInfoEXT) <= DD_STAGE_NODE_BYTES &&
                   sizeof(VkDebugUtilsObjectNameInfoEXT) <= DD_STAGE_NODE_BYTES,
               "stage chain node too small");

/* The VkShaderModuleCreateInfo anywhere in a stage's pNext chain, or NULL. */
static inline const VkShaderModuleCreateInfo* dd_inline_module(const void* chain) {
    for (const VkBaseInStructure* s = (const VkBaseInStructure*)chain; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) return (const VkShaderModuleCreateInfo*)s;
    }
    return NULL;
}

/* The head of a chain like `chain` with `module` (found in it by dd_inline_module) replaced by
 * `replacement` (a copy of it with other code: its pNext stays what followed the module). The
 * structures ahead of the module are copied into *storage. NULL when one of them is unknown or
 * there are too many: the caller keeps the original chain. */
static inline const void* dd_replace_inline_module(const void* chain, const VkShaderModuleCreateInfo* module,
                                                   VkShaderModuleCreateInfo* replacement, DdStageChain* storage) {
    replacement->pNext = module->pNext;
    const void* head = replacement;
    VkBaseOutStructure* last = NULL;
    int used = 0;
    for (const VkBaseInStructure* s = (const VkBaseInStructure*)chain; s != (const VkBaseInStructure*)module; s = s->pNext) {
        size_t size = s ? dd_stage_struct_size(s->sType) : 0;
        if (!size || used == DD_STAGE_CHAIN_MAX) return NULL;
        VkBaseOutStructure* copy = (VkBaseOutStructure*)storage->node[used++];
        memcpy(copy, s, size);
        copy->pNext = (VkBaseOutStructure*)replacement;
        if (last) last->pNext = copy;
        else head = copy;
        last = copy;
    }
    return head;
}

#endif
