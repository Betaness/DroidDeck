/*
 * [mali-g2] VK_KHR_maintenance5 / VK_KHR_maintenance6 for the Vortek client.
 *
 * DXVK 3.x (shipped with Proton Experimental ARM64) refuses devices without the
 * maintenance5 and maintenance6 features. The Mali driver has both, but Vortek's
 * wire protocol predates them. Instead of extending the generated serializer,
 * this file emulates the new entry points on top of commands Vortek already
 * forwards, and folds the new 64-bit flag structs into the legacy flag fields
 * (the low 32 bits are defined to be identical). The server enables the two
 * features on the real driver when the extensions are requested
 * (patches/winlator-0003-enable-maintenance5-6.patch).
 *
 * VK_EXT_graphics_pipeline_library goes over the wire (patches/vortek-0008, winlator-0010:
 * features, properties, library create infos, library handles); this file only drops the
 * state that the spec ignores for the library subsets a create call does not provide.
 * VK_KHR_maintenance7, VK_KHR_unified_image_layouts and VK_KHR_shader_float_controls2 (no
 * commands) also go over the wire (vortek-0010, winlator-0014): nothing to emulate here.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <vulkan/vulkan.h>

VkResult vt_call_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
void vt_call_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
void vt_call_vkGetPhysicalDeviceProperties2(VkPhysicalDevice, VkPhysicalDeviceProperties2*);
VkResult vt_call_vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
VkResult vt_call_vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
VkResult vt_call_vkCreateBuffer(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer*);
VkResult vt_call_vkCreateImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*);
void vt_call_vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks*);
void vt_call_vkGetImageSubresourceLayout(VkDevice, VkImage, const VkImageSubresource*, VkSubresourceLayout*);
void vt_call_vkCmdBindIndexBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType);
void vt_call_vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*);
void vt_call_vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*);
void vt_call_vkCmdPushDescriptorSetKHR(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkWriteDescriptorSet*);
void vt_call_vkCmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer, VkDescriptorUpdateTemplate, VkPipelineLayout, uint32_t, const void*);

static void* findInChain(const void* chain, VkStructureType type) {
    for (const VkBaseInStructure* s = chain; s; s = s->pNext) {
        if (s->sType == type) return (void*)s;
    }
    return NULL;
}

/* VORTEK_TRACE=1: log the queries an application makes before vkCreateDevice (bring-up aid). */
#include <stdio.h>
#include <stdarg.h>
static int traceOn = -1;
static void trace(const char* fmt, ...) {
    if (traceOn < 0) { const char* e = getenv("VORTEK_TRACE"); traceOn = e && *e && *e != '0'; }
    if (!traceOn) return;
    va_list ap; va_start(ap, fmt);
    fputs("vortek-trace: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}
static void traceChain(const char* what, const void* chain) {
    if (traceOn == 0) return;
    char buf[1024]; int n = 0; buf[0] = 0;
    for (const VkBaseInStructure* s = chain; s && n < (int)sizeof(buf) - 16; s = s->pNext) n += snprintf(buf + n, sizeof(buf) - n, " %d", s->sType);
    trace("%s pNext:%s", what, buf);
}
/* The extension filter (vortek-0003) resolves every command of every advertised extension;
 * those lookups are not the application's, so they are not logged. */
static __thread int quietLookups;
void droiddeck_trace_missing(const char* name) { if (!quietLookups) trace("no entry point: %s", name); }

/* VORTEK_TRACE=2 also logs every request sent to the server, with the sending thread: the last
 * request of a thread that hangs is the one the server did not answer (codes: request_codes.h). */
#include <sys/syscall.h>
#include <unistd.h>
static int traceLevel = -1;
void droiddeck_trace_cmd(const char* name) {
    if (traceLevel < 0) { const char* e = getenv("VORTEK_TRACE"); traceLevel = e ? atoi(e) : 0; }
    if (traceLevel >= 3) fprintf(stderr, "vortek-trace: cmd %s\n", name);
}
void droiddeck_trace_request(int requestCode, int size) {
    if (traceLevel < 0) { const char* e = getenv("VORTEK_TRACE"); traceLevel = e ? atoi(e) : 0; }
    if (traceLevel < 2) return;
    fprintf(stderr, "vortek-trace: [%ld] request %d (%d bytes)\n", (long)syscall(SYS_gettid), requestCode & 0xffff, size);
}

static bool deviceHasExtension(VkPhysicalDevice physicalDevice, const char* name) {
    uint32_t count = 0;
    bool found = false;
    quietLookups++;
    if (vt_call_vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &count, NULL) == VK_SUCCESS && count) {
        VkExtensionProperties* props = calloc(count, sizeof(*props));
        if (props && vt_call_vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &count, props) >= 0) {
            for (uint32_t i = 0; i < count && !found; i++) found = strcmp(props[i].extensionName, name) == 0;
        }
        free(props);
    }
    quietLookups--;
    return found;
}

