/* [mali-g2] The stream output counts (src/xfb_stats.h) and their wiring in the Vortek client
 * (src/droiddeck_maint.c), without a server: the client's vt_call_* functions are stubs here.
 *   1 xfb_spirv_declares on glslang output (xfb.vert declares Xfb; xfb_plain.vert and xfb.geom do not)
 *     and on damaged code;
 *   2 the counters through the header: modules that declare Xfb are remembered until destroyed,
 *     pipelines are classified by inline code and by module, failed (null) pipelines are not counted,
 *     the first pipeline logs at once, later lines wait for the interval and come only from every
 *     256th command or 64th present, a flush logs once and never repeats an unchanged line;
 *   3 the client: its overrides for vkCreateShaderModule / vkDestroyShaderModule /
 *     vkCreateGraphicsPipelines / vkCmdBeginTransformFeedbackEXT / vkCmdDrawIndirectByteCountEXT
 *     count and forward every argument, vkDestroyDevice logs "vortek-client: xfb stats: ...".
 * Prints XFB-STATS-PASS.
 *
 * Build (after ../build-client.sh, for its Vulkan headers in out/build-client/vkinc):
 *   cc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
 *      -I../out/build-client/vkinc -I../src -include ../src/droiddeck_defaults.h \
 *      -DXFB_LOG_INTERVAL_NS=200000000ull -o xfb_stats_test xfb_stats_test.c ../src/droiddeck_maint.c -lpthread
 * Shaders: xfb_stats_gen.sh -> xfb_stats_spv.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xfb_stats.h"
#include "xfb_stats_spv.h"

static int failures;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            printf("FAIL line %d: %s\n", __LINE__, #c);                       \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/* ---- stderr into a file, read back ---- */

static FILE* captured; /* its own open file description: reading does not move stderr's offset */
static int savedStderr = -1;

static void captureStart(void) {
    char path[] = "/tmp/xfb_stats_test.XXXXXX";
    int fd = mkstemp(path);
    captured = fopen(path, "r");
    unlink(path);
    fflush(stderr);
    savedStderr = dup(2);
    dup2(fd, 2);
    close(fd);
}

/* What was written since the last call. */
static char* captureRead(void) {
    fflush(stderr);
    size_t cap = 4096, n = 0;
    char* text = malloc(cap);
    size_t got;
    clearerr(captured);
    while ((got = fread(text + n, 1, cap - n - 1, captured)) > 0) {
        n += got;
        if (n + 1 == cap) text = realloc(text, cap *= 2);
    }
    text[n] = 0;
    return text;
}

static void captureStop(void) {
    fflush(stderr);
    dup2(savedStderr, 2);
    close(savedStderr);
}

