/*
 * [mali-g2] Command statistics and the colour load skip of single-draw passes. icd.c's
 * vkGetDeviceProcAddr hands out the wrappers here in front of whatever it would hand out otherwise
 * (this library's own wrappers or the driver's functions) when either is on; with both off nothing
 * is wrapped.
 *
 * DROIDDECK_HYBRIS_CMD_STATS=1: counts of the commands the application records and submits (render
 * passes and how they load and store, copies, blits, uploads, dispatches, clears, queries, barriers,
 * submissions, debug labels). Every DROIDDECK_HYBRIS_CMD_STATS_LOG_S seconds (10) of presents a line
 * "hybris-icd: cmd stats: ..." gives the counts since the previous line and the presents in between
 * (dp); with VK_EXT_debug_utils labels (DXVK_DEBUG=markers) lines "hybris-icd: cmd label: ..." give,
 * per label text (pass numbers replaced by #), how often it was begun, the render passes and draws
 * recorded while it was the innermost label, and the passes among them that load a full-size
 * attachment. A pass is full size when its render area is at least 90 % of
 * DROIDDECK_HYBRIS_CMD_STATS_FULL (WxH, default the largest render area seen so far). Counted per
 * thread as recorded (DXVK records each command buffer on one thread).
 *
 * DROIDDECK_HYBRIS_LOAD_SKIP=full|all (default off): a colour attachment's LOAD becomes DONT_CARE
 * when the pass's one draw is proven to overwrite every pixel of it (full: only full-size passes as
 * above; all: any size). On a tiler a LOAD reads the whole attachment into the tile buffer. DXVK on
 * a tiler records each pass into a secondary command buffer and then begins rendering and executes
 * it; vkCmdBeginRendering is held back until the vkCmdExecuteCommands after it, whose secondary is
 * then inspected. Every one of these must hold, otherwise the LOAD stays:
 *   pass        one colour attachment, LOAD; no depth or stencil attachment; layerCount 1, no
 *               multiview; flags only CONTENTS_SECONDARY; no pNext on VkRenderingInfo; on the
 *               attachment only VkAttachmentFeedbackLoopInfoEXT (disabled) and
 *               VkRenderingAttachmentFlagsInfoKHR (without input attachment feedback); layout
 *               GENERAL, ATTACHMENT_OPTIMAL or COLOR_ATTACHMENT_OPTIMAL; render area the whole image
 *               view at its mip level; one sample; nothing recorded between the begin and one
 *               vkCmdExecuteCommands of one secondary; not inside conditional rendering. Every
 *               vkCmd* not wrapped here by name, other than pure state (vkCmdSet*, vkCmdBind*,
 *               vkCmdPush*), goes through a generic stub (aarch64) that hands the held-back begin to
 *               the driver first; without the stubs nothing is held back (unwrapped).
 *   secondary   begun and ended through this library, one-time submit like the primary, nothing
 *               in its inheritance pNext but VkCommandBufferInheritanceRenderingInfo and disabled
 *               conditional rendering, nothing in it this library does not follow (shader
 *               objects, nested secondaries, commands through the generic stub); exactly
 *               one vkCmdDraw or vkCmdDrawIndexed, no other draw, clear or barrier.
 *   pipeline    created through this library with all four parts (or linked from libraries that
 *               have them), no tessellation, geometry or mesh stages, no feedback loop flags, no
 *               pNext it does not know; blending off, colour write mask complete for the format and
 *               writes enabled, logic op off, rasterizer discard off, polygon mode fill, one sample,
 *               sample mask bit 0 set, alpha to coverage off, sample locations off, rasterization
 *               stream 0, no discard
 *               rectangles, no underestimating conservative rasterization; every dynamic state
 *               either harmless here or tracked and set in the secondary before the draw.
 *   fragment    the shader cannot reach a discard, demote or terminate with the pipeline's
 *               specialization constants (DXVK's D3D9 alpha test is one), nor write SampleMask
 *               (spv_fs_may_discard), nor read attachments (input attachments, tile images).
 *   viewport    viewport 0 contains every pixel centre of the render area, scissor 0 the area.
 *   geometry    the draw's vertices, read from host-mapped memory where the vertex and index buffers
 *               are bound, are run through the vertex shader on the CPU (spv_vs_run): every
 *               position is known, finite and within 1e6 w, clip and cull distances never written or not negative, Layer and
 *               ViewportIndex never written or 0, and the triangles cover every pixel centre
 *               (ls_cover: one triangle, or a quad of two triangles sharing an edge, with a margin).
 *               A pipeline keeps its last run: the same inputs, byte for byte, are not run again.
 *               The data must not be written by the GPU in any command buffer not yet reset since
 *               (copies, fills, updates, query results, transform feedback, shaders that write
 *               memory). It is read when the pass is recorded: it must not change before the pass
 *               runs (DXVK writes it before it records the draw); at each submission it is read
 *               again and a change is counted (stale).
 * Every DROIDDECK_HYBRIS_CMD_STATS_LOG_S seconds of presents a line "hybris-icd: load skip: ..."
 * counts the passes that had a colour LOAD: applied, and the ones kept by the first check that
 * failed (its reason). DROIDDECK_HYBRIS_LOAD_SKIP_FILE=<path>: "off", "full" or "all" in that file
 * applies instead (read at most twice a second at submissions and presents), for A/B runs.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include "icd.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "load_skip.h"
#include "spirv_eval.h"
#include "gpl_library_state.h"
#include "stage_inline_module.h"

#ifndef VK_KHR_unified_image_layouts
#define VK_STRUCTURE_TYPE_ATTACHMENT_FEEDBACK_LOOP_INFO_EXT ((VkStructureType)1000527001)
typedef struct VkAttachmentFeedbackLoopInfoEXT {
    VkStructureType sType;
    const void* pNext;
    VkBool32 feedbackLoopEnable;
} VkAttachmentFeedbackLoopInfoEXT;
#endif
#ifndef VK_KHR_maintenance10
#define VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_FLAGS_INFO_KHR ((VkStructureType)1000630002)
#define VK_RENDERING_ATTACHMENT_INPUT_ATTACHMENT_FEEDBACK_BIT_KHR 0x00000001
typedef VkFlags VkRenderingAttachmentFlagsKHR;
typedef struct VkRenderingAttachmentFlagsInfoKHR {
    VkStructureType sType;
    const void* pNext;
    VkRenderingAttachmentFlagsKHR flags;
} VkRenderingAttachmentFlagsInfoKHR;
#endif
#define LAYOUT_RENDERING_LOCAL_READ ((VkImageLayout)1000232000)
#define DYN_DEPTH_CLAMP_RANGE ((VkDynamicState)1000582000)

enum {
    /* render passes */
    S_BEGIN_RENDERING, S_END_RENDERING, S_RESUMING, S_SUSPENDING, S_RP_LEGACY,
    S_AREA_FULL, S_AREA_HALF, S_AREA_SMALL, S_FULL_LOAD, S_FULL_COLOR_LOAD, S_FULL_DEPTH_LOAD,
    S_C_LOAD, S_C_CLEAR, S_C_DONTCARE, S_C_NONE, S_C_STORE, S_C_STORE_DONTCARE, S_C_STORE_NONE, S_C_RESOLVE,
    S_D_LOAD, S_D_CLEAR, S_D_DONTCARE, S_D_NONE, S_D_STORE, S_D_STORE_DONTCARE, S_D_STORE_NONE, S_D_RESOLVE,
    S_S_LOAD, S_S_CLEAR,
    S_PASS_0DRAW, S_PASS_1DRAW, S_PASS_2_4DRAW, S_PASS_5DRAW, S_FULL_PASS_LE2DRAW,
    /* draws, dispatches */
    S_DRAW, S_DRAW_INDEXED, S_DRAW_INDIRECT, S_DRAW_OUTSIDE,
    S_DISPATCH, S_DISPATCH_INDIRECT, S_DISPATCH_BASE, S_EXECUTE_COMMANDS,
    S_BIND_PIPELINE_GFX, S_BIND_PIPELINE_COMPUTE,
    /* transfers */
    S_COPY_BUFFER, S_COPY_BUFFER_BYTES, S_COPY_B2I, S_COPY_B2I_REGIONS, S_COPY_B2I_KTEXELS, S_COPY_B2I_LARGE,
    S_COPY_IMAGE, S_COPY_IMAGE_KTEXELS, S_BLIT_IMAGE, S_BLIT_KTEXELS, S_RESOLVE_IMAGE, S_RESOLVE_KTEXELS,
    S_COPY_I2B, S_COPY_I2B_KTEXELS, S_FILL_BUFFER, S_UPDATE_BUFFER,
    S_CLEAR_COLOR_IMAGE, S_CLEAR_DS_IMAGE, S_CLEAR_ATTACHMENTS, S_CLEAR_ATTACHMENTS_RECTS,
    /* queries */
    S_Q_OCCLUSION, S_Q_PIPELINE_STATS, S_Q_OTHER, S_END_QUERY, S_TIMESTAMP, S_CMD_RESET_QUERY, S_COPY_QUERY_RESULTS,
    S_HOST_QUERY_RESULTS, S_HOST_RESET_QUERY,
    /* barriers */
    S_BARRIER2, S_BARRIER2_MEMORY, S_BARRIER2_BUFFER, S_BARRIER2_IMAGE, S_BARRIER2_LAYOUT, S_BARRIER2_IN_PASS, S_BARRIER1,
    S_SET_EVENT, S_WAIT_EVENTS,
    /* submissions and waits */
    S_SUBMIT, S_SUBMIT_CMDBUFS, S_WAIT_FENCES, S_WAIT_SEMAPHORES, S_QUEUE_WAIT_IDLE,
    /* labels */
    S_LABEL_BEGIN, S_LABEL_INSERT,
    /* secondary command buffers executed in a pass */
    S_SEC_PASS, S_SEC_0DRAW, S_SEC_1DRAW, S_SEC_2_4DRAW, S_SEC_5DRAW,
    S_COUNT
};

static const char* const statNames[S_COUNT] = {
    "rendering", "end-rendering", "resuming", "suspending", "renderpass-legacy",
    "area-full", "area-half", "area-small", "full-load", "full-color-load", "full-depth-load",
    "c-load", "c-clear", "c-dontcare", "c-none", "c-store", "c-store-dontcare", "c-store-none", "c-resolve",
    "d-load", "d-clear", "d-dontcare", "d-none", "d-store", "d-store-dontcare", "d-store-none", "d-resolve",
    "s-load", "s-clear",
    "pass-0draw", "pass-1draw", "pass-2-4draw", "pass-5+draw", "full-pass-le2draw",
    "draw", "draw-indexed", "draw-indirect", "draw-outside-pass",
    "dispatch", "dispatch-indirect", "dispatch-base", "execute-commands",
    "bind-gfx", "bind-compute",
    "copy-buffer", "copy-buffer-bytes", "copy-b2i", "copy-b2i-regions", "copy-b2i-ktexels", "copy-b2i-large",
    "copy-image", "copy-image-ktexels", "blit", "blit-ktexels", "resolve", "resolve-ktexels",
    "copy-i2b", "copy-i2b-ktexels", "fill-buffer", "update-buffer",
    "clear-color-image", "clear-ds-image", "clear-attachments", "clear-attachments-rects",
    "q-occlusion", "q-pipeline-stats", "q-other", "end-query", "timestamp", "cmd-reset-query", "copy-query-results",
    "host-query-results", "host-reset-query",
    "barrier2", "barrier2-memory", "barrier2-buffer", "barrier2-image", "barrier2-layout", "barrier2-in-pass", "barrier1",
    "set-event", "wait-events",
    "submit", "submit-cmdbufs", "wait-fences", "wait-semaphores", "queue-wait-idle",
    "label-begin", "label-insert",
    "sec-pass", "sec-0draw", "sec-1draw", "sec-2-4draw", "sec-5+draw"};

/* load skip outcomes: applied, then the reasons a pass with a colour LOAD kept it */
enum {
    L_APPLIED, L_COLOR_COUNT, L_FLAGS, L_INLINE, L_PNEXT, L_DEPTH, L_LAYERS, L_FEEDBACK, L_AREA, L_SMALL, L_ORDER,
    L_SECONDARY, L_DRAWS, L_BARRIER, L_PIPELINE, L_BLEND, L_MASK, L_LOGIC_OP, L_RASTER, L_DISCARD, L_DYNAMIC,
    L_VIEWPORT, L_SCISSOR, L_VERTEX, L_VS, L_GEOMETRY, L_GEOMETRY_EDGE, L_CONDITIONAL, L_UNWRAPPED, L_OFF, L_STALE, L_COUNT
};
static const char* const loadNames[L_COUNT] = {
    "applied", "color-count", "flags", "inline", "pnext", "depth", "layers", "feedback", "area", "small", "order",
    "secondary", "draws", "barrier", "pipeline", "blend", "mask", "logic-op", "raster", "discard", "dynamic",
    "viewport", "scissor", "vertex", "vs", "geometry", "geometry-edge", "conditional", "unwrapped", "off", "stale"};

static uint64_t stats[S_COUNT], logged[S_COUNT];
static uint64_t loads[L_COUNT], loadsLogged[L_COUNT];
static int statsOn = -1;
static int skipMode; /* 0 off, 1 full, 2 all */
static int skipApply; /* the same, or what DROIDDECK_HYBRIS_LOAD_SKIP_FILE says */
static const char* skipFile;
static uint64_t skipFileNs;
static bool genMissing; /* a vkCmd* was handed out without the generic wrapper */
static const char* const modeNames[3] = {"off", "full", "all"};
static uint32_t logSeconds = 10;
static uint64_t fullArea; /* pixels; 0: the largest seen */
static uint64_t maxArea;
static uint64_t presents, loggedPresents, startNs, lastLogNs;
static pthread_mutex_t logLock = PTHREAD_MUTEX_INITIALIZER;

#define ADD(i, n) (statsOn > 0 ? (void)__atomic_add_fetch(&stats[i], (uint64_t)(n), __ATOMIC_RELAXED) : (void)0)
#define INC(i) ADD(i, 1)
#define LOADC(i) __atomic_add_fetch(&loads[i], 1, __ATOMIC_RELAXED)

/* ---- labels ---- */

#define LABELS 512
#define LABEL_TEXT 160
typedef struct {
    char text[LABEL_TEXT];
    uint64_t begun, passes, draws, fullLoads, loggedBegun, loggedPasses, loggedDraws, loggedFullLoads;
} Label;
static Label labels[LABELS];
static int labelCount;
static uint64_t labelOverflow;
static pthread_mutex_t labelLock = PTHREAD_MUTEX_INITIALIZER;

#define STACK 16
static __thread int tlStack[STACK];
static __thread int tlDepth;
static __thread int tlInPass, tlPassFull;
static __thread uint64_t tlPassDraws;

