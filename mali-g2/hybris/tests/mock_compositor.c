/*
 * Stand-in for DroidDeck's compositor (app/src/main/cpp/waylandcomp/src/vortek_present.c) as a glibc
 * program: the AHardwareBuffers come through libhybris (src/ahb.c), like in the ICD. Listens on
 * argv[1], takes clients one after another and exits after argv[2] (default 1) of them:
 *   CHAIN_CREATE: receives the buffers;
 *   PRESENT: waits for the acquire fence, reads the centre pixel (when the buffer can be locked for
 *     CPU reads: DROIDDECK_HYBRIS_AHB_CPU_READ=1 in the client) and checks swapchain_test.c's marker
 *     (B = 0xa5) and that frame numbers (R + 256 G) advance; keeps the image "on screen" and releases
 *     the one shown before. MOCK_RELEASE=early (default) hands a dup of the new present's acquire
 *     fence back as release fence, MOCK_RELEASE=late releases without a fence after it signalled.
 * Prints "MOCK chain ..." per chain and "MOCK-RESULT ok|fail" when a client disconnects.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include <android/hardware_buffer.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct { uint32_t type, chain, index, imageCount, width, height; int32_t x, y, w, h; uint32_t rootW, rootH, keyHi, keyLo, topLevel; } Msg;

#define CHAINS 64
#define IMAGES 8

typedef struct {
    uint32_t id;
    AHardwareBuffer* bufs[IMAGES];
    uint32_t imageCount, width, height;
    int shown;
    unsigned presents, checked, badPixel, outOfOrder, fenceTimeouts, noFence;
    int lastFrame;
    int32_t rect[4];
} Chain;

static Chain chains[CHAINS];

static Chain* chainOf(uint32_t id, bool create) {
    for (int i = 0; i < CHAINS; i++) if (chains[i].id == id) return &chains[i];
    if (!create) return NULL;
    for (int i = 0; i < CHAINS; i++) {
        if (!chains[i].id) {
            memset(&chains[i], 0, sizeof(chains[i]));
            chains[i].id = id;
            chains[i].shown = -1;
            chains[i].lastFrame = -1;
            return &chains[i];
        }
    }
    return NULL;
}

static int recvMsg(int fd, Msg* m, int* gotFd) {
    struct iovec iov = {m, sizeof(*m)};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr h = {0};
    h.msg_iov = &iov;
    h.msg_iovlen = 1;
    h.msg_control = cbuf;
    h.msg_controllen = sizeof(cbuf);
    *gotFd = -1;
    ssize_t r = recvmsg(fd, &h, MSG_CMSG_CLOEXEC);
    if (r != (ssize_t)sizeof(*m)) return -1;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(gotFd, CMSG_DATA(c), sizeof(int));
    return 0;
}

static void sendMsg(int fd, const Msg* m, int passFd) {
    struct iovec iov = {(void*)m, sizeof(*m)};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr h = {0};
    h.msg_iov = &iov;
    h.msg_iovlen = 1;
    if (passFd >= 0) {
        memset(cbuf, 0, sizeof(cbuf));
        h.msg_control = cbuf;
        h.msg_controllen = sizeof(cbuf);
        struct cmsghdr* c = CMSG_FIRSTHDR(&h);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &passFd, sizeof(int));
    }
    sendmsg(fd, &h, MSG_NOSIGNAL);
}

/* The centre pixel as B | G << 8 | R << 16 (the buffers are BGRA or RGBA); -1: not readable. */
static long centrePixel(AHardwareBuffer* b, bool* rgba) {
    AHardwareBuffer_Desc d;
    AHardwareBuffer_describe(b, &d);
    void* px = NULL;
    if (AHardwareBuffer_lock(b, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &px) != 0 || !px) return -1;
    const uint8_t* p = (const uint8_t*)px + ((size_t)(d.height / 2) * d.stride + d.width / 2) * 4;
    *rgba = d.format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    long v = *rgba ? (p[2] | p[1] << 8 | p[0] << 16) : (p[0] | p[1] << 8 | p[2] << 16);
    AHardwareBuffer_unlock(b, NULL);
    return v;
}

