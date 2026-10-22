/*
 * Requests the DroidDeck build of Vortek adds to the protocol. They sit below
 * REQUEST_CODE_VK_CALL_START (100), so upstream's tables are untouched.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef VORTEK_DROIDDECK_REQUESTS_H
#define VORTEK_DROIDDECK_REQUESTS_H

#include <stdint.h>

/* Payload: DDPresentZeroCopy, then waitCount uint64 semaphore ids. Reply: a VkResult, and
 * VK_ERROR_FEATURE_NOT_PRESENT means the semaphores were not touched (present through X11). */
#define REQUEST_CODE_DD_PRESENT_ZERO_COPY 90

typedef struct DDPresentZeroCopy {
    uint64_t swapchainId;
    uint32_t imageIndex;
    int32_t x, y, w, h;
    uint32_t waitCount;
} DDPresentZeroCopy;

#endif