/* "Render pass 123 (" -> "Render pass # (": DXVK numbers its passes. */
static void normalize(const char* in, char* out) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < LABEL_TEXT;) {
        if (!strncmp(in + i, "pass ", 5) && in[i + 5] >= '0' && in[i + 5] <= '9') {
            if (o + 7 >= LABEL_TEXT) break;
            memcpy(out + o, "pass #", 6);
            o += 6;
            i += 5;
            while (in[i] >= '0' && in[i] <= '9') i++;
            continue;
        }
        char c = in[i++];
        out[o++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    out[o] = 0;
}

static int labelIndex(const char* name) {
    char text[LABEL_TEXT];
    normalize(name ? name : "(null)", text);
    pthread_mutex_lock(&labelLock);
    int found = -1;
    for (int i = 0; i < labelCount; i++)
        if (!strcmp(labels[i].text, text)) {
            found = i;
            break;
        }
    if (found < 0 && labelCount < LABELS) {
        found = labelCount++;
        strcpy(labels[found].text, text);
    }
    if (found < 0) labelOverflow++;
    pthread_mutex_unlock(&labelLock);
    return found;
}

static int currentLabel(void) {
    return tlDepth > 0 && tlDepth <= STACK ? tlStack[tlDepth - 1] : -1;
}

/* ---- switches ---- */

/* DROIDDECK_HYBRIS_LOAD_SKIP_FILE: "off", "full" or "all" in it; skipMode when missing or anything else */
static int skipFileMode(void) {
    char buf[16];
    int fd = skipFile ? open(skipFile, O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0) return skipMode;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    buf[strcspn(buf, " \t\r\n")] = 0;
    for (int i = 0; i < 3; i++)
        if (!strcmp(buf, modeNames[i])) return i;
    return skipMode;
}

static void readEnv(void) {
    const char* on = getenv("DROIDDECK_HYBRIS_CMD_STATS");
    statsOn = on && *on && strcmp(on, "0") ? 1 : 0;
    const char* skip = getenv("DROIDDECK_HYBRIS_LOAD_SKIP");
    skipMode = skip && !strcmp(skip, "full") ? 1 : skip && !strcmp(skip, "all") ? 2 : 0;
    if (skip && *skip && strcmp(skip, "off") && strcmp(skip, "0") && !skipMode)
        fprintf(stderr, "hybris-icd: DROIDDECK_HYBRIS_LOAD_SKIP=%s: not off, full or all; off\n", skip);
    if (!statsOn && !skipMode) return;
    int seconds = icd_env_int("DROIDDECK_HYBRIS_CMD_STATS_LOG_S", 10);
    logSeconds = seconds > 0 ? (uint32_t)seconds : 10;
    const char* full = getenv("DROIDDECK_HYBRIS_CMD_STATS_FULL");
    unsigned w = 0, h = 0;
    if (full && sscanf(full, "%ux%u", &w, &h) == 2) fullArea = (uint64_t)w * h;
    startNs = lastLogNs = icd_now_ns();
    skipApply = skipMode;
    if (skipMode) {
        skipFile = getenv("DROIDDECK_HYBRIS_LOAD_SKIP_FILE");
        if (skipFile && !*skipFile) skipFile = NULL;
        skipApply = skipFileMode();
    }
    if (statsOn)
        fprintf(stderr, "hybris-icd: DROIDDECK_HYBRIS_CMD_STATS on, every %u s, full area %llu\n", logSeconds, (unsigned long long)fullArea);
    if (skipMode)
        fprintf(stderr, "hybris-icd: DROIDDECK_HYBRIS_LOAD_SKIP=%s: colour LOAD -> DONT_CARE in %s passes whose one draw covers the attachment, every %u s a line\n",
                skipMode == 1 ? "full" : "all", skipMode == 1 ? "full-size" : "all", logSeconds);
    if (skipFile)
        fprintf(stderr, "hybris-icd: DROIDDECK_HYBRIS_LOAD_SKIP_FILE=%s: off, full or all in it applies instead (now %s)\n", skipFile,
                modeNames[skipApply]);
}

static int applyMode(void) {
    return __atomic_load_n(&skipApply, __ATOMIC_RELAXED);
}

static pthread_once_t envOnce = PTHREAD_ONCE_INIT;
bool cmd_stats_on(void) {
    pthread_once(&envOnce, readEnv);
    return statsOn > 0 || skipMode > 0;
}

static bool isFull(uint64_t area) {
    uint64_t full = fullArea ? fullArea : __atomic_load_n(&maxArea, __ATOMIC_RELAXED);
    return area * 10 >= full * 9;
}

static void seeArea(uint64_t a) {
    uint64_t m = __atomic_load_n(&maxArea, __ATOMIC_RELAXED);
    while (a > m && !__atomic_compare_exchange_n(&maxArea, &m, a, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

/* ---- a map of handles ---- */

typedef struct {
    uint64_t* keys;
    void** vals;
    uint8_t* state; /* 0 empty, 1 used, 2 removed */
    uint32_t cap, used, filled;
} Map;

static uint32_t mix(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    return (uint32_t)k;
}

static void* mapGet(const Map* m, uint64_t key) {
    if (!m->cap) return NULL;
    for (uint32_t i = 0, h = mix(key) & (m->cap - 1); i < m->cap; i++, h = (h + 1) & (m->cap - 1)) {
        if (!m->state[h]) return NULL;
        if (m->state[h] == 1 && m->keys[h] == key) return m->vals[h];
    }
    return NULL;
}

static bool mapPut(Map* m, uint64_t key, void* val);

static bool mapGrow(Map* m) {
    Map n = {0};
    n.cap = m->cap ? (m->used * 4 >= m->cap ? m->cap * 2 : m->cap) : 64;
    n.keys = calloc(n.cap, sizeof *n.keys);
    n.vals = calloc(n.cap, sizeof *n.vals);
    n.state = calloc(n.cap, 1);
    if (!n.keys || !n.vals || !n.state) {
        free(n.keys), free(n.vals), free(n.state);
        return false;
    }
    for (uint32_t i = 0; i < m->cap; i++)
        if (m->state[i] == 1) mapPut(&n, m->keys[i], m->vals[i]);
    free(m->keys), free(m->vals), free(m->state);
    *m = n;
    return true;
}

static bool mapPut(Map* m, uint64_t key, void* val) {
    if ((m->filled + 1) * 2 > m->cap && !mapGrow(m)) return false;
    uint32_t h = mix(key) & (m->cap - 1), slot = UINT32_MAX;
    for (uint32_t i = 0; i < m->cap; i++, h = (h + 1) & (m->cap - 1)) {
        if (m->state[h] == 1 && m->keys[h] == key) {
            m->vals[h] = val;
            return true;
        }
        if (m->state[h] == 2 && slot == UINT32_MAX) slot = h;
        if (!m->state[h]) {
            if (slot == UINT32_MAX) {
                slot = h;
                m->filled++;
            }
            break;
        }
    }
    if (slot == UINT32_MAX) return false;
    m->keys[slot] = key;
    m->vals[slot] = val;
    m->state[slot] = 1;
    m->used++;
    return true;
}

static void* mapDel(Map* m, uint64_t key) {
    if (!m->cap) return NULL;
    for (uint32_t i = 0, h = mix(key) & (m->cap - 1); i < m->cap; i++, h = (h + 1) & (m->cap - 1)) {
        if (!m->state[h]) return NULL;
        if (m->state[h] == 1 && m->keys[h] == key) {
            m->state[h] = 2;
            m->used--;
            return m->vals[h];
        }
    }
    return NULL;
}

#define H(x) ((uint64_t)(uintptr_t)(x))

/* ---- what the load skip tracks ---- */

/* Everything below is guarded by trk. */
static pthread_mutex_t trk = PTHREAD_MUTEX_INITIALIZER;
static Map modules, pipes, images, views, buffers, memories, cbs;

typedef struct {
    SpvProgram* prog; /* NULL when it does not parse */
    bool writes;
} Module;

static void moduleFree(Module* m) {
    if (m) spv_unref(m->prog);
    free(m);
}

typedef struct {
    VkExtent3D extent;
    uint32_t mips;
    VkSampleCountFlagBits samples;
} ImageInfo;

typedef struct {
    uint32_t width, height;
    VkSampleCountFlagBits samples;
    VkFormat format;
} ViewInfo;

typedef struct {
    VkDeviceSize size;
    VkDeviceMemory memory;
    VkDeviceSize offset;
} BufInfo;

typedef struct {
    VkDeviceSize size;
    const uint8_t* ptr; /* the host address of mapOffset */
    VkDeviceSize mapOffset, mapSize;
} MemInfo;

typedef struct {
    VkSpecializationInfo info;
    VkSpecializationMapEntry* entries;
    void* data;
} SpecCopy;

#define MAX_VB 16
#define MAX_ATTRS 32

/* dynamic states: tracked here (D_*), harmless for the load skip, or neither */
enum {
    D_VIEWPORT, D_SCISSOR, D_CULL, D_FRONT, D_TOPOLOGY, D_RESTART, D_RASTER_DISCARD, D_VERTEX_INPUT, D_STRIDE,
    D_WRITE_ENABLE, D_WRITE_MASK, D_BLEND_ENABLE, D_LOGIC_OP_ENABLE, D_A2C, D_SAMPLE_MASK, D_SAMPLES, D_POLYGON, D_SAMPLE_LOCATIONS,
    D_RASTER_STREAM, D_COUNT
};

static int dynamicKind(VkDynamicState s) {
    switch ((int)s) {
    case VK_DYNAMIC_STATE_VIEWPORT: case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT: return D_VIEWPORT;
    case VK_DYNAMIC_STATE_SCISSOR: case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT: return D_SCISSOR;
    case VK_DYNAMIC_STATE_CULL_MODE: return D_CULL;
    case VK_DYNAMIC_STATE_FRONT_FACE: return D_FRONT;
    case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY: return D_TOPOLOGY;
    case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE: return D_RESTART;
    case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE: return D_RASTER_DISCARD;
    case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT: return D_VERTEX_INPUT;
    case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE: return D_STRIDE;
    case VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT: return D_WRITE_ENABLE;
    case VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT: return D_WRITE_MASK;
    case VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT: return D_BLEND_ENABLE;
    case VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT: return D_LOGIC_OP_ENABLE;
    case VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT: return D_A2C;
    case VK_DYNAMIC_STATE_SAMPLE_MASK_EXT: return D_SAMPLE_MASK;
    case VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT: return D_SAMPLES;
    case VK_DYNAMIC_STATE_POLYGON_MODE_EXT: return D_POLYGON;
    case VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_ENABLE_EXT: return D_SAMPLE_LOCATIONS;
    case VK_DYNAMIC_STATE_RASTERIZATION_STREAM_EXT: return D_RASTER_STREAM;
    /* the locations matter only when enabled, which is checked */
    case VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT:
    /* no effect on which pixels one opaque, depth-less, single-sampled triangle draw writes */
    case VK_DYNAMIC_STATE_LINE_WIDTH: case VK_DYNAMIC_STATE_DEPTH_BIAS: case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
    case VK_DYNAMIC_STATE_DEPTH_BOUNDS: case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK: case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
    case VK_DYNAMIC_STATE_STENCIL_REFERENCE: case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE: case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:
    case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP: case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE: case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE:
    case VK_DYNAMIC_STATE_STENCIL_OP: case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE: case VK_DYNAMIC_STATE_LOGIC_OP_EXT:
    case VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT: case VK_DYNAMIC_STATE_TESSELLATION_DOMAIN_ORIGIN_EXT:
    case VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT: case VK_DYNAMIC_STATE_ALPHA_TO_ONE_ENABLE_EXT:
    case VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT:
    case VK_DYNAMIC_STATE_EXTRA_PRIMITIVE_OVERESTIMATION_SIZE_EXT: case VK_DYNAMIC_STATE_DEPTH_CLIP_ENABLE_EXT:
    case VK_DYNAMIC_STATE_COLOR_BLEND_ADVANCED_EXT: case VK_DYNAMIC_STATE_PROVOKING_VERTEX_MODE_EXT:
    case VK_DYNAMIC_STATE_LINE_RASTERIZATION_MODE_EXT: case VK_DYNAMIC_STATE_LINE_STIPPLE_ENABLE_EXT:
    case VK_DYNAMIC_STATE_LINE_STIPPLE_EXT: case VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR: case DYN_DEPTH_CLAMP_RANGE:
        return -1;
    default:
        return -2;
    }
}

#define MAX_VERTS 64

/* the last vertex shader run of a pipeline: its inputs and what came of them */
typedef struct {
    SpvVertexIn in[MAX_VERTS];
    uint32_t count;
    int result;
    float pos[MAX_VERTS][4];
} VsCache;

#define PART_VI VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT
#define PART_PR VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT
#define PART_FS VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT
#define PART_FO VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT
#define PARTS_ALL (PART_VI | PART_PR | PART_FS | PART_FO)

typedef struct Pipe {
    int refs;
    bool compute;
    uint32_t parts;
    bool unknown;       /* state this library does not follow */
    bool writes;        /* a stage may write memory (or its code is not known) */
    bool feedbackLoop;
    uint32_t dynamic;   /* D_* bits */
    bool untrackedDynamic;
    /* vertex input */
    uint32_t bindingCount, attrCount;
    VkVertexInputBindingDescription bindings[MAX_VB];
    VkVertexInputAttributeDescription attrs[MAX_ATTRS];
    VkPrimitiveTopology topology;
    bool restart;
    /* pre-rasterization */
    SpvProgram* vs;
    char vsEntry[64];
    SpecCopy vsSpec;
    bool otherStages;
    uint32_t viewportCount, scissorCount;
    VkViewport viewport;
    VkRect2D scissor;
    bool rasterDiscard, negativeOneToOne;
    VkPolygonMode polygon;
    VkCullModeFlags cull;
    VkFrontFace front;
    uint32_t rasterStream;
    /* fragment shader */
    int fsDiscard; /* SPV_DISCARD_* */
    /* fragment output */
    uint32_t colorCount;
    bool blend0, logicOp, writeEnable0, a2c, sampleMaskOff, sampleLocations;
    VkColorComponentFlags mask0;
    VkSampleCountFlagBits samples;
    VsCache* cache; /* guarded by trk */
} Pipe;

static void specFree(SpecCopy* s) {
    free(s->entries);
    free(s->data);
    memset(s, 0, sizeof *s);
}

static bool specCopy(SpecCopy* dst, const VkSpecializationInfo* src) {
    memset(dst, 0, sizeof *dst);
    if (!src || !src->mapEntryCount) return true;
    if (src->mapEntryCount > 4096 || src->dataSize > 65536 || !src->pMapEntries || (src->dataSize && !src->pData)) return false;
    dst->entries = malloc(src->mapEntryCount * sizeof *dst->entries);
    dst->data = malloc(src->dataSize ? src->dataSize : 1);
    if (!dst->entries || !dst->data) {
        specFree(dst);
        return false;
    }
    memcpy(dst->entries, src->pMapEntries, src->mapEntryCount * sizeof *dst->entries);
    if (src->dataSize) memcpy(dst->data, src->pData, src->dataSize);
    dst->info = (VkSpecializationInfo){src->mapEntryCount, dst->entries, src->dataSize, dst->data};
    return true;
}

static void pipeUnref(Pipe* p) {
    if (!p || __atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL)) return;
    spv_unref(p->vs);
    specFree(&p->vsSpec);
    free(p->cache);
    free(p);
}

static void pipeRef(Pipe* p) {
    if (p) __atomic_add_fetch(&p->refs, 1, __ATOMIC_RELAXED);
}

/* ---- command buffers ---- */

typedef struct {
    VkDeviceMemory memory;
    VkDeviceSize offset, size;
} Range;

typedef struct {
    Pipe* pipe;
    uint32_t dynSet;
    uint32_t viewportCount, scissorCount;
    VkViewport viewport;
    VkRect2D scissor;
    VkCullModeFlags cull;
    VkFrontFace front;
    VkPrimitiveTopology topology;
    bool restart, rasterDiscard, a2c, logicOp, blend0, writeEnable0, sampleLocations;
    VkColorComponentFlags mask0;
    VkSampleCountFlagBits samples;
    VkSampleMask sampleMask0;
    VkPolygonMode polygon;
    uint32_t rasterStream;
    uint32_t viBindingCount, viAttrCount;
    VkVertexInputBindingDescription2EXT viBindings[MAX_VB];
    VkVertexInputAttributeDescription2EXT viAttrs[MAX_ATTRS];
    VkDeviceSize strides[MAX_VB];
    uint32_t strideSet;
    VkBuffer vb[MAX_VB];
    VkDeviceSize vbOffset[MAX_VB], vbSize[MAX_VB];
    uint32_t vbSet;
    VkBuffer ib;
    VkDeviceSize ibOffset, ibSize;
    VkIndexType ibType;
    bool ibSet;
} RecState;

typedef struct {
    RecState st;
    bool indexed;
    uint32_t count, instances, first, firstInstance;
    int32_t vertexOffset;
} DrawSnap;

typedef struct {
    VkRenderingInfo info;
    VkRenderingAttachmentInfo color;
    VkAttachmentFeedbackLoopInfoEXT feedback;
    VkRenderingAttachmentFlagsInfoKHR flags;
    VkRenderingAttachmentInfo depth, stencil;
    bool khr;
} Pending;

typedef struct {
    VkDeviceMemory memory;
    VkDeviceSize offset, size;
    uint64_t hash;
} Dep;

typedef struct Cb {
    VkCommandBuffer handle;
    VkCommandPool pool;
    bool recording, ended, ours, secondary, oneTime;
    bool unknown;     /* a command this library does not follow */
    uint32_t conditional; /* open conditional rendering blocks */
    RecState st;
    uint32_t draws, others, barriers;
    DrawSnap snap;
    bool hasSnap;
    Pipe* compute;
    bool pending;
    Pending pb;
    /* GPU writes recorded in it, live until it is reset */
    bool taintAll;
    Range* taints;
    uint32_t taintCount, taintCap;
    Dep* deps;
    uint32_t depCount, depCap;
} Cb;

static uint32_t taintAllCount;

static void cbClear(Cb* c) {
    pipeUnref(c->st.pipe);
    if (c->hasSnap) pipeUnref(c->snap.st.pipe);
    pipeUnref(c->compute);
    if (c->taintAll) taintAllCount--;
    Range* taints = c->taints;
    uint32_t taintCap = c->taintCap;
    Dep* deps = c->deps;
    uint32_t depCap = c->depCap;
    VkCommandBuffer handle = c->handle;
    VkCommandPool pool = c->pool;
    memset(c, 0, sizeof *c);
    c->handle = handle;
    c->pool = pool;
    c->taints = taints;
    c->taintCap = taintCap;
    c->deps = deps;
    c->depCap = depCap;
}

/* The record of a command buffer (records are never freed: a handle keeps its record). trk held. */
static Cb* cbGet(VkCommandBuffer handle, bool create) {
    Cb* c = mapGet(&cbs, H(handle));
    if (!c && create) {
        c = calloc(1, sizeof *c);
        if (!c) return NULL;
        c->handle = handle;
        if (!mapPut(&cbs, H(handle), c)) {
            free(c);
            return NULL;
        }
    }
    return c;
}

static __thread VkCommandBuffer tlCb;
static __thread Cb* tlRec;

static Cb* cbOf(VkCommandBuffer handle) {
    if (handle == tlCb && tlRec) return tlRec;
    pthread_mutex_lock(&trk);
    Cb* c = cbGet(handle, true);
    pthread_mutex_unlock(&trk);
    tlCb = handle;
    tlRec = c;
    return c;
}

static bool addTaint(Cb* c, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) {
    pthread_mutex_lock(&trk);
    BufInfo* b = mapGet(&buffers, H(buffer));
    bool ok = b && b->memory;
    if (ok) {
        if (size == VK_WHOLE_SIZE) size = offset < b->size ? b->size - offset : 0;
        if (c->taintCount == c->taintCap) {
            uint32_t cap = c->taintCap ? c->taintCap * 2 : 16;
            Range* t = realloc(c->taints, cap * sizeof *t);
            if (t) c->taints = t, c->taintCap = cap;
        }
        if (c->taintCount < c->taintCap) c->taints[c->taintCount++] = (Range){b->memory, b->offset + offset, size};
        else ok = false;
    }
    if (!ok && !c->taintAll) {
        /* a buffer not known here: anything may have been written */
        c->taintAll = true;
        taintAllCount++;
    }
    pthread_mutex_unlock(&trk);
    return ok;
}

static void taintEverything(Cb* c) {
    pthread_mutex_lock(&trk);
    if (!c->taintAll) {
        c->taintAll = true;
        taintAllCount++;
    }
    pthread_mutex_unlock(&trk);
}

/* trk held: whether the GPU may write [offset, offset + size) of memory in a recorded command */
static bool tainted(VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size) {
    if (taintAllCount) return true;
    for (uint32_t i = 0; i < cbs.cap; i++) {
        if (cbs.state[i] != 1) continue;
        const Cb* c = cbs.vals[i];
        for (uint32_t j = 0; j < c->taintCount; j++) {
            const Range* r = &c->taints[j];
            if (r->memory == memory && r->offset < offset + size && offset < r->offset + r->size) return true;
        }
    }
    return false;
}

static uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* ---- wrappers: slots ---- */

enum {
    W_BEGIN_RENDERING, W_BEGIN_RENDERING_KHR, W_END_RENDERING, W_END_RENDERING_KHR, W_BEGIN_RENDER_PASS, W_BEGIN_RENDER_PASS2,
    W_DRAW, W_DRAW_INDEXED, W_DRAW_INDIRECT, W_DRAW_INDEXED_INDIRECT, W_DRAW_INDIRECT_COUNT, W_DRAW_INDEXED_INDIRECT_COUNT,
    W_DRAW_MULTI, W_DRAW_MULTI_INDEXED, W_DRAW_BYTE_COUNT, W_DRAW_MESH, W_DRAW_MESH_INDIRECT, W_DRAW_MESH_INDIRECT_COUNT,
    W_DISPATCH, W_DISPATCH_INDIRECT, W_DISPATCH_BASE, W_EXECUTE_COMMANDS, W_BIND_PIPELINE, W_BIND_SHADERS,
    W_COPY_BUFFER, W_COPY_BUFFER2, W_COPY_B2I, W_COPY_B2I2, W_COPY_IMAGE, W_COPY_IMAGE2, W_BLIT_IMAGE, W_BLIT_IMAGE2,
    W_RESOLVE_IMAGE, W_RESOLVE_IMAGE2, W_COPY_I2B, W_COPY_I2B2, W_FILL_BUFFER, W_UPDATE_BUFFER,
    W_CLEAR_COLOR_IMAGE, W_CLEAR_DS_IMAGE, W_CLEAR_ATTACHMENTS,
    W_CREATE_QUERY_POOL, W_BEGIN_QUERY, W_END_QUERY, W_BEGIN_QUERY_INDEXED, W_END_QUERY_INDEXED, W_WRITE_TIMESTAMP,
    W_WRITE_TIMESTAMP2, W_CMD_RESET_QUERY, W_COPY_QUERY_RESULTS, W_GET_QUERY_RESULTS, W_RESET_QUERY_POOL,
    W_BARRIER2, W_BARRIER2_KHR, W_BARRIER1, W_SET_EVENT2, W_WAIT_EVENTS2, W_WAIT_EVENTS,
    W_SUBMIT, W_SUBMIT2, W_SUBMIT2_KHR, W_WAIT_FENCES, W_WAIT_SEMAPHORES, W_QUEUE_WAIT_IDLE, W_PRESENT,
    W_LABEL_BEGIN, W_LABEL_END, W_LABEL_INSERT,
    W_BEGIN_CB, W_END_CB, W_RESET_CB, W_ALLOCATE_CBS, W_FREE_CBS, W_RESET_POOL, W_DESTROY_POOL,
    W_CREATE_GFX_PIPELINES, W_CREATE_COMPUTE_PIPELINES, W_DESTROY_PIPELINE, W_CREATE_SHADER_MODULE, W_DESTROY_SHADER_MODULE,
    W_CREATE_IMAGE, W_DESTROY_IMAGE, W_CREATE_IMAGE_VIEW, W_DESTROY_IMAGE_VIEW, W_CREATE_BUFFER, W_DESTROY_BUFFER,
    W_BIND_BUFFER_MEMORY, W_BIND_BUFFER_MEMORY2, W_ALLOCATE_MEMORY, W_FREE_MEMORY, W_MAP_MEMORY, W_MAP_MEMORY2,
    W_UNMAP_MEMORY, W_UNMAP_MEMORY2,
    W_BIND_VERTEX_BUFFERS, W_BIND_VERTEX_BUFFERS2, W_BIND_INDEX_BUFFER, W_BIND_INDEX_BUFFER2, W_BIND_XFB_BUFFERS,
    W_SET_VIEWPORT, W_SET_VIEWPORT_COUNT, W_SET_SCISSOR, W_SET_SCISSOR_COUNT, W_SET_CULL, W_SET_FRONT, W_SET_TOPOLOGY,
    W_SET_RESTART, W_SET_RASTER_DISCARD, W_SET_VERTEX_INPUT, W_SET_WRITE_ENABLE, W_SET_WRITE_MASK, W_SET_BLEND_ENABLE,
    W_SET_LOGIC_OP_ENABLE, W_SET_A2C, W_SET_SAMPLE_MASK, W_SET_SAMPLES, W_SET_POLYGON, W_SET_SAMPLE_LOCATIONS,
    W_SET_RASTER_STREAM, W_BEGIN_CONDITIONAL, W_END_CONDITIONAL,
    W_COUNT
};
static const char* const wrapNames[W_COUNT] = {
    "vkCmdBeginRendering", "vkCmdBeginRenderingKHR", "vkCmdEndRendering", "vkCmdEndRenderingKHR", "vkCmdBeginRenderPass",
    "vkCmdBeginRenderPass2",
    "vkCmdDraw", "vkCmdDrawIndexed", "vkCmdDrawIndirect", "vkCmdDrawIndexedIndirect", "vkCmdDrawIndirectCount",
    "vkCmdDrawIndexedIndirectCount", "vkCmdDrawMultiEXT", "vkCmdDrawMultiIndexedEXT", "vkCmdDrawIndirectByteCountEXT",
    "vkCmdDrawMeshTasksEXT", "vkCmdDrawMeshTasksIndirectEXT", "vkCmdDrawMeshTasksIndirectCountEXT",
    "vkCmdDispatch", "vkCmdDispatchIndirect", "vkCmdDispatchBase", "vkCmdExecuteCommands", "vkCmdBindPipeline", "vkCmdBindShadersEXT",
    "vkCmdCopyBuffer", "vkCmdCopyBuffer2", "vkCmdCopyBufferToImage", "vkCmdCopyBufferToImage2", "vkCmdCopyImage", "vkCmdCopyImage2",
    "vkCmdBlitImage", "vkCmdBlitImage2", "vkCmdResolveImage", "vkCmdResolveImage2", "vkCmdCopyImageToBuffer", "vkCmdCopyImageToBuffer2",
    "vkCmdFillBuffer", "vkCmdUpdateBuffer",
    "vkCmdClearColorImage", "vkCmdClearDepthStencilImage", "vkCmdClearAttachments",
    "vkCreateQueryPool", "vkCmdBeginQuery", "vkCmdEndQuery", "vkCmdBeginQueryIndexedEXT", "vkCmdEndQueryIndexedEXT",
    "vkCmdWriteTimestamp", "vkCmdWriteTimestamp2", "vkCmdResetQueryPool", "vkCmdCopyQueryPoolResults", "vkGetQueryPoolResults",
    "vkResetQueryPool",
    "vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR", "vkCmdPipelineBarrier", "vkCmdSetEvent2", "vkCmdWaitEvents2", "vkCmdWaitEvents",
    "vkQueueSubmit", "vkQueueSubmit2", "vkQueueSubmit2KHR", "vkWaitForFences", "vkWaitSemaphores", "vkQueueWaitIdle", "vkQueuePresentKHR",
    "vkCmdBeginDebugUtilsLabelEXT", "vkCmdEndDebugUtilsLabelEXT", "vkCmdInsertDebugUtilsLabelEXT",
    "vkBeginCommandBuffer", "vkEndCommandBuffer", "vkResetCommandBuffer", "vkAllocateCommandBuffers", "vkFreeCommandBuffers",
    "vkResetCommandPool", "vkDestroyCommandPool",
    "vkCreateGraphicsPipelines", "vkCreateComputePipelines", "vkDestroyPipeline", "vkCreateShaderModule", "vkDestroyShaderModule",
    "vkCreateImage", "vkDestroyImage", "vkCreateImageView", "vkDestroyImageView", "vkCreateBuffer", "vkDestroyBuffer",
    "vkBindBufferMemory", "vkBindBufferMemory2", "vkAllocateMemory", "vkFreeMemory", "vkMapMemory", "vkMapMemory2",
    "vkUnmapMemory", "vkUnmapMemory2",
    "vkCmdBindVertexBuffers", "vkCmdBindVertexBuffers2", "vkCmdBindIndexBuffer", "vkCmdBindIndexBuffer2",
    "vkCmdBindTransformFeedbackBuffersEXT",
    "vkCmdSetViewport", "vkCmdSetViewportWithCount", "vkCmdSetScissor", "vkCmdSetScissorWithCount", "vkCmdSetCullMode",
    "vkCmdSetFrontFace", "vkCmdSetPrimitiveTopology", "vkCmdSetPrimitiveRestartEnable", "vkCmdSetRasterizerDiscardEnable",
    "vkCmdSetVertexInputEXT", "vkCmdSetColorWriteEnableEXT", "vkCmdSetColorWriteMaskEXT", "vkCmdSetColorBlendEnableEXT",
    "vkCmdSetLogicOpEnableEXT", "vkCmdSetAlphaToCoverageEnableEXT", "vkCmdSetSampleMaskEXT", "vkCmdSetRasterizationSamplesEXT",
    "vkCmdSetPolygonModeEXT", "vkCmdSetSampleLocationsEnableEXT",
    "vkCmdSetRasterizationStreamEXT", "vkCmdBeginConditionalRenderingEXT", "vkCmdEndConditionalRenderingEXT"};
/* aliases handled by the same wrapper slot */
static const struct {
    const char* name;
    int index;
} aliases[] = {
    {"vkCmdBeginRenderPass2KHR", W_BEGIN_RENDER_PASS2}, {"vkCmdDrawIndirectCountKHR", W_DRAW_INDIRECT_COUNT},
    {"vkCmdDrawIndirectCountAMD", W_DRAW_INDIRECT_COUNT}, {"vkCmdDrawIndexedIndirectCountKHR", W_DRAW_INDEXED_INDIRECT_COUNT},
    {"vkCmdDrawIndexedIndirectCountAMD", W_DRAW_INDEXED_INDIRECT_COUNT}, {"vkCmdDispatchBaseKHR", W_DISPATCH_BASE},
    {"vkCmdCopyBuffer2KHR", W_COPY_BUFFER2}, {"vkCmdCopyBufferToImage2KHR", W_COPY_B2I2}, {"vkCmdCopyImage2KHR", W_COPY_IMAGE2},
    {"vkCmdBlitImage2KHR", W_BLIT_IMAGE2}, {"vkCmdResolveImage2KHR", W_RESOLVE_IMAGE2}, {"vkCmdCopyImageToBuffer2KHR", W_COPY_I2B2},
    {"vkCmdWriteTimestamp2KHR", W_WRITE_TIMESTAMP2}, {"vkCmdSetEvent2KHR", W_SET_EVENT2}, {"vkCmdWaitEvents2KHR", W_WAIT_EVENTS2},
    {"vkResetQueryPoolEXT", W_RESET_QUERY_POOL}, {"vkWaitSemaphoresKHR", W_WAIT_SEMAPHORES},
    {"vkBindBufferMemory2KHR", W_BIND_BUFFER_MEMORY2}, {"vkMapMemory2KHR", W_MAP_MEMORY2}, {"vkUnmapMemory2KHR", W_UNMAP_MEMORY2},
    {"vkCmdBindVertexBuffers2EXT", W_BIND_VERTEX_BUFFERS2}, {"vkCmdBindIndexBuffer2KHR", W_BIND_INDEX_BUFFER2},
    {"vkCmdSetViewportWithCountEXT", W_SET_VIEWPORT_COUNT}, {"vkCmdSetScissorWithCountEXT", W_SET_SCISSOR_COUNT},
    {"vkCmdSetCullModeEXT", W_SET_CULL}, {"vkCmdSetFrontFaceEXT", W_SET_FRONT}, {"vkCmdSetPrimitiveTopologyEXT", W_SET_TOPOLOGY},
    {"vkCmdSetPrimitiveRestartEnableEXT", W_SET_RESTART}, {"vkCmdSetRasterizerDiscardEnableEXT", W_SET_RASTER_DISCARD}};

static PFN_vkVoidFunction nextFn[W_COUNT];
#define N(i, type) ((type)__atomic_load_n(&nextFn[i], __ATOMIC_ACQUIRE))
#define TRACK (skipMode > 0)
#define RECORD (skipMode > 0 || statsOn > 0)

static uint64_t kt(const VkExtent3D* e) {
    return (uint64_t)e->width * e->height * (e->depth ? e->depth : 1);
}

/* query pool -> type */
#define POOLS 1024
static struct {
    uint64_t pool;
    VkQueryType type;
} pools[POOLS];
static pthread_mutex_t poolLock = PTHREAD_MUTEX_INITIALIZER;

static VkQueryType poolType(VkQueryPool pool) {
    uint64_t key = (uint64_t)pool;
    VkQueryType type = VK_QUERY_TYPE_MAX_ENUM;
    pthread_mutex_lock(&poolLock);
    for (unsigned i = 0, h = (unsigned)(key >> 4) % POOLS; i < POOLS; i++, h = (h + 1) % POOLS) {
        if (!pools[h].pool) break;
        if (pools[h].pool == key) {
            type = pools[h].type;
            break;
        }
    }
    pthread_mutex_unlock(&poolLock);
    return type;
}

static VkResult VKAPI_CALL CreateQueryPool(VkDevice d, const VkQueryPoolCreateInfo* info, const VkAllocationCallbacks* a, VkQueryPool* pool) {
    VkResult r = N(W_CREATE_QUERY_POOL, PFN_vkCreateQueryPool)(d, info, a, pool);
    if (r == VK_SUCCESS && info && statsOn > 0) {
        uint64_t key = (uint64_t)*pool;
        pthread_mutex_lock(&poolLock);
        for (unsigned i = 0, h = (unsigned)(key >> 4) % POOLS; i < POOLS; i++, h = (h + 1) % POOLS)
            if (!pools[h].pool || pools[h].pool == key) {
                pools[h].pool = key;
                pools[h].type = info->queryType;
                break;
            }
        pthread_mutex_unlock(&poolLock);
    }
    return r;
}

static const void* findNext(const void* chain, VkStructureType type) {
    for (const VkBaseInStructure* b = chain; b; b = b->pNext)
        if (b->sType == type) return b;
    return NULL;
}

/* ---- resources ---- */

static VkResult VKAPI_CALL CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* a,
                                              VkShaderModule* module) {
    VkResult r = N(W_CREATE_SHADER_MODULE, PFN_vkCreateShaderModule)(d, info, a, module);
    if (r == VK_SUCCESS && TRACK && info) {
        Module* m = calloc(1, sizeof *m);
        if (m) {
            m->prog = spv_parse(info->pCode, info->codeSize);
            m->writes = m->prog ? spv_writes_memory(m->prog) : spv_code_writes_memory(info->pCode, info->codeSize);
        }
        pthread_mutex_lock(&trk);
        moduleFree(mapDel(&modules, H(*module)));
        if (m && !mapPut(&modules, H(*module), m)) moduleFree(m);
        pthread_mutex_unlock(&trk);
    }
    return r;
}

static void VKAPI_CALL DestroyShaderModule(VkDevice d, VkShaderModule module, const VkAllocationCallbacks* a) {
    if (TRACK && module) {
        pthread_mutex_lock(&trk);
        moduleFree(mapDel(&modules, H(module)));
        pthread_mutex_unlock(&trk);
    }
    N(W_DESTROY_SHADER_MODULE, PFN_vkDestroyShaderModule)(d, module, a);
}

static VkResult VKAPI_CALL CreateImage(VkDevice d, const VkImageCreateInfo* info, const VkAllocationCallbacks* a, VkImage* image) {
    VkResult r = N(W_CREATE_IMAGE, PFN_vkCreateImage)(d, info, a, image);
    if (r == VK_SUCCESS && TRACK && info) {
        ImageInfo* i = malloc(sizeof *i);
        pthread_mutex_lock(&trk);
        free(mapDel(&images, H(*image)));
        if (i) {
            *i = (ImageInfo){info->extent, info->mipLevels, info->samples};
            if (!mapPut(&images, H(*image), i)) free(i);
        }
        pthread_mutex_unlock(&trk);
    }
    return r;
}

static void VKAPI_CALL DestroyImage(VkDevice d, VkImage image, const VkAllocationCallbacks* a) {
    if (TRACK && image) {
        pthread_mutex_lock(&trk);
        free(mapDel(&images, H(image)));
        pthread_mutex_unlock(&trk);
    }
    N(W_DESTROY_IMAGE, PFN_vkDestroyImage)(d, image, a);
}

static VkResult VKAPI_CALL CreateImageView(VkDevice d, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* a, VkImageView* view) {
    VkResult r = N(W_CREATE_IMAGE_VIEW, PFN_vkCreateImageView)(d, info, a, view);
    if (r == VK_SUCCESS && TRACK && info) {
        pthread_mutex_lock(&trk);
        free(mapDel(&views, H(*view)));
        const ImageInfo* im = mapGet(&images, H(info->image));
        uint32_t mip = info->subresourceRange.baseMipLevel;
        if (im && mip < im->mips && mip < 32 && (info->subresourceRange.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT)) {
            ViewInfo* v = malloc(sizeof *v);
            if (v) {
                uint32_t w = im->extent.width >> mip, h = im->extent.height >> mip;
                *v = (ViewInfo){w ? w : 1, h ? h : 1, im->samples, info->format};
                if (!mapPut(&views, H(*view), v)) free(v);
            }
        }
        pthread_mutex_unlock(&trk);
    }
    return r;
}

static void VKAPI_CALL DestroyImageView(VkDevice d, VkImageView view, const VkAllocationCallbacks* a) {
    if (TRACK && view) {
        pthread_mutex_lock(&trk);
        free(mapDel(&views, H(view)));
        pthread_mutex_unlock(&trk);
    }
    N(W_DESTROY_IMAGE_VIEW, PFN_vkDestroyImageView)(d, view, a);
}

static VkResult VKAPI_CALL CreateBuffer(VkDevice d, const VkBufferCreateInfo* info, const VkAllocationCallbacks* a, VkBuffer* buffer) {
    VkResult r = N(W_CREATE_BUFFER, PFN_vkCreateBuffer)(d, info, a, buffer);
    if (r == VK_SUCCESS && TRACK && info) {
        BufInfo* b = calloc(1, sizeof *b);
        pthread_mutex_lock(&trk);
        free(mapDel(&buffers, H(*buffer)));
        if (b) {
            b->size = info->size;
            if (!mapPut(&buffers, H(*buffer), b)) free(b);
        }
        pthread_mutex_unlock(&trk);
    }
    return r;
}

static void VKAPI_CALL DestroyBuffer(VkDevice d, VkBuffer buffer, const VkAllocationCallbacks* a) {
    if (TRACK && buffer) {
        pthread_mutex_lock(&trk);
        free(mapDel(&buffers, H(buffer)));
        pthread_mutex_unlock(&trk);
    }
    N(W_DESTROY_BUFFER, PFN_vkDestroyBuffer)(d, buffer, a);
}

static void bound(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    pthread_mutex_lock(&trk);
    BufInfo* b = mapGet(&buffers, H(buffer));
    if (b) b->memory = memory, b->offset = offset;
    pthread_mutex_unlock(&trk);
}

static VkResult VKAPI_CALL BindBufferMemory(VkDevice d, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    VkResult r = N(W_BIND_BUFFER_MEMORY, PFN_vkBindBufferMemory)(d, buffer, memory, offset);
    if (r == VK_SUCCESS && TRACK) bound(buffer, memory, offset);
    return r;
}

static VkResult VKAPI_CALL BindBufferMemory2(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo* infos) {
    VkResult r = N(W_BIND_BUFFER_MEMORY2, PFN_vkBindBufferMemory2)(d, n, infos);
    for (uint32_t i = 0; TRACK && infos && i < n; i++) {
        /* with VkBindMemoryStatus a failed one is not bound: forget the binding of all */
        bound(infos[i].buffer, r == VK_SUCCESS ? infos[i].memory : VK_NULL_HANDLE, infos[i].memoryOffset);
    }
    return r;
}

static VkResult VKAPI_CALL AllocateMemory(VkDevice d, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* a, VkDeviceMemory* memory) {
    VkResult r = N(W_ALLOCATE_MEMORY, PFN_vkAllocateMemory)(d, info, a, memory);
    if (r == VK_SUCCESS && TRACK && info) {
        MemInfo* m = calloc(1, sizeof *m);
        pthread_mutex_lock(&trk);
        free(mapDel(&memories, H(*memory)));
        if (m) {
            m->size = info->allocationSize;
            if (!mapPut(&memories, H(*memory), m)) free(m);
        }
        pthread_mutex_unlock(&trk);
    }
    return r;
}

static void VKAPI_CALL FreeMemory(VkDevice d, VkDeviceMemory memory, const VkAllocationCallbacks* a) {
    /* held across the call: no pass reads it meanwhile */
    if (TRACK && memory) pthread_mutex_lock(&trk), free(mapDel(&memories, H(memory)));
    N(W_FREE_MEMORY, PFN_vkFreeMemory)(d, memory, a);
    if (TRACK && memory) pthread_mutex_unlock(&trk);
}

static void mapped(VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, void* ptr) {
    pthread_mutex_lock(&trk);
    MemInfo* m = mapGet(&memories, H(memory));
    if (m) {
        m->ptr = ptr;
        m->mapOffset = offset;
        m->mapSize = size == VK_WHOLE_SIZE ? (offset < m->size ? m->size - offset : 0) : size;
    }
    pthread_mutex_unlock(&trk);
}

static VkResult VKAPI_CALL MapMemory(VkDevice d, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags,
                                     void** data) {
    VkResult r = N(W_MAP_MEMORY, PFN_vkMapMemory)(d, memory, offset, size, flags, data);
    if (r == VK_SUCCESS && TRACK && data) mapped(memory, offset, size, *data);
    return r;
}

static VkResult VKAPI_CALL MapMemory2(VkDevice d, const VkMemoryMapInfoKHR* info, void** data) {
    VkResult r = N(W_MAP_MEMORY2, PFN_vkMapMemory2KHR)(d, info, data);
    if (r == VK_SUCCESS && TRACK && info && data) mapped(info->memory, info->offset, info->size, *data);
    return r;
}

static void VKAPI_CALL UnmapMemory(VkDevice d, VkDeviceMemory memory) {
    if (TRACK) pthread_mutex_lock(&trk);
    MemInfo* m = TRACK ? mapGet(&memories, H(memory)) : NULL;
    if (m) m->ptr = NULL;
    N(W_UNMAP_MEMORY, PFN_vkUnmapMemory)(d, memory);
    if (TRACK) pthread_mutex_unlock(&trk);
}

static VkResult VKAPI_CALL UnmapMemory2(VkDevice d, const VkMemoryUnmapInfoKHR* info) {
    if (TRACK) pthread_mutex_lock(&trk);
    MemInfo* m = TRACK && info ? mapGet(&memories, H(info->memory)) : NULL;
    if (m) m->ptr = NULL;
    VkResult r = N(W_UNMAP_MEMORY2, PFN_vkUnmapMemory2KHR)(d, info);
    if (TRACK) pthread_mutex_unlock(&trk);
    return r;
}

/* ---- pipelines ---- */

/* the stage's code: its module's, or its inline VkShaderModuleCreateInfo; NULL when not known
 * (a module identifier, code that does not parse). *writes: whether it may write memory. */
static SpvProgram* stageProgram(const VkPipelineShaderStageCreateInfo* s, bool* writes) {
    const VkShaderModuleCreateInfo* inl = dd_inline_module(s->pNext);
    *writes = true;
    if (s->module) {
        pthread_mutex_lock(&trk);
        const Module* m = mapGet(&modules, H(s->module));
        SpvProgram* p = m ? m->prog : NULL;
        spv_ref(p);
        if (m) *writes = m->writes;
        pthread_mutex_unlock(&trk);
        return p;
    }
    if (!inl) return NULL;
    SpvProgram* p = spv_parse(inl->pCode, inl->codeSize);
    *writes = p ? spv_writes_memory(p) : spv_code_writes_memory(inl->pCode, inl->codeSize);
    return p;
}

static bool knownPipelineNext(VkStructureType t) {
    switch (t) {
    case VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR: case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT:
    case VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR: case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO:
    case VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO: case VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT:
    case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR:
    case VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT:
        return true;
    default:
        return false;
    }
}

static void copyPart(Pipe* p, const Pipe* lib, uint32_t part) {
    if (part == PART_VI) {
        p->bindingCount = lib->bindingCount, p->attrCount = lib->attrCount;
        memcpy(p->bindings, lib->bindings, sizeof p->bindings);
        memcpy(p->attrs, lib->attrs, sizeof p->attrs);
        p->topology = lib->topology, p->restart = lib->restart;
    } else if (part == PART_PR) {
        spv_ref(lib->vs);
        spv_unref(p->vs);
        p->vs = lib->vs;
        memcpy(p->vsEntry, lib->vsEntry, sizeof p->vsEntry);
        specFree(&p->vsSpec);
        if (!specCopy(&p->vsSpec, lib->vsSpec.entries ? &lib->vsSpec.info : NULL)) p->unknown = true;
        p->otherStages = lib->otherStages;
        p->viewportCount = lib->viewportCount, p->scissorCount = lib->scissorCount;
        p->viewport = lib->viewport, p->scissor = lib->scissor;
        p->rasterDiscard = lib->rasterDiscard, p->negativeOneToOne = lib->negativeOneToOne;
        p->polygon = lib->polygon, p->cull = lib->cull, p->front = lib->front;
        p->rasterStream = lib->rasterStream;
    } else if (part == PART_FS) {
        p->fsDiscard = lib->fsDiscard;
    } else {
        p->colorCount = lib->colorCount;
        p->blend0 = lib->blend0, p->logicOp = lib->logicOp, p->writeEnable0 = lib->writeEnable0;
        p->a2c = lib->a2c, p->sampleMaskOff = lib->sampleMaskOff, p->mask0 = lib->mask0, p->samples = lib->samples;
        p->sampleLocations = lib->sampleLocations;
    }
}

static Pipe* pipeFromInfo(const VkGraphicsPipelineCreateInfo* ci) {
    Pipe* p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->refs = 1;
    p->fsDiscard = SPV_DISCARD_UNKNOWN;
    for (const VkBaseInStructure* s = ci->pNext; s; s = s->pNext)
        if (!knownPipelineNext(s->sType)) p->unknown = true;
    const VkPipelineCreateFlags2CreateInfoKHR* f2 = findNext(ci->pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR);
    uint64_t flags = f2 ? f2->flags : ci->flags;
    if (flags & (VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT | VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT))
        p->feedbackLoop = true;
    uint32_t libraryCount = 0;
    uint32_t own = dd_gpl_parts(ci, &libraryCount);
    const VkPipelineLibraryCreateInfoKHR* libs = findNext(ci->pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);
    if (libs && libraryCount && !findNext(ci->pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT) &&
        !(flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR))
        own = 0;
    for (uint32_t i = 0; libs && i < libs->libraryCount; i++) {
        pthread_mutex_lock(&trk);
        Pipe* lib = mapGet(&pipes, H(libs->pLibraries[i]));
        pipeRef(lib);
        pthread_mutex_unlock(&trk);
        if (!lib || lib->compute) {
            p->unknown = true;
            pipeUnref(lib);
            continue;
        }
        p->unknown |= lib->unknown;
        p->writes |= lib->writes;
        p->feedbackLoop |= lib->feedbackLoop;
        p->dynamic |= lib->dynamic;
        p->untrackedDynamic |= lib->untrackedDynamic;
        for (uint32_t part = 1; part <= PART_FO; part <<= 1)
            if ((lib->parts & part) && !(p->parts & part)) {
                copyPart(p, lib, part);
                p->parts |= part;
            }
        pipeUnref(lib);
    }
    own &= ~p->parts;
    p->parts |= own;
    if (ci->pDynamicState && (own & PARTS_ALL))
        for (uint32_t i = 0; i < ci->pDynamicState->dynamicStateCount; i++) {
            int k = dynamicKind(ci->pDynamicState->pDynamicStates[i]);
            if (k >= 0) p->dynamic |= 1u << k;
            else if (k == -2) p->untrackedDynamic = true;
        }
    if (own & PART_VI) {
        const VkPipelineVertexInputStateCreateInfo* vi = ci->pVertexInputState;
        const VkPipelineInputAssemblyStateCreateInfo* ia = ci->pInputAssemblyState;
        if (vi && vi->vertexBindingDescriptionCount <= MAX_VB && vi->vertexAttributeDescriptionCount <= MAX_ATTRS) {
            p->bindingCount = vi->vertexBindingDescriptionCount;
            p->attrCount = vi->vertexAttributeDescriptionCount;
            if (p->bindingCount) memcpy(p->bindings, vi->pVertexBindingDescriptions, p->bindingCount * sizeof *p->bindings);
            if (p->attrCount) memcpy(p->attrs, vi->pVertexAttributeDescriptions, p->attrCount * sizeof *p->attrs);
        } else if (!(p->dynamic & (1u << D_VERTEX_INPUT))) {
            p->unknown = true;
        }
        if (ia) p->topology = ia->topology, p->restart = ia->primitiveRestartEnable;
        else if (!(p->dynamic & (1u << D_TOPOLOGY))) p->unknown = true;
    }
    if (own & PART_PR) {
        for (uint32_t i = 0; i < ci->stageCount && ci->pStages; i++) {
            const VkPipelineShaderStageCreateInfo* s = &ci->pStages[i];
            if (s->stage == VK_SHADER_STAGE_FRAGMENT_BIT) continue;
            bool writes;
            SpvProgram* prog = stageProgram(s, &writes);
            p->writes |= writes;
            if (s->stage == VK_SHADER_STAGE_VERTEX_BIT && prog && s->pName && strlen(s->pName) < sizeof p->vsEntry &&
                spv_has_entry(prog, s->pName, 0) && !p->vs) {
                p->vs = prog;
                strcpy(p->vsEntry, s->pName);
                if (!specCopy(&p->vsSpec, s->pSpecializationInfo)) p->unknown = true;
                continue;
            }
            if (s->stage != VK_SHADER_STAGE_VERTEX_BIT) p->otherStages = true;
            spv_unref(prog);
        }
        const VkPipelineViewportStateCreateInfo* vp = ci->pViewportState;
        const VkPipelineRasterizationStateCreateInfo* rs = ci->pRasterizationState;
        if (vp) {
            for (const VkBaseInStructure* s = vp->pNext; s; s = s->pNext) {
                if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT)
                    p->negativeOneToOne = ((const VkPipelineViewportDepthClipControlCreateInfoEXT*)s)->negativeOneToOne;
                else
                    p->unknown = true;
            }
            p->viewportCount = vp->viewportCount, p->scissorCount = vp->scissorCount;
            if (vp->pViewports && vp->viewportCount) p->viewport = vp->pViewports[0];
            if (vp->pScissors && vp->scissorCount) p->scissor = vp->pScissors[0];
        }
        if (rs) {
            p->rasterDiscard = rs->rasterizerDiscardEnable;
            p->polygon = rs->polygonMode, p->cull = rs->cullMode, p->front = rs->frontFace;
            for (const VkBaseInStructure* s = rs->pNext; s; s = s->pNext) {
                if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT) {
                    if (((const VkPipelineRasterizationConservativeStateCreateInfoEXT*)s)->conservativeRasterizationMode ==
                        VK_CONSERVATIVE_RASTERIZATION_MODE_UNDERESTIMATE_EXT)
                        p->unknown = true;
                } else if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT) {
                    p->rasterStream = ((const VkPipelineRasterizationStateStreamCreateInfoEXT*)s)->rasterizationStream;
                } else if (s->sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT &&
                           s->sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT &&
                           s->sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT) {
                    p->unknown = true;
                }
            }
        } else if (!(p->dynamic & (1u << D_RASTER_DISCARD))) {
            p->unknown = true;
        }
        const VkPipelineDiscardRectangleStateCreateInfoEXT* dr =
            findNext(ci->pNext, VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT);
        if (dr && dr->discardRectangleCount) p->unknown = true;
        const VkPipelineRenderingCreateInfo* ri = findNext(ci->pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
        if (ri && ri->viewMask) p->unknown = true;
    }
    if (own & PART_FS) {
        for (uint32_t i = 0; i < ci->stageCount && ci->pStages; i++) {
            const VkPipelineShaderStageCreateInfo* s = &ci->pStages[i];
            if (s->stage != VK_SHADER_STAGE_FRAGMENT_BIT) continue;
            bool writes;
            SpvProgram* prog = stageProgram(s, &writes);
            p->writes |= writes;
            p->fsDiscard = prog && s->pName ? spv_fs_may_discard(prog, s->pName, s->pSpecializationInfo) : SPV_DISCARD_UNKNOWN;
            /* input attachments (dynamic rendering local read) or tile images: the attachment's old contents */
            if (prog && spv_reads_attachments(prog)) p->feedbackLoop = true;
            spv_unref(prog);
        }
    }
    if (own & PART_FO) {
        const VkPipelineColorBlendStateCreateInfo* cb = ci->pColorBlendState;
        const VkPipelineMultisampleStateCreateInfo* ms = ci->pMultisampleState;
        const VkPipelineRenderingCreateInfo* ri = findNext(ci->pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
        p->colorCount = ri ? ri->colorAttachmentCount : 0;
        if (ci->renderPass) p->unknown = true;
        p->writeEnable0 = true;
        if (cb) {
            p->logicOp = cb->logicOpEnable;
            if (cb->attachmentCount && cb->pAttachments) {
                p->blend0 = cb->pAttachments[0].blendEnable;
                p->mask0 = cb->pAttachments[0].colorWriteMask;
            }
            for (const VkBaseInStructure* s = cb->pNext; s; s = s->pNext) {
                if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT) {
                    const VkPipelineColorWriteCreateInfoEXT* cw = (const VkPipelineColorWriteCreateInfoEXT*)s;
                    if (cw->attachmentCount && cw->pColorWriteEnables) p->writeEnable0 = cw->pColorWriteEnables[0];
                } else {
                    p->unknown = true;
                }
            }
        } else if (!(p->dynamic & (1u << D_BLEND_ENABLE)) || !(p->dynamic & (1u << D_WRITE_MASK))) {
            p->unknown = true;
        }
        if (ms) {
            p->samples = ms->rasterizationSamples;
            p->a2c = ms->alphaToCoverageEnable;
            p->sampleMaskOff = ms->pSampleMask && !(ms->pSampleMask[0] & 1);
            for (const VkBaseInStructure* s = ms->pNext; s; s = s->pNext) {
                if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT)
                    p->sampleLocations = ((const VkPipelineSampleLocationsStateCreateInfoEXT*)s)->sampleLocationsEnable;
                else
                    p->unknown = true;
            }
        } else if (!(p->dynamic & (1u << D_SAMPLES))) {
            p->unknown = true;
        }
    }
    return p;
}