/* ---- feature / property queries ---- */

static void dd_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures) {
    traceChain("GetPhysicalDeviceFeatures2", pFeatures->pNext);
    vt_call_vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
    VkPhysicalDeviceMaintenance5FeaturesKHR* m5 = findInChain(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR);
    VkPhysicalDeviceMaintenance6FeaturesKHR* m6 = findInChain(pFeatures->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR);
    if (m5) m5->maintenance5 = deviceHasExtension(physicalDevice, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    if (m6) m6->maintenance6 = deviceHasExtension(physicalDevice, VK_KHR_MAINTENANCE_6_EXTENSION_NAME);
}

static void dd_vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties) {
    traceChain("GetPhysicalDeviceProperties2", pProperties->pNext);
    vt_call_vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);
    trace("  -> apiVersion %u.%u.%u type %d", VK_API_VERSION_MAJOR(pProperties->properties.apiVersion), VK_API_VERSION_MINOR(pProperties->properties.apiVersion), VK_API_VERSION_PATCH(pProperties->properties.apiVersion), pProperties->properties.deviceType);
    /* Conservative values; on Mali-G2 the driver reports 1,1,1,0,0,0 and 1,1,1. */
    VkPhysicalDeviceMaintenance5PropertiesKHR* p5 = findInChain(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_PROPERTIES_KHR);
    if (p5) {
        p5->earlyFragmentMultisampleCoverageAfterSampleCounting = VK_TRUE;
        p5->earlyFragmentSampleMaskTestBeforeSampleCounting = VK_TRUE;
        p5->depthStencilSwizzleOneSupport = VK_FALSE;
        p5->polygonModePointSize = VK_FALSE;
        p5->nonStrictSinglePixelWideLinesUseParallelogram = VK_FALSE;
        p5->nonStrictWideLinesUseParallelogram = VK_FALSE;
    }
    VkPhysicalDeviceMaintenance6PropertiesKHR* p6 = findInChain(pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_PROPERTIES_KHR);
    if (p6) {
        p6->blockTexelViewCompatibleMultipleLayers = VK_FALSE;
        p6->maxCombinedImageSamplerDescriptorCount = 1;
        p6->fragmentShadingRateClampCombinerInputs = VK_FALSE;
    }
}

/* ---- VK_EXT_graphics_pipeline_library ---- */

/* The spec ignores the state of a subset that a library or linking call does not provide, so
 * those pointers/handles may dangle; the serializer would follow them. Drop them.
 *
 * Known gap on the server (ShaderInspector, shader_inspector.c): its SPIR-V fixes only apply
 * to VkShaderModule objects and only see the stages of one create call, and a module is
 * rewritten and compiled once, on its first use. So for module-based apps the cross-stage fixes
 * (USCALED/SSCALED vertex formats, VS/FS varying sizes) are skipped for library parts, and once
 * a module has been used in a library, later monolithic pipelines with that module do not get
 * them either. Inline SPIR-V (DXVK) is never inspected; zink is not offered GPL. */
