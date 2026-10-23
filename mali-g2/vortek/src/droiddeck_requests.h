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

/* Payload: DDPresentZeroCopy, then waitCount uint64 semaphore ids. Reply: a VkResult. With
 * VK_ERROR_FEATURE_NOT_PRESENT (never for this swapchain) or VK_NOT_READY (not this frame) the
 * semaphores were not touched and the frame must be presented through X11. */
#define REQUEST_CODE_DD_PRESENT_ZERO_COPY 90

typedef struct DDPresentZeroCopy {
    uint64_t swapchainId;
    uint32_t imageIndex;
    int32_t x, y, w, h;
    uint32_t rootW, rootH;
    uint32_t waitCount;
} DDPresentZeroCopy;

/* Flags in the high bits of DDPresentZeroCopy.waitCount. STACK_KEY: a uint64 stacking key follows
 * the struct, before the semaphore ids; the compositor puts windows with a higher key on a higher
 * layer. HIDE: no image, take this swapchain's layer off the screen (its window is gone or hidden). */
#define DD_ZC_FLAG_STACK_KEY 0x80000000u
#define DD_ZC_FLAG_HIDE 0x40000000u
/* RESTACK (with STACK_KEY): no image, the swapchain's window moved in the X stacking order. */
#define DD_ZC_FLAG_RESTACK 0x20000000u
/* TOPLEVEL (with STACK_KEY): a uint64 top-level window id (the root's child that holds the window)
 * ends the payload, after the semaphore ids, so a server without the flag reads the rest as before.
 * The compositor breaks ties between different top-level windows with the same root-level key by the
 * most recent report: the other one's key is stale (an idle window, raised over or lowered since). */
#define DD_ZC_FLAG_TOPLEVEL 0x10000000u
#define DD_ZC_WAIT_COUNT_MASK 0x0000ffffu

/* Payload: the layout of REQUEST_CODE_VK_END_COMMAND_BUFFER (uint64 command buffer id, then the
 * recorded commands, each an 8-byte header {requestCode, size} and its serialized arguments). The
 * server replays the commands into the command buffer without ending it. No reply. The client sends
 * one whenever a command buffer's batch reaches VORTEK_CMD_CHUNK bytes (default 32 KiB, 0 = never),
 * so the server records into the driver while the application is still recording. */
#define REQUEST_CODE_DD_CMD_CHUNK 91

#endif
