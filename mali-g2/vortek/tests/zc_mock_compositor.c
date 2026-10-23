/* [mali-g2] Mock of DroidDeck's compositor (app/src/main/cpp/waylandcomp/src/vortek_present.c) for
 * tests/zc_acquire_test.c, run as a bionic program next to the Vortek server. It receives the
 * swapchains as AHardwareBuffers, waits for each present's acquire fence and reads the centre pixel,
 * which zc_acquire_test.c clears to the frame number (R = low byte, G = high byte, B = 0xa5).
 * It keeps one image on screen and releases the previous one when the next present arrives:
 *   MOCK_RELEASE=early (default): released as soon as the present message is read, with a dup of
 *     that present's acquire fence as release fence (like a compositor whose release fence is the
 *     next buffer's latch), so the server receives fences that are still pending;
 *   MOCK_RELEASE=late: released after the acquire fence signalled, without a release fence (-1);
 *   MOCK_RELEASE=gpu: released like early, but the release fence is a sync fd exported from a job the
 *     mock submits on its own Vulkan device (MOCK_GPU_MS long, default 30), so it signals well after
 *     the client could start rendering. When the image is presented again, its acquire fence must not
 *     signal before that release fence: otherwise the server let the GPU write the image without
 *     waiting for the release fence ("release order" violations, MOCK-RESULT fail). The early mode
 *     cannot catch this: its release fence is the next present's acquire fence, which the server's own
 *     queue already orders before later work.
 * Checks per chain: every pixel carries the marker, frame numbers advance by one, and an image is
 * not overwritten while the compositor holds it (re-read just before its release).
 * Prints one "MOCK chain ..." line per chain and "MOCK-RESULT ok|fail" when the server disconnects.
 * Build: $NDK/toolchains/llvm/prebuilt/<host>/bin/clang --target=aarch64-linux-android30 -O2
 *        -I<vortekrenderer>/include -o zc_mock_compositor zc_mock_compositor.c -lnativewindow -lvulkan
 *        (any Vulkan headers will do; the NDK's sysroot has them, libvulkan.so is the system loader) */
#include <android/hardware_buffer.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

typedef struct { uint32_t type, chain, index, imageCount, width, height; int32_t x, y, w, h; uint32_t rootW, rootH, keyHi, keyLo, topLevel; } Msg;

#define CHAINS 64
#define IMAGES 8

typedef struct {
    AHardwareBuffer* bufs[IMAGES];
    uint32_t imageCount;
    int shownFrame[IMAGES];
    unsigned presents, badPixel, outOfOrder, overwritten;
    int lastFrame;
    double fenceWait, maxFenceWait;
    unsigned pendingAtRelease;
    int releaseFd[IMAGES]; /* MOCK_RELEASE=gpu: copy of the release fence last sent for each image, -1 none */
    unsigned releaseChecked, releaseViolations;
} Chain;

static Chain chains[CHAINS];

static int recvMsg(int fd, Msg* m, int* gotFd) {
    struct iovec iov = {m, sizeof(*m)};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr h = {0};
    h.msg_iov = &iov;
    h.msg_iovlen = 1;
    h.msg_control = cbuf;
    h.msg_controllen = sizeof(cbuf);
    *gotFd = -1;
    ssize_t r = recvmsg(fd, &h, 0);
    if (r != sizeof(*m)) return -1;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c))
        if (c->cmsg_type == SCM_RIGHTS) memcpy(gotFd, CMSG_DATA(c), sizeof(int));
    return 0;
}

static void sendRelease(int sock, uint32_t chain, uint32_t index, int fence) {
    Msg m = {5, chain, index};
    struct iovec iov = {&m, sizeof(m)};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr h = {0};
    h.msg_iov = &iov;
    h.msg_iovlen = 1;
    if (fence >= 0) {
        memset(cbuf, 0, sizeof(cbuf));
        h.msg_control = cbuf;
        h.msg_controllen = sizeof(cbuf);
        struct cmsghdr* c = CMSG_FIRSTHDR(&h);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fence, sizeof(int));
    }
    sendmsg(sock, &h, MSG_NOSIGNAL);
    if (fence >= 0) close(fence);
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* MOCK_RELEASE=gpu: release fences from fill jobs on the mock's own device */
#define JOBS 16
#define FILL_SIZE (16u << 20)
static struct {
    VkDevice dev;
    VkQueue queue;
    VkCommandBuffer cmd[JOBS];
    VkFence fence[JOBS];
    unsigned next;
    PFN_vkGetFenceFdKHR getFenceFd;
} gpu;