static void dropIgnoredLibraryState(VkGraphicsPipelineCreateInfo* info) {
    const VkGraphicsPipelineLibraryCreateInfoEXT* gpl = findInChain(info->pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT);
    const VkPipelineLibraryCreateInfoKHR* libs = findInChain(info->pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);
    uint32_t libraryCount = libs ? libs->libraryCount : 0;
    VkGraphicsPipelineLibraryFlagsEXT parts;
    if (gpl) parts = gpl->flags;
    else if ((info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) || libraryCount) parts = 0;
    else return; /* a complete pipeline */

    bool vertexInput = parts & VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
    bool preRaster = parts & VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
    bool fragmentShader = parts & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
    bool fragmentOutput = parts & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
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

/* ---- 64-bit flag structs folded into the legacy fields ---- */

/* The Mali shader compiler (cmpbe_v2_compile_multiple_shaders) segfaults, taking the whole Vortek
 * server down, on any store to a ClipDistance / CullDistance member of an output block, and in
 * pipeline libraries also to a standalone ClipDistance variable; the driver reports
 * maxClipDistances = maxCullDistances = 0 anyway. DXVK's D3D9 fixed-function vertex shader writes
 * them (A.V.A., 2026-10-23), so do D3D11 SV_ClipDistance shaders. Those stores, and the access
 * chains that only fed them, become OpNop (same length, still valid SPIR-V): user clip planes do
 * nothing, as with the device's limits of 0. VORTEK_KEEP_CLIP_DISTANCE=1 keeps the shaders as they are. */
enum { OpNop = 0, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpVariable = 59, OpStore = 62,
       OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpMemberDecorate = 72,
       OpCopyObject = 83, OpFunction = 54, OpLoad = 61, OpExtInst = 12, OpSwitch = 251, OpLine = 8, OpNoLine = 317,
       OpName = 5, DecBuiltIn = 11, BuiltInClipDistance = 3, BuiltInCullDistance = 4 };

typedef struct { uint32_t structId, member; } Member;

static uint32_t* dd_strip_clip_cull(const uint32_t* code, size_t words, size_t* outWords) {
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3];
    Member members[16];
    int nmembers = 0;
    uint32_t vars[16];
    int nvars = 0;
    /* pass 1: clip/cull block members */
    for (size_t i = 5; i < words;) {
        uint32_t n = code[i] >> 16, op = code[i] & 0xffff;
        if (n == 0 || i + n > words) return NULL;
        if (op == OpMemberDecorate && n >= 5 && code[i + 3] == DecBuiltIn &&
            (code[i + 4] == BuiltInClipDistance || code[i + 4] == BuiltInCullDistance) && nmembers < 16)
            members[nmembers++] = (Member){code[i + 1], code[i + 2]};
        else if (op == OpDecorate && n >= 4 && code[i + 2] == DecBuiltIn &&
                 (code[i + 3] == BuiltInClipDistance || code[i + 3] == BuiltInCullDistance) && nvars < 16)
            vars[nvars++] = code[i + 1];
        i += n;
    }
    if (!nmembers && !nvars) return NULL;

    uint32_t* w = malloc(words * 4);
    uint32_t* constVal = calloc(bound, 4);     /* OpConstant value + 1 (0 = not a constant) */
    uint32_t* pointee = calloc(bound, 4);      /* pointer type -> pointee type */
    uint32_t* varStruct = calloc(bound, 4);    /* variable -> pointee struct type */
    uint8_t* tainted = calloc(bound, 1);       /* pointer into a clip/cull member */
    uint32_t* uses = calloc(bound, 4);
    uint8_t* removed = calloc(bound, 1);
    if (!w || !constVal || !pointee || !varStruct || !tainted || !uses || !removed) goto fail;
    memcpy(w, code, words * 4);
    for (int v = 0; v < nvars; v++) if (vars[v] < bound) tainted[vars[v]] = 1;

    for (size_t i = 5; i < words; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (op == OpConstant && n >= 4 && w[i + 2] < bound) constVal[w[i + 2]] = w[i + 3] + 1;
        else if (op == OpTypePointer && n >= 4 && w[i + 1] < bound) pointee[w[i + 1]] = w[i + 3];
        else if (op == OpVariable && n >= 4 && w[i + 2] < bound && w[i + 1] < bound) varStruct[w[i + 2]] = pointee[w[i + 1]];
        else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 4) {
            uint32_t res = w[i + 2], base = w[i + 3];
            if (res >= bound || base >= bound) continue;
            if (tainted[base]) tainted[res] = 1;
            else if (n >= 5 && w[i + 4] < bound && constVal[w[i + 4]]) {
                for (int m = 0; m < nmembers; m++)
                    if (varStruct[base] == members[m].structId && constVal[w[i + 4]] - 1 == members[m].member) tainted[res] = 1;
            }
        }
        else if (op == OpCopyObject && n >= 4 && w[i + 3] < bound && tainted[w[i + 3]] && w[i + 2] < bound) tainted[w[i + 2]] = 1;
    }

    int changed = 0;
    for (size_t i = 5; i < words; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16;
        if ((w[i] & 0xffff) == OpStore && n >= 3 && w[i + 1] < bound && tainted[w[i + 1]]) {
            for (uint32_t j = 0; j < n; j++) w[i + j] = (1u << 16) | OpNop;
            changed = 1;
        }
    }
    /* the access chains (and copies) that fed only those stores: counted as uses are the id
     * operands of instructions inside functions (literal operands are skipped, a literal equal to
     * a chain's id would keep the chain, and a chain left pointing at the built-in still crashes) */
    for (int again = 1; again;) {
        again = 0;
        memset(uses, 0, bound * 4);
        int inFunction = 0;
        for (size_t i = 5; i < words; i += w[i] >> 16) {
            uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
            if (op == OpFunction) inFunction = 1;
            if (!inFunction || op == OpNop || op == OpLine || op == OpNoLine) continue;
            uint32_t first = 1, last = n;                      /* operand words [first, last) to count */
            if (op == OpLoad) { first = 3; last = n < 4 ? n : 4; }
            else if (op == OpStore) { first = 1; last = n < 3 ? n : 3; }
            else if (op == OpExtInst) { first = 3; }           /* word 4 (instruction number) skipped below */
            else if (op == OpSwitch) { first = 1; last = n < 3 ? n : 3; }
            else if (op == OpAccessChain || op == OpInBoundsAccessChain || op == OpCopyObject) first = 3;
            for (uint32_t j = first; j < last; j++) {
                if (op == OpExtInst && j == 4) continue;
                if (w[i + j] < bound) uses[w[i + j]]++;
            }
            if (op == OpSwitch) for (uint32_t j = 4; j < n; j += 2) if (w[i + j] < bound) uses[w[i + j]]++;  /* labels */
        }
        for (size_t i = 5; i < words; i += w[i] >> 16) {
            uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
            if ((op == OpAccessChain || op == OpInBoundsAccessChain || op == OpCopyObject) && n >= 4 &&
                w[i + 2] < bound && tainted[w[i + 2]] && uses[w[i + 2]] == 0) {
                removed[w[i + 2]] = 1;
                for (uint32_t j = 0; j < n; j++) w[i + j] = (1u << 16) | OpNop;
                again = changed = 1;
            }
        }
    }
    /* decorations of removed ids (NonUniform and the like) would name ids that no longer exist */
    for (size_t i = 5; i < words; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if ((op == OpDecorate || op == OpName) && n >= 2 && w[i + 1] < bound && removed[w[i + 1]])
            for (uint32_t j = 0; j < n; j++) w[i + j] = (1u << 16) | OpNop;
    }
    free(constVal); free(pointee); free(varStruct); free(tainted); free(uses); free(removed);
    if (!changed) { free(w); return NULL; }
    *outWords = words;
    return w;
