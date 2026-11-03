/*
 * [mali-g2] VK_EXT_graphics_pipeline_library: the state of a VkGraphicsPipelineCreateInfo that the
 * spec ignores because the call does not provide the library subset it belongs to. Those
 * pointers and handles may dangle (pStages of a vertex input or fragment output part, the
 * pStages of a fast link, ...), so they are dropped before anything follows them. Shared by the
 * Vortek client (droiddeck_maint.c), whose serializer would follow them, and the in-process
 * libhybris path (mali-g2/hybris/src/mali_fixes.c), whose shader rewrites, implicit pipeline
 * cache and xfb statistics read pStages.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef DROIDDECK_GPL_LIBRARY_STATE_H
#define DROIDDECK_GPL_LIBRARY_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#define DD_GPL_ALL_PARTS                                                                                                  \
    (VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT | \
     VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT)

/* The library subsets `info` provides: all four for a complete pipeline (no
 * VkGraphicsPipelineLibraryCreateInfoEXT, not a library, links none); otherwise the structure's
 * flags, or none when it is missing (VUID: "as if flags is 0"). The LIBRARY bit is taken from
 * VkPipelineCreateFlags2CreateInfoKHR when the chain has one (it replaces flags then). */
static inline VkGraphicsPipelineLibraryFlagsEXT dd_gpl_parts(const VkGraphicsPipelineCreateInfo* info, uint32_t* libraryCount) {
    const VkGraphicsPipelineLibraryCreateInfoEXT* gpl = NULL;
    const VkPipelineLibraryCreateInfoKHR* libs = NULL;
    const VkPipelineCreateFlags2CreateInfoKHR* f2 = NULL;
    for (const VkBaseInStructure* s = (const VkBaseInStructure*)info->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT) {
            if (!gpl) gpl = (const VkGraphicsPipelineLibraryCreateInfoEXT*)s;
        } else if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) {
            if (!libs) libs = (const VkPipelineLibraryCreateInfoKHR*)s;
        } else if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR) {
            if (!f2) f2 = (const VkPipelineCreateFlags2CreateInfoKHR*)s;
        }
    }
    uint64_t flags = f2 ? f2->flags : info->flags;
    *libraryCount = libs ? libs->libraryCount : 0;
    if (gpl) return gpl->flags;
    if ((flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) || *libraryCount) return 0;
    return DD_GPL_ALL_PARTS;
}

/* Clears in `info` (a copy of the application's) the state of the subsets it does not provide. */
static inline void dd_gpl_drop_ignored_state(VkGraphicsPipelineCreateInfo* info) {
    uint32_t libraryCount;
    VkGraphicsPipelineLibraryFlagsEXT parts = dd_gpl_parts(info, &libraryCount);
    bool vertexInput = parts & VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
    bool preRaster = parts & VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
    bool fragmentShader = parts & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
    bool fragmentOutput = parts & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
    if ((parts & DD_GPL_ALL_PARTS) == DD_GPL_ALL_PARTS) return;
    if (!vertexInput) {
        info->pVertexInputState = NULL;
        info->pInputAssemblyState = NULL;
    }
    if (!preRaster) {
        info->pTessellationState = NULL;
        info->pViewportState = NULL;
        info->pRasterizationState = NULL;
    }
    if (!preRaster && !fragmentShader) {
        info->stageCount = 0;
        info->pStages = NULL;
        if (!libraryCount) info->layout = VK_NULL_HANDLE; /* a linked pipeline still uses its layout */
    }
    if (!fragmentShader) info->pDepthStencilState = NULL;
    if (!fragmentShader && !fragmentOutput) info->pMultisampleState = NULL;
    if (!fragmentOutput) info->pColorBlendState = NULL;
    if (!preRaster && !fragmentShader && !fragmentOutput) {
        /* VUID-VkGraphicsPipelineCreateInfo-flags-06643: renderPass only matters for those three */
        info->renderPass = VK_NULL_HANDLE;
        info->subpass = 0;
    }
}

#endif