static void remember(VkPipeline handle, Pipe* p) {
    pthread_mutex_lock(&trk);
    Pipe* old = mapDel(&pipes, H(handle));
    if (!mapPut(&pipes, H(handle), p)) pipeUnref(p);
    pthread_mutex_unlock(&trk);
    pipeUnref(old);
}

static VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo* ci,
                                                   const VkAllocationCallbacks* a, VkPipeline* p) {
    VkResult r = N(W_CREATE_GFX_PIPELINES, PFN_vkCreateGraphicsPipelines)(d, c, n, ci, a, p);
    for (uint32_t i = 0; TRACK && i < n && ci && p; i++) {
        if (!p[i]) continue;
        Pipe* pipe = pipeFromInfo(&ci[i]);
        if (pipe) remember(p[i], pipe);
    }
    return r;
}

static VkResult VKAPI_CALL CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo* ci,
                                                  const VkAllocationCallbacks* a, VkPipeline* p) {
    VkResult r = N(W_CREATE_COMPUTE_PIPELINES, PFN_vkCreateComputePipelines)(d, c, n, ci, a, p);
    for (uint32_t i = 0; TRACK && i < n && ci && p; i++) {
        if (!p[i]) continue;
        Pipe* pipe = calloc(1, sizeof *pipe);
        if (!pipe) continue;
        pipe->refs = 1;
        pipe->compute = true;
        bool writes;
        spv_unref(stageProgram(&ci[i].stage, &writes));
        pipe->writes = writes;
        remember(p[i], pipe);
    }
    return r;
}