static bool serve(int c, bool early) {
    bool ok = true;
    for (;;) {
        Msg m;
        int fd;
        if (recvMsg(c, &m, &fd) != 0) break;
        if (m.type == 1) {
            Chain* ch = chainOf(m.chain, true);
            ch->imageCount = m.imageCount;
            ch->width = m.width;
            ch->height = m.height;
            for (uint32_t i = 0; i < m.imageCount; i++) AHardwareBuffer_recvHandleFromUnixSocket(c, &ch->bufs[i % IMAGES]);
            printf("MOCK create chain %u: %ux%u, %u buffers\n", m.chain, m.width, m.height, m.imageCount);
        }
        else if (m.type == 2) {
            Chain* ch = chainOf(m.chain, false);
            if (!ch || m.index >= ch->imageCount) {
                printf("MOCK present to unknown chain %u index %u\n", m.chain, m.index);
                ok = false;
                if (fd >= 0) close(fd);
                continue;
            }
            ch->presents++;
            memcpy(ch->rect, &m.x, sizeof(ch->rect));
            int releaseFd = -1;
            if (fd >= 0) {
                if (early) releaseFd = dup(fd);
                struct pollfd p = {fd, POLLIN, 0};
                if (poll(&p, 1, 2000) != 1) ch->fenceTimeouts++;
                close(fd);
            }
            else ch->noFence++;
            bool rgba;
            long px = centrePixel(ch->bufs[m.index], &rgba);
            if (px >= 0) {
                ch->checked++;
                int frame = (int)((px >> 16) & 0xff) | (int)(px & 0xff00);
                if ((px & 0xff) != 0xa5) ch->badPixel++;
                else if (ch->lastFrame >= 0 && frame != ch->lastFrame + 1) ch->outOfOrder++;
                ch->lastFrame = frame;
            }
            if (ch->shown >= 0 && ch->shown != (int)m.index) {
                Msg r = {.type = 5, .chain = m.chain, .index = (uint32_t)ch->shown};
                sendMsg(c, &r, releaseFd);
            }
            if (releaseFd >= 0) close(releaseFd);
            ch->shown = (int)m.index;
        }
        else if (m.type == 4) {
            Chain* ch = chainOf(m.chain, false);
            if (ch) {
                printf("MOCK chain %u: %u presents, %u pixels checked, %u bad, %u out of order, %u acquire fence timeouts, %u without fence, "
                       "last window rect %d,%d %dx%d\n", ch->id, ch->presents, ch->checked, ch->badPixel, ch->outOfOrder, ch->fenceTimeouts,
                       ch->noFence, ch->rect[0], ch->rect[1], ch->rect[2], ch->rect[3]);
                if (ch->badPixel || ch->outOfOrder || ch->fenceTimeouts || !ch->presents) ok = false;
                for (uint32_t i = 0; i < ch->imageCount && i < IMAGES; i++) if (ch->bufs[i]) AHardwareBuffer_release(ch->bufs[i]);
                ch->id = 0;
            }
        }
        else if (fd >= 0) close(fd);
    }
    for (int i = 0; i < CHAINS; i++) {
        if (!chains[i].id) continue;
        printf("MOCK chain %u never destroyed (%u presents)\n", chains[i].id, chains[i].presents);
        chains[i].id = 0;
    }
    return ok;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    int clients = argc > 2 ? atoi(argv[2]) : 1;
    const char* release = getenv("MOCK_RELEASE");
    bool early = !release || strcmp(release, "late") != 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!DroidDeckAhb_load()) {
        printf("MOCK-RESULT fail (no libnativewindow)\n");
        return 1;
    }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", argv[1]);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    unlink(argv[1]);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(s, 4) != 0) {
        perror("bind");
        return 1;
    }
    bool ok = true;
    for (int i = 0; i < clients; i++) {
        int c = accept(s, NULL, NULL);
        if (c < 0) break;
        ok = serve(c, early) && ok;
        close(c);
        printf("MOCK client %d gone\n", i + 1);
    }
    printf("MOCK-RESULT %s\n", ok ? "ok" : "fail");
    unlink(argv[1]);
    return ok ? 0 : 1;
}