fail:
    free(w); free(constVal); free(pointee); free(varStruct); free(tainted); free(uses); free(removed);
    return NULL;
}

static bool clipStripOn(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("VORTEK_KEEP_CLIP_DISTANCE"); on = !(e && *e == '1'); }
    return on;
}

/* Returns a rewritten copy of `info` in *out (code to free in *freeCode), or false to use `info`. */
static bool stripClipCull(const VkShaderModuleCreateInfo* info, VkShaderModuleCreateInfo* out, uint32_t** freeCode) {
    if (!clipStripOn() || !info->pCode || info->codeSize < 20) return false;
    size_t words = 0;
    uint32_t* code = dd_strip_clip_cull(info->pCode, info->codeSize / 4, &words);
    if (!code) return false;
    static int logged;
    if (!logged) { logged = 1; fprintf(stderr, "vortek: clip/cull distance writes removed from a shader (the Mali compiler crashes on them)\n"); }
    *out = *info;
    out->pCode = code;
    out->codeSize = words * 4;
    *freeCode = code;
    return true;
}

/* VORTEK_DUMP_SPIRV=<dir>: every shader module and every inline stage of a pipeline is written to
 * <dir> before the call, and pipelines.log gets a line per pipeline (synced), so the last line names
 * the pipeline the driver was compiling when it crashed. */
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
static const char* dumpDir(void) {
    static const char* dir = (const char*)1;
    if (dir == (const char*)1) { dir = getenv("VORTEK_DUMP_SPIRV"); if (dir && !*dir) dir = NULL; }
    return dir;
}
static pthread_mutex_t dumpLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned dumpSeq;
static void dumpWrite(const char* name, const void* data, size_t size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dumpDir(), name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (write(fd, data, size) != (ssize_t)size) {}
    close(fd);
}
static void dumpLog(const char* line) {
    char path[512];
    snprintf(path, sizeof(path), "%s/pipelines.log", dumpDir());
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (write(fd, line, strlen(line)) < 0) {}
    fsync(fd);
    close(fd);
}
static void dumpPipeline(const char* kind, const VkPipelineShaderStageCreateInfo* stages, uint32_t stageCount, uint32_t flags) {
    pthread_mutex_lock(&dumpLock);
    unsigned seq = ++dumpSeq;
    char line[1024];
    int n = snprintf(line, sizeof(line), "%s %u pid %d flags 0x%x:", kind, seq, getpid(), flags);
    for (uint32_t s = 0; s < stageCount && n < (int)sizeof(line) - 96; s++) {
        const VkShaderModuleCreateInfo* inl = findInChain(stages[s].pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        if (inl) {
            char name[96];
            snprintf(name, sizeof(name), "p%05u-%d-s%x.spv", seq, getpid(), (unsigned)stages[s].stage);
            dumpWrite(name, inl->pCode, inl->codeSize);
            n += snprintf(line + n, sizeof(line) - n, " stage 0x%x %s", (unsigned)stages[s].stage, name);
        }
        else n += snprintf(line + n, sizeof(line) - n, " stage 0x%x module %p", (unsigned)stages[s].stage, (void*)stages[s].module);
    }
    snprintf(line + n, sizeof(line) - n, "\n");
    dumpLog(line);
    pthread_mutex_unlock(&dumpLock);
}

VkResult vt_call_vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
static VkResult dd_vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* alloc, VkShaderModule* module) {
    VkShaderModuleCreateInfo stripped;
    uint32_t* strippedCode = NULL;
    VkResult r = vt_call_vkCreateShaderModule(device, stripClipCull(info, &stripped, &strippedCode) ? &stripped : info, alloc, module);
    free(strippedCode);
    if (dumpDir() && r == VK_SUCCESS) {
        char name[96];
        snprintf(name, sizeof(name), "m%p.spv", (void*)*module);
        dumpWrite(name, info->pCode, info->codeSize);
    }
    return r;
}