static void VKAPI_CALL DestroyPipeline(VkDevice d, VkPipeline pipeline, const VkAllocationCallbacks* a) {
    if (TRACK && pipeline) {
        pthread_mutex_lock(&trk);
        Pipe* p = mapDel(&pipes, H(pipeline));
        pthread_mutex_unlock(&trk);
        pipeUnref(p);
    }
    N(W_DESTROY_PIPELINE, PFN_vkDestroyPipeline)(d, pipeline, a);
}

/* ---- command buffer lifetime ---- */

static VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out) {
    VkResult r = N(W_ALLOCATE_CBS, PFN_vkAllocateCommandBuffers)(d, info, out);
    if (r == VK_SUCCESS && TRACK && info && out) {
        pthread_mutex_lock(&trk);
        for (uint32_t i = 0; i < info->commandBufferCount; i++) {
            Cb* c = cbGet(out[i], true);
            if (!c) continue;
            cbClear(c);
            c->pool = info->commandPool;
        }
        pthread_mutex_unlock(&trk);
    }
    return r;
}

static void VKAPI_CALL FreeCommandBuffers(VkDevice d, VkCommandPool pool, uint32_t n, const VkCommandBuffer* list) {
    if (TRACK && list) {
        pthread_mutex_lock(&trk);
        for (uint32_t i = 0; i < n; i++) {
            Cb* c = list[i] ? cbGet(list[i], false) : NULL;
            if (c) cbClear(c), c->pool = VK_NULL_HANDLE;
        }
        pthread_mutex_unlock(&trk);
    }
    N(W_FREE_CBS, PFN_vkFreeCommandBuffers)(d, pool, n, list);
}

static void poolReset(VkCommandPool pool) {
    pthread_mutex_lock(&trk);
    for (uint32_t i = 0; i < cbs.cap; i++)
        if (cbs.state[i] == 1 && ((Cb*)cbs.vals[i])->pool == pool) cbClear(cbs.vals[i]);
    pthread_mutex_unlock(&trk);
}

static VkResult VKAPI_CALL ResetCommandPool(VkDevice d, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    VkResult r = N(W_RESET_POOL, PFN_vkResetCommandPool)(d, pool, flags);
    if (TRACK && pool) poolReset(pool);
    return r;
}

static void VKAPI_CALL DestroyCommandPool(VkDevice d, VkCommandPool pool, const VkAllocationCallbacks* a) {
    if (TRACK && pool) poolReset(pool);
    N(W_DESTROY_POOL, PFN_vkDestroyCommandPool)(d, pool, a);
}

static VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    if (RECORD) {
        Cb* c = cbOf(cb);
        pthread_mutex_lock(&trk);
        if (c) cbClear(c);
        pthread_mutex_unlock(&trk);
    }
    return N(W_RESET_CB, PFN_vkResetCommandBuffer)(cb, flags);
}

static VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info) {
    if (RECORD) {
        Cb* c = cbOf(cb);
        if (c) {
            pthread_mutex_lock(&trk);
            cbClear(c);
            pthread_mutex_unlock(&trk);
            c->recording = true;
            c->ours = true;
            c->secondary = info && info->pInheritanceInfo && (info->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT);
            c->oneTime = info && (info->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
            /* inheritance with a legacy render pass, inherited conditional rendering, attachment locations,
             * viewports or anything else not known: not followed */
            if (c->secondary && info->pInheritanceInfo->renderPass) c->unknown = true;
            for (const VkBaseInStructure* s = c->secondary ? info->pInheritanceInfo->pNext : NULL; s; s = s->pNext) {
                if (s->sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO) continue;
                if (s->sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_CONDITIONAL_RENDERING_INFO_EXT &&
                    !((const VkCommandBufferInheritanceConditionalRenderingInfoEXT*)s)->conditionalRenderingEnable)
                    continue;
                c->unknown = true;
            }
        }
    }
    return N(W_BEGIN_CB, PFN_vkBeginCommandBuffer)(cb, info);
}

static void flushPending(VkCommandBuffer cb, Cb* c, int reason);

/* The record of a command being recorded; a held-back vkCmdBeginRendering goes to the driver first. */
static Cb* rec(VkCommandBuffer cb) {
    if (!RECORD) return NULL;
    Cb* c = cbOf(cb);
    if (c && c->pending) flushPending(cb, c, L_ORDER);
    return c;
}

static VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer cb) {
    Cb* c = rec(cb);
    VkResult r = N(W_END_CB, PFN_vkEndCommandBuffer)(cb);
    if (c) {
        c->recording = false;
        c->ended = r == VK_SUCCESS;
    }
    return r;
}

/* ---- stats helpers ---- */

static void countLoad(VkAttachmentLoadOp op, int base) {
    switch (op) {
    case VK_ATTACHMENT_LOAD_OP_LOAD: INC(base); break;
    case VK_ATTACHMENT_LOAD_OP_CLEAR: INC(base + 1); break;
    case VK_ATTACHMENT_LOAD_OP_DONT_CARE: INC(base + 2); break;
    default: INC(base + 3); break;
    }
}

static void countStore(VkAttachmentStoreOp op, int base) {
    switch (op) {
    case VK_ATTACHMENT_STORE_OP_STORE: INC(base); break;
    case VK_ATTACHMENT_STORE_OP_DONT_CARE: INC(base + 1); break;
    default: INC(base + 2); break;
    }
}

static void passBegun(const VkRect2D* area, bool colorLoad, bool depthLoad) {
    uint64_t a = (uint64_t)area->extent.width * area->extent.height;
    seeArea(a);
    if (statsOn <= 0) return;
    uint64_t m = __atomic_load_n(&maxArea, __ATOMIC_RELAXED);
    uint64_t full = fullArea ? fullArea : m;
    bool full9 = a * 10 >= full * 9;
    INC(full9 ? S_AREA_FULL : a * 10 >= full * 4 ? S_AREA_HALF : S_AREA_SMALL);
    if (full9 && (colorLoad || depthLoad)) INC(S_FULL_LOAD);
    if (full9 && colorLoad) INC(S_FULL_COLOR_LOAD);
    if (full9 && depthLoad) INC(S_FULL_DEPTH_LOAD);
    tlInPass = 1;
    tlPassFull = full9;
    tlPassDraws = 0;
    int l = currentLabel();
    if (l >= 0) {
        __atomic_add_fetch(&labels[l].passes, 1, __ATOMIC_RELAXED);
        if (full9 && (colorLoad || depthLoad)) __atomic_add_fetch(&labels[l].fullLoads, 1, __ATOMIC_RELAXED);
    }
}

static void countRendering(const VkRenderingInfo* info) {
    INC(S_BEGIN_RENDERING);
    if (!info) return;
    if (info->flags & VK_RENDERING_RESUMING_BIT) INC(S_RESUMING);
    if (info->flags & VK_RENDERING_SUSPENDING_BIT) INC(S_SUSPENDING);
    bool colorLoad = false, depthLoad = false;
    for (uint32_t i = 0; i < info->colorAttachmentCount && info->pColorAttachments; i++) {
        const VkRenderingAttachmentInfo* c = &info->pColorAttachments[i];
        if (!c->imageView) continue;
        countLoad(c->loadOp, S_C_LOAD);
        countStore(c->storeOp, S_C_STORE);
        if (c->resolveMode != VK_RESOLVE_MODE_NONE) INC(S_C_RESOLVE);
        colorLoad |= c->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD;
    }
    if (info->pDepthAttachment && info->pDepthAttachment->imageView) {
        const VkRenderingAttachmentInfo* d = info->pDepthAttachment;
        countLoad(d->loadOp, S_D_LOAD);
        countStore(d->storeOp, S_D_STORE);
        if (d->resolveMode != VK_RESOLVE_MODE_NONE) INC(S_D_RESOLVE);
        depthLoad = d->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD;
    }
    if (info->pStencilAttachment && info->pStencilAttachment->imageView) {
        if (info->pStencilAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) INC(S_S_LOAD);
        if (info->pStencilAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) INC(S_S_CLEAR);
    }
    passBegun(&info->renderArea, colorLoad, depthLoad);
}

/* ---- the pass: checks that need no secondary ---- */

static uint32_t formatComponents(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_SRGB:
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT:
        return VK_COLOR_COMPONENT_R_BIT;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8_SRGB:
    case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT:
        return VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        return VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    default:
        return 0xf;
    }
}