static VkCommandBuffer recordFills(VkCommandBuffer cmd, VkBuffer buf, int fills) {
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &bi);
    for (int i = 0; i < fills; i++) {
        vkCmdFillBuffer(cmd, buf, 0, VK_WHOLE_SIZE, (uint32_t)i);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    vkEndCommandBuffer(cmd);
    return cmd;
}

static double runJob(int k) {
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &gpu.cmd[k]};
    vkResetFences(gpu.dev, 1, &gpu.fence[k]);
    double t = now();
    vkQueueSubmit(gpu.queue, 1, &si, gpu.fence[k]);
    vkWaitForFences(gpu.dev, 1, &gpu.fence[k], VK_TRUE, UINT64_MAX);
    return now() - t;
}

static int gpuInit(void) {
    const char* ms = getenv("MOCK_GPU_MS");
    int targetMs = ms && *ms ? atoi(ms) : 30;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "zc_mock_compositor", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst;
    VkPhysicalDevice pd;
    uint32_t n = 1;
    if (vkCreateInstance(&ii, NULL, &inst) != VK_SUCCESS || vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) return -1;
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    const char* ext[] = {"VK_KHR_external_fence", "VK_KHR_external_fence_fd"};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qi, 0, NULL, 2, ext};
    if (vkCreateDevice(pd, &di, NULL, &gpu.dev) != VK_SUCCESS) return -1;
    vkGetDeviceQueue(gpu.dev, 0, 0, &gpu.queue);
    gpu.getFenceFd = (PFN_vkGetFenceFdKHR)vkGetDeviceProcAddr(gpu.dev, "vkGetFenceFdKHR");
    if (!gpu.getFenceFd) return -1;

    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, FILL_SIZE, VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buf;
    if (vkCreateBuffer(gpu.dev, &bci, NULL, &buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(gpu.dev, buf, &mr);
    uint32_t type = 0;
    while (type < 32 && !(mr.memoryTypeBits >> type & 1)) type++;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, type};
    VkDeviceMemory mem;
    if (vkAllocateMemory(gpu.dev, &ai, NULL, &mem) != VK_SUCCESS || vkBindBufferMemory(gpu.dev, buf, mem, 0) != VK_SUCCESS) return -1;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    VkCommandPool pool;
    if (vkCreateCommandPool(gpu.dev, &pci, NULL, &pool) != VK_SUCCESS) return -1;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, JOBS};
    if (vkAllocateCommandBuffers(gpu.dev, &cai, gpu.cmd) != VK_SUCCESS) return -1;
    VkExportFenceCreateInfo efi = {VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO, NULL, VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, &efi, VK_FENCE_CREATE_SIGNALED_BIT};
    for (int k = 0; k < JOBS; k++)
        if (vkCreateFence(gpu.dev, &fci, NULL, &gpu.fence[k]) != VK_SUCCESS) return -1;

    /* calibrate: fills per job for MOCK_GPU_MS */
    recordFills(gpu.cmd[0], buf, 16);
    runJob(0);
    double perFill = runJob(0) / 16;
    int fills = perFill > 0 ? (int)(targetMs / 1000.0 / perFill) + 1 : 16;
    if (fills > 4096) fills = 4096;
    for (int k = 0; k < JOBS; k++) recordFills(gpu.cmd[k], buf, fills);
    printf("gpu release jobs: %d fills of 16 MiB, %.1f ms (target %d ms)\n", fills, runJob(0) * 1000, targetMs);
    fflush(stdout);
    return 0;
}