static int countOf(const char* text, const char* needle) {
    int n = 0;
    for (const char* p = text; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}

/* The numbers of the last "<tag>: xfb stats:" line in text; false when there is none. */
static bool lastLine(const char* text, const char* tag, unsigned long long v[5]) {
    char needle[64];
    snprintf(needle, sizeof(needle), "%s: xfb stats: pid ", tag);
    const char* last = NULL;
    for (const char* p = text; (p = strstr(p, needle)); p++) last = p;
    if (!last) return false;
    int pid;
    return sscanf(last + strlen(needle), "%d, pipelines %llu, gs %llu, begins %llu, drawauto %llu, seconds %llu", &pid, &v[0], &v[1], &v[2],
                  &v[3], &v[4]) == 6 &&
           pid == getpid();
}

static void sleepMs(int ms) { usleep(ms * 1000); }

/* ---- 1: the SPIR-V scan ---- */

static void spirvScan(void) {
    CHECK(xfb_spirv_declares(xfb_vert_spv, sizeof(xfb_vert_spv) / 4));
    CHECK(!xfb_spirv_declares(xfb_plain_vert_spv, sizeof(xfb_plain_vert_spv) / 4));
    CHECK(!xfb_spirv_declares(xfb_geom_spv, sizeof(xfb_geom_spv) / 4));
    CHECK(!xfb_spirv_declares(NULL, 100));
    CHECK(!xfb_spirv_declares(xfb_vert_spv, 4));
    uint32_t copy[sizeof(xfb_vert_spv) / 4];
    memcpy(copy, xfb_vert_spv, sizeof(copy));
    copy[0] = 0x03022307; /* other byte order */
    CHECK(!xfb_spirv_declares(copy, sizeof(copy) / 4));
    memcpy(copy, xfb_vert_spv, sizeof(copy));
    copy[5] = 17 | (0u << 16); /* an instruction of no words */
    CHECK(!xfb_spirv_declares(copy, sizeof(copy) / 4));
    copy[5] = 17 | (4000u << 16); /* longer than the code */
    CHECK(!xfb_spirv_declares(copy, sizeof(copy) / 4));
    /* header, OpCapability Shader, OpExecutionMode %1 Xfb */
    const uint32_t minimal[] = {0x07230203, 0x00010000, 0, 10, 0, 17 | (2u << 16), 1, 16 | (3u << 16), 1, 11};
    CHECK(xfb_spirv_declares(minimal, 10));
    CHECK(!xfb_spirv_declares(minimal, 9)); /* cut inside the execution mode */
    /* the same after a type declaration: past the execution mode section, not looked at */
    const uint32_t late[] = {0x07230203, 0x00010000, 0, 10, 0, 19 | (2u << 16), 2, 16 | (3u << 16), 1, 11};
    CHECK(!xfb_spirv_declares(late, 10));
}

/* ---- 2: the counters ---- */

static VkShaderModuleCreateInfo moduleInfo(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo m = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    m.codeSize = bytes;
    m.pCode = code;
    return m;
}

static VkPipelineShaderStageCreateInfo stageOf(VkShaderStageFlagBits stage, VkShaderModule module, const void* next) {
    VkPipelineShaderStageCreateInfo s = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    s.pNext = next;
    s.stage = stage;
    s.module = module;
    s.pName = "main";
    return s;
}

static void counters(void) {
    unsigned long long v[5];
    xfb_stats_device_created();
    VkShaderModuleCreateInfo xfbCode = moduleInfo(xfb_vert_spv, sizeof(xfb_vert_spv));
    VkShaderModuleCreateInfo plainCode = moduleInfo(xfb_plain_vert_spv, sizeof(xfb_plain_vert_spv));
    VkShaderModuleCreateInfo geomCode = moduleInfo(xfb_geom_spv, sizeof(xfb_geom_spv));
    /* a chain with another structure ahead of the module */
    VkPipelineRobustnessCreateInfoEXT robust = {VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT};
    robust.pNext = &xfbCode;

    VkShaderModule modXfb = (VkShaderModule)(uintptr_t)0x1000, modPlain = (VkShaderModule)(uintptr_t)0x2000;
    xfb_stats_module_created((uint64_t)(uintptr_t)modXfb, &xfbCode);
    xfb_stats_module_created((uint64_t)(uintptr_t)modPlain, &plainCode);
    for (uintptr_t h = 0x3000; h < 0x3000 + 40; h++) xfb_stats_module_created(h, &xfbCode); /* the list grows */
    VkPipelineShaderStageCreateInfo byModule = stageOf(VK_SHADER_STAGE_VERTEX_BIT, modXfb, NULL);
    VkPipelineShaderStageCreateInfo byPlainModule = stageOf(VK_SHADER_STAGE_VERTEX_BIT, modPlain, NULL);
    CHECK(xfb_stage_declares(&byModule));
    CHECK(!xfb_stage_declares(&byPlainModule));
    for (uintptr_t h = 0x3000; h < 0x3000 + 40; h++) xfb_stats_module_destroyed(h);
    CHECK(xfb_stage_declares(&byModule));
    xfb_stats_module_destroyed((uint64_t)(uintptr_t)modXfb);
    CHECK(!xfb_stage_declares(&byModule));
    CHECK(__atomic_load_n(&xfbStats.moduleCount, __ATOMIC_RELAXED) == 0);
    xfb_stats_module_created((uint64_t)(uintptr_t)modXfb, &xfbCode);

    char* text = captureRead();
    CHECK(!strstr(text, "xfb stats")); /* modules alone log nothing */
    free(text);

    VkPipelineShaderStageCreateInfo p0[2] = {stageOf(VK_SHADER_STAGE_VERTEX_BIT, VK_NULL_HANDLE, &robust),
                                             stageOf(VK_SHADER_STAGE_FRAGMENT_BIT, modPlain, NULL)};
    VkPipelineShaderStageCreateInfo p1[2] = {stageOf(VK_SHADER_STAGE_VERTEX_BIT, VK_NULL_HANDLE, &plainCode),
                                             stageOf(VK_SHADER_STAGE_GEOMETRY_BIT, VK_NULL_HANDLE, &geomCode)};
    VkPipelineShaderStageCreateInfo p2[1] = {byModule};
    VkPipelineShaderStageCreateInfo p3[1] = {stageOf(VK_SHADER_STAGE_VERTEX_BIT, VK_NULL_HANDLE, &xfbCode)};
    VkGraphicsPipelineCreateInfo infos[5] = {{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}};
    for (int i = 1; i < 5; i++) infos[i] = infos[0];
    infos[0].stageCount = 2, infos[0].pStages = p0;
    infos[1].stageCount = 2, infos[1].pStages = p1;
    infos[2].stageCount = 1, infos[2].pStages = p2;
    infos[3].stageCount = 1, infos[3].pStages = p3; /* failed: VK_NULL_HANDLE */
    infos[4].stageCount = 0, infos[4].pStages = NULL; /* a link of libraries */
    VkPipeline pipelines[5] = {(VkPipeline)(uintptr_t)1, (VkPipeline)(uintptr_t)2, (VkPipeline)(uintptr_t)3, VK_NULL_HANDLE,
                               (VkPipeline)(uintptr_t)5};
    xfb_stats_pipelines_created("t", 5, infos, pipelines);
    text = captureRead();
    CHECK(countOf(text, "t: xfb stats:") == 1);
    CHECK(lastLine(text, "t", v) && v[0] == 2 && v[1] == 1 && v[2] == 0 && v[3] == 0);
    free(text);

    /* within the interval: counted, not logged */
    xfb_stats_pipelines_created("t", 1, &infos[2], &pipelines[2]);
    for (int i = 0; i < 300; i++) xfb_stats_begin("t");
    text = captureRead();
    CHECK(!strstr(text, "xfb stats"));
    free(text);
    sleepMs(250);
    /* past it: the 512th begin logs, the commands before it do not look */
    for (int i = 300; i < 511; i++) xfb_stats_begin("t");
    text = captureRead();
    CHECK(!strstr(text, "xfb stats"));
    free(text);
    xfb_stats_begin("t");
    text = captureRead();
    CHECK(countOf(text, "xfb stats:") == 1);
    CHECK(lastLine(text, "t", v) && v[0] == 3 && v[1] == 1 && v[2] == 512 && v[3] == 0);
    free(text);

    /* presents: nothing while nothing changed; the 64th present after the interval logs a change */
    sleepMs(250);
    for (int i = 0; i < 200; i++) xfb_stats_present("t");
    text = captureRead();
    CHECK(!strstr(text, "xfb stats"));
    free(text);
    xfb_stats_draw_auto("t");
    for (int i = 0; i < 64; i++) xfb_stats_present("t");
    text = captureRead();
    CHECK(countOf(text, "xfb stats:") == 1);
    CHECK(lastLine(text, "t", v) && v[2] == 512 && v[3] == 1);
    free(text);

    /* a flush logs the change at once, and only once */
    xfb_stats_begin("t");
    xfb_stats_flush("t");
    xfb_stats_flush("t");
    text = captureRead();
    CHECK(countOf(text, "xfb stats:") == 1);
    CHECK(lastLine(text, "t", v) && v[2] == 513);
    free(text);
}

/* ---- 3: the Vortek client, with stub vt_call_* ---- */

extern void* droiddeck_find_override(const char* name);

static int created, destroyed, begun, byteCount, pipelinesMade, devicesDestroyed;
static VkCommandBuffer lastCommandBuffer;
static uint32_t lastArgs[4];
static VkBuffer lastBuffer;
static VkDeviceSize lastOffset;

VkResult vt_call_vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* a, VkShaderModule* m) {
    *m = (VkShaderModule)(uintptr_t)(0x9000 + created++);
    return VK_SUCCESS;
}
void vt_call_vkDestroyShaderModule(VkDevice d, VkShaderModule m, const VkAllocationCallbacks* a) { destroyed++; }
VkResult vt_call_vkCreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t count, const VkGraphicsPipelineCreateInfo* infos,
                                           const VkAllocationCallbacks* a, VkPipeline* out) {
    for (uint32_t i = 0; i < count; i++) out[i] = (VkPipeline)(uintptr_t)(0x7000 + pipelinesMade++);
    return VK_SUCCESS;
}
void vt_call_vkCmdBeginTransformFeedbackEXT(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer* buffers,
                                            const VkDeviceSize* offsets) {
    begun++;
    lastCommandBuffer = cb;
    lastArgs[0] = first, lastArgs[1] = count;
    lastBuffer = buffers ? buffers[0] : VK_NULL_HANDLE;
    lastOffset = offsets ? offsets[0] : 0;
}
void vt_call_vkCmdDrawIndirectByteCountEXT(VkCommandBuffer cb, uint32_t instances, uint32_t firstInstance, VkBuffer counter,
                                           VkDeviceSize counterOffset, uint32_t offset, uint32_t stride) {
    byteCount++;
    lastCommandBuffer = cb;
    lastArgs[0] = instances, lastArgs[1] = firstInstance, lastArgs[2] = offset, lastArgs[3] = stride;
    lastBuffer = counter;
    lastOffset = counterOffset;
}
void vt_call_vkDestroyDevice(VkDevice d, const VkAllocationCallbacks* a) { devicesDestroyed++; }
VkResult vt_call_vkCreateDevice(VkPhysicalDevice p, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* d) {
    *d = (VkDevice)(uintptr_t)0x5000;
    return VK_SUCCESS;
}
/* The other commands droiddeck_maint.c forwards: not reached here. */
#define UNUSED_STUB(name) \
    void name(void) { abort(); }
