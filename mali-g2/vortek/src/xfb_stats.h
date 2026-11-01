/*
 * [mali-g2] D3D11 stream output on the GPU paths, counted: does a game use it, and how much? (Is moving
 * stream output into the vertex shader, as blobferry does for the Mali, worth porting?) DXVK turns D3D11
 * stream output into VK_EXT_transform_feedback, which the Mali has but runs slowly. Shared by the Vortek
 * client (droiddeck_maint.c) and the in-process libhybris path (mali-g2/hybris/src/mali_fixes.c), one
 * set of counters per process:
 *   - pipelines: graphics pipelines (pipeline libraries included) created with a stage whose SPIR-V
 *     declares ExecutionMode Xfb, as inline code (VK_KHR_maintenance5) or from a VkShaderModule that does;
 *   - gs: graphics pipelines created with a geometry shader stage;
 *   - begins: vkCmdBeginTransformFeedbackEXT calls. DXVK begins transform feedback again whenever the
 *     pipeline or the stream-output buffers change between stream-output draws (and after every render
 *     pass), so this is a lower bound of the stream-output draws;
 *   - drawauto: vkCmdDrawIndirectByteCountEXT calls (D3D11 DrawAuto).
 * Counters only: commands add to a counter (a relaxed atomic add; begins and DrawAuto are the only
 * commands wrapped for it), pipeline creation scans each stage's first SPIR-V instructions (up to the
 * execution modes). The one lock guards the handles of shader modules that declare Xfb (none in most
 * games) and the logging; commands only ever try it, every 256th call.
 *
 * Nothing is logged until something is counted. Then a line on stderr (the game's log),
 *   "<tag>: xfb stats: pid P, pipelines N, gs G, begins B, drawauto A, seconds S"
 * with S the seconds since the process's first device: right away for the first pipeline counted,
 * afterwards at most once a minute (looked at when pipelines are created, every 256th counted command and
 * every 64th present), and when a device is destroyed. Once something was counted, a line comes once a
 * minute even when the counts stay the same, so S of the last line stays within a minute of the time
 * the process has run: a game that uses stream output only while loading must not get its loading rate
 * as its rate per minute. A line never repeats the line before it. The play journal keeps the last line
 * of each process (LogSliceScanner, the record's gpu.xfb).
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef DROIDDECK_XFB_STATS_H
#define DROIDDECK_XFB_STATS_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "stage_inline_module.h"

#ifndef XFB_LOG_INTERVAL_NS /* tests shorten it */
#define XFB_LOG_INTERVAL_NS 60000000000ull
#endif
#define XFB_COMMAND_CHECK_MASK 255u
#define XFB_PRESENT_CHECK_MASK 63u

/* Whether SPIR-V code declares ExecutionMode Xfb. The execution modes come before everything but the
 * capabilities, extensions, imports, memory model and entry points (the logical layout of a module), so
 * the scan stops at the first other instruction. */
static inline bool xfb_spirv_declares(const uint32_t* code, size_t words) {
    if (!code || words < 5 || code[0] != 0x07230203u) return false;
    for (size_t i = 5; i < words;) {
        uint32_t op = code[i] & 0xffffu, n = code[i] >> 16;
        if (!n || n > words - i) return false;
        switch (op) {
        case 16: /* OpExecutionMode <entry> <mode> */
            if (n >= 3 && code[i + 2] == 11 /* Xfb */) return true;
            break;
        case 10:  /* OpExtension */
        case 11:  /* OpExtInstImport */
        case 14:  /* OpMemoryModel */
        case 15:  /* OpEntryPoint */
        case 17:  /* OpCapability */
        case 331: /* OpExecutionModeId */
            break;
        default:
            return false;
        }
        i += n;
    }
    return false;
}

typedef struct {
    uint64_t pipelines, gs, begins, drawAuto; /* atomics */
    uint64_t startNs;                         /* the first device (atomic) */
    uint32_t counted;                         /* counted anything ever (atomic) */
    uint32_t presents;                        /* atomic */
    pthread_mutex_t lock;                     /* modules, and the fields below */
    uint64_t* modules;                        /* handles of shader modules that declare Xfb */
    uint32_t moduleCount, moduleCap;          /* moduleCount is read without the lock (atomic) */
    uint64_t lastLogNs, logged[5];
} XfbStats;