static VkResult dd_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* alloc, VkPipeline* pipelines) {
    for (uint32_t i = 0; dumpDir() && i < count; i++) dumpPipeline("graphics", infos[i].pStages, infos[i].stageCount, infos[i].flags);
    VkGraphicsPipelineCreateInfo* copy = malloc(count * sizeof(*copy));
    if (!copy) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(copy, infos, count * sizeof(*copy));
    /* Inline stage code (DXVK 3.x, pipeline libraries) gets the clip/cull rewrite: the stage and its
     * VkShaderModuleCreateInfo are copied when the latter heads the stage's pNext chain. */
    uint32_t totalStages = 0;
    for (uint32_t i = 0; i < count; i++) totalStages += infos[i].stageCount;
    VkPipelineShaderStageCreateInfo* stageCopies = totalStages ? calloc(totalStages, sizeof(*stageCopies)) : NULL;
    VkShaderModuleCreateInfo* moduleCopies = totalStages ? calloc(totalStages, sizeof(*moduleCopies)) : NULL;
    uint32_t** codes = totalStages ? calloc(totalStages, sizeof(*codes)) : NULL;
    uint32_t used = 0;
    for (uint32_t i = 0; i < count; i++) {
        const VkPipelineCreateFlags2CreateInfoKHR* f2 = findInChain(infos[i].pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR);
        if (f2) copy[i].flags = (VkPipelineCreateFlags)(f2->flags & 0xffffffffull);
        dropIgnoredLibraryState(&copy[i]);
        if (!stageCopies || !moduleCopies || !codes) continue;
        bool rewritten = false;
        uint32_t base = used;
        for (uint32_t s = 0; s < infos[i].stageCount; s++, used++) {
            stageCopies[used] = infos[i].pStages[s];
            const VkShaderModuleCreateInfo* inl = infos[i].pStages[s].pNext;
            if (inl && inl->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO &&
                stripClipCull(inl, &moduleCopies[used], &codes[used])) {
                stageCopies[used].pNext = &moduleCopies[used];
                rewritten = true;
            }
        }
        if (rewritten) copy[i].pStages = &stageCopies[base];
    }
    for (uint32_t i = 0; traceOn > 0 && i < count; i++) {
        const VkGraphicsPipelineCreateInfo* g = &copy[i];
        traceChain("CreateGraphicsPipelines", g->pNext);
        trace("  flags 0x%x stages %u layout %p renderPass %p", g->flags, g->stageCount, (void*)g->layout, (void*)g->renderPass);
        const VkGraphicsPipelineLibraryCreateInfoEXT* gl = findInChain(g->pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT);
        const VkPipelineLibraryCreateInfoKHR* pl = findInChain(g->pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);
        if (gl || pl) trace("  library parts 0x%x, links %u libraries", gl ? gl->flags : 0, pl ? pl->libraryCount : 0);
        for (uint32_t s = 0; s < g->stageCount; s++) traceChain("  stage", g->pStages[s].pNext);
        if (g->pVertexInputState) {
            const VkPipelineVertexInputStateCreateInfo* v = g->pVertexInputState;
            traceChain("  vertexInput", v->pNext);
            for (uint32_t b = 0; b < v->vertexBindingDescriptionCount; b++) trace("    binding %u stride %u rate %d", v->pVertexBindingDescriptions[b].binding, v->pVertexBindingDescriptions[b].stride, v->pVertexBindingDescriptions[b].inputRate);
            for (uint32_t a = 0; a < v->vertexAttributeDescriptionCount; a++) trace("    attr loc %u binding %u format %d offset %u", v->pVertexAttributeDescriptions[a].location, v->pVertexAttributeDescriptions[a].binding, v->pVertexAttributeDescriptions[a].format, v->pVertexAttributeDescriptions[a].offset);
        } else trace("  vertexInput NULL");
        if (g->pInputAssemblyState) trace("  topology %d restart %d", g->pInputAssemblyState->topology, g->pInputAssemblyState->primitiveRestartEnable);
        if (g->pRasterizationState) { traceChain("  raster", g->pRasterizationState->pNext); trace("    cull %d front %d poly %d depthClamp %d discard %d lineWidth %f", g->pRasterizationState->cullMode, g->pRasterizationState->frontFace, g->pRasterizationState->polygonMode, g->pRasterizationState->depthClampEnable, g->pRasterizationState->rasterizerDiscardEnable, g->pRasterizationState->lineWidth); }
        if (g->pMultisampleState) trace("  samples %d", g->pMultisampleState->rasterizationSamples);
        if (g->pColorBlendState) trace("  blend attachments %u logicOp %d", g->pColorBlendState->attachmentCount, g->pColorBlendState->logicOpEnable);
        if (g->pDepthStencilState) trace("  depth test %d write %d", g->pDepthStencilState->depthTestEnable, g->pDepthStencilState->depthWriteEnable);
        if (g->pViewportState) trace("  viewports %u scissors %u", g->pViewportState->viewportCount, g->pViewportState->scissorCount);
        if (g->pDynamicState) { char b[512]; int n = 0; b[0] = 0; for (uint32_t d = 0; d < g->pDynamicState->dynamicStateCount && n < 480; d++) n += snprintf(b + n, sizeof(b) - n, " %d", g->pDynamicState->pDynamicStates[d]); trace("  dynamic:%s", b); }
        const VkPipelineRenderingCreateInfo* r = findInChain(g->pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
        if (r) trace("  rendering colors %u fmt0 %d depth %d stencil %d viewMask %u", r->colorAttachmentCount, r->colorAttachmentCount ? r->pColorAttachmentFormats[0] : 0, r->depthAttachmentFormat, r->stencilAttachmentFormat, r->viewMask);
    }
    VkResult result = vt_call_vkCreateGraphicsPipelines(device, cache, count, copy, alloc, pipelines);
    for (uint32_t i = 0; codes && i < used; i++) free(codes[i]);
    free(codes);
    free(moduleCopies);
    free(stageCopies);
    free(copy);
    return result;
}

static VkResult dd_vkCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* alloc, VkPipeline* pipelines) {
    VkComputePipelineCreateInfo* copy = malloc(count * sizeof(*copy));
    if (!copy) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(copy, infos, count * sizeof(*copy));
    for (uint32_t i = 0; i < count; i++) {
        const VkPipelineCreateFlags2CreateInfoKHR* f2 = findInChain(infos[i].pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR);
        if (f2) copy[i].flags = (VkPipelineCreateFlags)(f2->flags & 0xffffffffull);
    }
    VkResult result = vt_call_vkCreateComputePipelines(device, cache, count, copy, alloc, pipelines);
    free(copy);
    return result;
}

