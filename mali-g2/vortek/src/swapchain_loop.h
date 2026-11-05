/*
 * [mali-g2] Recreation-loop guard for the X11 WSIs of the Vortek client (client_x11_present.c) and the
 * in-process libhybris path (mali-g2/hybris/src/wsi.c). Both report VK_SUBOPTIMAL_KHR when the window's
 * size differs from the swapchain's. An application that answers with a new swapchain of the very same
 * parameters can never fix that mismatch, and recreates its swapchain every frame (the screen flickers,
 * thousands of swapchains). Proton's fullscreen hack did this when the WSI reported a minImageExtent
 * smaller than the window (it sizes its host swapchain from minImageExtent).
 *
 * swapchain_loop_created() is called for every swapchain created for a window: when that window gets
 * more than VORTEK_RECREATE_LIMIT (default 10, 0: no guard) swapchains with identical parameters within
 * a second, the new one is "quiet": it does not report a size mismatch as VK_SUBOPTIMAL_KHR while the
 * window keeps the size it has at the swapchain's first mismatched present (the caller logs this once).
 * Later swapchains with the same parameters inherit it. When the window changes size, the quiet
 * swapchain reports VK_SUBOPTIMAL_KHR again and the count starts over (swapchain_loop_reset()), so an
 * application that does follow the window is told about it. Other parameters start the count over too.
 *
 * The table is small (windows recreating swapchains at the same time) and only touched when swapchains
 * are created and when a quiet one sees its window change size; presents of other swapchains never
 * look at it.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef MALI_G2_SWAPCHAIN_LOOP_H
#define MALI_G2_SWAPCHAIN_LOOP_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define SWAPCHAIN_LOOP_WINDOWS 16

typedef struct {
    uint32_t window;   /* 0: free */
    uint64_t key;      /* the create parameters */
    double start;      /* the second being counted */
    uint32_t count;    /* swapchains with these parameters in it */
    bool quiet;
    double lastUse;
} SwapchainLoopEntry;

static pthread_mutex_t swapchainLoopLock = PTHREAD_MUTEX_INITIALIZER;
static SwapchainLoopEntry swapchainLoopEntries[SWAPCHAIN_LOOP_WINDOWS];

static inline uint64_t swapchain_loop_mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

/* Everything of the create info a recreation could change (the old swapchain and pNext aside). */
static inline uint64_t swapchain_loop_key(const VkSwapchainCreateInfoKHR* info) {
    uint64_t h = 0x84222325cbf29ce4ull;
    h = swapchain_loop_mix(h, info->flags);
    h = swapchain_loop_mix(h, info->minImageCount);
    h = swapchain_loop_mix(h, (uint64_t)info->imageFormat);
    h = swapchain_loop_mix(h, (uint64_t)info->imageColorSpace);
    h = swapchain_loop_mix(h, (uint64_t)info->imageExtent.width << 32 | info->imageExtent.height);
    h = swapchain_loop_mix(h, info->imageArrayLayers);
    h = swapchain_loop_mix(h, info->imageUsage);
    h = swapchain_loop_mix(h, (uint64_t)info->imageSharingMode);
    h = swapchain_loop_mix(h, (uint64_t)info->preTransform);
    h = swapchain_loop_mix(h, (uint64_t)info->compositeAlpha);
    h = swapchain_loop_mix(h, (uint64_t)info->presentMode);
    h = swapchain_loop_mix(h, info->clipped);
    return h ? h : 1;
}

/* A new swapchain with these parameters for this window at time now (seconds). Returns whether it is
 * quiet; *count is the number of such swapchains within the second, *justQuiet whether this one made
 * the window quiet (log it). */
static inline bool swapchain_loop_created(uint32_t window, uint64_t key, double now, int limit, uint32_t* count, bool* justQuiet) {
    *count = 0;
    *justQuiet = false;
    if (limit <= 0 || !window) return false;
    pthread_mutex_lock(&swapchainLoopLock);
    SwapchainLoopEntry* e = NULL;
    SwapchainLoopEntry* oldest = &swapchainLoopEntries[0];
    for (int i = 0; i < SWAPCHAIN_LOOP_WINDOWS; i++) {
        SwapchainLoopEntry* c = &swapchainLoopEntries[i];
        if (c->window == window) {
            e = c;
            break;
        }
        if (!c->window || (oldest->window && c->lastUse < oldest->lastUse)) oldest = c;
    }
    if (!e) {
        e = oldest;
        memset(e, 0, sizeof(*e));
        e->window = window;
    }
    e->lastUse = now;
    if (e->key != key) {
        e->key = key;
        e->count = 0;
        e->start = now;
        e->quiet = false;
    }
    else if (now - e->start > 1.0) {
        e->count = 0;
        e->start = now;
    }
    e->count++;
    if (!e->quiet && e->count > (uint32_t)limit) {
        e->quiet = true;
        *justQuiet = true;
    }
    bool quiet = e->quiet;
    *count = e->count;
    pthread_mutex_unlock(&swapchainLoopLock);
    return quiet;
}

/* The window of a quiet swapchain changed size: count its swapchains anew. */
static inline void swapchain_loop_reset(uint32_t window, uint64_t key) {
    pthread_mutex_lock(&swapchainLoopLock);
    for (int i = 0; i < SWAPCHAIN_LOOP_WINDOWS; i++) {
        SwapchainLoopEntry* e = &swapchainLoopEntries[i];
        if (e->window == window && e->key == key) {
            e->quiet = false;
            e->count = 0;
            e->start = 0;
        }
    }
    pthread_mutex_unlock(&swapchainLoopLock);
}

/* Per swapchain: whether a present whose window is w x h reports VK_SUBOPTIMAL_KHR. quietW/H hold the
 * window size the quiet swapchain keeps quiet for (0 until its first mismatch). Returns 1 to report,
 * 0 not to, -1 to report because the window of a quiet swapchain changed size (the caller logs it and
 * calls swapchain_loop_reset()). */
static inline int swapchain_loop_suboptimal(bool* quiet, uint32_t* quietW, uint32_t* quietH, VkExtent2D extent, uint32_t w, uint32_t h) {
    if (w == extent.width && h == extent.height) return 0;
    if (!*quiet) return 1;
    if (!*quietW) {
        *quietW = w;
        *quietH = h;
        return 0;
    }
    if (w == *quietW && h == *quietH) return 0;
    *quiet = false;
    return -1;
}

#endif
