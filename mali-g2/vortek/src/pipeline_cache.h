/*
 * [mali-g2] Persistent implicit pipeline cache of the Vortek server (pipeline_cache.c).
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

/* True for a create info that only fast-links pipeline libraries (no LINK_TIME_OPTIMIZATION):
 * not worth a cache entry. */
bool PipelineCache_isFastLink(const VkGraphicsPipelineCreateInfo* createInfo);

/* True when a stage passes a shader module identifier (VK_EXT_shader_module_identifier): such a
 * call must not use the implicit cache, the identifier hashes the application's SPIR-V, which the
 * client may have rewritten differently when the cached pipeline was compiled. */
bool PipelineCache_usesModuleIdentifier(const VkPipelineShaderStageCreateInfo* stages, uint32_t stageCount);

#endif
