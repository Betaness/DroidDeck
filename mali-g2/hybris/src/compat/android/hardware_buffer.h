/*
 * The part of the NDK's AHardwareBuffer API this glibc library uses (stable NDK ABI). The calls go to
 * the device's libnativewindow.so, loaded through libhybris (ahb.c).
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef DROIDDECK_HYBRIS_HARDWARE_BUFFER_H
#define DROIDDECK_HYBRIS_HARDWARE_BUFFER_H

#include <stdint.h>

typedef struct AHardwareBuffer AHardwareBuffer;

typedef struct AHardwareBuffer_Desc {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint32_t rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;

enum {
    AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM = 1,
    AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM = 2,
    AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT = 0x16,
    AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM = 0x2b,
};

enum {
    AHARDWAREBUFFER_USAGE_CPU_READ_RARELY = 2ull,
    AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN = 3ull,
    AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY = 2ull << 4,
    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE = 1ull << 8,
    AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER = 1ull << 9,
    AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY = 1ull << 11,
};

#define AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER

struct ARect;

/* False when libnativewindow could not be loaded; the functions below then fail. */
int DroidDeckAhb_load(void);

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** outBuffer);
void AHardwareBuffer_acquire(AHardwareBuffer* buffer);
void AHardwareBuffer_release(AHardwareBuffer* buffer);
void AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc);
int AHardwareBuffer_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence, const struct ARect* rect, void** outVirtualAddress);
int AHardwareBuffer_unlock(AHardwareBuffer* buffer, int32_t* fence);
int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer* buffer, int socketFd);
int AHardwareBuffer_recvHandleFromUnixSocket(int socketFd, AHardwareBuffer** outBuffer);

#endif