static XfbStats xfbStats = {.lock = PTHREAD_MUTEX_INITIALIZER};

static inline uint64_t xfb_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* At every device creation: the first one starts the clock of the line's seconds. */
static inline void xfb_stats_device_created(void) {
    uint64_t zero = 0;
    if (!__atomic_load_n(&xfbStats.startNs, __ATOMIC_RELAXED))
        __atomic_compare_exchange_n(&xfbStats.startNs, &zero, xfb_now_ns(), false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

/* The line, once something was counted, if (unless `force`) a minute has passed since the last one and
 * the counts or the seconds differ from it. `wait`: take the lock (pipeline creation, device destruction);
 * commands and presents only try it. */
static inline void xfb_stats_maybe_log(const char* tag, bool force, bool wait) {
    if (!__atomic_load_n(&xfbStats.counted, __ATOMIC_RELAXED)) return;
    uint64_t now = xfb_now_ns();
    if (!force && now - __atomic_load_n(&xfbStats.lastLogNs, __ATOMIC_RELAXED) < XFB_LOG_INTERVAL_NS) return;
    if (wait) pthread_mutex_lock(&xfbStats.lock);
    else if (pthread_mutex_trylock(&xfbStats.lock)) return;
    if (force || now - xfbStats.lastLogNs >= XFB_LOG_INTERVAL_NS) {
        uint64_t v[4] = {__atomic_load_n(&xfbStats.pipelines, __ATOMIC_RELAXED), __atomic_load_n(&xfbStats.gs, __ATOMIC_RELAXED),
                         __atomic_load_n(&xfbStats.begins, __ATOMIC_RELAXED), __atomic_load_n(&xfbStats.drawAuto, __ATOMIC_RELAXED)};
        uint64_t start = __atomic_load_n(&xfbStats.startNs, __ATOMIC_RELAXED);
        uint64_t seconds = start && now > start ? (now - start) / 1000000000ull : 0;
        if (v[0] != xfbStats.logged[0] || v[1] != xfbStats.logged[1] || v[2] != xfbStats.logged[2] || v[3] != xfbStats.logged[3]
            || seconds != xfbStats.logged[4]) {
            fprintf(stderr, "%s: xfb stats: pid %d, pipelines %llu, gs %llu, begins %llu, drawauto %llu, seconds %llu\n", tag, (int)getpid(),
                    (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)v[3],
                    (unsigned long long)seconds);
            for (int i = 0; i < 4; i++) xfbStats.logged[i] = v[i];
            xfbStats.logged[4] = seconds;
            __atomic_store_n(&xfbStats.lastLogNs, now, __ATOMIC_RELAXED);
        }
    }
    pthread_mutex_unlock(&xfbStats.lock);
}

/* After a vkCreateShaderModule that succeeded, with the application's code. */
static inline void xfb_stats_module_created(uint64_t handle, const VkShaderModuleCreateInfo* info) {
    if (!handle || !info || !xfb_spirv_declares(info->pCode, info->codeSize / 4)) return;
    pthread_mutex_lock(&xfbStats.lock);
    if (xfbStats.moduleCount == xfbStats.moduleCap) {
        uint32_t cap = xfbStats.moduleCap ? xfbStats.moduleCap * 2 : 16;
        uint64_t* grown = realloc(xfbStats.modules, cap * sizeof(*grown));
        if (grown) {
            xfbStats.modules = grown;
            xfbStats.moduleCap = cap;
        }
    }
    if (xfbStats.moduleCount < xfbStats.moduleCap) {
        xfbStats.modules[xfbStats.moduleCount] = handle;
        __atomic_store_n(&xfbStats.moduleCount, xfbStats.moduleCount + 1, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&xfbStats.lock);
}

/* Before a vkDestroyShaderModule (the handle may come back for another module). */
static inline void xfb_stats_module_destroyed(uint64_t handle) {
    if (!handle || !__atomic_load_n(&xfbStats.moduleCount, __ATOMIC_RELAXED)) return;
    pthread_mutex_lock(&xfbStats.lock);
    for (uint32_t i = 0; i < xfbStats.moduleCount; i++) {
        if (xfbStats.modules[i] != handle) continue;
        xfbStats.modules[i] = xfbStats.modules[xfbStats.moduleCount - 1];
        __atomic_store_n(&xfbStats.moduleCount, xfbStats.moduleCount - 1, __ATOMIC_RELAXED);
        break;
    }
    pthread_mutex_unlock(&xfbStats.lock);
}

static inline bool xfb_stage_declares(const VkPipelineShaderStageCreateInfo* stage) {
    const VkShaderModuleCreateInfo* inl = dd_inline_module(stage->pNext);
    if (inl) return xfb_spirv_declares(inl->pCode, inl->codeSize / 4);
    uint64_t handle = (uint64_t)(uintptr_t)stage->module;
    if (!handle || !__atomic_load_n(&xfbStats.moduleCount, __ATOMIC_RELAXED)) return false;
    bool found = false;
    pthread_mutex_lock(&xfbStats.lock);
    for (uint32_t i = 0; i < xfbStats.moduleCount && !found; i++) found = xfbStats.modules[i] == handle;
    pthread_mutex_unlock(&xfbStats.lock);
    return found;
}

/* After a vkCreateGraphicsPipelines, with the application's create infos: the pipelines created. */
static inline void xfb_stats_pipelines_created(const char* tag, uint32_t count, const VkGraphicsPipelineCreateInfo* infos,
                                        const VkPipeline* pipelines) {
    uint64_t xfb = 0, gs = 0;
    for (uint32_t i = 0; infos && pipelines && i < count; i++) {
        if (!pipelines[i]) continue;
        bool hasXfb = false, hasGs = false;
        for (uint32_t s = 0; infos[i].pStages && s < infos[i].stageCount; s++) {
            const VkPipelineShaderStageCreateInfo* stage = &infos[i].pStages[s];
            if (stage->stage == VK_SHADER_STAGE_GEOMETRY_BIT) hasGs = true;
            if (!hasXfb) hasXfb = xfb_stage_declares(stage);
        }
        xfb += hasXfb;
        gs += hasGs;
    }
    if (!xfb && !gs) return;
    uint64_t before = __atomic_fetch_add(&xfbStats.pipelines, xfb, __ATOMIC_RELAXED) + __atomic_fetch_add(&xfbStats.gs, gs, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&xfbStats.counted, __ATOMIC_RELAXED)) __atomic_store_n(&xfbStats.counted, 1, __ATOMIC_RELAXED);
    xfb_stats_maybe_log(tag, before == 0, true);
}

static inline void xfb_stats_begin(const char* tag) {
    uint64_t n = __atomic_add_fetch(&xfbStats.begins, 1, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&xfbStats.counted, __ATOMIC_RELAXED)) __atomic_store_n(&xfbStats.counted, 1, __ATOMIC_RELAXED);
    if (!(n & XFB_COMMAND_CHECK_MASK)) xfb_stats_maybe_log(tag, false, false);
}

static inline void xfb_stats_draw_auto(const char* tag) {
    uint64_t n = __atomic_add_fetch(&xfbStats.drawAuto, 1, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&xfbStats.counted, __ATOMIC_RELAXED)) __atomic_store_n(&xfbStats.counted, 1, __ATOMIC_RELAXED);
    if (!(n & XFB_COMMAND_CHECK_MASK)) xfb_stats_maybe_log(tag, false, false);
}

/* At every present: nothing to do (one relaxed load) until something was counted. */
static inline void xfb_stats_present(const char* tag) {
    if (!__atomic_load_n(&xfbStats.counted, __ATOMIC_RELAXED)) return;
    if (!(__atomic_add_fetch(&xfbStats.presents, 1, __ATOMIC_RELAXED) & XFB_PRESENT_CHECK_MASK)) xfb_stats_maybe_log(tag, false, false);
}

/* At device destruction: the counts so far, whatever the time. */
static inline void xfb_stats_flush(const char* tag) {
    xfb_stats_maybe_log(tag, true, true);
}

#endif
