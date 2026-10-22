/*
 * See zero_copy.h.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include "zero_copy.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int connFd = -1;
static bool triedOnce;
static ZeroCopyReleaseFunc releaseHandler;
static volatile int accepting = 1;

static int sendMsg(int fd, const ZeroCopyMsg* msg, int passFd) {
    struct iovec iov = {(void*)msg, sizeof(*msg)};
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
    ssize_t r;
    do r = sendmsg(fd, &h, MSG_NOSIGNAL); while (r < 0 && errno == EINTR);
    return r == (ssize_t)sizeof(*msg) ? 0 : -1;
}

static void disconnectLocked(void) {
    if (connFd >= 0) shutdown(connFd, SHUT_RDWR);
    connFd = -1;
}

static void* readerThread(void* arg) {
    int fd = (int)(intptr_t)arg;
    for (;;) {
        ZeroCopyMsg msg;
        struct iovec iov = {&msg, sizeof(msg)};
        char cbuf[CMSG_SPACE(sizeof(int))];
        struct msghdr h = {0};
        h.msg_iov = &iov;
        h.msg_iovlen = 1;
        h.msg_control = cbuf;
        h.msg_controllen = sizeof(cbuf);
        ssize_t r;
        do r = recvmsg(fd, &h, MSG_CMSG_CLOEXEC); while (r < 0 && errno == EINTR);
        if (r <= 0) break;
        int fence = -1;
        for (struct cmsghdr* c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&fence, CMSG_DATA(c), sizeof(int));
        if (r == (ssize_t)sizeof(msg) && msg.type == ZC_MSG_RELEASE && releaseHandler) {
            releaseHandler(msg.chain, msg.index, fence);
            fence = -1;
        }
        else if (r == (ssize_t)sizeof(msg) && msg.type == ZC_MSG_REFUSED && releaseHandler) {
            releaseHandler(msg.chain, ZC_REFUSED, -1);
        }
        else if (r == (ssize_t)sizeof(msg) && msg.type == ZC_MSG_MODE) {
            accepting = msg.index != 0;
            fprintf(stderr, "vortek-server: compositor %s zero-copy presents\n", accepting ? "accepts" : "pauses");
        }
        if (fence >= 0) close(fence);
    }
    pthread_mutex_lock(&lock);
    if (connFd == fd) connFd = -1;
    accepting = 1;
    pthread_mutex_unlock(&lock);
    close(fd);
    if (releaseHandler) releaseHandler(0, ZC_RELEASE_ALL, -1);
    fprintf(stderr, "vortek-server: zero-copy presenter connection closed\n");
    return NULL;
}

static bool connectLocked(void) {
    if (connFd >= 0) return true;
    const char* path = getenv("VORTEK_PRESENT_SOCKET");
    const char* enabled = getenv("VORTEK_ZERO_COPY");
    if (!path || !*path || (enabled && *enabled == '0')) return false;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(addr.sun_path)) return false;
    strcpy(addr.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        if (!triedOnce) fprintf(stderr, "vortek-server: no zero-copy presenter at %s (%s)\n", path, strerror(errno));
        triedOnce = true;
        close(fd);
        return false;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, readerThread, (void*)(intptr_t)fd) != 0) {
        close(fd);
        return false;
    }
    pthread_detach(thread);
    connFd = fd;
    fprintf(stderr, "vortek-server: connected to the zero-copy presenter at %s\n", path);
    return true;
}

bool ZeroCopy_available(void) {
    pthread_mutex_lock(&lock);
    bool ok = connectLocked();
    pthread_mutex_unlock(&lock);
    return ok;
}

void ZeroCopy_setReleaseHandler(ZeroCopyReleaseFunc func) {
    releaseHandler = func;
}

bool ZeroCopy_createChain(uint32_t chain, uint32_t width, uint32_t height, AHardwareBuffer** buffers, uint32_t count) {
    ZeroCopyMsg msg = {.type = ZC_MSG_CHAIN_CREATE, .chain = chain, .imageCount = count, .width = width, .height = height};
    pthread_mutex_lock(&lock);
    bool ok = connectLocked() && sendMsg(connFd, &msg, -1) == 0;
    for (uint32_t i = 0; ok && i < count; i++) ok = AHardwareBuffer_sendHandleToUnixSocket(buffers[i], connFd) == 0;
    if (!ok) disconnectLocked();
    pthread_mutex_unlock(&lock);
    return ok;
}

bool ZeroCopy_accepting(void) {
    return accepting != 0;
}

bool ZeroCopy_present(uint32_t chain, uint32_t index, int32_t x, int32_t y, int32_t w, int32_t h,
                      uint32_t rootW, uint32_t rootH, int acquireFd) {
    ZeroCopyMsg msg = {.type = ZC_MSG_PRESENT, .chain = chain, .index = index, .x = x, .y = y, .w = w, .h = h,
                       .rootW = rootW, .rootH = rootH};
    pthread_mutex_lock(&lock);
    bool ok = connFd >= 0 && sendMsg(connFd, &msg, acquireFd) == 0;
    if (!ok) disconnectLocked();
    pthread_mutex_unlock(&lock);
    if (acquireFd >= 0) close(acquireFd);
    return ok;
}

static void sendSimple(uint32_t type, uint32_t chain) {
    ZeroCopyMsg msg = {.type = type, .chain = chain};
    pthread_mutex_lock(&lock);
    if (connFd >= 0 && sendMsg(connFd, &msg, -1) != 0) disconnectLocked();
    pthread_mutex_unlock(&lock);
}

void ZeroCopy_hide(uint32_t chain) {
    sendSimple(ZC_MSG_HIDE, chain);
}

void ZeroCopy_destroyChain(uint32_t chain) {
    sendSimple(ZC_MSG_CHAIN_DESTROY, chain);
}