/* L_APPLIED when the pass may be held back for its secondary. */
static int passChecks(const VkRenderingInfo* info, ViewInfo* view, int apply) {
    if (info->colorAttachmentCount != 1 || !info->pColorAttachments) return L_COLOR_COUNT;
    const VkRenderingAttachmentInfo* c = &info->pColorAttachments[0];
    if (info->flags & ~(VkRenderingFlags)VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) return L_FLAGS;
    if (!(info->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT)) return L_INLINE;
    if (info->pNext) return L_PNEXT;
    if ((info->pDepthAttachment && info->pDepthAttachment->imageView) || (info->pStencilAttachment && info->pStencilAttachment->imageView))
        return L_DEPTH;
    if ((info->pDepthAttachment && info->pDepthAttachment->pNext) || (info->pStencilAttachment && info->pStencilAttachment->pNext))
        return L_PNEXT;
    if (info->layerCount != 1 || info->viewMask) return L_LAYERS;
    for (const VkBaseInStructure* s = c->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_ATTACHMENT_FEEDBACK_LOOP_INFO_EXT) {
            if (((const VkAttachmentFeedbackLoopInfoEXT*)s)->feedbackLoopEnable) return L_FEEDBACK;
        } else if (s->sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_FLAGS_INFO_KHR) {
            if (((const VkRenderingAttachmentFlagsInfoKHR*)s)->flags & VK_RENDERING_ATTACHMENT_INPUT_ATTACHMENT_FEEDBACK_BIT_KHR)
                return L_FEEDBACK;
        } else {
            return L_PNEXT;
        }
    }
    if (c->imageLayout != VK_IMAGE_LAYOUT_GENERAL && c->imageLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        c->imageLayout != VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL)
        return L_FEEDBACK;
    pthread_mutex_lock(&trk);
    const ViewInfo* v = mapGet(&views, H(c->imageView));
    if (v) *view = *v;
    pthread_mutex_unlock(&trk);
    if (!v || info->renderArea.offset.x || info->renderArea.offset.y || info->renderArea.extent.width != view->width ||
        info->renderArea.extent.height != view->height)
        return L_AREA;
    if (view->samples != VK_SAMPLE_COUNT_1_BIT) return L_RASTER;
    if (apply == 1 && !isFull((uint64_t)view->width * view->height)) return L_SMALL;
    return L_APPLIED;
}

static void forwardBegin(VkCommandBuffer cb, const VkRenderingInfo* info, bool khr) {
    if (khr) N(W_BEGIN_RENDERING_KHR, PFN_vkCmdBeginRendering)(cb, info);
    else N(W_BEGIN_RENDERING, PFN_vkCmdBeginRendering)(cb, info);
}

static void flushPending(VkCommandBuffer cb, Cb* c, int reason) {
    c->pending = false;
    LOADC(reason);
    forwardBegin(cb, &c->pb.info, c->pb.khr);
}

static void beginRendering(VkCommandBuffer cb, const VkRenderingInfo* info, bool khr) {
    countRendering(info);
    if (statsOn <= 0 && info) seeArea((uint64_t)info->renderArea.extent.width * info->renderArea.extent.height);
    Cb* c = rec(cb);
    bool colorLoad = false;
    for (uint32_t i = 0; info && info->pColorAttachments && i < info->colorAttachmentCount; i++)
        colorLoad |= info->pColorAttachments[i].imageView && info->pColorAttachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD;
    if (!TRACK || !c || !colorLoad) {
        forwardBegin(cb, info, khr);
        return;
    }
    ViewInfo view;
    int apply = applyMode();
    int reason = !apply ? L_OFF : c->conditional ? L_CONDITIONAL : genMissing ? L_UNWRAPPED : passChecks(info, &view, apply);
    if (reason != L_APPLIED) {
        LOADC(reason);
        forwardBegin(cb, info, khr);
        return;
    }
    /* held back: a copy of what passChecks let through */
    Pending* p = &c->pb;
    memset(p, 0, sizeof *p);
    p->khr = khr;
    p->info = *info;
    p->color = info->pColorAttachments[0];
    p->info.pColorAttachments = &p->color;
    const void* head = NULL;
    VkBaseOutStructure* last = NULL;
    for (const VkBaseInStructure* s = info->pColorAttachments[0].pNext; s; s = s->pNext) {
        VkBaseOutStructure* copy;
        if (s->sType == VK_STRUCTURE_TYPE_ATTACHMENT_FEEDBACK_LOOP_INFO_EXT) {
            p->feedback = *(const VkAttachmentFeedbackLoopInfoEXT*)s;
            copy = (VkBaseOutStructure*)&p->feedback;
        } else {
            p->flags = *(const VkRenderingAttachmentFlagsInfoKHR*)s;
            copy = (VkBaseOutStructure*)&p->flags;
        }
        copy->pNext = NULL;
        if (last) last->pNext = copy;
        else head = copy;
        last = copy;
    }
    p->color.pNext = head;
    if (info->pDepthAttachment) p->depth = *info->pDepthAttachment, p->info.pDepthAttachment = &p->depth;
    if (info->pStencilAttachment) p->stencil = *info->pStencilAttachment, p->info.pStencilAttachment = &p->stencil;
    c->pending = true;
}

static void VKAPI_CALL CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* info) {
    beginRendering(cb, info, false);
}
static void VKAPI_CALL CmdBeginRenderingKHR(VkCommandBuffer cb, const VkRenderingInfo* info) {
    beginRendering(cb, info, true);
}

static void passEnded(void) {
    INC(S_END_RENDERING);
    if (!tlInPass) return;
    uint64_t d = tlPassDraws;
    INC(d == 0 ? S_PASS_0DRAW : d == 1 ? S_PASS_1DRAW : d <= 4 ? S_PASS_2_4DRAW : S_PASS_5DRAW);
    if (tlPassFull && d <= 2) INC(S_FULL_PASS_LE2DRAW);
    tlInPass = 0;
}
static void VKAPI_CALL CmdEndRendering(VkCommandBuffer cb) {
    rec(cb);
    passEnded();
    N(W_END_RENDERING, PFN_vkCmdEndRendering)(cb);
}
static void VKAPI_CALL CmdEndRenderingKHR(VkCommandBuffer cb) {
    rec(cb);
    passEnded();
    N(W_END_RENDERING_KHR, PFN_vkCmdEndRendering)(cb);
}
static void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, VkSubpassContents c) {
    rec(cb);
    INC(S_RP_LEGACY);
    N(W_BEGIN_RENDER_PASS, PFN_vkCmdBeginRenderPass)(cb, info, c);
}
static void VKAPI_CALL CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* s) {
    rec(cb);
    INC(S_RP_LEGACY);
    N(W_BEGIN_RENDER_PASS2, PFN_vkCmdBeginRenderPass2)(cb, info, s);
}

/* ---- draws ---- */

static void drawStats(int kind) {
    if (statsOn <= 0) return;
    INC(kind);
    if (tlInPass) tlPassDraws++;
    else INC(S_DRAW_OUTSIDE);
    int l = currentLabel();
    if (l >= 0) __atomic_add_fetch(&labels[l].draws, 1, __ATOMIC_RELAXED);
}

/* a draw of the bound pipeline: what it may write; the first plain draw's state is kept */
static void drawn(Cb* c, bool plain, bool indexed, uint32_t count, uint32_t instances, uint32_t first, int32_t vertexOffset,
                  uint32_t firstInstance) {
    if (!c) return;
    Pipe* p = c->st.pipe;
    if (TRACK && (!p || p->writes)) taintEverything(c);
    if (!plain) {
        c->others++;
        return;
    }
    if (!c->draws++ && !c->hasSnap) {
        c->snap = (DrawSnap){c->st, indexed, count, instances, first, firstInstance, vertexOffset};
        pipeRef(c->snap.st.pipe);
        c->hasSnap = true;
    }
}

static void VKAPI_CALL CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    drawStats(S_DRAW);
    drawn(rec(cb), true, false, vc, ic, fv, 0, fi);
    N(W_DRAW, PFN_vkCmdDraw)(cb, vc, ic, fv, fi);
}
static void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t inst, uint32_t fi, int32_t vo, uint32_t finst) {
    drawStats(S_DRAW_INDEXED);
    drawn(rec(cb), true, true, ic, inst, fi, vo, finst);
    N(W_DRAW_INDEXED, PFN_vkCmdDrawIndexed)(cb, ic, inst, fi, vo, finst);
}
#define OTHER_DRAW(cb) drawn(rec(cb), false, false, 0, 0, 0, 0, 0)
static void VKAPI_CALL CmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    drawStats(S_DRAW_INDIRECT);
    OTHER_DRAW(cb);
    N(W_DRAW_INDIRECT, PFN_vkCmdDrawIndirect)(cb, b, o, n, s);
}
static void VKAPI_CALL CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    drawStats(S_DRAW_INDIRECT);
    OTHER_DRAW(cb);
    N(W_DRAW_INDEXED_INDIRECT, PFN_vkCmdDrawIndexedIndirect)(cb, b, o, n, s);
}
static void VKAPI_CALL CmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t m,
                                            uint32_t s) {
    drawStats(S_DRAW_INDIRECT);
    OTHER_DRAW(cb);
    N(W_DRAW_INDIRECT_COUNT, PFN_vkCmdDrawIndirectCount)(cb, b, o, cbuf, co, m, s);
}
static void VKAPI_CALL CmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co,
                                                   uint32_t m, uint32_t s) {
    drawStats(S_DRAW_INDIRECT);
    OTHER_DRAW(cb);
    N(W_DRAW_INDEXED_INDIRECT_COUNT, PFN_vkCmdDrawIndexedIndirectCount)(cb, b, o, cbuf, co, m, s);
}
static void VKAPI_CALL CmdDrawMultiEXT(VkCommandBuffer cb, uint32_t n, const VkMultiDrawInfoEXT* v, uint32_t ic, uint32_t fi, uint32_t s) {
    drawStats(S_DRAW);
    OTHER_DRAW(cb);
    N(W_DRAW_MULTI, PFN_vkCmdDrawMultiEXT)(cb, n, v, ic, fi, s);
}
static void VKAPI_CALL CmdDrawMultiIndexedEXT(VkCommandBuffer cb, uint32_t n, const VkMultiDrawIndexedInfoEXT* v, uint32_t ic, uint32_t fi,
                                              uint32_t s, const int32_t* vo) {
    drawStats(S_DRAW_INDEXED);
    OTHER_DRAW(cb);
    N(W_DRAW_MULTI_INDEXED, PFN_vkCmdDrawMultiIndexedEXT)(cb, n, v, ic, fi, s, vo);
}
static void VKAPI_CALL CmdDrawIndirectByteCountEXT(VkCommandBuffer cb, uint32_t ic, uint32_t fi, VkBuffer b, VkDeviceSize o, uint32_t co,
                                                   uint32_t s) {
    drawStats(S_DRAW_INDIRECT);
    OTHER_DRAW(cb);
    N(W_DRAW_BYTE_COUNT, PFN_vkCmdDrawIndirectByteCountEXT)(cb, ic, fi, b, o, co, s);
}
static void VKAPI_CALL CmdDrawMeshTasksEXT(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    drawStats(S_DRAW);
    OTHER_DRAW(cb);
    N(W_DRAW_MESH, PFN_vkCmdDrawMeshTasksEXT)(cb, x, y, z);
}
static void VKAPI_CALL CmdDrawMeshTasksIndirectEXT(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    drawStats(S_DRAW_INDIRECT);
    OTHER_DRAW(cb);
    N(W_DRAW_MESH_INDIRECT, PFN_vkCmdDrawMeshTasksIndirectEXT)(cb, b, o, n, s);
}
static void VKAPI_CALL CmdDrawMeshTasksIndirectCountEXT(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co,
                                                        uint32_t m, uint32_t s) {
    drawStats(S_DRAW_INDIRECT);
    OTHER_DRAW(cb);
    N(W_DRAW_MESH_INDIRECT_COUNT, PFN_vkCmdDrawMeshTasksIndirectCountEXT)(cb, b, o, cbuf, co, m, s);
}

static void dispatched(VkCommandBuffer cb) {
    Cb* c = rec(cb);
    if (TRACK && c && (!c->compute || c->compute->writes)) taintEverything(c);
}
static void VKAPI_CALL CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    INC(S_DISPATCH);
    dispatched(cb);
    N(W_DISPATCH, PFN_vkCmdDispatch)(cb, x, y, z);
}
static void VKAPI_CALL CmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o) {
    INC(S_DISPATCH_INDIRECT);
    dispatched(cb);
    N(W_DISPATCH_INDIRECT, PFN_vkCmdDispatchIndirect)(cb, b, o);
}
static void VKAPI_CALL CmdDispatchBase(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, uint32_t x, uint32_t y, uint32_t z) {
    INC(S_DISPATCH_BASE);
    dispatched(cb);
    N(W_DISPATCH_BASE, PFN_vkCmdDispatchBase)(cb, a, b, c, x, y, z);
}

static void VKAPI_CALL CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    INC(bp == VK_PIPELINE_BIND_POINT_COMPUTE ? S_BIND_PIPELINE_COMPUTE : S_BIND_PIPELINE_GFX);
    Cb* c = rec(cb);
    if (!TRACK) {
    } else if (c && (bp == VK_PIPELINE_BIND_POINT_GRAPHICS || bp == VK_PIPELINE_BIND_POINT_COMPUTE)) {
        pthread_mutex_lock(&trk);
        Pipe* pipe = mapGet(&pipes, H(p));
        pipeRef(pipe);
        pthread_mutex_unlock(&trk);
        Pipe** slot = bp == VK_PIPELINE_BIND_POINT_GRAPHICS ? &c->st.pipe : &c->compute;
        pipeUnref(*slot);
        *slot = pipe && pipe->compute == (bp == VK_PIPELINE_BIND_POINT_COMPUTE) ? pipe : NULL;
        if (*slot != pipe) pipeUnref(pipe);
    } else if (c) {
        c->unknown = true;
    }
    N(W_BIND_PIPELINE, PFN_vkCmdBindPipeline)(cb, bp, p);
}

static void VKAPI_CALL CmdBindShadersEXT(VkCommandBuffer cb, uint32_t n, const VkShaderStageFlagBits* stages, const VkShaderEXT* shaders) {
    Cb* c = rec(cb);
    if (c) c->unknown = true;
    if (c && TRACK) taintEverything(c);
    N(W_BIND_SHADERS, PFN_vkCmdBindShadersEXT)(cb, n, stages, shaders);
}

/* ---- state ---- */

static void VKAPI_CALL CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b, const VkDeviceSize* o) {
    Cb* c = rec(cb);
    for (uint32_t i = 0; c && b && o && i < n && first + i < MAX_VB; i++) {
        c->st.vb[first + i] = b[i], c->st.vbOffset[first + i] = o[i], c->st.vbSize[first + i] = VK_WHOLE_SIZE;
        c->st.vbSet |= 1u << (first + i);
    }
    N(W_BIND_VERTEX_BUFFERS, PFN_vkCmdBindVertexBuffers)(cb, first, n, b, o);
}
static void VKAPI_CALL CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b, const VkDeviceSize* o,
                                             const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    Cb* c = rec(cb);
    for (uint32_t i = 0; c && b && o && i < n && first + i < MAX_VB; i++) {
        c->st.vb[first + i] = b[i], c->st.vbOffset[first + i] = o[i], c->st.vbSize[first + i] = sizes ? sizes[i] : VK_WHOLE_SIZE;
        c->st.vbSet |= 1u << (first + i);
        if (strides) c->st.strides[first + i] = strides[i], c->st.strideSet |= 1u << (first + i);
    }
    if (c && strides) c->st.dynSet |= 1u << D_STRIDE;
    N(W_BIND_VERTEX_BUFFERS2, PFN_vkCmdBindVertexBuffers2)(cb, first, n, b, o, sizes, strides);
}
static void VKAPI_CALL CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t) {
    Cb* c = rec(cb);
    if (c) c->st.ib = b, c->st.ibOffset = o, c->st.ibSize = VK_WHOLE_SIZE, c->st.ibType = t, c->st.ibSet = true;
    N(W_BIND_INDEX_BUFFER, PFN_vkCmdBindIndexBuffer)(cb, b, o, t);
}
static void VKAPI_CALL CmdBindIndexBuffer2(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize s, VkIndexType t) {
    Cb* c = rec(cb);
    if (c) c->st.ib = b, c->st.ibOffset = o, c->st.ibSize = s, c->st.ibType = t, c->st.ibSet = true;
    N(W_BIND_INDEX_BUFFER2, PFN_vkCmdBindIndexBuffer2KHR)(cb, b, o, s, t);
}
static void VKAPI_CALL CmdBindTransformFeedbackBuffersEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b,
                                                          const VkDeviceSize* o, const VkDeviceSize* s) {
    Cb* c = rec(cb);
    for (uint32_t i = 0; c && TRACK && b && o && i < n; i++) addTaint(c, b[i], o[i], s ? s[i] : VK_WHOLE_SIZE);
    N(W_BIND_XFB_BUFFERS, PFN_vkCmdBindTransformFeedbackBuffersEXT)(cb, first, n, b, o, s);
}