UNUSED_STUB(vt_call_vkCmdBindDescriptorSets)
UNUSED_STUB(vt_call_vkCmdBindIndexBuffer)
UNUSED_STUB(vt_call_vkCmdPushConstants)
UNUSED_STUB(vt_call_vkCmdPushDescriptorSetKHR)
UNUSED_STUB(vt_call_vkCmdPushDescriptorSetWithTemplateKHR)
UNUSED_STUB(vt_call_vkCreateBuffer)
UNUSED_STUB(vt_call_vkCreateComputePipelines)
UNUSED_STUB(vt_call_vkCreateImage)
UNUSED_STUB(vt_call_vkCreateInstance)
UNUSED_STUB(vt_call_vkCreateSemaphore)
UNUSED_STUB(vt_call_vkDestroyImage)
UNUSED_STUB(vt_call_vkEnumerateDeviceExtensionProperties)
UNUSED_STUB(vt_call_vkEnumeratePhysicalDevices)
UNUSED_STUB(vt_call_vkGetImageSubresourceLayout)
UNUSED_STUB(vt_call_vkGetPhysicalDeviceFeatures2)
UNUSED_STUB(vt_call_vkGetPhysicalDeviceFormatProperties)
UNUSED_STUB(vt_call_vkGetPhysicalDeviceProperties)
UNUSED_STUB(vt_call_vkGetPhysicalDeviceProperties2)
UNUSED_STUB(vt_call_vkGetPhysicalDeviceQueueFamilyProperties)
UNUSED_STUB(vt_call_vkQueueSubmit)

