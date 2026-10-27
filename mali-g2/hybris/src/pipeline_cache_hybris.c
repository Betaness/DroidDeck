/*
 * The Vortek server's persistent implicit pipeline cache (../vortek/src/pipeline_cache.c), built
 * into the game's process: the same files, file format, crash marker, lazy load, merging and limits.
 * The driver is reached through the device and instance tables, the variables are
 * DROIDDECK_HYBRIS_PIPELINE_CACHE_* (for the switch, the limits and the log, VORTEK_PIPELINE_CACHE_*
 * when those are unset; not for the directory, which is the server's own) and logs go to stderr.
 * droiddeck-game-env sets DROIDDECK_HYBRIS_PIPELINE_CACHE_DIR; unset, there is no implicit cache.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include "icd.h"

#define PCACHE_LOG(fmt, ...) fprintf(stderr, "hybris-icd: " fmt "\n", ##__VA_ARGS__)
#define PCACHE_VK(device, name) (icd_device(device)->name)
#define PCACHE_VK_PHYSICAL(physicalDevice, name) (icd_instance_of(physicalDevice)->name)
#define PCACHE_ENV "DROIDDECK_HYBRIS_PIPELINE_CACHE"
#define PCACHE_ENV_FALLBACK "VORTEK_PIPELINE_CACHE"

#include "pipeline_cache.c"
