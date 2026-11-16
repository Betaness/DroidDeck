/*
 * DROIDDECK_HYBRIS_LOAD_SKIP and DROIDDECK_HYBRIS_CMD_STATS through this ICD's entry points over the
 * fake driver (tests/fake_icd.c as libfake_mali_icd.so, DROIDDECK_HYBRIS_NEXT_ICD), no GPU. Each case
 * records one pass the way DXVK does on a tiler (the draw in a secondary command buffer, then
 * vkCmdBeginRendering and vkCmdExecuteCommands in the primary) on a device of its own, and checks
 * the load op the driver gets and the "load skip" line logged when the device goes: applied, or
 * kept for the expected reason. The switches are read once per process:
 *   load_skip_test all     DROIDDECK_HYBRIS_LOAD_SKIP=all and DROIDDECK_HYBRIS_CMD_STATS=1
 *   load_skip_test full    DROIDDECK_HYBRIS_LOAD_SKIP=full
 *   load_skip_test off     neither: nothing is wrapped
 * Before them the SPIR-V evaluator and the coverage test run on their own (spirv_eval.c,
 * load_skip.h, built into this test).
 *   sh tests/guards_test.sh        (builds and runs this with the ICD built from src/)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fake_icd.h"
#include "load_skip.h"
#include "load_skip_spv.h"
#include "spirv_eval.h"

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);

#define VK_STRUCTURE_TYPE_ATTACHMENT_FEEDBACK_LOOP_INFO_EXT_ ((VkStructureType)1000527001)
typedef struct {
    VkStructureType sType;
    const void* pNext;
    VkBool32 feedbackLoopEnable;
} FeedbackLoopInfo;

static int failures, checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);    \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

/* ---- the evaluator and the coverage test on their own ---- */

#define PROG(code) spv_parse(code, sizeof code)

static void setPosition(SpvVertexIn* v, float x, float y, float z, float w) {
    memset(v, 0, sizeof *v);
    v->location[0].kind = SPV_ATTR_FLOAT;
    const float p[4] = {x, y, z, w};
    memcpy(v->location[0].value, p, sizeof p);
    v->location[1].kind = SPV_ATTR_FLOAT;
}

