/* [mali-g2] Command streaming (vortek-0009 / winlator-0012): a command buffer whose recorded
 * commands outgrow VORTEK_CMD_CHUNK (default 32 KiB, 0 = off) reaches the server in several
 * REQUEST_CODE_DD_CMD_CHUNK requests before vkEndCommandBuffer. One primary command buffer of
 * roughly 750 KiB of commands, checked by readback every iteration:
 *   - render pass 1 (inline): 4096 draws, one 4x4 cell of a 256x256 image each, colour and
 *     rectangle in push constants, with vkCmdSetScissor / vkCmdBindPipeline mixed in;
 *   - render pass 2 (LOAD): vkCmdExecuteCommands of a secondary command buffer (also streamed)
 *     that overdraws every 7th cell, so the order across chunk boundaries and command buffers shows;
 *   - transfer: vkCmdFillBuffer, then 4096 vkCmdUpdateBuffer from a stack array that is scribbled
 *     over right after each call (data must be copied at record time), then vkCmdFillBuffer over
 *     part of every 5th record, then vkCmdCopyBuffer into the readback buffer.
 * Iterations re-record the same command buffers: after vkResetCommandBuffer of a half-recorded
 * buffer (chunks already replayed on the server), by an implicit reset in vkBeginCommandBuffer, and
 * while another thread keeps making driver calls (the chunk send only tries the client's call lock).
 * Then timing: record / end / submit+wait wall time, without and with simulated application work
 * per draw (CMD_STREAM_WORK_NS, default 1000 ns); run it with VORTEK_CMD_CHUNK=0 and unset to
 * compare. Prints CMDSTREAM-PASS.
 * Build: cc -O2 -o cmd_stream_test cmd_stream_test.c -lvulkan -lpthread
 *        (shaders: cmd_stream.vert/cmd_stream.frag -> cmd_stream_spv.h with glslangValidator -V --vn) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <vulkan/vulkan.h>
#include "cmd_stream_spv.h"

#define W 256
#define H 256
#define CELL 4
#define CELLS_X (W / CELL)
#define CELLS (CELLS_X * (H / CELL))   /* 4096 */
#define SLOTS 4096                     /* 16-byte records in the transfer part */
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

typedef struct { float rect[4]; float color[4]; } Push;

static VkDevice dev;
static VkPipelineLayout layout;
static VkPipeline pipeline;
static VkRenderPass rp1, rp2;
static VkFramebuffer fb;
static VkImage img;
static VkBuffer tbuf, rbuf;   /* transfer target, readback */
static uint8_t* rmap;
static long workNs;

/* another thread making driver calls while the main thread records: the client's call lock is
 * busy now and then when a chunk is due (the chunk send only tries the lock) */
