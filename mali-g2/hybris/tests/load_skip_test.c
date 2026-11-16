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

static bool near(float a, float b) {
    return a - b < 1e-6f && b - a < 1e-6f;
}

/* what spv_vs_run reads beyond the vertex: buffers by set and binding, push constants */
static void resourceTests(SpvProgram* ubo) {
    SpvVertexIn in[1];
    SpvVertexOut out[1];
    uint8_t why;
    SpvBufferUse uses[4];
    CHECK(spv_buffers(ubo, uses, 4) == 1 && uses[0].set == 0 && uses[0].binding == 0 && uses[0].bytes == 16 && uses[0].kind == SPV_BUF_UNIFORM &&
              !uses[0].arrayed,
          "the uniform block declared");
    const float offset[4] = {0.25f, -0.5f, 0, 0};
    SpvBuffer buffer = {0, 0, (const uint8_t*)offset, sizeof offset, 0};
    SpvResources res = {&buffer, 1, NULL, 0};
    setPosition(&in[0], 0.5f, 0.5f, 0, 1);
    CHECK(spv_vs_run(ubo, "main", NULL, &res, in, 1, out, NULL) == SPV_RUN_OK && out[0].positionKnown && out[0].position[0] == 0.75f &&
              out[0].position[1] == 0 && out[0].position[3] == 1,
          "uniform offset read");
    buffer.size = 8;
    CHECK(spv_vs_run(ubo, "main", NULL, &res, in, 1, out, NULL) == SPV_RUN_OK && !out[0].positionKnown && out[0].why == SPV_WHY_RANGE,
          "half the block given: the rest unknown");
    buffer = (SpvBuffer){0, 0, NULL, 0, SPV_WHY_PENDING};
    CHECK(spv_vs_run(ubo, "main", NULL, &res, in, 1, out, NULL) == SPV_RUN_OK && !out[0].positionKnown && out[0].why == SPV_WHY_PENDING,
          "a buffer the GPU may write: unknown, and why");
    buffer = (SpvBuffer){0, 1, (const uint8_t*)offset, sizeof offset, 0};
    CHECK(spv_vs_run(ubo, "main", NULL, &res, in, 1, out, NULL) == SPV_RUN_OK && !out[0].positionKnown && out[0].why == SPV_WHY_DESCRIPTOR,
          "another binding: not this block");

    SpvProgram* push = PROG(ls_vs_push);
    CHECK(push && spv_buffers(push, uses, 4) == 1 && uses[0].kind == SPV_BUF_PUSH && uses[0].bytes == 32, "the push constant block declared");
    uint8_t bytes[SPV_PUSH_MAX] = {0};
    memcpy(bytes + 16, offset, sizeof offset);
    res = (SpvResources){NULL, 0, bytes, 0xf0};
    CHECK(spv_vs_run(push, "main", NULL, &res, in, 1, out, NULL) == SPV_RUN_OK && out[0].positionKnown && out[0].position[0] == 0.75f &&
              out[0].position[1] == 0,
          "push constants read");
    res.pushKnown = 0x70;
    CHECK(spv_vs_run(push, "main", NULL, &res, in, 1, out, NULL) == SPV_RUN_OK && !out[0].positionKnown && out[0].why == SPV_WHY_PUSH,
          "a push constant word not set: unknown");

    /* explicit layouts: row_major rm, column_major cm (MatrixStride 16), an int after them */
    SpvProgram* mat = PROG(ls_vs_mat);
    CHECK(mat && spv_buffers(mat, uses, 4) == 1 && uses[0].bytes == 132, "the matrix block: 2 * 64 + 4 bytes");
    float m[33] = {0};
    const float rows[4][4] = {{2, 0, 0, -1}, {0, 2, 0, -1}, {0, 0, 1, 0}, {0, 0, 0, 1}}; /* x' = 2x - 1, y' = 2y - 1 */
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) m[4 * r + c] = rows[r][c], m[16 + 4 * c + r] = rows[r][c] * 0.5f;
    int32_t sel = 0;
    memcpy(&m[32], &sel, 4);
    buffer = (SpvBuffer){0, 0, (const uint8_t*)m, sizeof m, 0};
    res = (SpvResources){&buffer, 1, NULL, 0};
    setPosition(&in[0], 1, 0.25f, 0.5f, 1);
    CHECK(spv_vs_run(mat, "main", NULL, &res, in, 1, out, NULL) == SPV_RUN_OK && out[0].positionKnown && out[0].position[0] == 1 &&
              out[0].position[1] == -0.5f && out[0].position[2] == 0.5f && out[0].position[3] == 1,
          "row-major matrix: (%g %g %g %g)", out[0].position[0], out[0].position[1], out[0].position[2], out[0].position[3]);
    sel = 1;
    memcpy(&m[32], &sel, 4);
    CHECK(spv_vs_run(mat, "main", NULL, &res, in, 1, out, NULL) == SPV_RUN_OK && out[0].positionKnown && out[0].position[0] == 0.5f &&
              out[0].position[1] == -0.25f && out[0].position[3] == 0.5f,
          "column-major matrix, chosen by an integer from the buffer");
    buffer.size = 128;
    CHECK(spv_vs_run(mat, "main", NULL, &res, in, 1, out, &why) == SPV_RUN_CONTROL && why == SPV_WHY_RANGE, "a branch on bytes not given");
    res.bufferCount = 0;
    CHECK(spv_vs_run(mat, "main", NULL, &res, in, 1, out, &why) == SPV_RUN_CONTROL && why == SPV_WHY_DESCRIPTOR, "a branch on a buffer not bound");

    SpvProgram* image = PROG(ls_vs_image);
    CHECK(image && spv_vs_run(image, "main", NULL, NULL, in, 1, out, NULL) == SPV_RUN_OK && !out[0].positionKnown && out[0].why == SPV_WHY_IMAGE,
          "a texel: unknown, an image");
    SpvProgram* branch = PROG(ls_vs_branch);
    CHECK(branch && spv_vs_run(branch, "main", NULL, NULL, in, 1, out, &why) == SPV_RUN_CONTROL && why == SPV_WHY_NONE,
          "a branch on a square root: control, no source");

    /* the DXVK-shaped shader: the matrix in c0..c3 maps [0, 1] to [-1, 1] */
    SpvProgram* dxso = PROG(ls_vs_dxso);
    CHECK(dxso && spv_buffers(dxso, uses, 4) == 2, "the D3D9 shader declares a uniform and a push block");
    float c[5][4];
    memcpy(c, rows, sizeof rows);
    c[2][2] = 0, c[4][0] = c[4][1] = 1, c[4][2] = c[4][3] = 0;
    SpvBuffer cb = {1, 3, (const uint8_t*)c, sizeof c, 0};
    memset(bytes, 0, sizeof bytes);
    const uint16_t wScale = 1;
    memcpy(bytes + 2, &wScale, 2);
    res = (SpvResources){&cb, 1, bytes, 1};
    SpvVertexIn quad[4];
    SpvVertexOut qout[4];
    const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; i++) setPosition(&quad[i], corners[i][0], corners[i][1], 0, 1);
    CHECK(spv_vs_run(dxso, "main", NULL, &res, quad, 4, qout, NULL) == SPV_RUN_OK && qout[0].positionKnown && qout[0].position[0] == -1 &&
              qout[0].position[1] == -1 && qout[2].position[0] == 1 && qout[2].position[1] == 1 && qout[2].position[2] == 0 &&
              qout[2].position[3] == 1,
          "D3D9 shape: c0..c3 transform the quad");
    const uint32_t clipOne = 1;
    VkSpecializationMapEntry stateEntry = {0, 0, 4};
    VkSpecializationInfo state = {1, &stateEntry, 4, &clipOne};
    CHECK(spv_vs_run(dxso, "main", &state, &res, quad, 1, qout, NULL) == SPV_RUN_OK && qout[0].position[2] == 1,
          "D3D9 shape: the specialized branch taken");
    res.pushKnown = 0;
    CHECK(spv_vs_run(dxso, "main", NULL, &res, quad, 1, qout, NULL) == SPV_RUN_OK && !qout[0].positionKnown && qout[0].why == SPV_WHY_PUSH,
          "D3D9 shape: the 16-bit push member not set");
    const uint16_t wTwo = 2;
    memcpy(bytes + 2, &wTwo, 2);
    res.pushKnown = 1;
    CHECK(spv_vs_run(dxso, "main", NULL, &res, quad, 1, qout, NULL) == SPV_RUN_OK && near(qout[0].position[3], 2), "D3D9 shape: w scaled by 2");

    /* mediump (RelaxedPrecision): the GPU may compute it in 16 bits, the CPU does not know the result */
    SpvProgram* relaxed = PROG(ls_vs_relaxed);
    const float zero[4] = {0};
    buffer = (SpvBuffer){0, 0, (const uint8_t*)zero, sizeof zero, 0};
    res = (SpvResources){&buffer, 1, NULL, 0};
    setPosition(&in[0], 0.5f, 0.5f, 0, 1);
    CHECK(relaxed && spv_vs_run(relaxed, "main", NULL, &res, in, 1, out, NULL) == SPV_RUN_OK && !out[0].positionKnown,
          "a position through a mediump variable: unknown");
    spv_unref(push), spv_unref(mat), spv_unref(image), spv_unref(branch), spv_unref(dxso), spv_unref(relaxed);
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
    CHECK(pass && spv_vs_run(pass, "main", NULL, NULL, in, 3, out, NULL) == SPV_RUN_OK, "pass-through runs");
    CHECK(out[1].positionKnown && out[1].position[0] == 3 && out[1].position[2] == 0.5f && out[2].position[3] == 2, "pass-through position");
    uint8_t kinds[SPV_LOCATIONS];
    spv_vs_inputs(pass, "main", kinds);
    CHECK(kinds[0] == SPV_ATTR_FLOAT && kinds[1] == SPV_ATTR_FLOAT && kinds[2] == SPV_ATTR_NONE, "inputs");
    in[0].location[0].kind = SPV_ATTR_SINT;
    CHECK(spv_vs_run(pass, "main", NULL, NULL, in, 1, out, NULL) == SPV_RUN_OK && !out[0].positionKnown && out[0].why == SPV_WHY_INPUT,
          "an integer attribute for a float input: unknown");

    SpvProgram* tri = PROG(ls_vs_tri);
    for (int i = 0; i < 3; i++) memset(&in[i], 0, sizeof in[i]), in[i].vertexIndex = (uint32_t)i;
    CHECK(tri && spv_vs_run(tri, "main", NULL, NULL, in, 3, out, NULL) == SPV_RUN_OK, "vertex index triangle runs");
    CHECK(out[0].position[0] == -1 && out[0].position[1] == -1 && out[1].position[1] == 3 && out[2].position[0] == 3 && out[2].position[3] == 1,
          "vertex index triangle: (-1,-1) (-1,3) (3,-1)");

    SpvProgram* ubo = PROG(ls_vs_ubo);
    setPosition(&in[0], 0, 0, 0, 1);
    CHECK(ubo && spv_vs_run(ubo, "main", NULL, NULL, in, 1, out, NULL) == SPV_RUN_OK && !out[0].positionKnown && out[0].why == SPV_WHY_DESCRIPTOR,
          "uniform offset without its buffer: unknown");
    resourceTests(ubo);
    SpvProgram* clip = PROG(ls_vs_clip);
    setPosition(&in[0], -0.5f, 0, 0, 1);
    CHECK(clip && spv_vs_run(clip, "main", NULL, NULL, in, 1, out, NULL) == SPV_RUN_OK && out[0].clipCount == 1 && out[0].clipState[0] == 1 &&
              out[0].clip[0] == -0.5f,
          "clip distance");
    SpvProgram* scaled = PROG(ls_vs_spec);
    float half = 0.5f;
    VkSpecializationMapEntry scaleEntry = {3, 0, 4};
    VkSpecializationInfo scale = {1, &scaleEntry, 4, &half};
    setPosition(&in[0], -1, 1, 0, 1);
    CHECK(scaled && spv_vs_run(scaled, "main", &scale, NULL, in, 1, out, NULL) == SPV_RUN_OK && out[0].position[0] == -0.5f && out[0].position[1] == 0.5f,
          "specialized scale");
    CHECK(spv_vs_run(scaled, "main", NULL, NULL, in, 1, out, NULL) == SPV_RUN_OK && out[0].position[0] == -1, "default scale");
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
    /* a one-word OpStore at the end: a write, nothing read past it */
    uint32_t* cut = malloc(sizeof ls_fs_plain + 4);
    memcpy(cut, ls_fs_plain, sizeof ls_fs_plain);
    cut[sizeof ls_fs_plain / 4] = (1u << 16) | 62;
    SpvProgram* cutProg = spv_parse(cut, sizeof ls_fs_plain + 4);
    CHECK(!cutProg || spv_writes_memory(cutProg), "one-word OpStore");
    spv_unref(cutProg);
    free(cut);
    SpvProgram* sw64 = PROG(ls_fs_switch64);
    CHECK(sw64 && spv_fs_may_discard(sw64, "main", NULL) == SPV_DISCARD_MAY, "kill through case 0 of a 64-bit switch: may");
    SpvProgram* input = PROG(ls_fs_input);
    CHECK(input && spv_reads_attachments(input) && !spv_reads_attachments(plain) && !spv_reads_attachments(alpha),
          "input attachment read");
    CHECK(input && spv_fs_may_discard(input, "main", NULL) == SPV_DISCARD_NO, "input attachment: no discard");
    spv_unref(sw64), spv_unref(input);

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
    const float inf = __builtin_inff();
    const float infinite[3][4] = {{inf, inf, 0, 1}, {inf, -inf, 0, 1}, {-inf, inf, 0, 1}};
    CHECK(ls_cover(infinite, 3, one, 1, &vp, area, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, false) == LS_COVER_CLIP, "infinite");
    const float huge[3][4] = {{-1, -1, 0, 1}, {3e7f, -1, 0, 1}, {-1, 3e7f, 0, 1}};
    CHECK(ls_cover(huge, 3, one, 1, &vp, area, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, false) == LS_COVER_CLIP, "past 1e6 w");
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
    bool conditional;        /* the pass inside conditional rendering on the primary */
    bool inheritConditional; /* the secondary begun with inherited conditional rendering */
    bool inheritUnknown;     /* a structure in the inheritance pNext not known */
    bool unknownBetween;     /* a command not wrapped by name between the begin and the execute */
    bool unknownInSecondary;
    uint32_t stream;         /* rasterization stream, static */
    bool dynStream;          /* dynamic, set to `stream` */
    const char* order;       /* the trace expected; NULL: the usual */
    int ubo;                 /* the vertex shader's uniform block bound through a descriptor set: UBO_* */
    int push;                /* its push constants pushed: 1 vkCmdPushConstants, 2 vkCmdPushConstants2KHR */
    float shift;             /* added to x by the uniform or push data */
    bool uboUnmapped, uboPending, uboStale, writtenAfterBind, pushDescriptor;
    bool vertexNonCoherent, uboNonCoherent; /* in a memory type that is not HOST_COHERENT */
    int imageOverUbo;        /* an image bound in the uniform's memory: 1 over it, 2 beside it, 3 over it and destroyed */
    bool bindBetween;        /* sets bound and constants pushed on the primary between the begin and the execute */
    bool shiftSecond;        /* with twice: the second pass's set points at data that shifts x by 1 */
} Case;