#define SET(cb, kind) Cb* c = rec(cb); if (c) c->st.dynSet |= 1u << (kind); if (c)
static void VKAPI_CALL CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
    Cb* c = rec(cb);
    if (c && first == 0 && n && v) c->st.viewport = v[0], c->st.dynSet |= 1u << D_VIEWPORT;
    if (c && first + n > c->st.viewportCount) c->st.viewportCount = first + n;
    N(W_SET_VIEWPORT, PFN_vkCmdSetViewport)(cb, first, n, v);
}
static void VKAPI_CALL CmdSetViewportWithCount(VkCommandBuffer cb, uint32_t n, const VkViewport* v) {
    SET(cb, D_VIEWPORT) {
        c->st.viewportCount = n;
        if (n && v) c->st.viewport = v[0];
    }
    N(W_SET_VIEWPORT_COUNT, PFN_vkCmdSetViewportWithCount)(cb, n, v);
}
static void VKAPI_CALL CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* s) {
    Cb* c = rec(cb);
    if (c && first == 0 && n && s) c->st.scissor = s[0], c->st.dynSet |= 1u << D_SCISSOR;
    if (c && first + n > c->st.scissorCount) c->st.scissorCount = first + n;
    N(W_SET_SCISSOR, PFN_vkCmdSetScissor)(cb, first, n, s);
}
static void VKAPI_CALL CmdSetScissorWithCount(VkCommandBuffer cb, uint32_t n, const VkRect2D* s) {
    SET(cb, D_SCISSOR) {
        c->st.scissorCount = n;
        if (n && s) c->st.scissor = s[0];
    }
    N(W_SET_SCISSOR_COUNT, PFN_vkCmdSetScissorWithCount)(cb, n, s);
}
static void VKAPI_CALL CmdSetCullMode(VkCommandBuffer cb, VkCullModeFlags m) {
    SET(cb, D_CULL) c->st.cull = m;
    N(W_SET_CULL, PFN_vkCmdSetCullMode)(cb, m);
}
static void VKAPI_CALL CmdSetFrontFace(VkCommandBuffer cb, VkFrontFace f) {
    SET(cb, D_FRONT) c->st.front = f;
    N(W_SET_FRONT, PFN_vkCmdSetFrontFace)(cb, f);
}
static void VKAPI_CALL CmdSetPrimitiveTopology(VkCommandBuffer cb, VkPrimitiveTopology t) {
    SET(cb, D_TOPOLOGY) c->st.topology = t;
    N(W_SET_TOPOLOGY, PFN_vkCmdSetPrimitiveTopology)(cb, t);
}
static void VKAPI_CALL CmdSetPrimitiveRestartEnable(VkCommandBuffer cb, VkBool32 e) {
    SET(cb, D_RESTART) c->st.restart = e;
    N(W_SET_RESTART, PFN_vkCmdSetPrimitiveRestartEnable)(cb, e);
}
static void VKAPI_CALL CmdSetRasterizerDiscardEnable(VkCommandBuffer cb, VkBool32 e) {
    SET(cb, D_RASTER_DISCARD) c->st.rasterDiscard = e;
    N(W_SET_RASTER_DISCARD, PFN_vkCmdSetRasterizerDiscardEnable)(cb, e);
}
static void VKAPI_CALL CmdSetVertexInputEXT(VkCommandBuffer cb, uint32_t nb, const VkVertexInputBindingDescription2EXT* b, uint32_t na,
                                            const VkVertexInputAttributeDescription2EXT* a) {
    SET(cb, D_VERTEX_INPUT) {
        if (nb <= MAX_VB && na <= MAX_ATTRS && (b || !nb) && (a || !na)) {
            c->st.viBindingCount = nb, c->st.viAttrCount = na;
            if (nb) memcpy(c->st.viBindings, b, nb * sizeof *b);
            if (na) memcpy(c->st.viAttrs, a, na * sizeof *a);
        } else {
            c->st.dynSet &= ~(1u << D_VERTEX_INPUT);
        }
    }
    N(W_SET_VERTEX_INPUT, PFN_vkCmdSetVertexInputEXT)(cb, nb, b, na, a);
}
static void VKAPI_CALL CmdSetColorWriteEnableEXT(VkCommandBuffer cb, uint32_t n, const VkBool32* e) {
    Cb* c = rec(cb);
    if (c && n && e) c->st.writeEnable0 = e[0], c->st.dynSet |= 1u << D_WRITE_ENABLE;
    N(W_SET_WRITE_ENABLE, PFN_vkCmdSetColorWriteEnableEXT)(cb, n, e);
}
static void VKAPI_CALL CmdSetColorWriteMaskEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorComponentFlags* m) {
    Cb* c = rec(cb);
    if (c && first == 0 && n && m) c->st.mask0 = m[0], c->st.dynSet |= 1u << D_WRITE_MASK;
    N(W_SET_WRITE_MASK, PFN_vkCmdSetColorWriteMaskEXT)(cb, first, n, m);
}
static void VKAPI_CALL CmdSetColorBlendEnableEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBool32* e) {
    Cb* c = rec(cb);
    if (c && first == 0 && n && e) c->st.blend0 = e[0], c->st.dynSet |= 1u << D_BLEND_ENABLE;
    N(W_SET_BLEND_ENABLE, PFN_vkCmdSetColorBlendEnableEXT)(cb, first, n, e);
}
static void VKAPI_CALL CmdSetLogicOpEnableEXT(VkCommandBuffer cb, VkBool32 e) {
    SET(cb, D_LOGIC_OP_ENABLE) c->st.logicOp = e;
    N(W_SET_LOGIC_OP_ENABLE, PFN_vkCmdSetLogicOpEnableEXT)(cb, e);
}
static void VKAPI_CALL CmdSetAlphaToCoverageEnableEXT(VkCommandBuffer cb, VkBool32 e) {
    SET(cb, D_A2C) c->st.a2c = e;
    N(W_SET_A2C, PFN_vkCmdSetAlphaToCoverageEnableEXT)(cb, e);
}
static void VKAPI_CALL CmdSetSampleMaskEXT(VkCommandBuffer cb, VkSampleCountFlagBits s, const VkSampleMask* m) {
    Cb* c = rec(cb);
    if (c) c->st.sampleMask0 = m ? m[0] : ~0u, c->st.dynSet |= 1u << D_SAMPLE_MASK;
    N(W_SET_SAMPLE_MASK, PFN_vkCmdSetSampleMaskEXT)(cb, s, m);
}
static void VKAPI_CALL CmdSetRasterizationSamplesEXT(VkCommandBuffer cb, VkSampleCountFlagBits s) {
    SET(cb, D_SAMPLES) c->st.samples = s;
    N(W_SET_SAMPLES, PFN_vkCmdSetRasterizationSamplesEXT)(cb, s);
}
static void VKAPI_CALL CmdSetPolygonModeEXT(VkCommandBuffer cb, VkPolygonMode m) {
    SET(cb, D_POLYGON) c->st.polygon = m;
    N(W_SET_POLYGON, PFN_vkCmdSetPolygonModeEXT)(cb, m);
}
static void VKAPI_CALL CmdSetSampleLocationsEnableEXT(VkCommandBuffer cb, VkBool32 e) {
    SET(cb, D_SAMPLE_LOCATIONS) c->st.sampleLocations = e;
    N(W_SET_SAMPLE_LOCATIONS, PFN_vkCmdSetSampleLocationsEnableEXT)(cb, e);
}
static void VKAPI_CALL CmdSetRasterizationStreamEXT(VkCommandBuffer cb, uint32_t stream) {
    SET(cb, D_RASTER_STREAM) c->st.rasterStream = stream;
    N(W_SET_RASTER_STREAM, PFN_vkCmdSetRasterizationStreamEXT)(cb, stream);
}

/* A pass begun inside conditional rendering may draw nothing; in a secondary it is not followed. */
static void VKAPI_CALL CmdBeginConditionalRenderingEXT(VkCommandBuffer cb, const VkConditionalRenderingBeginInfoEXT* info) {
    Cb* c = rec(cb);
    if (c) {
        if (c->secondary) c->unknown = true;
        c->conditional++;
    }
    N(W_BEGIN_CONDITIONAL, PFN_vkCmdBeginConditionalRenderingEXT)(cb, info);
}
static void VKAPI_CALL CmdEndConditionalRenderingEXT(VkCommandBuffer cb) {
    Cb* c = rec(cb);
    if (c && c->conditional) c->conditional--;
    N(W_END_CONDITIONAL, PFN_vkCmdEndConditionalRenderingEXT)(cb);
}

/* ---- the decision ---- */

/* trk held. Reads `size` bytes of buffer memory at buffer offset `offset`; false when they are
 * not host-mapped, out of the buffer, or may be written by the GPU. */
static bool readBuffer(Cb* primary, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, void* out) {
    const BufInfo* b = mapGet(&buffers, H(buffer));
    if (!b || !b->memory || offset > b->size || size > b->size - offset) return false;
    const MemInfo* m = mapGet(&memories, H(b->memory));
    VkDeviceSize at = b->offset + offset;
    if (!m || !m->ptr || at < m->mapOffset || at - m->mapOffset > m->mapSize || size > m->mapSize - (at - m->mapOffset)) return false;
    if (tainted(b->memory, at, size)) return false;
    const uint8_t* src = m->ptr + (at - m->mapOffset);
    memcpy(out, src, size);
    if (primary->depCount == primary->depCap) {
        uint32_t cap = primary->depCap ? primary->depCap * 2 : 16;
        Dep* d = realloc(primary->deps, cap * sizeof *d);
        if (!d) return false;
        primary->deps = d, primary->depCap = cap;
    }
    primary->deps[primary->depCount++] = (Dep){b->memory, at, size, fnv(src, size)};
    return true;
}

/* trk held. The vertices of the snapshot's draw (instance firstInstance) as the shader gets them,
 * and its triangles. L_APPLIED or the reason. */
static int gather(Cb* primary, const DrawSnap* d, const Pipe* p, SpvVertexIn* in, uint32_t* vcount, uint32_t (*tris)[3], uint32_t* tcount) {
    const RecState* st = &d->st;
    VkPrimitiveTopology topology = (p->dynamic & (1u << D_TOPOLOGY)) ? st->topology : p->topology;
    bool restart = (p->dynamic & (1u << D_RESTART)) ? st->restart : p->restart;
    if (!d->instances || !d->count) return L_DRAWS;
    if (d->count > MAX_VERTS) return L_GEOMETRY;
    if (topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP &&
        topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)
        return L_GEOMETRY;
    /* vertex indices */
    uint32_t index[MAX_VERTS];
    if (d->indexed) {
        if (!st->ibSet) return L_VERTEX;
        uint32_t size = st->ibType == VK_INDEX_TYPE_UINT16 ? 2 : st->ibType == VK_INDEX_TYPE_UINT32 ? 4 : st->ibType == VK_INDEX_TYPE_UINT8_EXT ? 1 : 0;
        if (!size) return L_VERTEX;
        uint8_t raw[4 * MAX_VERTS];
        VkDeviceSize off = st->ibOffset + (VkDeviceSize)d->first * size, bytes = (VkDeviceSize)d->count * size;
        if (st->ibSize != VK_WHOLE_SIZE && (VkDeviceSize)(d->first + d->count) * size > st->ibSize) return L_VERTEX;
        if (!readBuffer(primary, st->ib, off, bytes, raw)) return L_VERTEX;
        uint32_t restartValue = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
        for (uint32_t i = 0; i < d->count; i++) {
            uint32_t v = raw[i];
            if (size == 2) v = (uint32_t)raw[2 * i] | (uint32_t)raw[2 * i + 1] << 8;
            if (size == 4) memcpy(&v, raw + 4 * i, 4);
            if (restart && v == restartValue) return L_GEOMETRY;
            index[i] = (uint32_t)((int64_t)v + d->vertexOffset);
        }
    } else {
        for (uint32_t i = 0; i < d->count; i++) index[i] = d->first + i;
    }
    /* triangles over positions in index order */
    uint32_t t = 0;
    if (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) {
        for (uint32_t i = 0; i + 2 < d->count; i += 3) tris[t][0] = i, tris[t][1] = i + 1, tris[t][2] = i + 2, t++;
    } else if (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) {
        for (uint32_t i = 0; i + 2 < d->count; i++) {
            tris[t][0] = i, tris[t][1] = i + 1 + i % 2, tris[t][2] = i + 2 - i % 2;
            t++;
        }
    } else {
        for (uint32_t i = 1; i + 1 < d->count; i++) tris[t][0] = i, tris[t][1] = i + 1, tris[t][2] = 0, t++;
    }
    if (!t || t > 16) return L_GEOMETRY;
    *tcount = t;
    /* vertex input description */
    bool dynVi = p->dynamic & (1u << D_VERTEX_INPUT);
    uint32_t nb = dynVi ? st->viBindingCount : p->bindingCount, na = dynVi ? st->viAttrCount : p->attrCount;
    uint8_t kinds[SPV_LOCATIONS];
    spv_vs_inputs(p->vs, p->vsEntry, kinds);
    for (uint32_t i = 0; i < d->count; i++) {
        SpvVertexIn* v = &in[i];
        memset(v, 0, sizeof *v);
        v->vertexIndex = index[i];
        v->instanceIndex = d->firstInstance;
        v->baseVertex = (uint32_t)d->vertexOffset;
        v->baseInstance = d->firstInstance;
        if (!d->indexed) v->baseVertex = d->first;
        for (uint32_t a = 0; a < na; a++) {
            uint32_t loc = dynVi ? st->viAttrs[a].location : p->attrs[a].location;
            uint32_t binding = dynVi ? st->viAttrs[a].binding : p->attrs[a].binding;
            VkFormat format = dynVi ? st->viAttrs[a].format : p->attrs[a].format;
            uint32_t offset = dynVi ? st->viAttrs[a].offset : p->attrs[a].offset;
            if (loc >= SPV_LOCATIONS || !kinds[loc]) continue;
            /* the binding */
            bool found = false;
            uint32_t stride = 0;
            VkVertexInputRate rate = VK_VERTEX_INPUT_RATE_VERTEX;
            for (uint32_t b = 0; b < nb; b++) {
                uint32_t bb = dynVi ? st->viBindings[b].binding : p->bindings[b].binding;
                if (bb != binding) continue;
                stride = dynVi ? st->viBindings[b].stride : p->bindings[b].stride;
                rate = dynVi ? st->viBindings[b].inputRate : p->bindings[b].inputRate;
                found = true;
            }
            if (!found || binding >= MAX_VB || !(st->vbSet & (1u << binding))) return L_VERTEX;
            if ((p->dynamic & (1u << D_STRIDE)) && !dynVi) {
                if (!(st->strideSet & (1u << binding))) return L_DYNAMIC;
                stride = (uint32_t)st->strides[binding];
            }
            /* instance rate: one instance is looked at, the first (any divisor gives its element) */
            uint32_t element = rate == VK_VERTEX_INPUT_RATE_VERTEX ? index[i] : d->firstInstance;
            uint8_t raw[16] = {0};
            SpvAttr attr;
            memset(&attr, 0, sizeof attr);
            uint32_t bytes = ls_decode(format, raw, &attr);
            if (!bytes) return L_VERTEX;
            VkDeviceSize at = st->vbOffset[binding] + (VkDeviceSize)element * stride + offset;
            if (st->vbSize[binding] != VK_WHOLE_SIZE && (VkDeviceSize)element * stride + offset + bytes > st->vbSize[binding]) return L_VERTEX;
            if (!readBuffer(primary, st->vb[binding], at, bytes, raw)) return L_VERTEX;
            ls_decode(format, raw, &attr);
            v->location[loc] = attr;
        }
    }
    *vcount = d->count;
    return L_APPLIED;
}

static int decide(Cb* primary, const Pending* pb, uint32_t n, const VkCommandBuffer* list) {
    if (n != 1 || !list || !list[0]) return L_SECONDARY;
    pthread_mutex_lock(&trk);
    Cb* s = cbGet(list[0], false);
    pthread_mutex_unlock(&trk);
    if (!s || !s->ours || !s->ended || !s->secondary || !s->oneTime || !primary->oneTime || s->unknown) return L_SECONDARY;
    if (s->draws != 1 || s->others || !s->hasSnap) return L_DRAWS;
    if (s->barriers) return L_BARRIER;
    const DrawSnap* d = &s->snap;
    const Pipe* p = d->st.pipe;
    const RecState* st = &d->st;
    if (!p || p->compute || p->unknown || (p->parts & PARTS_ALL) != PARTS_ALL || p->otherStages || !p->vs) return L_PIPELINE;
    if (p->feedbackLoop) return L_FEEDBACK;
    if (p->colorCount != 1) return L_PIPELINE;
    if (p->untrackedDynamic || (p->dynamic & ~st->dynSet & ~(1u << D_STRIDE))) return L_DYNAMIC;
#define DYN(kind, field) ((p->dynamic & (1u << (kind))) ? st->field : p->field)
    if (DYN(D_BLEND_ENABLE, blend0)) return L_BLEND;
    VkFormat format;
    {
        pthread_mutex_lock(&trk);
        const ViewInfo* v = mapGet(&views, H(pb->color.imageView));
        format = v ? v->format : VK_FORMAT_UNDEFINED;
        pthread_mutex_unlock(&trk);
    }
    uint32_t need = formatComponents(format);
    if ((DYN(D_WRITE_MASK, mask0) & need) != need || !DYN(D_WRITE_ENABLE, writeEnable0)) return L_MASK;
    if (DYN(D_LOGIC_OP_ENABLE, logicOp)) return L_LOGIC_OP;
    if (DYN(D_RASTER_DISCARD, rasterDiscard) || DYN(D_POLYGON, polygon) != VK_POLYGON_MODE_FILL ||
        DYN(D_SAMPLES, samples) != VK_SAMPLE_COUNT_1_BIT || DYN(D_A2C, a2c))
        return L_RASTER;
    if ((p->dynamic & (1u << D_SAMPLE_MASK)) ? !(st->sampleMask0 & 1) : p->sampleMaskOff) return L_RASTER;
    if (DYN(D_SAMPLE_LOCATIONS, sampleLocations) || DYN(D_RASTER_STREAM, rasterStream)) return L_RASTER;
    if (p->fsDiscard != SPV_DISCARD_NO) return L_DISCARD;
    VkViewport vp = DYN(D_VIEWPORT, viewport);
    VkRect2D sc = DYN(D_SCISSOR, scissor);
    uint32_t vpCount = DYN(D_VIEWPORT, viewportCount), scCount = DYN(D_SCISSOR, scissorCount);
    if (!vpCount || !ls_viewport_covers(&vp, pb->info.renderArea)) return L_VIEWPORT;
    if (!scCount || !ls_scissor_covers(&sc, pb->info.renderArea)) return L_SCISSOR;
    VkCullModeFlags cull = DYN(D_CULL, cull);
    VkFrontFace front = DYN(D_FRONT, front);
#undef DYN
    /* geometry */
    SpvVertexIn in[MAX_VERTS];
    SpvVertexOut out[MAX_VERTS];
    uint32_t tris[MAX_VERTS][3], vcount = 0, tcount = 0;
    uint32_t deps = primary->depCount;
    pthread_mutex_lock(&trk);
    int r = gather(primary, d, p, in, &vcount, tris, &tcount);
    pthread_mutex_unlock(&trk);
    if (r != L_APPLIED) {
        primary->depCount = deps;
        return r;
    }
    float pos[MAX_VERTS][4];
    pthread_mutex_lock(&trk);
    const VsCache* hit = p->cache && p->cache->count == vcount && !memcmp(p->cache->in, in, vcount * sizeof *in) ? p->cache : NULL;
    if (hit) r = hit->result, memcpy(pos, hit->pos, vcount * sizeof pos[0]);
    pthread_mutex_unlock(&trk);
    if (!hit && spv_vs_run(p->vs, p->vsEntry, p->vsSpec.entries ? &p->vsSpec.info : NULL, in, vcount, out) != SPV_RUN_OK) r = L_VS;
    for (uint32_t i = 0; !hit && r == L_APPLIED && i < vcount; i++) {
        const SpvVertexOut* o = &out[i];
        if (!o->positionKnown || o->clipCount > SPV_CLIP_MAX || o->cullCount > SPV_CLIP_MAX || o->layerState == 2 ||
            (o->layerState == 1 && o->layer) || o->viewportState == 2 || (o->viewportState == 1 && o->viewport)) {
            r = L_VS;
            break;
        }
        for (uint32_t k = 0; k < o->clipCount && r == L_APPLIED; k++)
            if (o->clipState[k] == 2 || (o->clipState[k] == 1 && !(o->clip[k] >= 0.0f))) r = L_VS;
        for (uint32_t k = 0; k < o->cullCount && r == L_APPLIED; k++)
            if (o->cullState[k] == 2 || (o->cullState[k] == 1 && !(o->cull[k] >= 0.0f))) r = L_VS;
        memcpy(pos[i], o->position, sizeof pos[i]);
    }
    if (!hit) {
        pthread_mutex_lock(&trk);
        Pipe* writable = (Pipe*)p;
        if (!writable->cache) writable->cache = malloc(sizeof *writable->cache);
        if (writable->cache) {
            memcpy(writable->cache->in, in, vcount * sizeof *in);
            writable->cache->count = vcount, writable->cache->result = r;
            memcpy(writable->cache->pos, pos, vcount * sizeof pos[0]);
        }
        pthread_mutex_unlock(&trk);
    }
    if (r == L_APPLIED) {
        int cover = ls_cover((const float(*)[4])pos, vcount, (const uint32_t(*)[3])tris, tcount, &vp, pb->info.renderArea, cull, front,
                             p->negativeOneToOne);
        r = cover == LS_COVER_YES ? L_APPLIED : cover == LS_COVER_EDGE ? L_GEOMETRY_EDGE : L_GEOMETRY;
    }
    if (r != L_APPLIED) primary->depCount = deps;
    return r;
}

/* secondaries' GPU writes become the primary's: they may run until it is reset */
static void inherit(Cb* primary, uint32_t n, const VkCommandBuffer* list) {
    pthread_mutex_lock(&trk);
    for (uint32_t i = 0; list && i < n; i++) {
        Cb* s = cbGet(list[i], false);
        if (!s) continue;
        if (s->taintAll && !primary->taintAll) primary->taintAll = true, taintAllCount++;
        for (uint32_t j = 0; j < s->taintCount; j++) {
            if (primary->taintCount == primary->taintCap) {
                uint32_t cap = primary->taintCap ? primary->taintCap * 2 : 16;
                Range* t = realloc(primary->taints, cap * sizeof *t);
                if (!t) {
                    if (!primary->taintAll) primary->taintAll = true, taintAllCount++;
                    break;
                }
                primary->taints = t, primary->taintCap = cap;
            }
            primary->taints[primary->taintCount++] = s->taints[j];
        }
    }
    pthread_mutex_unlock(&trk);
}

static void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer* list) {
    INC(S_EXECUTE_COMMANDS);
    Cb* c = RECORD ? cbOf(cb) : NULL;
    if (statsOn > 0 && n == 1 && list) {
        pthread_mutex_lock(&trk);
        const Cb* s = cbGet(list[0], false);
        uint32_t draws = s ? s->draws + s->others : 0;
        bool known = s && s->ours && s->secondary;
        pthread_mutex_unlock(&trk);
        if (known) {
            INC(S_SEC_PASS);
            INC(draws == 0 ? S_SEC_0DRAW : draws == 1 ? S_SEC_1DRAW : draws <= 4 ? S_SEC_2_4DRAW : S_SEC_5DRAW);
        }
    }
    if (c && c->pending) {
        c->pending = false;
        int r = decide(c, &c->pb, n, list);
        LOADC(r);
        if (r == L_APPLIED) c->pb.color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        forwardBegin(cb, &c->pb.info, c->pb.khr);
    }
    if (c) {
        if (TRACK) inherit(c, n, list);
        if (c->secondary) c->unknown = true; /* nested */
    }
    N(W_EXECUTE_COMMANDS, PFN_vkCmdExecuteCommands)(cb, n, list);
}