static void evaluatorTests(void) {
    SpvProgram* plain = PROG(ls_fs_plain);
    SpvProgram* discard = PROG(ls_fs_discard);
    SpvProgram* alpha = PROG(ls_fs_alpha);
    SpvProgram* mask = PROG(ls_fs_mask);
    CHECK(plain && discard && alpha && mask, "fragment shaders parse");
    CHECK(spv_fs_may_discard(plain, "main", NULL) == SPV_DISCARD_NO, "plain: no discard");
    CHECK(spv_fs_may_discard(plain, "other", NULL) == SPV_DISCARD_UNKNOWN, "no such entry point: unknown");
    CHECK(spv_fs_may_discard(discard, "main", NULL) == SPV_DISCARD_MAY, "discard: may");
    CHECK(spv_fs_may_discard(mask, "main", NULL) == SPV_DISCARD_MAY, "SampleMask written: may");
    CHECK(spv_fs_may_discard(alpha, "main", NULL) == SPV_DISCARD_NO, "alpha test at its default (always): no");
    uint32_t op;
    VkSpecializationMapEntry entry = {0, 0, 4};
    VkSpecializationInfo spec = {1, &entry, 4, &op};
    for (op = 0; op < 8; op++)
        CHECK(spv_fs_may_discard(alpha, "main", &spec) == (op == 7 ? SPV_DISCARD_NO : SPV_DISCARD_MAY), "alpha compare op %u", op);
    SpvProgram* cs = PROG(ls_cs_write);
    CHECK(cs && spv_writes_memory(cs) && !spv_writes_memory(plain), "memory writes: the compute shader's");

    SpvProgram* pass = PROG(ls_vs_pass);
    SpvVertexIn in[3];
    SpvVertexOut out[3];
    setPosition(&in[0], -1, -1, 0, 1);
    setPosition(&in[1], 3, -1, 0.5f, 1);
    setPosition(&in[2], -1, 3, 1, 2);
    CHECK(pass && spv_vs_run(pass, "main", NULL, in, 3, out) == SPV_RUN_OK, "pass-through runs");
    CHECK(out[1].positionKnown && out[1].position[0] == 3 && out[1].position[2] == 0.5f && out[2].position[3] == 2, "pass-through position");
    uint8_t kinds[SPV_LOCATIONS];
    spv_vs_inputs(pass, "main", kinds);
    CHECK(kinds[0] == SPV_ATTR_FLOAT && kinds[1] == SPV_ATTR_FLOAT && kinds[2] == SPV_ATTR_NONE, "inputs");
    in[0].location[0].kind = SPV_ATTR_SINT;
    CHECK(spv_vs_run(pass, "main", NULL, in, 1, out) == SPV_RUN_OK && !out[0].positionKnown, "an integer attribute for a float input: unknown");

    SpvProgram* tri = PROG(ls_vs_tri);
    for (int i = 0; i < 3; i++) memset(&in[i], 0, sizeof in[i]), in[i].vertexIndex = (uint32_t)i;
    CHECK(tri && spv_vs_run(tri, "main", NULL, in, 3, out) == SPV_RUN_OK, "vertex index triangle runs");
    CHECK(out[0].position[0] == -1 && out[0].position[1] == -1 && out[1].position[1] == 3 && out[2].position[0] == 3 && out[2].position[3] == 1,
          "vertex index triangle: (-1,-1) (-1,3) (3,-1)");

    SpvProgram* ubo = PROG(ls_vs_ubo);
    setPosition(&in[0], 0, 0, 0, 1);
    CHECK(ubo && spv_vs_run(ubo, "main", NULL, in, 1, out) == SPV_RUN_OK && !out[0].positionKnown, "uniform offset: unknown");
    SpvProgram* clip = PROG(ls_vs_clip);
    setPosition(&in[0], -0.5f, 0, 0, 1);
    CHECK(clip && spv_vs_run(clip, "main", NULL, in, 1, out) == SPV_RUN_OK && out[0].clipCount == 1 && out[0].clipState[0] == 1 &&
              out[0].clip[0] == -0.5f,
          "clip distance");
    SpvProgram* scaled = PROG(ls_vs_spec);
    float half = 0.5f;
    VkSpecializationMapEntry scaleEntry = {3, 0, 4};
    VkSpecializationInfo scale = {1, &scaleEntry, 4, &half};
    setPosition(&in[0], -1, 1, 0, 1);
    CHECK(scaled && spv_vs_run(scaled, "main", &scale, in, 1, out) == SPV_RUN_OK && out[0].position[0] == -0.5f && out[0].position[1] == 0.5f,
          "specialized scale");
    CHECK(spv_vs_run(scaled, "main", NULL, in, 1, out) == SPV_RUN_OK && out[0].position[0] == -1, "default scale");
    CHECK(!spv_parse(ls_vs_pass, 16) && !spv_parse((const uint32_t*)"not spir-v at all", 16), "not SPIR-V");
    CHECK(spv_code_writes_memory(ls_cs_write, sizeof ls_cs_write) && !spv_code_writes_memory(ls_fs_plain, sizeof ls_fs_plain) &&
              spv_code_writes_memory((const uint32_t*)"not spir-v at all", 16),
          "memory writes without a parse");
    /* an instruction the parser does not know, ahead of the functions: no parse, the scan still answers */
    uint32_t odd[sizeof ls_fs_plain / 4 + 1];
    size_t at = 5;
    while (at < sizeof ls_fs_plain / 4 && (ls_fs_plain[at] & 0xffff) != 54 /* OpFunction */) at += ls_fs_plain[at] >> 16;
    memcpy(odd, ls_fs_plain, at * 4);
    odd[at] = (1u << 16) | 0x7ffe;
    memcpy(odd + at + 1, ls_fs_plain + at, sizeof ls_fs_plain - at * 4);
    CHECK(!spv_parse(odd, sizeof odd) && !spv_code_writes_memory(odd, sizeof odd), "unknown instruction: no parse, no writes");

    /* coverage */
    const VkRect2D area = {{0, 0}, {64, 32}};
    const VkViewport vp = {0, 0, 64, 32, 0, 1};
    const float quad[4][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}, {-1, 1, 0, 1}};
    const uint32_t two[2][3] = {{0, 1, 2}, {0, 2, 3}};
    CHECK(ls_cover(quad, 4, two, 2, &vp, area, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, false) == LS_COVER_YES, "quad");
    CHECK(ls_cover(quad, 4, two, 1, &vp, area, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, false) == LS_COVER_NO, "half a quad");
    CHECK(ls_cover(quad, 4, two, 2, &vp, area, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, false) == LS_COVER_NO, "quad culled");
    CHECK(ls_cover(quad, 4, two, 2, &vp, area, VK_CULL_MODE_FRONT_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, false) == LS_COVER_YES,
          "quad, the other face culled");
    const float big[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    const uint32_t one[1][3] = {{0, 1, 2}};
    CHECK(ls_cover(big, 3, one, 1, &vp, area, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, false) == LS_COVER_YES, "big triangle");
    const float behind[3][4] = {{-1, -1, -0.5f, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    CHECK(ls_cover(behind, 3, one, 1, &vp, area, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, false) == LS_COVER_CLIP, "near plane");
    CHECK(ls_cover(behind, 3, one, 1, &vp, area, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, true) == LS_COVER_YES, "-w <= z");
    const VkViewport dxvk = {0.5f, 32.5f, 64, -32, 0, 1};
    CHECK(ls_viewport_covers(&dxvk, area), "DXVK's half-pixel viewport covers the centres");
    CHECK(ls_cover(quad, 4, two, 2, &dxvk, area, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, false) == LS_COVER_EDGE,
          "a quad on the first column's centres");
    const VkViewport narrow = {0.75f, 0, 63, 32, 0, 1};
    CHECK(!ls_viewport_covers(&narrow, area), "a viewport past the first centre");
    const VkRect2D scissor = {{0, 0}, {63, 32}};
    CHECK(!ls_scissor_covers(&scissor, area) && ls_scissor_covers(&area, area), "scissor");
    SpvAttr a;
    const uint8_t bytes[4] = {255, 0, 128, 255};
    CHECK(ls_decode(VK_FORMAT_B8G8R8A8_UNORM, bytes, &a) == 4 && a.value[2] == ls_fbits(1.0f) && a.value[0] == ls_fbits(128 / 255.0f), "BGRA");
    const uint16_t h[2] = {0x3c00, 0xc000};
    CHECK(ls_decode(VK_FORMAT_R16G16_SFLOAT, (const uint8_t*)h, &a) == 4 && a.value[0] == ls_fbits(1.0f) && a.value[1] == ls_fbits(-2.0f) &&
              a.value[3] == ls_fbits(1.0f),
          "half floats, w 1");
    spv_unref(plain), spv_unref(discard), spv_unref(alpha), spv_unref(mask), spv_unref(cs), spv_unref(pass), spv_unref(tri);
    spv_unref(ubo), spv_unref(clip), spv_unref(scaled);
}

/* ---- through the ICD ---- */

static FakeIcd* fk;
static PFN_vkVoidFunction (*fakeProc)(const char* name);
static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static PFN_vkGetDeviceProcAddr gdpa;

#define INST(name) ((PFN_vk##name)vk_icdGetInstanceProcAddr(instance, "vk" #name))
#define DEV(name) ((PFN_vk##name)gdpa(device, "vk" #name))
#define FAKE(name) fakeProc("vk" #name)

static void setUp(void) {
    void* lib = dlopen(getenv("DROIDDECK_HYBRIS_NEXT_ICD"), RTLD_NOW | RTLD_LOCAL);
    fk = lib ? dlsym(lib, "fake_icd") : NULL;
    fakeProc = lib ? (PFN_vkVoidFunction(*)(const char*))dlsym(lib, "fake_icd_device_proc") : NULL;
    if (!fk || !fakeProc) {
        fprintf(stderr, "FAIL: no fake driver (DROIDDECK_HYBRIS_NEXT_ICD): %s\n", dlerror());
        exit(1);
    }
    VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    CHECK(createInstance && createInstance(&instanceInfo, NULL, &instance) == VK_SUCCESS, "instance");
    uint32_t count = 1;
    CHECK(INST(EnumeratePhysicalDevices)(instance, &count, &physical) == VK_SUCCESS && physical, "physical device");
    gdpa = (PFN_vkGetDeviceProcAddr)vk_icdGetInstanceProcAddr(instance, "vkGetDeviceProcAddr");
}

static void openDevice(void) {
    float priority = 0;
    VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    CHECK(INST(CreateDevice)(physical, &deviceInfo, NULL, &device) == VK_SUCCESS, "device");
}

/* the stderr of fn() */
static void captured(void (*fn)(void* ctx), void* ctx, char* out, size_t size) {
    char path[] = "/tmp/load_skip_test.XXXXXX";
    int fd = mkstemp(path);
    fflush(stderr);
    int saved = dup(2);
    dup2(fd, 2);
    fn(ctx);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, out, size - 1);
    out[n > 0 ? n : 0] = 0;
    close(fd);
    unlink(path);
}

#define W 64
#define H 32

typedef struct {
    const char* name;
    const char* reason; /* NULL: applied */
    const uint32_t* vs;
    size_t vsSize;
    const uint32_t* fs;
    size_t fsSize;
    const VkSpecializationInfo *vsSpec, *fsSpec;
    float quad[4];          /* x0 y0 x1 y1 (NDC); all 0: [-1, 1] */
    bool dxvkViewport;      /* x + 0.5, y + h + 0.5, height -h */
    bool triangle;          /* ls_vs_tri: vkCmdDraw(3), no vertex buffers */
    uint32_t width, height; /* of the image; 0: W x H */
    int colors;
    bool depth, feedback, renderingNext, inlineDraw, orderBreak;
    VkRenderingFlags flags;
    uint32_t layers;
    VkExtent2D area;
    bool reusable, twoDraws, barrier, clear;
    bool pipelineNext, blend, logicOp, a2c, library, dynStride, dynCull, dynUntracked, withCount;
    VkColorComponentFlags mask;
    VkCullModeFlags cull;
    float viewportScale, scissorScale;
    bool unmapped, gpuCopy, computeWrite, stale;
    bool twice; /* two passes, the second from the pipeline's cached vertex shader run */
} Case;

static VkShaderModule module(const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size;
    info.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    CHECK(DEV(CreateShaderModule)(device, &info, NULL, &m) == VK_SUCCESS, "module");
    return m;
}

static const VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

static VkPipeline pipeline(const Case* k, VkShaderModule vs, VkShaderModule fs) {
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = vs, stages[0].pName = "main", stages[0].pSpecializationInfo = k->vsSpec;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = fs, stages[1].pName = "main", stages[1].pSpecializationInfo = k->fsSpec;
    VkVertexInputBindingDescription binding = {0, 24, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 16}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (!k->triangle) {
        vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 2, vi.pVertexAttributeDescriptions = attrs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = k->withCount ? 0 : 1, vp.scissorCount = k->withCount ? 0 : 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = k->cull, rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT, ms.alphaToCoverageEnable = k->a2c;
    VkPipelineColorBlendAttachmentState att = {0};
    att.blendEnable = k->blend, att.colorWriteMask = k->mask ? k->mask : 0xf;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.logicOpEnable = k->logicOp, cb.attachmentCount = 1, cb.pAttachments = &att;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkDynamicState dyn[8];
    uint32_t dynCount = 0;
    dyn[dynCount++] = k->withCount ? VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT : VK_DYNAMIC_STATE_VIEWPORT;
    dyn[dynCount++] = k->withCount ? VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT : VK_DYNAMIC_STATE_SCISSOR;
    dyn[dynCount++] = VK_DYNAMIC_STATE_DEPTH_BIAS;
    if (k->dynCull) dyn[dynCount++] = VK_DYNAMIC_STATE_CULL_MODE;
    if (k->dynUntracked) dyn[dynCount++] = VK_DYNAMIC_STATE_DISCARD_RECTANGLE_EXT;
    if (k->dynStride) dyn[dynCount++] = VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = dynCount, dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1, ri.pColorAttachmentFormats = &colorFormat;
    VkPipelineRepresentativeFragmentTestStateCreateInfoNV rep = {VK_STRUCTURE_TYPE_PIPELINE_REPRESENTATIVE_FRAGMENT_TEST_STATE_CREATE_INFO_NV};
    rep.representativeFragmentTestEnable = VK_TRUE;
    if (k->pipelineNext) ri.pNext = &rep;
    VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &ri;
    info.stageCount = 2, info.pStages = stages;
    info.pVertexInputState = &vi, info.pInputAssemblyState = &ia, info.pViewportState = &vp, info.pRasterizationState = &rs;
    info.pMultisampleState = &ms, info.pDepthStencilState = &ds, info.pColorBlendState = &cb, info.pDynamicState = &dy;
    VkPipeline out = VK_NULL_HANDLE;
    if (!k->library) {
        CHECK(DEV(CreateGraphicsPipelines)(device, VK_NULL_HANDLE, 1, &info, NULL, &out) == VK_SUCCESS, "pipeline");
        return out;
    }
    /* four libraries, then linked; each carries only its part's state */
    static const VkGraphicsPipelineLibraryFlagsEXT parts[4] = {
        VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
    VkPipeline libs[4];
    for (int i = 0; i < 4; i++) {
        VkGraphicsPipelineLibraryCreateInfoEXT gpl = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, &ri, parts[i]};
        VkGraphicsPipelineCreateInfo part = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        part.pNext = &gpl;
        part.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
        part.pDynamicState = &dy;
        if (i == 0) part.pVertexInputState = &vi, part.pInputAssemblyState = &ia;
        if (i == 1) part.stageCount = 1, part.pStages = &stages[0], part.pViewportState = &vp, part.pRasterizationState = &rs;
        if (i == 2) part.stageCount = 1, part.pStages = &stages[1], part.pDepthStencilState = &ds, part.pMultisampleState = &ms;
        if (i == 3) part.pColorBlendState = &cb, part.pMultisampleState = &ms;
        CHECK(DEV(CreateGraphicsPipelines)(device, VK_NULL_HANDLE, 1, &part, NULL, &libs[i]) == VK_SUCCESS, "library %d", i);
    }
    VkPipelineLibraryCreateInfoKHR link = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, NULL, 4, libs};
    VkGraphicsPipelineCreateInfo linked = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    linked.pNext = &link;
    CHECK(DEV(CreateGraphicsPipelines)(device, VK_NULL_HANDLE, 1, &linked, NULL, &out) == VK_SUCCESS, "linked pipeline");
    for (int i = 0; i < 4; i++) DEV(DestroyPipeline)(device, libs[i], NULL);
    return out;
}

typedef struct {
    const Case* k;
    VkAttachmentLoadOp load;
    char trace[64];
} Run;

static void recordDraw(const Case* k, VkCommandBuffer cb, VkPipeline pipe, VkBuffer vb, VkBuffer ib) {
    uint32_t w = k->width ? k->width : W, h = k->height ? k->height : H;
    DEV(CmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    float vs = k->viewportScale ? k->viewportScale : 1, ss = k->scissorScale ? k->scissorScale : 1;
    VkViewport vp = {0, 0, w * vs, h, 0, 1};
    if (k->dxvkViewport) vp = (VkViewport){0.5f, h + 0.5f, w * vs, -(float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {(uint32_t)(w * ss), h}};
    if (k->withCount) {
        DEV(CmdSetViewportWithCount)(cb, 1, &vp);
        DEV(CmdSetScissorWithCount)(cb, 1, &sc);
    } else {
        DEV(CmdSetViewport)(cb, 0, 1, &vp);
        DEV(CmdSetScissor)(cb, 0, 1, &sc);
    }
    if (k->triangle) {
        DEV(CmdDraw)(cb, 3, 1, 0, 0);
    } else {
        VkDeviceSize zero = 0, stride = 24;
        if (k->dynStride) DEV(CmdBindVertexBuffers2)(cb, 0, 1, &vb, &zero, NULL, &stride);
        else DEV(CmdBindVertexBuffers)(cb, 0, 1, &vb, &zero);
        DEV(CmdBindIndexBuffer)(cb, ib, 0, VK_INDEX_TYPE_UINT16);
        DEV(CmdDrawIndexed)(cb, 6, 1, 0, 0, 0);
    }
    if (k->twoDraws) DEV(CmdDraw)(cb, 3, 1, 0, 0);
    if (k->barrier) {
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        DEV(CmdPipelineBarrier2)(cb, &dep);
    }
    if (k->clear) {
        VkClearAttachment clear = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {{{0}}}};
        VkClearRect rect = {{{0, 0}, {w, h}}, 0, 1};
        DEV(CmdClearAttachments)(cb, 1, &clear, 1, &rect);
    }
}

static void runCase(void* ctx) {
    Run* run = ctx;
    const Case* k = run->k;
    uint32_t w = k->width ? k->width : W, h = k->height ? k->height : H;
    openDevice();
    VkQueue queue = VK_NULL_HANDLE;
    DEV(GetDeviceQueue)(device, 0, 0, &queue);
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D, ii.format = colorFormat, ii.extent = (VkExtent3D){w, h, 1}, ii.mipLevels = 1, ii.arrayLayers = 2;
    ii.samples = VK_SAMPLE_COUNT_1_BIT, ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage image, depthImage;
    CHECK(DEV(CreateImage)(device, &ii, NULL, &image) == VK_SUCCESS, "image");
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, vi.format = colorFormat;
    vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    VkImageView view, view2, depthView;
    CHECK(DEV(CreateImageView)(device, &vi, NULL, &view) == VK_SUCCESS, "view");
    CHECK(DEV(CreateImageView)(device, &vi, NULL, &view2) == VK_SUCCESS, "view");
    ii.format = VK_FORMAT_D24_UNORM_S8_UINT, ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    CHECK(DEV(CreateImage)(device, &ii, NULL, &depthImage) == VK_SUCCESS, "depth image");
    vi.image = depthImage, vi.format = ii.format, vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    CHECK(DEV(CreateImageView)(device, &vi, NULL, &depthView) == VK_SUCCESS, "depth view");

    /* vertex and index data in one host-mapped allocation */
    VkMemoryAllocateInfo mi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, 4096, 1};
    VkDeviceMemory memory;
    void* ptr = NULL;
    CHECK(DEV(AllocateMemory)(device, &mi, NULL, &memory) == VK_SUCCESS && DEV(MapMemory)(device, memory, 0, VK_WHOLE_SIZE, 0, &ptr) == VK_SUCCESS,
          "memory");
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = 1024, bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer vb, ib;
    CHECK(DEV(CreateBuffer)(device, &bi, NULL, &vb) == VK_SUCCESS && DEV(CreateBuffer)(device, &bi, NULL, &ib) == VK_SUCCESS, "buffers");
    CHECK(DEV(BindBufferMemory)(device, vb, memory, 0) == VK_SUCCESS && DEV(BindBufferMemory)(device, ib, memory, 1024) == VK_SUCCESS, "bound");
    float x0 = -1, y0 = -1, x1 = 1, y1 = 1;
    if (k->quad[0] || k->quad[1] || k->quad[2] || k->quad[3]) x0 = k->quad[0], y0 = k->quad[1], x1 = k->quad[2], y1 = k->quad[3];
    const float vertices[4][6] = {{x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0}, {x1, y1, 0, 1, 1, 1}, {x0, y1, 0, 1, 0, 1}};
    const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    memcpy(ptr, vertices, sizeof vertices);
    memcpy((char*)ptr + 1024, indices, sizeof indices);
    if (k->unmapped) DEV(UnmapMemory)(device, memory);

    VkShaderModule vs = module(k->vs ? k->vs : k->triangle ? ls_vs_tri : ls_vs_pass,
                               k->vs ? k->vsSize : k->triangle ? sizeof ls_vs_tri : sizeof ls_vs_pass);
    VkShaderModule fs = module(k->fs ? k->fs : ls_fs_plain, k->fs ? k->fsSize : sizeof ls_fs_plain);
    VkPipeline pipe = pipeline(k, vs, fs);

    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool;
    CHECK(DEV(CreateCommandPool)(device, &pi, NULL, &pool) == VK_SUCCESS, "pool");
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
    VkCommandBuffer primary[2], secondaries[2];
    CHECK(DEV(AllocateCommandBuffers)(device, &ai, primary) == VK_SUCCESS, "primaries");
    ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    CHECK(DEV(AllocateCommandBuffers)(device, &ai, secondaries) == VK_SUCCESS, "secondaries");

    /* another command buffer, recorded and not reset: what it writes stays written by the GPU */
    VkCommandBufferBeginInfo once = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (k->gpuCopy || k->computeWrite) {
        DEV(BeginCommandBuffer)(primary[1], &once);
        if (k->gpuCopy) {
            VkBufferCopy region = {0, 0, 96};
            DEV(CmdCopyBuffer)(primary[1], ib, vb, 1, &region);
        } else {
            VkShaderModule cs = module(ls_cs_write, sizeof ls_cs_write);
            VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs,
                                                         "main", NULL};
            VkPipeline cp;
            CHECK(DEV(CreateComputePipelines)(device, VK_NULL_HANDLE, 1, &ci, NULL, &cp) == VK_SUCCESS, "compute pipeline");
            DEV(CmdBindPipeline)(primary[1], VK_PIPELINE_BIND_POINT_COMPUTE, cp);
            DEV(CmdDispatch)(primary[1], 1, 1, 1);
        }
        DEV(EndCommandBuffer)(primary[1]);
    }

    VkCommandBufferInheritanceRenderingInfo inheritRendering = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
    inheritRendering.colorAttachmentCount = 1, inheritRendering.pColorAttachmentFormats = &colorFormat;
    inheritRendering.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkCommandBufferInheritanceInfo inherit = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, &inheritRendering};
    VkCommandBufferBeginInfo sb = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    sb.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | (k->reusable ? 0 : VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    sb.pInheritanceInfo = &inherit;
    for (int i = 0; !k->inlineDraw && i < (k->twice ? 2 : 1); i++) {
        DEV(BeginCommandBuffer)(secondaries[i], &sb);
        recordDraw(k, secondaries[i], pipe, vb, ib);
        DEV(EndCommandBuffer)(secondaries[i]);
    }

    DEV(BeginCommandBuffer)(primary[0], &once);
    VkRenderingAttachmentInfo colors[2] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
    FeedbackLoopInfo feedback = {VK_STRUCTURE_TYPE_ATTACHMENT_FEEDBACK_LOOP_INFO_EXT_, NULL, k->feedback};
    for (int i = 0; i < 2; i++) {
        colors[i].imageView = i ? view2 : view, colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    colors[0].pNext = &feedback;
    VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = depthView, depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkDeviceGroupRenderPassBeginInfo group = {VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO, NULL, 1};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.pNext = k->renderingNext ? &group : NULL;
    ri.flags = (k->inlineDraw ? 0 : VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) | k->flags;
    ri.renderArea.extent = k->area.width ? k->area : (VkExtent2D){w, h};
    ri.layerCount = k->layers ? k->layers : 1;
    ri.colorAttachmentCount = k->colors ? (uint32_t)k->colors : 1;
    ri.pColorAttachments = colors;
    ri.pDepthAttachment = k->depth ? &depth : NULL;
    fk->traceLen = 0;
    fk->trace[0] = 0;
    fk->lastColorLoad = VK_ATTACHMENT_LOAD_OP_MAX_ENUM;
    for (int i = 0; i < (k->twice ? 2 : 1); i++) {
        DEV(CmdBeginRendering)(primary[0], &ri);
        if (k->orderBreak) {
            VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
            DEV(CmdSetViewport)(primary[0], 0, 1, &vp);
        }
        if (k->inlineDraw) recordDraw(k, primary[0], pipe, vb, ib);
        else DEV(CmdExecuteCommands)(primary[0], 1, &secondaries[i]);
        DEV(CmdEndRendering)(primary[0]);
        if (i == 0) run->load = fk->lastColorLoad;
    }
    DEV(EndCommandBuffer)(primary[0]);
    if (run->load != fk->lastColorLoad) run->load = VK_ATTACHMENT_LOAD_OP_MAX_ENUM;
    strcpy(run->trace, fk->trace);
    if (k->stale) {
        float moved = 0.5f;
        memcpy(ptr, &moved, sizeof moved);
    }
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1, submit.pCommandBuffers = &primary[0];
    CHECK(DEV(QueueSubmit)(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS, "submit");

    DEV(DestroyPipeline)(device, pipe, NULL);
    DEV(DestroyShaderModule)(device, vs, NULL);
    DEV(DestroyShaderModule)(device, fs, NULL);
    DEV(DestroyCommandPool)(device, pool, NULL);
    DEV(DestroyBuffer)(device, vb, NULL);
    DEV(DestroyBuffer)(device, ib, NULL);
    DEV(FreeMemory)(device, memory, NULL);
    DEV(DestroyImageView)(device, view, NULL);
    DEV(DestroyImageView)(device, view2, NULL);
    DEV(DestroyImageView)(device, depthView, NULL);
    DEV(DestroyImage)(device, image, NULL);
    DEV(DestroyImage)(device, depthImage, NULL);
    DEV(DestroyDevice)(device, NULL);
}

static void check(const Case* k, bool skipOn) {
    Run run = {k};
    char log[16384];
    captured(runCase, &run, log, sizeof log);
    const char* line = strstr(log, "hybris-icd: load skip: ");
    char want[128];
    bool applied = skipOn && !k->reason;
    CHECK(run.load == (applied ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD), "%s: load op %d", k->name, run.load);
    CHECK(!strcmp(run.trace, k->inlineDraw ? "BDE" : k->twice ? "BXEBXE" : "BXE"), "%s: order %s", k->name, run.trace);
    if (!skipOn) {
        CHECK(!line, "%s: no load skip line when off", k->name);
        return;
    }
    int passes = k->twice ? 2 : 1;
    snprintf(want, sizeof want, ", passes %d,", passes);
    CHECK(line && strstr(line, want), "%s: passes counted: %s", k->name, line ? line : log);
    if (applied) snprintf(want, sizeof want, ", applied %d,", passes);
    else snprintf(want, sizeof want, ", %s %d", k->reason, passes);
    CHECK(line && strstr(line, want), "%s: \"%s\" in %s", k->name, want, line ? line : log);
    CHECK(!line || !strstr(line, k->stale ? ", stale 0" : ", stale 1"), "%s: stale count: %s", k->name, line ? line : "");
    if (getenv("LOAD_SKIP_TEST_VERBOSE")) fprintf(stderr, "%s: %.*s\n", k->name, (int)strcspn(line ? line : log, "\n"), line ? line : log);
}

#define VS(code) .vs = code, .vsSize = sizeof code
#define FS(code) .fs = code, .fsSize = sizeof code

static const float halfScale = 0.5f;
static const VkSpecializationMapEntry scaleEntry = {3, 0, 4};
static const VkSpecializationInfo scaleSpec = {1, &scaleEntry, 4, &halfScale};
static const uint32_t alphaLess = 3;
static const VkSpecializationMapEntry alphaEntry = {0, 0, 4};
static const VkSpecializationInfo alphaSpec = {1, &alphaEntry, 4, &alphaLess};
static const uint32_t alphaAlways = 0x77;
static const VkSpecializationInfo alphaAlwaysSpec = {1, &alphaEntry, 4, &alphaAlways};

/* UE3's full-screen quad, half a pixel up and left in clip space, under DXVK's viewport */
#define HALF_PIXEL_QUAD {-1 - 1.0f / W, -1 + 1.0f / H, 1 - 1.0f / W, 1 + 1.0f / H}

static const Case allCases[] = {
    {"quad", NULL},
    {"triangle from the vertex index", NULL, .triangle = true},
    {"dxvk half-pixel quad, viewport with count", NULL, .quad = HALF_PIXEL_QUAD, .dxvkViewport = true, .withCount = true},
    {"alpha test always", NULL, FS(ls_fs_alpha), .fsSpec = &alphaAlwaysSpec},
    {"linked libraries", NULL, .library = true},
    {"dynamic stride", NULL, .dynStride = true},
    {"other face culled", NULL, .cull = VK_CULL_MODE_FRONT_BIT},
    {"two passes", NULL, .twice = true},
    {"two passes kept", "geometry", .quad = {-1, -1, 0, 1}, .twice = true},
    {"two colour attachments", "color-count", .colors = 2},
    {"resuming", "flags", .flags = VK_RENDERING_RESUMING_BIT},
    {"inline draw", "inline", .inlineDraw = true},
    {"rendering pNext", "pnext", .renderingNext = true},
    {"depth attachment", "depth", .depth = true},
    {"two layers", "layers", .layers = 2},
    {"feedback loop", "feedback", .feedback = true},
    {"render area smaller", "area", .area = {W / 2, H}},
    {"a command before execute", "order", .orderBreak = true},
    {"reusable secondary", "secondary", .reusable = true},
    {"two draws", "draws", .twoDraws = true},
    {"a clear", "draws", .clear = true},
    {"barrier", "barrier", .barrier = true},
    {"unknown pipeline pNext", "pipeline", .pipelineNext = true},
    {"blend", "blend", .blend = true},
    {"write mask", "mask", .mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT},
    {"logic op", "logic-op", .logicOp = true},
    {"alpha to coverage", "raster", .a2c = true},
    {"discard", "discard", FS(ls_fs_discard)},
    {"alpha test less", "discard", FS(ls_fs_alpha), .fsSpec = &alphaSpec},
    {"sample mask written", "discard", FS(ls_fs_mask)},
    {"dynamic cull mode not set", "dynamic", .dynCull = true},
    {"untracked dynamic state", "dynamic", .dynUntracked = true},
    {"viewport narrow", "viewport", .viewportScale = 0.5f},
    {"scissor narrow", "scissor", .scissorScale = 0.5f},
    {"vertex memory not mapped", "vertex", .unmapped = true},
    {"vertex buffer written by a copy", "vertex", .gpuCopy = true},
    {"a shader writing memory", "vertex", .computeWrite = true},
    {"position from a uniform", "vs", VS(ls_vs_ubo)},
    {"negative clip distance", "vs", VS(ls_vs_clip)},
    {"left half", "geometry", .quad = {-1, -1, 0, 1}},
    {"culled", "geometry", .cull = VK_CULL_MODE_BACK_BIT},
    {"specialized scale", "geometry", VS(ls_vs_spec), .vsSpec = &scaleSpec},
    {"edge on the first centres", "geometry-edge", .dxvkViewport = true},
};

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "all";
    unsetenv("DROIDDECK_HYBRIS_CMD_STATS");
    unsetenv("DROIDDECK_HYBRIS_LOAD_SKIP");
    unsetenv("DROIDDECK_HYBRIS_CMD_STATS_FULL");
    if (!strcmp(mode, "all")) setenv("DROIDDECK_HYBRIS_LOAD_SKIP", "all", 1), setenv("DROIDDECK_HYBRIS_CMD_STATS", "1", 1);
    else if (!strcmp(mode, "full")) setenv("DROIDDECK_HYBRIS_LOAD_SKIP", "full", 1);
    else if (strcmp(mode, "off")) return fprintf(stderr, "usage: load_skip_test all|full|off\n"), 2;
    evaluatorTests();
    setUp();
    if (!strcmp(mode, "all")) {
        for (size_t i = 0; i < sizeof allCases / sizeof allCases[0]; i++) check(&allCases[i], true);
        const Case stale = {"data changed before the submission", NULL, .stale = true};
        check(&stale, true);
        /* the statistics line */
        Run run = {&allCases[0]};
        char log[16384];
        captured(runCase, &run, log, sizeof log);
        const char* line = strstr(log, "hybris-icd: cmd stats: ");
        CHECK(line && strstr(line, ", rendering 1,") && strstr(line, ", sec-pass 1, sec-1draw 1") && strstr(line, ", draw-indexed 1"),
              "cmd stats line: %s", line ? line : log);
    } else if (!strcmp(mode, "full")) {
        const Case big = {"full size", NULL, .width = 2 * W, .height = 2 * H};
        const Case small = {"half size", "small"};
        check(&big, true);
        check(&small, true);
        char log[16384];
        Run run = {&big};
        captured(runCase, &run, log, sizeof log);
        CHECK(!strstr(log, "cmd stats:"), "no statistics without DROIDDECK_HYBRIS_CMD_STATS");
    } else {
        openDevice();
        /* (vkCmdBeginRendering may be mali_fixes.c's) */
        CHECK(DEV(CmdDraw) == (PFN_vkCmdDraw)FAKE(CmdDraw) && DEV(CmdSetViewport) == (PFN_vkCmdSetViewport)FAKE(CmdSetViewport) &&
                  DEV(MapMemory) == (PFN_vkMapMemory)FAKE(MapMemory) && DEV(CmdExecuteCommands) == (PFN_vkCmdExecuteCommands)FAKE(CmdExecuteCommands) &&
                  DEV(CreateImageView) == (PFN_vkCreateImageView)FAKE(CreateImageView),
              "off: the driver's functions handed out");
        DEV(DestroyDevice)(device, NULL);
        check(&allCases[0], false);
    }
    fprintf(stderr, "load_skip_test %s: %d checks, %d failures\n", mode, checks, failures);
    return failures ? 1 : 0;
}