static VkResult dd_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* info, const VkAllocationCallbacks* alloc, VkBuffer* buffer) {
    const VkBufferUsageFlags2CreateInfoKHR* u2 = findInChain(info->pNext, VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR);
    if (!u2) return vt_call_vkCreateBuffer(device, info, alloc, buffer);
    VkBufferCreateInfo copy = *info;
    copy.usage = (VkBufferUsageFlags)(u2->usage & 0xffffffffull);
    return vt_call_vkCreateBuffer(device, &copy, alloc, buffer);
}

/* ---- maintenance5 commands ---- */

static void dd_vkCmdBindIndexBuffer2KHR(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, VkIndexType indexType) {
    (void)size; /* a range limit only; the legacy command binds to the end of the buffer */
    vt_call_vkCmdBindIndexBuffer(cmd, buffer, offset, indexType);
}

static void dd_vkGetRenderingAreaGranularityKHR(VkDevice device, const VkRenderingAreaInfoKHR* info, VkExtent2D* granularity) {
    (void)device; (void)info;
    granularity->width = 1;
    granularity->height = 1;
}

static void dd_vkGetImageSubresourceLayout2KHR(VkDevice device, VkImage image, const VkImageSubresource2KHR* sub, VkSubresourceLayout2KHR* layout) {
    vt_call_vkGetImageSubresourceLayout(device, image, &sub->imageSubresource, &layout->subresourceLayout);
}

static void dd_vkGetDeviceImageSubresourceLayoutKHR(VkDevice device, const VkDeviceImageSubresourceInfoKHR* info, VkSubresourceLayout2KHR* layout) {
    memset(&layout->subresourceLayout, 0, sizeof(layout->subresourceLayout));
    VkImage image = VK_NULL_HANDLE;
    if (vt_call_vkCreateImage(device, info->pCreateInfo, NULL, &image) != VK_SUCCESS) return;
    vt_call_vkGetImageSubresourceLayout(device, image, &info->pSubresource->imageSubresource, &layout->subresourceLayout);
    vt_call_vkDestroyImage(device, image, NULL);
}

/* ---- maintenance6 commands ---- */

#define DD_GRAPHICS_STAGES (VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT)

static void dd_vkCmdBindDescriptorSets2KHR(VkCommandBuffer cmd, const VkBindDescriptorSetsInfoKHR* info) {
    if (info->stageFlags & DD_GRAPHICS_STAGES)
        vt_call_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, info->firstSet, info->descriptorSetCount,
                                        info->pDescriptorSets, info->dynamicOffsetCount, info->pDynamicOffsets);
    if (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
        vt_call_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, info->layout, info->firstSet, info->descriptorSetCount,
                                        info->pDescriptorSets, info->dynamicOffsetCount, info->pDynamicOffsets);
}