static atomic_int helperStop;
static atomic_long helperCalls;
static void* helperThread(void* arg) {
    VkFence f = *(VkFence*)arg;
    while (!atomic_load(&helperStop)) {
        VkResult r = vkGetFenceStatus(dev, f);
        if (r != VK_SUCCESS && r != VK_NOT_READY) { printf("FAIL helper vkGetFenceStatus = %d\n", r); exit(1); }
        atomic_fetch_add(&helperCalls, 1);
    }
    return NULL;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void appWork(void) {   /* stands in for the application's own work per draw (DXVK) */
    if (workNs <= 0) return;
    double end = now() + workNs / 1e9;
    while (now() < end) {}
}

static uint32_t findType(const VkPhysicalDeviceMemoryProperties* mp, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
    printf("FAIL no memory type 0x%x\n", want);
    exit(1);
}

/* the expected colours, as bytes (r, g, b, 255) */
static void color1(int cell, int seed, uint8_t out[4]) {
    out[0] = (uint8_t)(cell * 7 + seed * 31);
    out[1] = (uint8_t)(cell >> 4);
    out[2] = (uint8_t)(cell * 13 + 5 + seed);
    out[3] = 255;
}
static void color2(int cell, int seed, uint8_t out[4]) {
    out[0] = (uint8_t)(255 - cell);
    out[1] = (uint8_t)(200 + seed);
    out[2] = (uint8_t)(cell * 3);
    out[3] = 255;
}

static void cellPush(int cell, const uint8_t c[4], Push* p) {
    int cx = cell % CELLS_X, cy = cell / CELLS_X;
    p->rect[0] = (cx * CELL) * 2.0f / W - 1.0f;
    p->rect[1] = (cy * CELL) * 2.0f / H - 1.0f;
    p->rect[2] = ((cx + 1) * CELL) * 2.0f / W - 1.0f;
    p->rect[3] = ((cy + 1) * CELL) * 2.0f / H - 1.0f;
    for (int i = 0; i < 4; i++) p->color[i] = c[i] / 255.0f;
}

static void setViewportScissor(VkCommandBuffer cb) {
    VkViewport vp = {0, 0, W, H, 0, 1};
    VkRect2D sc = {{0, 0}, {W, H}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

/* overdraw of every 7th cell, in a secondary command buffer */
static void recordSecondary(VkCommandBuffer sb, int seed) {
    VkCommandBufferInheritanceInfo inh = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, 0, rp2, 0, fb};
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0,
        VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, &inh};
    CK(vkBeginCommandBuffer(sb, &bi));
    vkCmdBindPipeline(sb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    setViewportScissor(sb);
    for (int cell = 3; cell < CELLS; cell += 7) {
        uint8_t c[4]; Push p;
        color2(cell, seed, c);
        cellPush(cell, c, &p);
        vkCmdPushConstants(sb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
        vkCmdDraw(sb, 6, 1, 0, 0);
        appWork();
    }
    CK(vkEndCommandBuffer(sb));
}

/* Records the primary. drawLimit < CELLS stops after that many draws without ending (the caller
 * then resets the half-recorded buffer). */
static void recordPrimary(VkCommandBuffer cb, VkCommandBuffer sb, int seed, int drawLimit) {
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};   /* resubmitted below */
    CK(vkBeginCommandBuffer(cb, &bi));

    /* render pass 1: every cell */
    VkClearValue cv = {.color = {{0, 0, 0, 0}}};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, rp1, fb, {{0, 0}, {W, H}}, 1, &cv};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    for (int cell = 0; cell < CELLS; cell++) {
        if (cell == drawLimit) return;
        if (cell % 64 == 0) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        if (cell % 64 == 0) setViewportScissor(cb);
        uint8_t c[4]; Push p;
        /* a cell drawn wrong first and then right: the second draw must land after the first */
        if (cell % 11 == 0) {
            uint8_t wrong[4] = {1, 2, 3, 255};
            cellPush(cell, wrong, &p);
            vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
            vkCmdDraw(cb, 6, 1, 0, 0);
        }
        color1(cell, seed, c);
        cellPush(cell, c, &p);
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
        memset(&p, 0x7f, sizeof(p));   /* push constant data must have been copied */
        vkCmdDraw(cb, 6, 1, 0, 0);
        appWork();
    }
    vkCmdEndRenderPass(cb);

    /* render pass 2: the secondary's overdraw */
    VkRenderPassBeginInfo rbi2 = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, rp2, fb, {{0, 0}, {W, H}}, 0, NULL};
    vkCmdBeginRenderPass(cb, &rbi2, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    vkCmdExecuteCommands(cb, 1, &sb);
    vkCmdEndRenderPass(cb);

    VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &bic);

    /* transfer part */
    VkMemoryBarrier tt = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdFillBuffer(cb, tbuf, 0, VK_WHOLE_SIZE, 0x11111111u);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &tt, 0, 0, 0, 0);
    for (int i = 0; i < SLOTS; i++) {
        uint32_t data[4] = {(uint32_t)i, (uint32_t)seed, (uint32_t)i * 3 + 1, ~(uint32_t)i};
        vkCmdUpdateBuffer(cb, tbuf, (VkDeviceSize)i * 16, 16, data);
        memset(data, 0xee, sizeof(data));   /* the update's data must have been copied */
        __asm__ volatile("" ::: "memory");
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &tt, 0, 0, 0, 0);
    for (int i = 0; i < SLOTS; i += 5) vkCmdFillBuffer(cb, tbuf, (VkDeviceSize)i * 16 + 8, 8, 0xdead0000u | (uint32_t)i);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &tt, 0, 0, 0, 0);
    VkBufferCopy bc = {0, (VkDeviceSize)W * H * 4, SLOTS * 16};
    vkCmdCopyBuffer(cb, tbuf, rbuf, 1, &bc);

    VkMemoryBarrier th = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &th, 0, 0, 0, 0);
    CK(vkEndCommandBuffer(cb));
}