static void VKAPI_CALL CmdClearAttachments(VkCommandBuffer cb, uint32_t n, const VkClearAttachment* a, uint32_t rn, const VkClearRect* r) {
    INC(S_CLEAR_ATTACHMENTS);
    ADD(S_CLEAR_ATTACHMENTS_RECTS, rn);
    Cb* c = rec(cb);
    if (c) c->others++;
    N(W_CLEAR_ATTACHMENTS, PFN_vkCmdClearAttachments)(cb, n, a, rn, r);
}

/* ---- transfers ---- */

static void VKAPI_CALL CmdCopyBuffer(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy* r) {
    INC(S_COPY_BUFFER);
    Cb* c = rec(cb);
    for (uint32_t i = 0; r && i < n; i++) {
        ADD(S_COPY_BUFFER_BYTES, r[i].size);
        if (c && TRACK) addTaint(c, d, r[i].dstOffset, r[i].size);
    }
    N(W_COPY_BUFFER, PFN_vkCmdCopyBuffer)(cb, s, d, n, r);
}
static void VKAPI_CALL CmdCopyBuffer2(VkCommandBuffer cb, const VkCopyBufferInfo2* info) {
    INC(S_COPY_BUFFER);
    Cb* c = rec(cb);
    for (uint32_t i = 0; info && i < info->regionCount; i++) {
        ADD(S_COPY_BUFFER_BYTES, info->pRegions[i].size);
        if (c && TRACK) addTaint(c, info->dstBuffer, info->pRegions[i].dstOffset, info->pRegions[i].size);
    }
    N(W_COPY_BUFFER2, PFN_vkCmdCopyBuffer2)(cb, info);
}
static void b2iRegion(const VkExtent3D* e) {
    INC(S_COPY_B2I_REGIONS);
    uint64_t t = kt(e);
    ADD(S_COPY_B2I_KTEXELS, t / 1000);
    if (t >= 1000000) INC(S_COPY_B2I_LARGE);
}
static void VKAPI_CALL CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer s, VkImage d, VkImageLayout l, uint32_t n, const VkBufferImageCopy* r) {
    INC(S_COPY_B2I);
    rec(cb);
    for (uint32_t i = 0; r && i < n; i++) b2iRegion(&r[i].imageExtent);
    N(W_COPY_B2I, PFN_vkCmdCopyBufferToImage)(cb, s, d, l, n, r);
}
static void VKAPI_CALL CmdCopyBufferToImage2(VkCommandBuffer cb, const VkCopyBufferToImageInfo2* info) {
    INC(S_COPY_B2I);
    rec(cb);
    for (uint32_t i = 0; info && i < info->regionCount; i++) b2iRegion(&info->pRegions[i].imageExtent);
    N(W_COPY_B2I2, PFN_vkCmdCopyBufferToImage2)(cb, info);
}
static void VKAPI_CALL CmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
                                    const VkImageCopy* r) {
    INC(S_COPY_IMAGE);
    rec(cb);
    for (uint32_t i = 0; r && i < n; i++) ADD(S_COPY_IMAGE_KTEXELS, kt(&r[i].extent) / 1000);
    N(W_COPY_IMAGE, PFN_vkCmdCopyImage)(cb, s, sl, d, dl, n, r);
}
static void VKAPI_CALL CmdCopyImage2(VkCommandBuffer cb, const VkCopyImageInfo2* info) {
    INC(S_COPY_IMAGE);
    rec(cb);
    for (uint32_t i = 0; info && i < info->regionCount; i++) ADD(S_COPY_IMAGE_KTEXELS, kt(&info->pRegions[i].extent) / 1000);
    N(W_COPY_IMAGE2, PFN_vkCmdCopyImage2)(cb, info);
}
static uint64_t blitTexels(const VkOffset3D* o) {
    uint64_t w = (uint64_t)abs(o[1].x - o[0].x), h = (uint64_t)abs(o[1].y - o[0].y), z = (uint64_t)abs(o[1].z - o[0].z);
    return w * h * (z ? z : 1);
}
static void VKAPI_CALL CmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
                                    const VkImageBlit* r, VkFilter f) {
    INC(S_BLIT_IMAGE);
    rec(cb);
    for (uint32_t i = 0; r && i < n; i++) ADD(S_BLIT_KTEXELS, blitTexels(r[i].dstOffsets) / 1000);
    N(W_BLIT_IMAGE, PFN_vkCmdBlitImage)(cb, s, sl, d, dl, n, r, f);
}
static void VKAPI_CALL CmdBlitImage2(VkCommandBuffer cb, const VkBlitImageInfo2* info) {
    INC(S_BLIT_IMAGE);
    rec(cb);
    for (uint32_t i = 0; info && i < info->regionCount; i++) ADD(S_BLIT_KTEXELS, blitTexels(info->pRegions[i].dstOffsets) / 1000);
    N(W_BLIT_IMAGE2, PFN_vkCmdBlitImage2)(cb, info);
}
static void VKAPI_CALL CmdResolveImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
                                       const VkImageResolve* r) {
    INC(S_RESOLVE_IMAGE);
    rec(cb);
    for (uint32_t i = 0; r && i < n; i++) ADD(S_RESOLVE_KTEXELS, kt(&r[i].extent) / 1000);
    N(W_RESOLVE_IMAGE, PFN_vkCmdResolveImage)(cb, s, sl, d, dl, n, r);
}
static void VKAPI_CALL CmdResolveImage2(VkCommandBuffer cb, const VkResolveImageInfo2* info) {
    INC(S_RESOLVE_IMAGE);
    rec(cb);
    for (uint32_t i = 0; info && i < info->regionCount; i++) ADD(S_RESOLVE_KTEXELS, kt(&info->pRegions[i].extent) / 1000);
    N(W_RESOLVE_IMAGE2, PFN_vkCmdResolveImage2)(cb, info);
}
static void VKAPI_CALL CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage s, VkImageLayout l, VkBuffer d, uint32_t n, const VkBufferImageCopy* r) {
    INC(S_COPY_I2B);
    Cb* c = rec(cb);
    if (c && TRACK) addTaint(c, d, 0, VK_WHOLE_SIZE);
    for (uint32_t i = 0; r && i < n; i++) ADD(S_COPY_I2B_KTEXELS, kt(&r[i].imageExtent) / 1000);
    N(W_COPY_I2B, PFN_vkCmdCopyImageToBuffer)(cb, s, l, d, n, r);
}
static void VKAPI_CALL CmdCopyImageToBuffer2(VkCommandBuffer cb, const VkCopyImageToBufferInfo2* info) {
    INC(S_COPY_I2B);
    Cb* c = rec(cb);
    if (c && TRACK && info) addTaint(c, info->dstBuffer, 0, VK_WHOLE_SIZE);
    for (uint32_t i = 0; info && i < info->regionCount; i++) ADD(S_COPY_I2B_KTEXELS, kt(&info->pRegions[i].imageExtent) / 1000);
    N(W_COPY_I2B2, PFN_vkCmdCopyImageToBuffer2)(cb, info);
}
static void VKAPI_CALL CmdFillBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize s, uint32_t d) {
    INC(S_FILL_BUFFER);
    Cb* c = rec(cb);
    if (c && TRACK) addTaint(c, b, o, s);
    N(W_FILL_BUFFER, PFN_vkCmdFillBuffer)(cb, b, o, s, d);
}
static void VKAPI_CALL CmdUpdateBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize s, const void* d) {
    INC(S_UPDATE_BUFFER);
    Cb* c = rec(cb);
    if (c && TRACK) addTaint(c, b, o, s);
    N(W_UPDATE_BUFFER, PFN_vkCmdUpdateBuffer)(cb, b, o, s, d);
}
static void VKAPI_CALL CmdClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue* c, uint32_t n,
                                          const VkImageSubresourceRange* r) {
    INC(S_CLEAR_COLOR_IMAGE);
    rec(cb);
    N(W_CLEAR_COLOR_IMAGE, PFN_vkCmdClearColorImage)(cb, i, l, c, n, r);
}
static void VKAPI_CALL CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue* c,
                                                 uint32_t n, const VkImageSubresourceRange* r) {
    INC(S_CLEAR_DS_IMAGE);
    rec(cb);
    N(W_CLEAR_DS_IMAGE, PFN_vkCmdClearDepthStencilImage)(cb, i, l, c, n, r);
}

/* ---- queries ---- */

static void VKAPI_CALL CmdBeginQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f) {
    rec(cb);
    if (statsOn > 0) {
        VkQueryType t = poolType(p);
        INC(t == VK_QUERY_TYPE_OCCLUSION ? S_Q_OCCLUSION : t == VK_QUERY_TYPE_PIPELINE_STATISTICS ? S_Q_PIPELINE_STATS : S_Q_OTHER);
    }
    N(W_BEGIN_QUERY, PFN_vkCmdBeginQuery)(cb, p, q, f);
}
static void VKAPI_CALL CmdEndQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q) {
    rec(cb);
    INC(S_END_QUERY);
    N(W_END_QUERY, PFN_vkCmdEndQuery)(cb, p, q);
}
static void VKAPI_CALL CmdBeginQueryIndexedEXT(VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f, uint32_t i) {
    rec(cb);
    INC(S_Q_OTHER);
    N(W_BEGIN_QUERY_INDEXED, PFN_vkCmdBeginQueryIndexedEXT)(cb, p, q, f, i);
}
static void VKAPI_CALL CmdEndQueryIndexedEXT(VkCommandBuffer cb, VkQueryPool p, uint32_t q, uint32_t i) {
    rec(cb);
    INC(S_END_QUERY);
    N(W_END_QUERY_INDEXED, PFN_vkCmdEndQueryIndexedEXT)(cb, p, q, i);
}
static void VKAPI_CALL CmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits s, VkQueryPool p, uint32_t q) {
    rec(cb);
    INC(S_TIMESTAMP);
    N(W_WRITE_TIMESTAMP, PFN_vkCmdWriteTimestamp)(cb, s, p, q);
}
static void VKAPI_CALL CmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 s, VkQueryPool p, uint32_t q) {
    rec(cb);
    INC(S_TIMESTAMP);
    N(W_WRITE_TIMESTAMP2, PFN_vkCmdWriteTimestamp2)(cb, s, p, q);
}
static void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer cb, VkQueryPool p, uint32_t f, uint32_t n) {
    rec(cb);
    INC(S_CMD_RESET_QUERY);
    N(W_CMD_RESET_QUERY, PFN_vkCmdResetQueryPool)(cb, p, f, n);
}
static void VKAPI_CALL CmdCopyQueryPoolResults(VkCommandBuffer cb, VkQueryPool p, uint32_t f, uint32_t n, VkBuffer b, VkDeviceSize o,
                                               VkDeviceSize s, VkQueryResultFlags fl) {
    INC(S_COPY_QUERY_RESULTS);
    Cb* c = rec(cb);
    if (c && TRACK) addTaint(c, b, o, n ? (VkDeviceSize)(n - 1) * s + 128 : 0);
    N(W_COPY_QUERY_RESULTS, PFN_vkCmdCopyQueryPoolResults)(cb, p, f, n, b, o, s, fl);
}
static VkResult VKAPI_CALL GetQueryPoolResults(VkDevice d, VkQueryPool p, uint32_t f, uint32_t n, size_t ds, void* data, VkDeviceSize s,
                                               VkQueryResultFlags fl) {
    INC(S_HOST_QUERY_RESULTS);
    return N(W_GET_QUERY_RESULTS, PFN_vkGetQueryPoolResults)(d, p, f, n, ds, data, s, fl);
}
static void VKAPI_CALL ResetQueryPool(VkDevice d, VkQueryPool p, uint32_t f, uint32_t n) {
    INC(S_HOST_RESET_QUERY);
    N(W_RESET_QUERY_POOL, PFN_vkResetQueryPool)(d, p, f, n);
}

/* ---- barriers ---- */

static void barrier(VkCommandBuffer cb) {
    Cb* c = rec(cb);
    if (c) c->barriers++;
    if (tlInPass) INC(S_BARRIER2_IN_PASS);
}
static void barrier2(const VkDependencyInfo* info) {
    INC(S_BARRIER2);
    if (!info || statsOn <= 0) return;
    ADD(S_BARRIER2_MEMORY, info->memoryBarrierCount);
    ADD(S_BARRIER2_BUFFER, info->bufferMemoryBarrierCount);
    ADD(S_BARRIER2_IMAGE, info->imageMemoryBarrierCount);
    for (uint32_t i = 0; i < info->imageMemoryBarrierCount; i++)
        if (info->pImageMemoryBarriers[i].oldLayout != info->pImageMemoryBarriers[i].newLayout) INC(S_BARRIER2_LAYOUT);
}
static void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* info) {
    barrier(cb);
    barrier2(info);
    N(W_BARRIER2, PFN_vkCmdPipelineBarrier2)(cb, info);
}
static void VKAPI_CALL CmdPipelineBarrier2KHR(VkCommandBuffer cb, const VkDependencyInfo* info) {
    barrier(cb);
    barrier2(info);
    N(W_BARRIER2_KHR, PFN_vkCmdPipelineBarrier2)(cb, info);
}
static void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f,
                                          uint32_t mn, const VkMemoryBarrier* m, uint32_t bn, const VkBufferMemoryBarrier* b, uint32_t in,
                                          const VkImageMemoryBarrier* im) {
    barrier(cb);
    INC(S_BARRIER1);
    N(W_BARRIER1, PFN_vkCmdPipelineBarrier)(cb, s, d, f, mn, m, bn, b, in, im);
}
static void VKAPI_CALL CmdSetEvent2(VkCommandBuffer cb, VkEvent e, const VkDependencyInfo* info) {
    rec(cb);
    INC(S_SET_EVENT);
    N(W_SET_EVENT2, PFN_vkCmdSetEvent2)(cb, e, info);
}
static void VKAPI_CALL CmdWaitEvents2(VkCommandBuffer cb, uint32_t n, const VkEvent* e, const VkDependencyInfo* info) {
    barrier(cb);
    INC(S_WAIT_EVENTS);
    N(W_WAIT_EVENTS2, PFN_vkCmdWaitEvents2)(cb, n, e, info);
}
static void VKAPI_CALL CmdWaitEvents(VkCommandBuffer cb, uint32_t n, const VkEvent* e, VkPipelineStageFlags s, VkPipelineStageFlags d,
                                     uint32_t mn, const VkMemoryBarrier* m, uint32_t bn, const VkBufferMemoryBarrier* b, uint32_t in,
                                     const VkImageMemoryBarrier* im) {
    barrier(cb);
    INC(S_WAIT_EVENTS);
    N(W_WAIT_EVENTS, PFN_vkCmdWaitEvents)(cb, n, e, s, d, mn, m, bn, b, in, im);
}

/* ---- submissions ---- */

/* the data the skipped loads were decided on, read again */
static void checkDeps(VkCommandBuffer cb) {
    pthread_mutex_lock(&trk);
    Cb* c = cbGet(cb, false);
    for (uint32_t i = 0; c && i < c->depCount; i++) {
        const Dep* d = &c->deps[i];
        const MemInfo* m = mapGet(&memories, H(d->memory));
        if (!m || !m->ptr || d->offset < m->mapOffset || d->offset - m->mapOffset > m->mapSize ||
            d->size > m->mapSize - (d->offset - m->mapOffset))
            continue;
        if (fnv(m->ptr + (d->offset - m->mapOffset), d->size) != d->hash) {
            LOADC(L_STALE);
            break;
        }
    }
    pthread_mutex_unlock(&trk);
}

static void readSkipFile(void);

static VkResult VKAPI_CALL QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    readSkipFile();
    INC(S_SUBMIT);
    for (uint32_t i = 0; s && i < n; i++) {
        ADD(S_SUBMIT_CMDBUFS, s[i].commandBufferCount);
        for (uint32_t j = 0; TRACK && j < s[i].commandBufferCount; j++) checkDeps(s[i].pCommandBuffers[j]);
    }
    return N(W_SUBMIT, PFN_vkQueueSubmit)(q, n, s, f);
}
static void submit2(uint32_t n, const VkSubmitInfo2* s) {
    readSkipFile();
    INC(S_SUBMIT);
    for (uint32_t i = 0; s && i < n; i++) {
        ADD(S_SUBMIT_CMDBUFS, s[i].commandBufferInfoCount);
        for (uint32_t j = 0; TRACK && j < s[i].commandBufferInfoCount; j++) checkDeps(s[i].pCommandBufferInfos[j].commandBuffer);
    }
}
static VkResult VKAPI_CALL QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    submit2(n, s);
    return N(W_SUBMIT2, PFN_vkQueueSubmit2)(q, n, s, f);
}
static VkResult VKAPI_CALL QueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    submit2(n, s);
    return N(W_SUBMIT2_KHR, PFN_vkQueueSubmit2)(q, n, s, f);
}
static VkResult VKAPI_CALL WaitForFences(VkDevice d, uint32_t n, const VkFence* f, VkBool32 all, uint64_t t) {
    INC(S_WAIT_FENCES);
    return N(W_WAIT_FENCES, PFN_vkWaitForFences)(d, n, f, all, t);
}
static VkResult VKAPI_CALL WaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo* w, uint64_t t) {
    INC(S_WAIT_SEMAPHORES);
    return N(W_WAIT_SEMAPHORES, PFN_vkWaitSemaphores)(d, w, t);
}
static VkResult VKAPI_CALL QueueWaitIdle(VkQueue q) {
    INC(S_QUEUE_WAIT_IDLE);
    return N(W_QUEUE_WAIT_IDLE, PFN_vkQueueWaitIdle)(q);
}

/* ---- logging ---- */

static void statsLog(bool force) {
    uint64_t now = icd_now_ns();
    if (!force && (int64_t)(now - __atomic_load_n(&lastLogNs, __ATOMIC_RELAXED)) < (int64_t)logSeconds * 1000000000ll) return;
    if (pthread_mutex_trylock(&logLock)) return;
    if (!force && (int64_t)(now - lastLogNs) < (int64_t)logSeconds * 1000000000ll) {
        pthread_mutex_unlock(&logLock);
        return;
    }
    uint64_t p = __atomic_load_n(&presents, __ATOMIC_RELAXED);
    char line[8192];
    if (statsOn > 0) {
        int n = snprintf(line, sizeof line, "hybris-icd: cmd stats: pid %d, seconds %.1f, dt %.2f, presents %llu, dp %llu, max-area %llu",
                         (int)getpid(), (now - startNs) / 1e9, (now - lastLogNs) / 1e9, (unsigned long long)p,
                         (unsigned long long)(p - loggedPresents), (unsigned long long)__atomic_load_n(&maxArea, __ATOMIC_RELAXED));
        for (int i = 0; i < S_COUNT && n > 0 && n < (int)sizeof line - 64; i++) {
            uint64_t v = __atomic_load_n(&stats[i], __ATOMIC_RELAXED);
            uint64_t d = v - logged[i];
            logged[i] = v;
            if (d) n += snprintf(line + n, sizeof line - n, ", %s %llu", statNames[i], (unsigned long long)d);
        }
        fprintf(stderr, "%s\n", line);
        pthread_mutex_lock(&labelLock);
        int count = labelCount;
        for (int i = 0; i < count; i++) {
            Label* l = &labels[i];
            uint64_t b = __atomic_load_n(&l->begun, __ATOMIC_RELAXED), ps = __atomic_load_n(&l->passes, __ATOMIC_RELAXED),
                     dr = __atomic_load_n(&l->draws, __ATOMIC_RELAXED), fl = __atomic_load_n(&l->fullLoads, __ATOMIC_RELAXED);
            if (b == l->loggedBegun && ps == l->loggedPasses && dr == l->loggedDraws) continue;
            fprintf(stderr, "hybris-icd: cmd label: dp %llu, begun %llu, passes %llu, draws %llu, full-loads %llu, text %s\n",
                    (unsigned long long)(p - loggedPresents), (unsigned long long)(b - l->loggedBegun),
                    (unsigned long long)(ps - l->loggedPasses), (unsigned long long)(dr - l->loggedDraws),
                    (unsigned long long)(fl - l->loggedFullLoads), l->text);
            l->loggedBegun = b;
            l->loggedPasses = ps;
            l->loggedDraws = dr;
            l->loggedFullLoads = fl;
        }
        if (labelOverflow) fprintf(stderr, "hybris-icd: cmd label: overflow %llu\n", (unsigned long long)labelOverflow);
        pthread_mutex_unlock(&labelLock);
    }
    if (skipMode) {
        uint64_t delta[L_COUNT], total = 0;
        for (int i = 0; i < L_COUNT; i++) {
            uint64_t v = __atomic_load_n(&loads[i], __ATOMIC_RELAXED);
            delta[i] = v - loadsLogged[i];
            loadsLogged[i] = v;
            if (i != L_STALE) total += delta[i];
        }
        int n = snprintf(line, sizeof line, "hybris-icd: load skip: mode %s, dt %.2f, dp %llu, passes %llu, applied %llu, stale %llu, kept %llu",
                         modeNames[applyMode()], (now - lastLogNs) / 1e9, (unsigned long long)(p - loggedPresents), (unsigned long long)total,
                         (unsigned long long)delta[L_APPLIED],
                         (unsigned long long)delta[L_STALE], (unsigned long long)(total - delta[L_APPLIED]));
        for (int i = L_APPLIED + 1; i < L_COUNT && n > 0 && n < (int)sizeof line - 64; i++)
            if (i != L_STALE && delta[i]) n += snprintf(line + n, sizeof line - n, ", %s %llu", loadNames[i], (unsigned long long)delta[i]);
        fprintf(stderr, "%s\n", line);
    }
    loggedPresents = p;
    __atomic_store_n(&lastLogNs, now, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&logLock);
}