/* starts a release job and returns its sync fd, -1 on failure */
static int gpuReleaseFence(void) {
    unsigned k = gpu.next++ % JOBS;
    vkWaitForFences(gpu.dev, 1, &gpu.fence[k], VK_TRUE, UINT64_MAX);
    vkResetFences(gpu.dev, 1, &gpu.fence[k]);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &gpu.cmd[k]};
    if (vkQueueSubmit(gpu.queue, 1, &si, gpu.fence[k]) != VK_SUCCESS) return -1;
    VkFenceGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, NULL, gpu.fence[k], VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    int fd = -1;
    return gpu.getFenceFd(gpu.dev, &gi, &fd) == VK_SUCCESS ? fd : -1;
}

static void resetChain(Chain* c) {
    for (int i = 0; i < IMAGES; i++)
        if (c->releaseFd[i] >= 0) close(c->releaseFd[i]);
    memset(c, 0, sizeof(*c));
    for (int i = 0; i < IMAGES; i++) c->releaseFd[i] = -1;
}

/* frame number in the centre pixel, -1 without the marker */
static int readFrame(AHardwareBuffer* b) {
    AHardwareBuffer_Desc d;
    AHardwareBuffer_describe(b, &d);
    void* px = NULL;
    if (AHardwareBuffer_lock(b, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &px) != 0 || !px) return -2;
    uint32_t v = ((uint32_t*)px)[(d.height / 2) * d.stride + d.width / 2];
    AHardwareBuffer_unlock(b, NULL);
    uint32_t r, g, bl;
    if (d.format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM) r = v & 0xff, g = v >> 8 & 0xff, bl = v >> 16 & 0xff;
    else r = v >> 16 & 0xff, g = v >> 8 & 0xff, bl = v & 0xff;
    return bl == 0xa5 ? (int)(r | g << 8) : -1;
}

static int pollFence(int fd, int ms) {
    struct pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, ms);
}

