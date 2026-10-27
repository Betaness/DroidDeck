/*
 * [mali-g2] Persistent implicit pipeline cache of the Vortek server and the in-process libhybris
 * ICD (pipeline_cache.c).
 *
 * Copyright (C) 2026 mali-v15-bringup contributors.
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef VORTEK_PIPELINE_CACHE_H
#define VORTEK_PIPELINE_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/* After vkCreateDevice: registers the device; its first PipelineCache_get loads the cache file of
 * (application, driver). appName/engineName come from the instance's VkApplicationInfo (either may
 * be NULL). */
void PipelineCache_deviceCreated(VkPhysicalDevice physicalDevice, VkDevice device, const char* appName, const char* engineName);

/* The device's implicit cache (loaded by the first call), or VK_NULL_HANDLE (disabled, no such
 * device, or it could not be created). */
VkPipelineCache PipelineCache_get(VkDevice device);

/* After a vkCreate*Pipelines call: count pipelines and compile time; withImplicitCache marks the
 * cache as having new content to save. */
void PipelineCache_noteCreated(VkDevice device, uint32_t count, uint64_t elapsedNs, bool withImplicitCache);

/* Before vkDestroyDevice (also when the server cleans up after a client that is gone): saves what is
 * new, logs the device's pipeline statistics and destroys the implicit cache. */
void PipelineCache_deviceDestroyed(VkDevice device);

/* The cache for a vkCreateGraphicsPipelines / vkCreateComputePipelines call to which the application
 * passed `cache`: that one when it is not VK_NULL_HANDLE, else the device's implicit cache
 * (PipelineCache_get), except for calls that only fast-link libraries (cheap, they would only fill
 * the cache) and calls with a shader module identifier (the identifier hashes the application's
 * SPIR-V, not what the client or the in-process ICD rewrote it to, so a cached pipeline could be
 * the wrong code): VK_NULL_HANDLE for those. */
VkPipelineCache PipelineCache_forGraphics(VkDevice device, VkPipelineCache cache, const VkGraphicsPipelineCreateInfo* createInfos,
                                          uint32_t count);
VkPipelineCache PipelineCache_forCompute(VkDevice device, VkPipelineCache cache, const VkComputePipelineCreateInfo* createInfos,
                                         uint32_t count);

/* True for a create info that only fast-links pipeline libraries (no LINK_TIME_OPTIMIZATION):
 * not worth a cache entry. */
bool PipelineCache_isFastLink(const VkGraphicsPipelineCreateInfo* createInfo);

/* True when a stage passes a shader module identifier (VK_EXT_shader_module_identifier). */
bool PipelineCache_usesModuleIdentifier(const VkPipelineShaderStageCreateInfo* stages, uint32_t stageCount);

#endif
