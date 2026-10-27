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
 *   - Core features DXVK requires that the Mali lacks are reported, as the Vortek server reports
 *     them (vortekrenderer's checkDeviceFeatures), and turned off again at vkCreateDevice (icd.c):
 *     fillModeNonSolid (what line or point fill then draws is up to the driver, as through Vortek),
 *     and shaderClipDistance / shaderCullDistance while their writes are removed from shaders.
 *     Without them DXVK finds no adapter ("Device does not support required feature
 *     'fillModeNonSolid'").
 *   - Every Nth vkResetCommandPool / vkResetCommandBuffer of the same pool or command buffer adds
 *     RELEASE_RESOURCES, as the Vortek server does (winlator-0018): the Mali keeps ~420 bytes of
 *     host heap per vkCmdBeginRendering in a command buffer that also records a query until it is
 *     reset with RELEASE_RESOURCES, and DXVK resets its pools with flags 0, so the game process
 *     grows by tens of MB a minute (vortek/tests/frame_leak_test.c). N is
 *     DROIDDECK_HYBRIS_CMD_RELEASE_EVERY, else VORTEK_CMD_RELEASE_EVERY, default 32; 1 = every
 *     reset, 0 = never. Implicit resets by vkBeginCommandBuffer are not covered.
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
    if (findInChain(properties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT) &&
        icd_placed_available(instance, physical))
        placed_fill_properties(properties->pNext);
}

static bool clipStripOn(void) {
    static int on = -1;
    if (on < 0) on = !icd_env_int("VORTEK_KEEP_CLIP_DISTANCE", 0);
    return on;
}

static void emulateFeatures(VkPhysicalDeviceFeatures* features) {
    features->fillModeNonSolid = VK_TRUE;
    if (clipStripOn()) {
        features->shaderClipDistance = VK_TRUE;
        features->shaderCullDistance = VK_TRUE;
    }
}

static void VKAPI_CALL GetPhysicalDeviceFeatures(VkPhysicalDevice physical, VkPhysicalDeviceFeatures* features) {
    Instance* instance = icd_instance_of(physical);
    if (!instance) return;
    instance->GetPhysicalDeviceFeatures(physical, features);
    emulateFeatures(features);
}

static void VKAPI_CALL GetPhysicalDeviceFeatures2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2* features) {
    Instance* instance = icd_instance_of(physical);
    if (!instance) return;
    instance->GetPhysicalDeviceFeatures2(physical, features);
    emulateFeatures(&features->features);
    if (findInChain(features->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT) &&
        icd_placed_available(instance, physical))
        placed_fill_features(features->pNext);
}

void mali_drop_emulated_features(Instance* instance, VkPhysicalDevice physical, VkPhysicalDeviceFeatures* enabled) {
    VkPhysicalDeviceFeatures driver = {0};
    instance->GetPhysicalDeviceFeatures(physical, &driver);
    VkPhysicalDeviceFeatures emulated = driver;
    emulateFeatures(&emulated);
    VkBool32* want = (VkBool32*)enabled;
    const VkBool32* has = (const VkBool32*)&driver;
    const VkBool32* fake = (const VkBool32*)&emulated;
    for (size_t i = 0; i < sizeof(*enabled) / sizeof(VkBool32); i++)
        if (want[i] && !has[i] && fake[i]) want[i] = VK_FALSE;
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

static int releaseEvery(void) {
    static int every = -1;
    int value = __atomic_load_n(&every, __ATOMIC_RELAXED);
    if (value >= 0) return value;
    const char* name = "DROIDDECK_HYBRIS_CMD_RELEASE_EVERY";
    const char* text = getenv(name);
    if (!text || !*text) text = getenv(name = "VORTEK_CMD_RELEASE_EVERY");
    value = text && *text ? atoi(text) : 32;
    if (value < 0) value = 0;
    if (text && *text) fprintf(stderr, "hybris-icd: %s=%d\n", name, value);
    __atomic_store_n(&every, value, __ATOMIC_RELAXED);
    return value;
}

#define RESET_SLOT_BITS 8
static pthread_mutex_t resetLock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t handle; uint32_t resets; } resetCounts[1u << RESET_SLOT_BITS];

/* A handle new to its slot (first reset, or a collision) releases. */
static bool releaseOnReset(uint64_t handle) {
    int every = releaseEvery();
    if (every <= 1) return every == 1;
    uint32_t slot = (uint32_t)((handle * 0x9E3779B97F4A7C15ull) >> (64 - RESET_SLOT_BITS));
    bool release = true;
    pthread_mutex_lock(&resetLock);
    if (resetCounts[slot].handle != handle) {
        resetCounts[slot].handle = handle;
        resetCounts[slot].resets = 0;
    }
    else if (++resetCounts[slot].resets < (uint32_t)every) release = false;
    else resetCounts[slot].resets = 0;
    pthread_mutex_unlock(&resetLock);
    return release;
}

static VkResult VKAPI_CALL ResetCommandPool(VkDevice handle, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    if (releaseOnReset((uint64_t)pool)) flags |= VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT;
    return device->ResetCommandPool(handle, pool, flags);
}

/* A command buffer does not name its device. Its vkResetCommandBuffer from libhybris is the Mali
 * driver's own function (in libGLES_mali.so), the same for every device. */
static PFN_vkResetCommandBuffer nextResetCommandBuffer;

void mali_device_created(Device* device) {
    PFN_vkResetCommandBuffer expected = NULL;
    if (device->ResetCommandBuffer)
        __atomic_compare_exchange_n(&nextResetCommandBuffer, &expected, device->ResetCommandBuffer, false, __ATOMIC_RELEASE,
                                    __ATOMIC_RELAXED);
}

static VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags) {
    PFN_vkResetCommandBuffer next = __atomic_load_n(&nextResetCommandBuffer, __ATOMIC_ACQUIRE);
    if (!next) return VK_ERROR_DEVICE_LOST;
    if (releaseOnReset((uint64_t)(uintptr_t)commandBuffer)) flags |= VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT;
    return next(commandBuffer, flags);
}

PFN_vkVoidFunction mali_instance_proc(const char* name) {
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures")) return (PFN_vkVoidFunction)GetPhysicalDeviceFeatures;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2") || !strcmp(name, "vkGetPhysicalDeviceFeatures2KHR"))
        return (PFN_vkVoidFunction)GetPhysicalDeviceFeatures2;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2") || !strcmp(name, "vkGetPhysicalDeviceProperties2KHR"))
        return (PFN_vkVoidFunction)GetPhysicalDeviceProperties2;
    return NULL;
}

PFN_vkVoidFunction mali_device_proc(const char* name) {
    if (!strcmp(name, "vkCreateShaderModule")) return (PFN_vkVoidFunction)CreateShaderModule;
    if (!strcmp(name, "vkCreateGraphicsPipelines")) return (PFN_vkVoidFunction)CreateGraphicsPipelines;
    if (!strcmp(name, "vkResetCommandPool")) return (PFN_vkVoidFunction)ResetCommandPool;
    if (!strcmp(name, "vkResetCommandBuffer")) return (PFN_vkVoidFunction)ResetCommandBuffer;
    return NULL;
}