static void client(void) {
    unsigned long long v[5];
    PFN_vkCreateDevice createDevice = (PFN_vkCreateDevice)droiddeck_find_override("vkCreateDevice");
    PFN_vkDestroyDevice destroyDevice = (PFN_vkDestroyDevice)droiddeck_find_override("vkDestroyDevice");
    PFN_vkCreateShaderModule createModule = (PFN_vkCreateShaderModule)droiddeck_find_override("vkCreateShaderModule");
    PFN_vkDestroyShaderModule destroyModule = (PFN_vkDestroyShaderModule)droiddeck_find_override("vkDestroyShaderModule");
    PFN_vkCreateGraphicsPipelines createPipelines = (PFN_vkCreateGraphicsPipelines)droiddeck_find_override("vkCreateGraphicsPipelines");
    PFN_vkCmdBeginTransformFeedbackEXT begin = (PFN_vkCmdBeginTransformFeedbackEXT)droiddeck_find_override("vkCmdBeginTransformFeedbackEXT");
    PFN_vkCmdDrawIndirectByteCountEXT drawAuto = (PFN_vkCmdDrawIndirectByteCountEXT)droiddeck_find_override("vkCmdDrawIndirectByteCountEXT");
    CHECK(createDevice && destroyDevice && createModule && destroyModule && createPipelines && begin && drawAuto);
    CHECK(!droiddeck_find_override("vkCmdEndTransformFeedbackEXT")); /* not wrapped: the driver's (Vortek's) own */
    if (!createDevice || !destroyDevice || !createModule || !destroyModule || !createPipelines || !begin || !drawAuto) return;

    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkDevice device;
    CHECK(createDevice((VkPhysicalDevice)(uintptr_t)0x4000, &dci, NULL, &device) == VK_SUCCESS);

    VkShaderModuleCreateInfo xfbCode = moduleInfo(xfb_vert_spv, sizeof(xfb_vert_spv));
    VkShaderModuleCreateInfo geomCode = moduleInfo(xfb_geom_spv, sizeof(xfb_geom_spv));
    VkShaderModule mod, gone;
    CHECK(createModule(device, &xfbCode, NULL, &mod) == VK_SUCCESS);
    CHECK(createModule(device, &xfbCode, NULL, &gone) == VK_SUCCESS);
    destroyModule(device, gone, NULL);
    CHECK(destroyed == 1);
    VkPipelineShaderStageCreateInfo s0[1] = {stageOf(VK_SHADER_STAGE_VERTEX_BIT, mod, NULL)};
    VkPipelineShaderStageCreateInfo s1[1] = {stageOf(VK_SHADER_STAGE_VERTEX_BIT, gone, NULL)};
    VkPipelineShaderStageCreateInfo s2[2] = {stageOf(VK_SHADER_STAGE_VERTEX_BIT, VK_NULL_HANDLE, &xfbCode),
                                             stageOf(VK_SHADER_STAGE_GEOMETRY_BIT, VK_NULL_HANDLE, &geomCode)};
    VkGraphicsPipelineCreateInfo infos[3] = {{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}};
    infos[1] = infos[2] = infos[0];
    infos[0].stageCount = 1, infos[0].pStages = s0;
    infos[1].stageCount = 1, infos[1].pStages = s1;
    infos[2].stageCount = 2, infos[2].pStages = s2;
    VkPipeline pipelines[3];
    CHECK(createPipelines(device, VK_NULL_HANDLE, 3, infos, NULL, pipelines) == VK_SUCCESS);
    CHECK(pipelinesMade == 3);
    char* text = captureRead();
    CHECK(lastLine(text, "vortek-client", v) && v[0] == 2 && v[1] == 1 && v[2] == 0);
    free(text);

    VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)0x6000;
    VkBuffer counter = (VkBuffer)(uintptr_t)0x6100;
    VkDeviceSize offset = 48;
    for (int i = 0; i < 10; i++) begin(cb, 0, 1, &counter, &offset);
    CHECK(begun == 10 && lastCommandBuffer == cb && lastArgs[0] == 0 && lastArgs[1] == 1 && lastBuffer == counter && lastOffset == 48);
    drawAuto(cb, 2, 3, counter, 64, 4, 16);
    CHECK(byteCount == 1 && lastArgs[0] == 2 && lastArgs[1] == 3 && lastArgs[2] == 4 && lastArgs[3] == 16 && lastBuffer == counter &&
          lastOffset == 64);
    destroyDevice(device, NULL);
    CHECK(devicesDestroyed == 1);
    text = captureRead();
    CHECK(countOf(text, "vortek-client: xfb stats:") == 1);
    CHECK(lastLine(text, "vortek-client", v) && v[0] == 2 && v[1] == 1 && v[2] == 10 && v[3] == 1);
    free(text);
}

int main(void) {
    spirvScan();
    captureStart();
    counters();
    client();
    captureStop();
    if (failures) {
        printf("XFB-STATS-FAIL (%d)\n", failures);
        return 1;
    }
    printf("XFB-STATS-PASS\n");
    return 0;
}
