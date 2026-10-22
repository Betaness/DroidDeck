/*
 * Zero-copy presentation from the standalone Vortek server to DroidDeck's compositor.
 *
 * Swapchains backed by AHardwareBuffers (headless_swapchain.c) are registered with the compositor
 * over the unix socket $VORTEK_PRESENT_SOCKET; a present hands over the image index, the X window's
 * rectangle and an acquire fence, and the compositor shows the buffer on a SurfaceControl layer and
 * sends it back with a release fence when it is replaced. The wire format is shared with
 * app/src/main/cpp/waylandcomp/src/vortek_present.h.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef VORTEK_ZERO_COPY_H
#define VORTEK_ZERO_COPY_H

#include <stdbool.h>
#include <stdint.h>
#include <android/hardware_buffer.h>

enum {
    ZC_MSG_CHAIN_CREATE = 1,
    ZC_MSG_PRESENT = 2,
    ZC_MSG_HIDE = 3,
    ZC_MSG_CHAIN_DESTROY = 4,
    ZC_MSG_RELEASE = 5,
};

typedef struct ZeroCopyMsg {
    uint32_t type;
    uint32_t chain;
    uint32_t index;
    uint32_t imageCount;
    uint32_t width, height;
    int32_t x, y, w, h;
} ZeroCopyMsg;

/* index ZC_RELEASE_ALL: the compositor went away and holds nothing any more. */
#define ZC_RELEASE_ALL 0xffffffffu
typedef void (*ZeroCopyReleaseFunc)(uint32_t chain, uint32_t index, int releaseFd);

/* Connects on first use; false when there is no compositor to present to. */
bool ZeroCopy_available(void);
void ZeroCopy_setReleaseHandler(ZeroCopyReleaseFunc func);
bool ZeroCopy_createChain(uint32_t chain, uint32_t width, uint32_t height, AHardwareBuffer** buffers, uint32_t count);
bool ZeroCopy_present(uint32_t chain, uint32_t index, int32_t x, int32_t y, int32_t w, int32_t h, int acquireFd);
void ZeroCopy_hide(uint32_t chain);
void ZeroCopy_destroyChain(uint32_t chain);

#endif