static int check(int seed) {
    int badPixels = 0, badSlots = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int cell = (y / CELL) * CELLS_X + x / CELL;
            uint8_t want[4];
            if (cell % 7 == 3) color2(cell, seed, want); else color1(cell, seed, want);
            const uint8_t* px = rmap + (y * W + x) * 4;
            if (memcmp(px, want, 4)) {
                if (badPixels < 5)
                    printf("  pixel (%d,%d) cell %d = %u %u %u %u, want %u %u %u %u\n", x, y, cell,
                           px[0], px[1], px[2], px[3], want[0], want[1], want[2], want[3]);
                badPixels++;
            }
        }
    const uint32_t* slots = (const uint32_t*)(rmap + W * H * 4);
    for (int i = 0; i < SLOTS; i++) {
        uint32_t want[4] = {(uint32_t)i, (uint32_t)seed, (uint32_t)i * 3 + 1, ~(uint32_t)i};
        if (i % 5 == 0) want[2] = want[3] = 0xdead0000u | (uint32_t)i;
        if (memcmp(slots + i * 4, want, 16)) {
            if (badSlots < 5)
                printf("  slot %d = %08x %08x %08x %08x, want %08x %08x %08x %08x\n", i, slots[i * 4], slots[i * 4 + 1],
                       slots[i * 4 + 2], slots[i * 4 + 3], want[0], want[1], want[2], want[3]);
            badSlots++;
        }
    }
    printf("  check seed %d: %d wrong pixels of %d, %d wrong records of %d\n", seed, badPixels, W * H, badSlots, SLOTS);
    return badPixels == 0 && badSlots == 0;
}