static void dd_vkCmdPushConstants2KHR(VkCommandBuffer cmd, const VkPushConstantsInfoKHR* info) {
    vt_call_vkCmdPushConstants(cmd, info->layout, info->stageFlags, info->offset, info->size, info->pValues);
}

static void dd_vkCmdPushDescriptorSet2KHR(VkCommandBuffer cmd, const VkPushDescriptorSetInfoKHR* info) {
    if (info->stageFlags & DD_GRAPHICS_STAGES)
        vt_call_vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, info->set, info->descriptorWriteCount, info->pDescriptorWrites);
    if (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
        vt_call_vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, info->layout, info->set, info->descriptorWriteCount, info->pDescriptorWrites);
}

static void dd_vkCmdPushDescriptorSetWithTemplate2KHR(VkCommandBuffer cmd, const VkPushDescriptorSetWithTemplateInfoKHR* info) {
    vt_call_vkCmdPushDescriptorSetWithTemplateKHR(cmd, info->descriptorUpdateTemplate, info->layout, info->set, info->pData);
}

/* ---- traced pass-throughs (only differ from Vortek's own entry points by logging) ---- */

VkResult vt_call_vkEnumeratePhysicalDevices(VkInstance, uint32_t*, VkPhysicalDevice*);
void vt_call_vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties*);
void vt_call_vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*);
void vt_call_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties*);
VkResult vt_call_vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);

static VkResult dd_vkEnumeratePhysicalDevices(VkInstance i, uint32_t* c, VkPhysicalDevice* p) {
    VkResult r = vt_call_vkEnumeratePhysicalDevices(i, c, p);
    trace("EnumeratePhysicalDevices -> %d count %u", r, c ? *c : 0);
    return r;
}
static void dd_vkGetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties* p) {
    vt_call_vkGetPhysicalDeviceProperties(pd, p);
    trace("GetPhysicalDeviceProperties -> api %u.%u type %d maxBoundDescriptorSets %u", VK_API_VERSION_MAJOR(p->apiVersion), VK_API_VERSION_MINOR(p->apiVersion), p->deviceType, p->limits.maxBoundDescriptorSets);
}
static void dd_vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice pd, uint32_t* c, VkQueueFamilyProperties* p) {
    vt_call_vkGetPhysicalDeviceQueueFamilyProperties(pd, c, p);
    if (p) for (uint32_t i = 0; i < *c; i++) trace("QueueFamily %u flags 0x%x count %u", i, p[i].queueFlags, p[i].queueCount);
}
static void dd_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice pd, VkFormat f, VkFormatProperties* p) {
    vt_call_vkGetPhysicalDeviceFormatProperties(pd, f, p);
    trace("FormatProperties %d -> opt 0x%x", f, p->optimalTilingFeatures);
}
static VkResult dd_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char* layer, uint32_t* c, VkExtensionProperties* p) {
    quietLookups++;
    VkResult r = vt_call_vkEnumerateDeviceExtensionProperties(pd, layer, c, p);
    quietLookups--;
    trace("EnumerateDeviceExtensionProperties(%s) -> %d count %u", layer ? layer : "-", r, c ? *c : 0);
    if (p && r >= 0 && traceOn > 0) for (uint32_t i = 0; i < *c; i++) trace("  %s", p[i].extensionName);
    return r;
}
static VkResult dd_vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* d) {
    traceChain("CreateDevice", ci->pNext);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) trace("  ext %s", ci->ppEnabledExtensionNames[i]);
    VkResult r = vt_call_vkCreateDevice(pd, ci, a, d);
    trace("CreateDevice -> %d", r);
    return r;
}

VkResult vt_call_vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
VkResult vt_call_vkCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore*);

