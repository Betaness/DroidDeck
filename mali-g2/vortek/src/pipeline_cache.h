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

/* After vkCreateDevice: loads the cache file of (application, driver) into a VkPipelineCache of the
 * device. appName/engineName come from the instance's VkApplicationInfo (either may be NULL). */
void PipelineCache_deviceCreated(VkPhysicalDevice physicalDevice, VkDevice device, const char* appName, const char* engineName);

/* The device's implicit cache, or VK_NULL_HANDLE (disabled, or no such device). */
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

#endif