int main(void) {
    const char* chunkEnv = getenv("VORTEK_CMD_CHUNK");
    const char* workEnv = getenv("CMD_STREAM_WORK_NS");
    long timedWorkNs = workEnv ? atol(workEnv) : 1000;
    printf("VORTEK_CMD_CHUNK=%s\n", chunkEnv ? chunkEnv : "unset (default 32 KiB)");

    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "cmd_stream", 1, "cmd_stream", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai};
    VkInstance inst; CK(vkCreateInstance(&ii, 0, &inst));
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s\n", props.deviceName);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qi};
    CK(vkCreateDevice(pd, &di, 0, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;

    /* image + view */
    VkImageCreateInfo imi = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, fmt, {W, H, 1}, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    CK(vkCreateImage(dev, &imi, 0, &img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, findType(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory imem; CK(vkAllocateMemory(dev, &ma, 0, &imem)); CK(vkBindImageMemory(dev, img, imem, 0));
    VkImageViewCreateInfo ivi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, img, VK_IMAGE_VIEW_TYPE_2D, fmt, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view; CK(vkCreateImageView(dev, &ivi, 0, &view));

    /* render passes: 1 clears, 2 loads and leaves the image ready for the copy */
    VkAttachmentDescription ad = {0, fmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 1, &ar};
    VkSubpassDependency dep1 = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, 0};
    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, 1, &ad, 1, &sd, 1, &dep1};
    CK(vkCreateRenderPass(dev, &rpi, 0, &rp1));
    VkAttachmentDescription ad2 = ad;
    ad2.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ad2.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ad2.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkSubpassDependency deps2[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0}};
    VkRenderPassCreateInfo rpi2 = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, 1, &ad2, 1, &sd, 2, deps2};
    CK(vkCreateRenderPass(dev, &rpi2, 0, &rp2));
    VkFramebufferCreateInfo fbi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0, 0, rp1, 1, &view, W, H, 1};
    CK(vkCreateFramebuffer(dev, &fbi, 0, &fb));

    /* pipeline: a quad from push constants, no vertex input */
    VkShaderModuleCreateInfo vsi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(cmd_stream_vert_spv), cmd_stream_vert_spv};
    VkShaderModuleCreateInfo fsi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(cmd_stream_frag_spv), cmd_stream_frag_spv};
    VkShaderModule vs, fs; CK(vkCreateShaderModule(dev, &vsi, 0, &vs)); CK(vkCreateShaderModule(dev, &fsi, 0, &fs));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 0, 0, 1, &pcr};
    CK(vkCreatePipelineLayout(dev, &pli, 0, &layout));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"}};
    VkPipelineVertexInputStateCreateInfo vis = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ias = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo mss = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &cba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 2, dyn};
    VkGraphicsPipelineCreateInfo gpi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, 0, 0, 2, stages, &vis, &ias, 0, &vps, &rs, &mss, 0, &cbs, &ds,
        layout, rp1, 0};
    CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, 0, &pipeline));

    /* buffers: the transfer target (device local) and the readback (image, then the records) */
    VkBufferCreateInfo tbi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, SLOTS * 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CK(vkCreateBuffer(dev, &tbi, 0, &tbuf));
    vkGetBufferMemoryRequirements(dev, tbuf, &mr);
    VkMemoryAllocateInfo mt = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, findType(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory tmem; CK(vkAllocateMemory(dev, &mt, 0, &tmem)); CK(vkBindBufferMemory(dev, tbuf, tmem, 0));
    VkBufferCreateInfo rbi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, W * H * 4 + SLOTS * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CK(vkCreateBuffer(dev, &rbi, 0, &rbuf));
    vkGetBufferMemoryRequirements(dev, rbuf, &mr);
    VkMemoryAllocateInfo mrb = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size,
        findType(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory rmem; CK(vkAllocateMemory(dev, &mrb, 0, &rmem)); CK(vkBindBufferMemory(dev, rbuf, rmem, 0));
    CK(vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, (void**)&rmap));

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpi, 0, &cp));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferAllocateInfo sai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cp, VK_COMMAND_BUFFER_LEVEL_SECONDARY, 1};
    VkCommandBuffer sb; CK(vkAllocateCommandBuffers(dev, &sai, &sb));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; CK(vkCreateFence(dev, &fci, 0, &fence));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb};

    /* correctness: 3 iterations, the 2nd after resetting a half-recorded buffer, the 3rd re-begun without a reset */
    int ok = 1;
    VkFence helperFence; CK(vkCreateFence(dev, &fci, 0, &helperFence));
    for (int seed = 0; seed < 4; seed++) {
        memset(rmap, 0xcd, W * H * 4 + SLOTS * 16);
        pthread_t helper;
        if (seed == 3) pthread_create(&helper, NULL, helperThread, &helperFence);
        if (seed == 1) {
            recordSecondary(sb, 99);
            recordPrimary(cb, sb, 77, 2500);   /* abandoned midway: ~300 KiB already streamed */
            CK(vkResetCommandBuffer(cb, 0));
            CK(vkResetCommandBuffer(sb, 0));
        }
        recordSecondary(sb, seed);
        recordPrimary(cb, sb, seed, CELLS);
        CK(vkQueueSubmit(q, 1, &si, fence));
        CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull));
        CK(vkResetFences(dev, 1, &fence));
        if (seed == 3) { atomic_store(&helperStop, 1); pthread_join(helper, NULL); }
        if (seed < 3) printf("iteration %d (%s):\n", seed, seed == 0 ? "first recording" : seed == 1 ? "after resetting a half-recorded buffer" : "implicit reset by vkBeginCommandBuffer");
        else printf("iteration 3 (another thread made %ld vkGetFenceStatus calls meanwhile):\n", (long)atomic_load(&helperCalls));
        ok &= check(seed);
    }
    vkDestroyFence(dev, helperFence, 0);

    /* timing: record + end, then submit + wait */
    for (int pass = 0; pass < 2; pass++) {
        workNs = pass == 0 ? 0 : timedWorkNs;
        if (pass == 1 && workNs <= 0) break;
        enum { RUNS = 8 };
        double rec = 0, wait = 0, total = 0, best = 1e9;
        for (int run = 0; run < RUNS + 1; run++) {
            int seed = 3 + run;
            recordSecondary(sb, seed);   /* not timed: the secondary is recorded before the frame */
            double t0 = now();
            recordPrimary(cb, sb, seed, CELLS);
            double t1 = now();
            CK(vkQueueSubmit(q, 1, &si, fence));
            CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull));
            double t2 = now();
            CK(vkResetFences(dev, 1, &fence));
            if (run == RUNS) { ok &= check(seed); continue; }   /* last run: checked, not timed */
            if (run == 0) continue;                             /* warm-up */
            rec += t1 - t0; wait += t2 - t1; total += t2 - t0;
            if (t2 - t0 < best) best = t2 - t0;
        }
        int timed = RUNS - 1;
        printf("TIMING VORTEK_CMD_CHUNK=%s work=%ldns/draw: record+end %.2f ms, submit+wait %.2f ms, total %.2f ms (best %.2f ms), avg of %d\n",
               chunkEnv ? chunkEnv : "default", workNs, rec / timed * 1e3, wait / timed * 1e3, total / timed * 1e3, best * 1e3, timed);
    }

    /* GPU (+ submit) time alone: resubmit the last recording, nothing to replay */
    {
        double total = 0;
        for (int run = 0; run < 5; run++) {
            double t0 = now();
            CK(vkQueueSubmit(q, 1, &si, fence));
            CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull));
            total += now() - t0;
            CK(vkResetFences(dev, 1, &fence));
        }
        printf("TIMING resubmit of the recorded command buffer (no replay): submit+wait %.2f ms, avg of 5\n", total / 5 * 1e3);
    }

    vkDestroyFence(dev, fence, 0);
    vkDestroyCommandPool(dev, cp, 0);
    vkDestroyPipeline(dev, pipeline, 0);
    vkDestroyPipelineLayout(dev, layout, 0);
    vkDestroyShaderModule(dev, vs, 0);
    vkDestroyShaderModule(dev, fs, 0);
    vkDestroyFramebuffer(dev, fb, 0);
    vkDestroyRenderPass(dev, rp1, 0);
    vkDestroyRenderPass(dev, rp2, 0);
    vkDestroyImageView(dev, view, 0);
    vkDestroyImage(dev, img, 0);
    vkFreeMemory(dev, imem, 0);
    vkDestroyBuffer(dev, tbuf, 0);
    vkFreeMemory(dev, tmem, 0);
    vkUnmapMemory(dev, rmem);
    vkDestroyBuffer(dev, rbuf, 0);
    vkFreeMemory(dev, rmem, 0);
    vkDestroyDevice(dev, 0);
    vkDestroyInstance(inst, 0);
    printf(ok ? "CMDSTREAM-PASS\n" : "CMDSTREAM-FAIL\n");
    return ok ? 0 : 1;
}