/* UBO_OVERFLOW: vs_set1's dynamic uniform at set 1, behind a set 0 of 256 storage buffers and a dynamic
 * uniform (more buffers than the layout tracking keeps) */
enum { UBO_NONE, UBO_SET, UBO_DYNAMIC, UBO_TEMPLATE, UBO_BIND2, UBO_DXSO, UBO_OVERFLOW };

/* the descriptor sets a secondary binds */
typedef struct {
    VkDescriptorSet sets[2];
    uint32_t first, count, dynamic;
    VkDescriptorSetLayout layout0;
} Bound;

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
    VkPipelineRasterizationStateStreamCreateInfoEXT stream = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT};
    stream.rasterizationStream = k->stream;
    if (k->stream && !k->dynStream) rs.pNext = &stream;
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
    if (k->dynStream) dyn[dynCount++] = VK_DYNAMIC_STATE_RASTERIZATION_STREAM_EXT;
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

static void recordDraw(const Case* k, VkCommandBuffer cb, VkPipeline pipe, VkBuffer vb, VkBuffer ib, const Bound* bound) {
    uint32_t w = k->width ? k->width : W, h = k->height ? k->height : H;
    DEV(CmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    if (bound && bound->count) {
        /* UBO_DYNAMIC: 256; UBO_OVERFLOW: 0 for set 0, 512 (the bad offset) for set 1 */
        const uint32_t dyn[2] = {k->ubo == UBO_OVERFLOW ? 0 : 256, 512};
        if (k->ubo == UBO_BIND2) {
            VkBindDescriptorSetsInfoKHR info = {VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO_KHR, NULL, VK_SHADER_STAGE_ALL_GRAPHICS, VK_NULL_HANDLE,
                                                bound->first, bound->count, bound->sets, 0, NULL};
            DEV(CmdBindDescriptorSets2KHR)(cb, &info);
        } else {
            DEV(CmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE, bound->first, bound->count, bound->sets,
                                       bound->dynamic, dyn);
        }
        if (k->pushDescriptor) {
            VkDescriptorBufferInfo none = {VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                          NULL, &none, NULL};
            DEV(CmdPushDescriptorSetKHR)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE, bound->first, 1, &write);
        }
    }
    if (k->push) {
        /* vs_push: a vec4 at byte 16; vs_dxso: two 16-bit members, the second scaling w */
        const float offset[4] = {k->shift, 0, 0, 0};
        const uint16_t dxso[2] = {0, 1};
        uint32_t at = k->ubo == UBO_DXSO ? 0 : 16, size = k->ubo == UBO_DXSO ? 4 : 16;
        const void* data = k->ubo == UBO_DXSO ? (const void*)dxso : (const void*)offset;
        if (k->push == 2) {
            VkPushConstantsInfoKHR info = {VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO_KHR, NULL, VK_NULL_HANDLE, VK_SHADER_STAGE_VERTEX_BIT, at, size, data};
            DEV(CmdPushConstants2KHR)(cb, &info);
        } else {
            DEV(CmdPushConstants)(cb, VK_NULL_HANDLE, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, at, size, data);
        }
    }
    if (k->dynStream) DEV(CmdSetRasterizationStreamEXT)(cb, k->stream);
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
    if (k->unknownInSecondary) DEV(CmdBeginTransformFeedbackEXT)(cb, 0, 0, NULL, NULL);
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
    VkMemoryAllocateInfo mi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, 4096, k->vertexNonCoherent ? 2 : 1};
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

    /* the uniform data at 2048 of the same memory (or of memory not mapped): an offset (shift, 0, 0, 0)
     * for vs_ubo, at 2048 + 256 for the dynamic offset (2048 itself holds a bad one), at 2048 + 512 the
     * second pass's with shiftSecond; vs_dxso: c0..c4 mapping [0, 1] to [-1, 1] */
    VkDeviceMemory uboMemory = memory;
    VkDeviceSize uboBase = 2048;
    void* uboPtr = (char*)ptr + 2048;
    if (k->uboUnmapped || k->uboNonCoherent) {
        mi.memoryTypeIndex = k->uboNonCoherent ? 2 : 1;
        CHECK(DEV(AllocateMemory)(device, &mi, NULL, &uboMemory) == VK_SUCCESS && DEV(MapMemory)(device, uboMemory, 0, VK_WHOLE_SIZE, 0, &uboPtr) == VK_SUCCESS,
              "uniform memory");
        uboBase = 0;
    }
    VkBuffer ub;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CHECK(DEV(CreateBuffer)(device, &bi, NULL, &ub) == VK_SUCCESS && DEV(BindBufferMemory)(device, ub, uboMemory, uboBase) == VK_SUCCESS, "uniform buffer");
    if (k->ubo == UBO_DXSO) {
        const float c[5][4] = {{2, 0, 0, -1 + k->shift}, {0, 2, 0, -1}, {0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 0, 0}};
        memcpy(uboPtr, c, sizeof c);
    } else {
        const float good[4] = {k->shift, 0, 0, 0}, bad[4] = {1, 0, 0, 0};
        memcpy(uboPtr, k->ubo == UBO_DYNAMIC ? bad : good, sizeof good);
        memcpy((char*)uboPtr + 256, good, sizeof good);
        memcpy((char*)uboPtr + 512, bad, sizeof bad);
    }
    if (k->uboUnmapped) DEV(UnmapMemory)(device, uboMemory);
    /* an image in the uniform's memory (the fake's images take 512 bytes): the GPU may write it */
    VkImage aliasImage = VK_NULL_HANDLE;
    if (k->imageOverUbo) {
        VkImageCreateInfo ai3 = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ai3.imageType = VK_IMAGE_TYPE_2D, ai3.format = colorFormat, ai3.extent = (VkExtent3D){8, 8, 1}, ai3.mipLevels = 1, ai3.arrayLayers = 1;
        ai3.samples = VK_SAMPLE_COUNT_1_BIT, ai3.tiling = VK_IMAGE_TILING_LINEAR, ai3.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        CHECK(DEV(CreateImage)(device, &ai3, NULL, &aliasImage) == VK_SUCCESS, "alias image");
        VkDeviceSize at = k->imageOverUbo == 2 ? uboBase + 1024 : uboBase + 256;
        if (k->imageOverUbo == 1) {
            VkBindImageMemoryInfo bind = {VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, NULL, aliasImage, uboMemory, at};
            CHECK(DEV(BindImageMemory2)(device, 1, &bind) == VK_SUCCESS, "alias image bound");
        } else {
            CHECK(DEV(BindImageMemory)(device, aliasImage, uboMemory, at) == VK_SUCCESS, "alias image bound");
        }
        if (k->imageOverUbo == 3) DEV(DestroyImage)(device, aliasImage, NULL), aliasImage = VK_NULL_HANDLE;
    }
    if (k->unmapped) DEV(UnmapMemory)(device, memory);

    /* descriptor sets: one per pass (the second at 512 with shiftSecond); vs_dxso's at set 1, binding 3,
     * behind a set 0 of a sampler */
    Bound bound[2] = {{{0}}};
    VkDescriptorSetLayout layouts[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorUpdateTemplate tmpl = VK_NULL_HANDLE;
    if (k->ubo) {
        bool dynamic = k->ubo == UBO_DYNAMIC || k->ubo == UBO_OVERFLOW;
        VkDescriptorSetLayoutBinding b = {k->ubo == UBO_DXSO ? 3 : 0, dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                          1, VK_SHADER_STAGE_VERTEX_BIT, NULL};
        VkDescriptorSetLayoutBinding sampler = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutBinding many[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256, VK_SHADER_STAGE_VERTEX_BIT, NULL},
                                                {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
        CHECK(DEV(CreateDescriptorSetLayout)(device, &li, NULL, &layouts[1]) == VK_SUCCESS, "set layout");
        li.pBindings = k->ubo == UBO_OVERFLOW ? many : &sampler, li.bindingCount = k->ubo == UBO_OVERFLOW ? 2 : 1;
        CHECK(DEV(CreateDescriptorSetLayout)(device, &li, NULL, &layouts[0]) == VK_SUCCESS, "set 0 layout");
        VkDescriptorPoolSize sizes[2] = {{b.descriptorType, 4}, {sampler.descriptorType, 4}};
        VkDescriptorPoolCreateInfo pi2 = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 8, 2, sizes};
        CHECK(DEV(CreateDescriptorPool)(device, &pi2, NULL, &dpool) == VK_SUCCESS, "descriptor pool");
        if (k->ubo == UBO_TEMPLATE) {
            VkDescriptorUpdateTemplateEntry e = {0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, sizeof(VkDescriptorBufferInfo)};
            VkDescriptorUpdateTemplateCreateInfo ti = {VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO, NULL, 0, 1, &e,
                                                       VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET, layouts[1]};
            CHECK(DEV(CreateDescriptorUpdateTemplate)(device, &ti, NULL, &tmpl) == VK_SUCCESS, "template");
        }
        for (int i = 0; i < (k->twice ? 2 : 1); i++) {
            Bound* bd = &bound[i];
            bool dxso = k->ubo == UBO_DXSO || k->ubo == UBO_OVERFLOW;
            VkDescriptorSetLayout want[2] = {layouts[0], layouts[1]};
            VkDescriptorSetAllocateInfo ai2 = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dpool, dxso ? 2 : 1, dxso ? want : &want[1]};
            CHECK(DEV(AllocateDescriptorSets)(device, &ai2, bd->sets) == VK_SUCCESS, "sets");
            bd->first = 0, bd->count = dxso ? 2 : 1, bd->dynamic = k->ubo == UBO_OVERFLOW ? 2 : k->ubo == UBO_DYNAMIC;
            VkDescriptorBufferInfo info = {ub, i && k->shiftSecond ? 512 : dynamic || k->ubo == UBO_DXSO ? 0 : 256,
                                           k->ubo == UBO_DXSO ? 80 : 16};
            VkDescriptorSet target = bd->sets[dxso ? 1 : 0];
            if (k->ubo == UBO_TEMPLATE) {
                DEV(UpdateDescriptorSetWithTemplate)(device, target, tmpl, &info);
            } else {
                VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, target, b.binding, 0, 1, b.descriptorType, NULL, &info, NULL};
                DEV(UpdateDescriptorSets)(device, 1, &write, 0, NULL);
            }
        }
    }

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
    if (k->gpuCopy || k->computeWrite || k->uboPending) {
        DEV(BeginCommandBuffer)(primary[1], &once);
        if (k->gpuCopy) {
            VkBufferCopy region = {0, 0, 96};
            DEV(CmdCopyBuffer)(primary[1], ib, vb, 1, &region);
        } else if (k->uboPending) {
            VkBufferCopy region = {0, 256, 16};
            DEV(CmdCopyBuffer)(primary[1], ib, ub, 1, &region);
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
    VkCommandBufferInheritanceConditionalRenderingInfoEXT inheritCond = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_CONDITIONAL_RENDERING_INFO_EXT};
    inheritCond.conditionalRenderingEnable = k->inheritConditional;
    inheritRendering.pNext = &inheritCond; /* disabled: no effect */
    /* VkRenderingAttachmentLocationInfoKHR, all locations unused */
    struct {
        VkStructureType sType;
        const void* pNext;
        uint32_t count;
        const uint32_t* locations;
    } locations = {(VkStructureType)1000232001, NULL, 1, (const uint32_t[]){VK_ATTACHMENT_UNUSED}};
    if (k->inheritUnknown) inheritCond.pNext = &locations;
    VkCommandBufferBeginInfo sb = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    sb.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | (k->reusable ? 0 : VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    sb.pInheritanceInfo = &inherit;
    for (int i = 0; !k->inlineDraw && i < (k->twice ? 2 : 1); i++) {
        DEV(BeginCommandBuffer)(secondaries[i], &sb);
        recordDraw(k, secondaries[i], pipe, vb, ib, &bound[i]);
        DEV(EndCommandBuffer)(secondaries[i]);
        if (k->writtenAfterBind) {
            VkDescriptorBufferInfo info = {ub, 256, 16};
            VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, bound[i].sets[0], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                          NULL, &info, NULL};
            DEV(UpdateDescriptorSets)(device, 1, &write, 0, NULL);
        }
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
    VkConditionalRenderingBeginInfoEXT cond = {VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT, NULL, vb, 0};
    if (k->conditional) DEV(CmdBeginConditionalRenderingEXT)(primary[0], &cond);
    for (int i = 0; i < (k->twice ? 2 : 1); i++) {
        DEV(CmdBeginRendering)(primary[0], &ri);
        if (k->unknownBetween) DEV(CmdBeginTransformFeedbackEXT)(primary[0], 0, 0, NULL, NULL);
        if (k->bindBetween) {
            const float bad[4] = {1, 0, 0, 0};
            DEV(CmdBindDescriptorSets)(primary[0], VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE, 0, 1, bound[0].sets, 0, NULL);
            DEV(CmdPushConstants)(primary[0], VK_NULL_HANDLE, VK_SHADER_STAGE_VERTEX_BIT, 16, 16, bad);
        }
        if (k->orderBreak) {
            VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
            DEV(CmdSetViewport)(primary[0], 0, 1, &vp);
        }
        if (k->inlineDraw) recordDraw(k, primary[0], pipe, vb, ib, NULL);
        else DEV(CmdExecuteCommands)(primary[0], 1, &secondaries[i]);
        DEV(CmdEndRendering)(primary[0]);
        if (i == 0) run->load = fk->lastColorLoad;
    }
    if (k->conditional) DEV(CmdEndConditionalRenderingEXT)(primary[0]);
    DEV(EndCommandBuffer)(primary[0]);
    if (run->load != fk->lastColorLoad) run->load = VK_ATTACHMENT_LOAD_OP_MAX_ENUM;
    strcpy(run->trace, fk->trace);
    if (k->stale) {
        float moved = 0.5f;
        memcpy(ptr, &moved, sizeof moved);
    }
    if (k->uboStale) {
        float moved = 0.5f;
        memcpy((char*)uboPtr + 256, &moved, sizeof moved);
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
    DEV(DestroyBuffer)(device, ub, NULL);
    if (dpool) DEV(DestroyDescriptorPool)(device, dpool, NULL);
    if (tmpl) DEV(DestroyDescriptorUpdateTemplate)(device, tmpl, NULL);
    for (int i = 0; i < 2; i++)
        if (layouts[i]) DEV(DestroyDescriptorSetLayout)(device, layouts[i], NULL);
    if (aliasImage) DEV(DestroyImage)(device, aliasImage, NULL);
    if (uboMemory != memory) DEV(FreeMemory)(device, uboMemory, NULL);
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
    CHECK(!strcmp(run.trace, k->order ? k->order : k->inlineDraw ? "BDE" : k->twice ? "BXEBXE" : "BXE"), "%s: order %s", k->name, run.trace);
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
    CHECK(!line || !strstr(line, k->stale || k->uboStale ? ", stale 0" : ", stale 1"), "%s: stale count: %s", k->name, line ? line : "");
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
    {"position from a uniform not bound", "vs-descriptor", VS(ls_vs_ubo)},
    {"uniform through a descriptor set", NULL, VS(ls_vs_ubo), .ubo = UBO_SET},
    {"uniform shifting the quad", "geometry", VS(ls_vs_ubo), .ubo = UBO_SET, .shift = 0.5f},
    {"uniform at a dynamic offset", NULL, VS(ls_vs_ubo), .ubo = UBO_DYNAMIC},
    {"uniform written through a template", NULL, VS(ls_vs_ubo), .ubo = UBO_TEMPLATE},
    {"uniform bound by vkCmdBindDescriptorSets2KHR", NULL, VS(ls_vs_ubo), .ubo = UBO_BIND2},
    {"uniform memory not mapped", "vs-ubo-unmapped", VS(ls_vs_ubo), .ubo = UBO_SET, .uboUnmapped = true},
    {"uniform buffer written by a copy", "vs-ubo-pending-write", VS(ls_vs_ubo), .ubo = UBO_SET, .uboPending = true},
    {"set written after it was bound", "vs-descriptor", VS(ls_vs_ubo), .ubo = UBO_SET, .writtenAfterBind = true},
    {"a push descriptor over the set", "vs-descriptor", VS(ls_vs_ubo), .ubo = UBO_SET, .pushDescriptor = true},
    {"two passes, uniforms from the cache", NULL, VS(ls_vs_ubo), .ubo = UBO_SET, .twice = true},
    {"sets bound and constants pushed on the primary meanwhile", NULL, VS(ls_vs_ubo), .ubo = UBO_SET, .bindBetween = true},
    {"push constants", NULL, VS(ls_vs_push), .push = 1},
    {"push constants through vkCmdPushConstants2KHR", NULL, VS(ls_vs_push), .push = 2},
    {"push constants shifting the quad", "geometry", VS(ls_vs_push), .push = 1, .shift = 0.5f},
    {"push constants not pushed", "vs-push", VS(ls_vs_push)},
    {"D3D9 shape: c0..c3 transform the quad", NULL, VS(ls_vs_dxso), .ubo = UBO_DXSO, .push = 1, .quad = {0, 0, 1, 1}},
    {"D3D9 shape, translated", "geometry", VS(ls_vs_dxso), .ubo = UBO_DXSO, .push = 1, .quad = {0, 0, 1, 1}, .shift = 0.5f},
    {"D3D9 shape, push data not pushed", "vs-push", VS(ls_vs_dxso), .ubo = UBO_DXSO, .quad = {0, 0, 1, 1}},
    {"position moved by a texel", "vs-image", VS(ls_vs_image)},
    {"set 1's dynamic offset behind a set 0 of more buffers than kept", "geometry", VS(ls_vs_set1), .ubo = UBO_OVERFLOW},
    {"uniform memory not host-coherent", "vs-ubo-unmapped", VS(ls_vs_ubo), .ubo = UBO_SET, .uboNonCoherent = true},
    {"vertex memory not host-coherent", "vertex", .vertexNonCoherent = true},
    {"an image bound over the uniform", "vs-ubo-pending-write", VS(ls_vs_ubo), .ubo = UBO_SET, .imageOverUbo = 1},
    {"an image bound beside the uniform", NULL, VS(ls_vs_ubo), .ubo = UBO_SET, .imageOverUbo = 2},
    {"an image bound over the uniform, destroyed", NULL, VS(ls_vs_ubo), .ubo = UBO_SET, .imageOverUbo = 3},
    {"uniform used in mediump", "vs", VS(ls_vs_relaxed), .ubo = UBO_SET},
    {"a branch on a square root", "vs-branch", VS(ls_vs_branch)},
    {"negative clip distance", "vs", VS(ls_vs_clip)},
    {"left half", "geometry", .quad = {-1, -1, 0, 1}},
    {"culled", "geometry", .cull = VK_CULL_MODE_BACK_BIT},
    {"specialized scale", "geometry", VS(ls_vs_spec), .vsSpec = &scaleSpec},
    {"edge on the first centres", "geometry-edge", .dxvkViewport = true},
    {"inside conditional rendering", "conditional", .conditional = true},
    {"inherited conditional rendering", "secondary", .inheritConditional = true},
    {"attachment locations inherited", "secondary", .inheritUnknown = true},
    {"an unwrapped command before execute", "order", .unknownBetween = true, .order = "BTXE"},
    {"an unwrapped command in the secondary", "secondary", .unknownInSecondary = true},
    {"rasterization stream 1", "raster", .stream = 1},
    {"dynamic rasterization stream 1", "raster", .stream = 1, .dynStream = true},
    {"dynamic rasterization stream 0", NULL, .dynStream = true},
    {"input attachment read", "feedback", FS(ls_fs_input)},
};

/* An unwrapped command through the generic stub: every argument arrives. */
static void testArgs(bool wrapped) {
    openDevice();
    PFN_vkCmdTestArgsFAKE fn = (PFN_vkCmdTestArgsFAKE)gdpa(device, "vkCmdTestArgsFAKE");
    CHECK(fn && (fn != (PFN_vkCmdTestArgsFAKE)FAKE(CmdTestArgsFAKE)) == wrapped, "vkCmdTestArgsFAKE %s", wrapped ? "wrapped" : "not wrapped");
    PFN_vkCmdTestArgsFAKE again = (PFN_vkCmdTestArgsFAKE)gdpa(device, "vkCmdTestArgsFAKE");
    CHECK(again == fn, "the same stub again");
    CHECK((gdpa(device, "vkCmdSetCullMode") != FAKE(CmdSetCullMode)) == wrapped, "a tracked state command");
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool;
    DEV(CreateCommandPool)(device, &pi, NULL, &pool);
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    DEV(AllocateCommandBuffers)(device, &ai, &cb);
    fk->testCalls = 0;
    if (fn) fn(cb, 0x1111111111111111ull, 0x22222222u, 0x3333333333333333ull, 0x44444444u, 0x5555555555555555ull, 0x66666666u,
               0x7777777777777777ull, 0x8888888888888888ull, 0.5f, 1.25, 2.5f, 3.75, 4.5f, 5.25, 6.5f, 7.75, 8.5f, 9.25);
    const uint64_t ints[8] = {0x1111111111111111ull, 0x22222222u, 0x3333333333333333ull, 0x44444444u, 0x5555555555555555ull, 0x66666666u,
                              0x7777777777777777ull, 0x8888888888888888ull};
    const double floats[10] = {0.5, 1.25, 2.5, 3.75, 4.5, 5.25, 6.5, 7.75, 8.5, 9.25};
    CHECK(fk->testCalls == 1 && fk->testCb == cb && !memcmp(fk->testInts, ints, sizeof ints) && !memcmp(fk->testFloats, floats, sizeof floats),
          "the arguments through the stub");
    DEV(DestroyCommandPool)(device, pool, NULL);
    DEV(DestroyDevice)(device, NULL);
}

static void writeMode(const char* path, const char* mode) {
    FILE* f = fopen(path, "w");
    if (f) fputs(mode, f), fclose(f);
}

/* a pass and its submission, after the file's next read is due: the mode it says applies from then */
static void switchMode(void) {
    usleep(600000);
    Run run = {&(Case){"switch", NULL}};
    char log[16384];
    captured(runCase, &run, log, sizeof log);
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "all";
    unsetenv("DROIDDECK_HYBRIS_CMD_STATS");
    unsetenv("DROIDDECK_HYBRIS_LOAD_SKIP");
    unsetenv("DROIDDECK_HYBRIS_CMD_STATS_FULL");
    char modeFile[64];
    snprintf(modeFile, sizeof modeFile, "/tmp/load_skip_test_mode.%d", (int)getpid());
    unlink(modeFile);
    unsetenv("DROIDDECK_HYBRIS_LOAD_SKIP_FILE");
    if (!strcmp(mode, "all"))
        setenv("DROIDDECK_HYBRIS_LOAD_SKIP", "all", 1), setenv("DROIDDECK_HYBRIS_CMD_STATS", "1", 1),
            setenv("DROIDDECK_HYBRIS_LOAD_SKIP_FILE", modeFile, 1);
    else if (!strcmp(mode, "full")) setenv("DROIDDECK_HYBRIS_LOAD_SKIP", "full", 1);
    else if (strcmp(mode, "off")) return fprintf(stderr, "usage: load_skip_test all|full|off\n"), 2;
    evaluatorTests();
    setUp();
    if (!strcmp(mode, "all")) {
        for (size_t i = 0; i < sizeof allCases / sizeof allCases[0]; i++) check(&allCases[i], true);
        const Case stale = {"data changed before the submission", NULL, .stale = true};
        check(&stale, true);
        const Case uboStale = {"uniform changed before the submission", NULL, VS(ls_vs_ubo), .ubo = UBO_SET, .uboStale = true};
        check(&uboStale, true);
        /* the same pipeline and vertices, other uniform data: not the cached run */
        const Case second = {"second pass's uniform shifted", NULL, VS(ls_vs_ubo), .ubo = UBO_SET, .twice = true, .shiftSecond = true};
        Run two = {&second};
        char twoLog[16384];
        captured(runCase, &two, twoLog, sizeof twoLog);
        const char* twoLine = strstr(twoLog, "hybris-icd: load skip: ");
        CHECK(twoLine && strstr(twoLine, ", applied 1,") && strstr(twoLine, ", geometry 1"), "%s: %s", second.name, twoLine ? twoLine : twoLog);
        /* the statistics line */
        Run run = {&allCases[0]};
        char log[16384];
        captured(runCase, &run, log, sizeof log);
        const char* line = strstr(log, "hybris-icd: cmd stats: ");
        CHECK(line && strstr(line, ", rendering 1,") && strstr(line, ", sec-pass 1, sec-1draw 1") && strstr(line, ", draw-indexed 1"),
              "cmd stats line: %s", line ? line : log);
        testArgs(true);
        /* the mode file: off, then full, then gone (the environment's all again) */
        writeMode(modeFile, "off\n");
        switchMode();
        const Case off = {"mode file off", "off"};
        check(&off, true);
        captured(runCase, &run, log, sizeof log);
        CHECK(strstr(log, "load skip: mode off, "), "mode in the line: %s", log);
        writeMode(modeFile, "full");
        switchMode();
        const Case big = {"mode file full, full size", NULL, .width = 2 * W, .height = 2 * H};
        const Case small = {"mode file full, half size", "small"};
        check(&big, true);
        check(&small, true);
        unlink(modeFile);
        switchMode();
        check(&allCases[0], true);
        captured(runCase, &run, log, sizeof log);
        CHECK(strstr(log, "load skip: mode all, "), "back to all: %s", log);
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
        testArgs(false);
    }
    fprintf(stderr, "load_skip_test %s: %d checks, %d failures\n", mode, checks, failures);
    return failures ? 1 : 0;
}