/* At submissions and presents, at most twice a second: the file's mode; a change ends the interval of the old one. */
static void readSkipFile(void) {
    if (!skipFile) return;
    uint64_t now = icd_now_ns(), last = __atomic_load_n(&skipFileNs, __ATOMIC_RELAXED);
    if (now - last < 500000000ull || !__atomic_compare_exchange_n(&skipFileNs, &last, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;
    int mode = skipFileMode();
    if (mode == applyMode()) return;
    statsLog(true);
    __atomic_store_n(&skipApply, mode, __ATOMIC_RELAXED);
    fprintf(stderr, "hybris-icd: load skip: mode %s\n", modeNames[mode]);
}

static VkResult VKAPI_CALL QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* info) {
    VkResult r = N(W_PRESENT, PFN_vkQueuePresentKHR)(q, info);
    __atomic_add_fetch(&presents, 1, __ATOMIC_RELAXED);
    readSkipFile();
    statsLog(false);
    return r;
}

static void VKAPI_CALL CmdBeginDebugUtilsLabelEXT(VkCommandBuffer cb, const VkDebugUtilsLabelEXT* label) {
    rec(cb);
    if (statsOn > 0) {
        INC(S_LABEL_BEGIN);
        int l = labelIndex(label ? label->pLabelName : NULL);
        if (l >= 0) __atomic_add_fetch(&labels[l].begun, 1, __ATOMIC_RELAXED);
        if (tlDepth < STACK) tlStack[tlDepth] = l;
        tlDepth++;
    }
    N(W_LABEL_BEGIN, PFN_vkCmdBeginDebugUtilsLabelEXT)(cb, label);
}
static void VKAPI_CALL CmdEndDebugUtilsLabelEXT(VkCommandBuffer cb) {
    rec(cb);
    if (statsOn > 0 && tlDepth > 0) tlDepth--;
    N(W_LABEL_END, PFN_vkCmdEndDebugUtilsLabelEXT)(cb);
}
static void VKAPI_CALL CmdInsertDebugUtilsLabelEXT(VkCommandBuffer cb, const VkDebugUtilsLabelEXT* label) {
    rec(cb);
    INC(S_LABEL_INSERT);
    N(W_LABEL_INSERT, PFN_vkCmdInsertDebugUtilsLabelEXT)(cb, label);
}

#define WR(slot, fn) [slot] = (PFN_vkVoidFunction)fn
static const PFN_vkVoidFunction wrappers[W_COUNT] = {
    WR(W_BEGIN_RENDERING, CmdBeginRendering), WR(W_BEGIN_RENDERING_KHR, CmdBeginRenderingKHR), WR(W_END_RENDERING, CmdEndRendering),
    WR(W_END_RENDERING_KHR, CmdEndRenderingKHR), WR(W_BEGIN_RENDER_PASS, CmdBeginRenderPass), WR(W_BEGIN_RENDER_PASS2, CmdBeginRenderPass2),
    WR(W_DRAW, CmdDraw), WR(W_DRAW_INDEXED, CmdDrawIndexed), WR(W_DRAW_INDIRECT, CmdDrawIndirect),
    WR(W_DRAW_INDEXED_INDIRECT, CmdDrawIndexedIndirect), WR(W_DRAW_INDIRECT_COUNT, CmdDrawIndirectCount),
    WR(W_DRAW_INDEXED_INDIRECT_COUNT, CmdDrawIndexedIndirectCount), WR(W_DRAW_MULTI, CmdDrawMultiEXT),
    WR(W_DRAW_MULTI_INDEXED, CmdDrawMultiIndexedEXT), WR(W_DRAW_BYTE_COUNT, CmdDrawIndirectByteCountEXT),
    WR(W_DRAW_MESH, CmdDrawMeshTasksEXT), WR(W_DRAW_MESH_INDIRECT, CmdDrawMeshTasksIndirectEXT),
    WR(W_DRAW_MESH_INDIRECT_COUNT, CmdDrawMeshTasksIndirectCountEXT),
    WR(W_DISPATCH, CmdDispatch), WR(W_DISPATCH_INDIRECT, CmdDispatchIndirect), WR(W_DISPATCH_BASE, CmdDispatchBase),
    WR(W_EXECUTE_COMMANDS, CmdExecuteCommands), WR(W_BIND_PIPELINE, CmdBindPipeline), WR(W_BIND_SHADERS, CmdBindShadersEXT),
    WR(W_COPY_BUFFER, CmdCopyBuffer), WR(W_COPY_BUFFER2, CmdCopyBuffer2), WR(W_COPY_B2I, CmdCopyBufferToImage),
    WR(W_COPY_B2I2, CmdCopyBufferToImage2), WR(W_COPY_IMAGE, CmdCopyImage), WR(W_COPY_IMAGE2, CmdCopyImage2),
    WR(W_BLIT_IMAGE, CmdBlitImage), WR(W_BLIT_IMAGE2, CmdBlitImage2), WR(W_RESOLVE_IMAGE, CmdResolveImage),
    WR(W_RESOLVE_IMAGE2, CmdResolveImage2), WR(W_COPY_I2B, CmdCopyImageToBuffer), WR(W_COPY_I2B2, CmdCopyImageToBuffer2),
    WR(W_FILL_BUFFER, CmdFillBuffer), WR(W_UPDATE_BUFFER, CmdUpdateBuffer),
    WR(W_CLEAR_COLOR_IMAGE, CmdClearColorImage), WR(W_CLEAR_DS_IMAGE, CmdClearDepthStencilImage), WR(W_CLEAR_ATTACHMENTS, CmdClearAttachments),
    WR(W_CREATE_QUERY_POOL, CreateQueryPool), WR(W_BEGIN_QUERY, CmdBeginQuery), WR(W_END_QUERY, CmdEndQuery),
    WR(W_BEGIN_QUERY_INDEXED, CmdBeginQueryIndexedEXT), WR(W_END_QUERY_INDEXED, CmdEndQueryIndexedEXT),
    WR(W_WRITE_TIMESTAMP, CmdWriteTimestamp), WR(W_WRITE_TIMESTAMP2, CmdWriteTimestamp2), WR(W_CMD_RESET_QUERY, CmdResetQueryPool),
    WR(W_COPY_QUERY_RESULTS, CmdCopyQueryPoolResults), WR(W_GET_QUERY_RESULTS, GetQueryPoolResults), WR(W_RESET_QUERY_POOL, ResetQueryPool),
    WR(W_BARRIER2, CmdPipelineBarrier2), WR(W_BARRIER2_KHR, CmdPipelineBarrier2KHR), WR(W_BARRIER1, CmdPipelineBarrier),
    WR(W_SET_EVENT2, CmdSetEvent2), WR(W_WAIT_EVENTS2, CmdWaitEvents2), WR(W_WAIT_EVENTS, CmdWaitEvents),
    WR(W_SUBMIT, QueueSubmit), WR(W_SUBMIT2, QueueSubmit2), WR(W_SUBMIT2_KHR, QueueSubmit2KHR), WR(W_WAIT_FENCES, WaitForFences),
    WR(W_WAIT_SEMAPHORES, WaitSemaphores), WR(W_QUEUE_WAIT_IDLE, QueueWaitIdle), WR(W_PRESENT, QueuePresentKHR),
    WR(W_LABEL_BEGIN, CmdBeginDebugUtilsLabelEXT), WR(W_LABEL_END, CmdEndDebugUtilsLabelEXT), WR(W_LABEL_INSERT, CmdInsertDebugUtilsLabelEXT),
    WR(W_BEGIN_CB, BeginCommandBuffer), WR(W_END_CB, EndCommandBuffer), WR(W_RESET_CB, ResetCommandBuffer),
    WR(W_ALLOCATE_CBS, AllocateCommandBuffers), WR(W_FREE_CBS, FreeCommandBuffers), WR(W_RESET_POOL, ResetCommandPool),
    WR(W_DESTROY_POOL, DestroyCommandPool),
    WR(W_CREATE_GFX_PIPELINES, CreateGraphicsPipelines), WR(W_CREATE_COMPUTE_PIPELINES, CreateComputePipelines),
    WR(W_DESTROY_PIPELINE, DestroyPipeline), WR(W_CREATE_SHADER_MODULE, CreateShaderModule), WR(W_DESTROY_SHADER_MODULE, DestroyShaderModule),
    WR(W_CREATE_IMAGE, CreateImage), WR(W_DESTROY_IMAGE, DestroyImage), WR(W_CREATE_IMAGE_VIEW, CreateImageView),
    WR(W_DESTROY_IMAGE_VIEW, DestroyImageView), WR(W_CREATE_BUFFER, CreateBuffer), WR(W_DESTROY_BUFFER, DestroyBuffer),
    WR(W_BIND_BUFFER_MEMORY, BindBufferMemory), WR(W_BIND_BUFFER_MEMORY2, BindBufferMemory2), WR(W_ALLOCATE_MEMORY, AllocateMemory),
    WR(W_FREE_MEMORY, FreeMemory), WR(W_MAP_MEMORY, MapMemory), WR(W_MAP_MEMORY2, MapMemory2), WR(W_UNMAP_MEMORY, UnmapMemory),
    WR(W_UNMAP_MEMORY2, UnmapMemory2),
    WR(W_BIND_VERTEX_BUFFERS, CmdBindVertexBuffers), WR(W_BIND_VERTEX_BUFFERS2, CmdBindVertexBuffers2),
    WR(W_BIND_INDEX_BUFFER, CmdBindIndexBuffer), WR(W_BIND_INDEX_BUFFER2, CmdBindIndexBuffer2),
    WR(W_BIND_XFB_BUFFERS, CmdBindTransformFeedbackBuffersEXT),
    WR(W_SET_VIEWPORT, CmdSetViewport), WR(W_SET_VIEWPORT_COUNT, CmdSetViewportWithCount), WR(W_SET_SCISSOR, CmdSetScissor),
    WR(W_SET_SCISSOR_COUNT, CmdSetScissorWithCount), WR(W_SET_CULL, CmdSetCullMode), WR(W_SET_FRONT, CmdSetFrontFace),
    WR(W_SET_TOPOLOGY, CmdSetPrimitiveTopology), WR(W_SET_RESTART, CmdSetPrimitiveRestartEnable),
    WR(W_SET_RASTER_DISCARD, CmdSetRasterizerDiscardEnable), WR(W_SET_VERTEX_INPUT, CmdSetVertexInputEXT),
    WR(W_SET_WRITE_ENABLE, CmdSetColorWriteEnableEXT), WR(W_SET_WRITE_MASK, CmdSetColorWriteMaskEXT),
    WR(W_SET_BLEND_ENABLE, CmdSetColorBlendEnableEXT), WR(W_SET_LOGIC_OP_ENABLE, CmdSetLogicOpEnableEXT),
    WR(W_SET_A2C, CmdSetAlphaToCoverageEnableEXT), WR(W_SET_SAMPLE_MASK, CmdSetSampleMaskEXT),
    WR(W_SET_SAMPLES, CmdSetRasterizationSamplesEXT), WR(W_SET_POLYGON, CmdSetPolygonModeEXT),
    WR(W_SET_SAMPLE_LOCATIONS, CmdSetSampleLocationsEnableEXT), WR(W_SET_RASTER_STREAM, CmdSetRasterizationStreamEXT),
    WR(W_BEGIN_CONDITIONAL, CmdBeginConditionalRenderingEXT), WR(W_END_CONDITIONAL, CmdEndConditionalRenderingEXT)};

_Static_assert(ARRAY_LEN(statNames) == S_COUNT, "statNames");
_Static_assert(ARRAY_LEN(loadNames) == L_COUNT, "loadNames");
_Static_assert(ARRAY_LEN(wrapNames) == W_COUNT, "wrapNames");
_Static_assert(ARRAY_LEN(wrappers) == W_COUNT, "wrappers");

/* ---- every other command ----
 * With the load skip on, any other vkCmd* that is not pure state goes through a stub: a held-back
 * vkCmdBeginRendering goes to the driver first (so nothing overtakes it), it may write any memory
 * (unless known not to), and a secondary recording it is not followed. The stubs are signature-free:
 * the argument registers are kept across the call and the driver's function is branched to. */

/* pure state, harmless out of order and checked through the pipeline's dynamic states */
static bool stateOnly(const char* name) {
    static const char* const notState[] = {"vkCmdSetRenderingAttachmentLocations", "vkCmdSetRenderingInputAttachmentIndices",
                                           "vkCmdSetEvent", "vkCmdSetDeviceMask", "vkCmdSetExclusiveScissor",
                                           "vkCmdSetViewportShadingRatePalette", "vkCmdSetCoarseSampleOrder",
                                           "vkCmdBindShadingRateImage", "vkCmdBindInvocationMask"};
    for (size_t i = 0; i < ARRAY_LEN(notState); i++)
        if (!strncmp(name, notState[i], strlen(notState[i]))) return false;
    return !strncmp(name, "vkCmdSet", 8) || !strncmp(name, "vkCmdBind", 9) || !strncmp(name, "vkCmdPush", 9);
}

static bool writesNothing(const char* name) {
    static const char* const none[] = {"vkCmdDebugMarker", "vkCmdResetEvent", "vkCmdEndRenderPass", "vkCmdNextSubpass",
                                       "vkCmdSetRenderingAttachmentLocations", "vkCmdSetRenderingInputAttachmentIndices",
                                       "vkCmdSetEvent", "vkCmdSetDeviceMask"};
    for (size_t i = 0; i < ARRAY_LEN(none); i++)
        if (!strncmp(name, none[i], strlen(none[i]))) return true;
    return false;
}

#define GEN_SLOTS 512
#define GEN_STUB 16
static char* genNames[GEN_SLOTS];
static PFN_vkVoidFunction genNext[GEN_SLOTS];
static bool genWrites[GEN_SLOTS];
static uint32_t genCount;
static pthread_mutex_t genLock = PTHREAD_MUTEX_INITIALIZER;

#if defined(__aarch64__)
__attribute__((used, visibility("hidden"))) PFN_vkVoidFunction dd_cmd_gen_enter(VkCommandBuffer cb, uint64_t slot);
PFN_vkVoidFunction dd_cmd_gen_enter(VkCommandBuffer cb, uint64_t slot) {
    PFN_vkVoidFunction next = __atomic_load_n(&genNext[slot], __ATOMIC_ACQUIRE);
    Cb* c = rec(cb);
    if (c) {
        if (genWrites[slot] && TRACK) taintEverything(c);
        if (c->secondary) c->unknown = true;
    }
    return next;
}

/* stub i: bti c; x17 = i; to the common part, which keeps x0-x8 and v0-v7 around dd_cmd_gen_enter */
extern const char dd_cmd_gen_stubs[] __attribute__((visibility("hidden")));
__asm__(".text\n"
        ".p2align 4\n"
        ".globl dd_cmd_gen_stubs\n"
        ".hidden dd_cmd_gen_stubs\n"
        ".type dd_cmd_gen_stubs, %function\n"
        "dd_cmd_gen_stubs:\n"
        ".set dd_cmd_gen_i, 0\n"
        ".rept 512\n"
        "hint #34\n"
        "movz x17, #dd_cmd_gen_i\n"
        "b dd_cmd_gen_common\n"
        "nop\n"
        ".set dd_cmd_gen_i, dd_cmd_gen_i + 1\n"
        ".endr\n"
        "dd_cmd_gen_common:\n"
        "stp x29, x30, [sp, #-224]!\n"
        "mov x29, sp\n"
        "stp x0, x1, [sp, #16]\n"
        "stp x2, x3, [sp, #32]\n"
        "stp x4, x5, [sp, #48]\n"
        "stp x6, x7, [sp, #64]\n"
        "stp x8, x17, [sp, #80]\n"
        "stp q0, q1, [sp, #96]\n"
        "stp q2, q3, [sp, #128]\n"
        "stp q4, q5, [sp, #160]\n"
        "stp q6, q7, [sp, #192]\n"
        "mov x1, x17\n"
        "bl dd_cmd_gen_enter\n"
        "mov x16, x0\n"
        "ldp q6, q7, [sp, #192]\n"
        "ldp q4, q5, [sp, #160]\n"
        "ldp q2, q3, [sp, #128]\n"
        "ldp q0, q1, [sp, #96]\n"
        "ldp x8, x17, [sp, #80]\n"
        "ldp x6, x7, [sp, #64]\n"
        "ldp x4, x5, [sp, #48]\n"
        "ldp x2, x3, [sp, #32]\n"
        "ldp x0, x1, [sp, #16]\n"
        "ldp x29, x30, [sp], #224\n"
        "br x16\n"
        ".size dd_cmd_gen_stubs, . - dd_cmd_gen_stubs\n");
_Static_assert(GEN_SLOTS == 512 && GEN_STUB == 16, "the stubs above");
#define GEN_STUB_AT(i) ((PFN_vkVoidFunction)(uintptr_t)(dd_cmd_gen_stubs + (size_t)(i) * GEN_STUB))
#endif

static PFN_vkVoidFunction genWrap(const char* name, PFN_vkVoidFunction next) {
#if defined(__aarch64__)
    pthread_mutex_lock(&genLock);
    uint32_t i = 0;
    while (i < genCount && strcmp(genNames[i], name)) i++;
    if (i == genCount && (genCount == GEN_SLOTS || !(genNames[i] = strdup(name)))) {
        genMissing = true;
        pthread_mutex_unlock(&genLock);
        fprintf(stderr, "hybris-icd: load skip: %s not wrapped; LOAD stays\n", name);
        return next;
    }
    if (i == genCount) genWrites[i] = !writesNothing(name), genCount++;
    PFN_vkVoidFunction stub = GEN_STUB_AT(i);
    if (next != stub) __atomic_store_n(&genNext[i], next, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&genLock);
    return stub;
#else
    genMissing = true;
    return next;
#endif
}

/* The wrapper in front of `next` (what vkGetDeviceProcAddr would hand out) for the commands here. */
PFN_vkVoidFunction cmd_stats_wrap(const char* name, PFN_vkVoidFunction next) {
    if (!next || !name) return next;
    int index = -1;
    for (int i = 0; i < W_COUNT && index < 0; i++)
        if (!strcmp(name, wrapNames[i])) index = i;
    for (size_t i = 0; i < ARRAY_LEN(aliases) && index < 0; i++)
        if (!strcmp(name, aliases[i].name)) index = aliases[i].index;
    if (index < 0 && TRACK && !strncmp(name, "vkCmd", 5) && !stateOnly(name)) return genWrap(name, next);
    if (index < 0 || !wrappers[index]) return next;
    if (next == wrappers[index]) return next;
    __atomic_store_n(&nextFn[index], next, __ATOMIC_RELEASE);
    return wrappers[index];
}

void cmd_stats_flush(void) {
    if (cmd_stats_on()) statsLog(true);
}