static void report(uint32_t id, Chain* c, unsigned* failures) {
    if (!c->presents) return;
    bool ok = !c->badPixel && !c->outOfOrder && !c->overwritten && !c->releaseViolations;
    if (!ok) (*failures)++;
    printf("MOCK chain %u: %u presents, last frame %d, bad pixels %u, out of order %u, overwritten while shown %u, "
           "releases with a pending fence %u, acquire fence wait avg %.2f ms max %.2f ms\n",
           id, c->presents, c->lastFrame, c->badPixel, c->outOfOrder, c->overwritten, c->pendingAtRelease,
           c->fenceWait * 1000 / c->presents, c->maxFenceWait * 1000);
    if (c->releaseChecked)
        printf("MOCK chain %u release order: %u of %u presents signalled their acquire fence before the image's release fence\n",
               id, c->releaseViolations, c->releaseChecked);
    fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <socket>\n", argv[0]);
        return 2;
    }
    const char* mode = getenv("MOCK_RELEASE");
    bool early = !mode || strcmp(mode, "late") != 0, gpuRelease = mode && strcmp(mode, "gpu") == 0;
    if (gpuRelease && gpuInit() != 0) {
        printf("mock compositor: no Vulkan device for MOCK_RELEASE=gpu\nMOCK-RESULT fail\n");
        return 1;
    }
    for (int i = 0; i < CHAINS; i++) resetChain(&chains[i]);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strncpy(a.sun_path, argv[1], sizeof(a.sun_path) - 1);
    unlink(argv[1]);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(ls, (void*)&a, sizeof(a)) != 0 || listen(ls, 2) != 0) {
        perror("mock compositor");
        return 1;
    }
    printf("mock compositor on %s, release %s\n", argv[1],
           gpuRelease ? "gpu (fence of a job on the mock's device)" : early ? "early (pending fence)" : "late (no fence)");
    fflush(stdout);
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) continue;
        printf("server connected\n");
        fflush(stdout);
        for (int i = 0; i < CHAINS; i++) resetChain(&chains[i]);
        int prevChain = -1, prevIndex = -1;
        unsigned failures = 0;
        for (;;) {
            Msg m;
            int fd;
            if (recvMsg(c, &m, &fd) != 0) break;
            Chain* ch = &chains[m.chain % CHAINS];
            if (m.type == 1) {
                resetChain(ch);
                ch->imageCount = m.imageCount;
                ch->lastFrame = -1;
                for (uint32_t i = 0; i < m.imageCount; i++) {
                    AHardwareBuffer_recvHandleFromUnixSocket(c, &ch->bufs[i % IMAGES]);
                    ch->shownFrame[i % IMAGES] = -1;
                }
                AHardwareBuffer_Desc d;
                AHardwareBuffer_describe(ch->bufs[0], &d);
                printf("chain %u: %u images %ux%u format %u\n", m.chain, m.imageCount, d.width, d.height, d.format);
                fflush(stdout);
            }
            else if (m.type == 2) {
                uint32_t index = m.index % IMAGES;
                if (prevChain >= 0) {
                    Chain* pc = &chains[prevChain % CHAINS];
                    int still = readFrame(pc->bufs[prevIndex % IMAGES]);
                    if (still != pc->shownFrame[prevIndex % IMAGES]) {
                        if (pc->overwritten++ < 5)
                            printf("chain %d image %d: frame %d overwritten (now %d) while shown\n", prevChain, prevIndex,
                                   pc->shownFrame[prevIndex % IMAGES], still);
                    }
                    if (early) {
                        int release = gpuRelease ? gpuReleaseFence() : fd >= 0 ? dup(fd) : -1;
                        if (release >= 0 && pollFence(release, 0) == 0) pc->pendingAtRelease++;
                        if (gpuRelease) {
                            int* kept = &pc->releaseFd[prevIndex % IMAGES];
                            if (*kept >= 0) close(*kept);
                            *kept = release >= 0 ? dup(release) : -1;
                        }
                        sendRelease(c, prevChain, prevIndex, release);
                        prevChain = -1;
                    }
                }
                double tw = now();
                if (fd >= 0) {
                    pollFence(fd, 2000);
                    close(fd);
                }
                double waited = now() - tw;
                int* kept = &ch->releaseFd[index];
                if (*kept >= 0) {
                    /* the acquire fence signalled: the image's release fence must have signalled first */
                    ch->releaseChecked++;
                    if (pollFence(*kept, 0) == 0 && ch->releaseViolations++ < 5)
                        printf("chain %u image %u: acquire fence signalled before the image's release fence\n", m.chain, m.index);
                    close(*kept);
                    *kept = -1;
                }
                ch->fenceWait += waited;
                if (waited > ch->maxFenceWait) ch->maxFenceWait = waited;
                int frame = readFrame(ch->bufs[index]);
                ch->shownFrame[index] = frame;
                if (frame < 0) {
                    if (ch->badPixel++ < 5) printf("chain %u present #%u image %u: no frame marker (%d)\n", m.chain, ch->presents, m.index, frame);
                }
                else {
                    if (ch->lastFrame >= 0 && frame != ch->lastFrame + 1) {
                        if (ch->outOfOrder++ < 5) printf("chain %u: frame %d after %d\n", m.chain, frame, ch->lastFrame);
                    }
                    ch->lastFrame = frame;
                }
                ch->presents++;
                if (!early && prevChain >= 0) sendRelease(c, prevChain, prevIndex, -1);
                prevChain = m.chain;
                prevIndex = m.index;
            }
            else if (m.type == 3 || m.type == 4) {
                if (fd >= 0) close(fd);
                if (prevChain == (int)m.chain) {
                    sendRelease(c, prevChain, prevIndex, -1);
                    prevChain = -1;
                }
                if (m.type == 4) {
                    report(m.chain, ch, &failures);
                    ch->presents = 0;
                }
            }
            else if (fd >= 0) close(fd);
        }
        for (int i = 0; i < CHAINS; i++) report(i, &chains[i], &failures);
        printf("MOCK-RESULT %s\n", failures ? "fail" : "ok");
        printf("server disconnected\n");
        fflush(stdout);
        close(c);
    }
}