static VkResult dd_vkCreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo* ci, const VkAllocationCallbacks* a, VkSemaphore* s) {
    VkResult r = vt_call_vkCreateSemaphore(d, ci, a, s);
    const VkSemaphoreTypeCreateInfo* t = findInChain(ci->pNext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
    trace("CreateSemaphore %p %s initial %llu -> %d", (void*)*s, t && t->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE ? "timeline" : "binary",
          t ? (unsigned long long)t->initialValue : 0ull, r);
    return r;
}

static VkResult dd_vkQueueSubmit(VkQueue q, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    if (traceOn > 0) {
        for (uint32_t i = 0; i < count; i++) {
            const VkSubmitInfo* s = &submits[i];
            const VkTimelineSemaphoreSubmitInfo* t = findInChain(s->pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
            traceChain("QueueSubmit", s->pNext);
            trace("  submit %u: %u cmdbufs fence %p", i, s->commandBufferCount, (void*)fence);
            for (uint32_t w = 0; w < s->waitSemaphoreCount; w++)
                trace("    wait %p stage 0x%x value %llu", (void*)s->pWaitSemaphores[w], s->pWaitDstStageMask[w],
                      t && w < t->waitSemaphoreValueCount && t->pWaitSemaphoreValues ? (unsigned long long)t->pWaitSemaphoreValues[w] : 0ull);
            for (uint32_t g = 0; g < s->signalSemaphoreCount; g++)
                trace("    signal %p value %llu", (void*)s->pSignalSemaphores[g],
                      t && g < t->signalSemaphoreValueCount && t->pSignalSemaphoreValues ? (unsigned long long)t->pSignalSemaphoreValues[g] : 0ull);
        }
    }
    return vt_call_vkQueueSubmit(q, count, submits, fence);
}

/* ---- lookup, consulted before Vortek's own dispatch table ---- */

static const struct { const char* name; void* func; } ddOverrides[] = {
    {"vkGetPhysicalDeviceFeatures2", dd_vkGetPhysicalDeviceFeatures2},
    {"vkGetPhysicalDeviceFeatures2KHR", dd_vkGetPhysicalDeviceFeatures2},
    {"vkGetPhysicalDeviceProperties2", dd_vkGetPhysicalDeviceProperties2},
    {"vkGetPhysicalDeviceProperties2KHR", dd_vkGetPhysicalDeviceProperties2},
    {"vkCreateGraphicsPipelines", dd_vkCreateGraphicsPipelines},
    {"vkCreateShaderModule", dd_vkCreateShaderModule},
    {"vkCreateComputePipelines", dd_vkCreateComputePipelines},
    {"vkCreateBuffer", dd_vkCreateBuffer},
    {"vkEnumeratePhysicalDevices", dd_vkEnumeratePhysicalDevices},
    {"vkGetPhysicalDeviceProperties", dd_vkGetPhysicalDeviceProperties},
    {"vkGetPhysicalDeviceQueueFamilyProperties", dd_vkGetPhysicalDeviceQueueFamilyProperties},
    {"vkGetPhysicalDeviceFormatProperties", dd_vkGetPhysicalDeviceFormatProperties},
    {"vkEnumerateDeviceExtensionProperties", dd_vkEnumerateDeviceExtensionProperties},
    {"vkCreateDevice", dd_vkCreateDevice},
    {"vkCreateSemaphore", dd_vkCreateSemaphore},
    {"vkQueueSubmit", dd_vkQueueSubmit},
    {"vkCmdBindIndexBuffer2KHR", dd_vkCmdBindIndexBuffer2KHR},
    {"vkCmdBindIndexBuffer2", dd_vkCmdBindIndexBuffer2KHR},
    {"vkGetRenderingAreaGranularityKHR", dd_vkGetRenderingAreaGranularityKHR},
    {"vkGetRenderingAreaGranularity", dd_vkGetRenderingAreaGranularityKHR},
    {"vkGetImageSubresourceLayout2KHR", dd_vkGetImageSubresourceLayout2KHR},
    {"vkGetImageSubresourceLayout2EXT", dd_vkGetImageSubresourceLayout2KHR},
    {"vkGetImageSubresourceLayout2", dd_vkGetImageSubresourceLayout2KHR},
    {"vkGetDeviceImageSubresourceLayoutKHR", dd_vkGetDeviceImageSubresourceLayoutKHR},
    {"vkGetDeviceImageSubresourceLayout", dd_vkGetDeviceImageSubresourceLayoutKHR},
    {"vkCmdBindDescriptorSets2KHR", dd_vkCmdBindDescriptorSets2KHR},
    {"vkCmdBindDescriptorSets2", dd_vkCmdBindDescriptorSets2KHR},
    {"vkCmdPushConstants2KHR", dd_vkCmdPushConstants2KHR},
    {"vkCmdPushConstants2", dd_vkCmdPushConstants2KHR},
    {"vkCmdPushDescriptorSet2KHR", dd_vkCmdPushDescriptorSet2KHR},
    {"vkCmdPushDescriptorSet2", dd_vkCmdPushDescriptorSet2KHR},
    {"vkCmdPushDescriptorSetWithTemplate2KHR", dd_vkCmdPushDescriptorSetWithTemplate2KHR},
    {"vkCmdPushDescriptorSetWithTemplate2", dd_vkCmdPushDescriptorSetWithTemplate2KHR},
};

void* droiddeck_find_override(const char* name) {
    for (size_t i = 0; i < sizeof(ddOverrides) / sizeof(ddOverrides[0]); i++) {
        if (strcmp(name, ddOverrides[i].name) == 0) return ddOverrides[i].func;
    }
    return NULL;
}
