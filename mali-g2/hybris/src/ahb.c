/*
 * AHardwareBuffer calls for a glibc process: forwarded to the device's libnativewindow.so, loaded
 * with libhybris's bionic linker (Android's libvulkan, which the libhybris ICD loads for instance
 * creation, already depends on it). Allocation goes to the gralloc allocator service over binder,
 * as for any app process.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include <android/hardware_buffer.h>

#include <pthread.h>
#include <stdio.h>
#include <hybris/common/dlfcn.h>

static struct {
    int (*allocate)(const AHardwareBuffer_Desc*, AHardwareBuffer**);
    void (*acquire)(AHardwareBuffer*);
    void (*release)(AHardwareBuffer*);
    void (*describe)(const AHardwareBuffer*, AHardwareBuffer_Desc*);
    int (*lock)(AHardwareBuffer*, uint64_t, int32_t, const struct ARect*, void**);
    int (*unlock)(AHardwareBuffer*, int32_t*);
    int (*sendHandle)(const AHardwareBuffer*, int);
    int (*recvHandle)(int, AHardwareBuffer**);
} ahb;

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int loaded;

static void load(void) {
    void* lib = hybris_dlopen("libnativewindow.so", 2 /* RTLD_NOW */);
    if (!lib) {
        fprintf(stderr, "hybris-wsi: libnativewindow.so not loaded: %s\n", hybris_dlerror());
        return;
    }
    ahb.allocate = hybris_dlsym(lib, "AHardwareBuffer_allocate");
    ahb.acquire = hybris_dlsym(lib, "AHardwareBuffer_acquire");
    ahb.release = hybris_dlsym(lib, "AHardwareBuffer_release");
    ahb.describe = hybris_dlsym(lib, "AHardwareBuffer_describe");
    ahb.lock = hybris_dlsym(lib, "AHardwareBuffer_lock");
    ahb.unlock = hybris_dlsym(lib, "AHardwareBuffer_unlock");
    ahb.sendHandle = hybris_dlsym(lib, "AHardwareBuffer_sendHandleToUnixSocket");
    ahb.recvHandle = hybris_dlsym(lib, "AHardwareBuffer_recvHandleFromUnixSocket");
    loaded = ahb.allocate && ahb.acquire && ahb.release && ahb.describe && ahb.sendHandle && ahb.recvHandle;
    if (!loaded) fprintf(stderr, "hybris-wsi: libnativewindow.so lacks the AHardwareBuffer calls\n");
}

int DroidDeckAhb_load(void) {
    pthread_once(&once, load);
    return loaded;
}

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** outBuffer) {
    return DroidDeckAhb_load() ? ahb.allocate(desc, outBuffer) : -1;
}

void AHardwareBuffer_acquire(AHardwareBuffer* buffer) {
    if (DroidDeckAhb_load()) ahb.acquire(buffer);
}

void AHardwareBuffer_release(AHardwareBuffer* buffer) {
    if (DroidDeckAhb_load()) ahb.release(buffer);
}

void AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc) {
    if (DroidDeckAhb_load()) ahb.describe(buffer, outDesc);
}

int AHardwareBuffer_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence, const struct ARect* rect, void** outVirtualAddress) {
    return DroidDeckAhb_load() && ahb.lock ? ahb.lock(buffer, usage, fence, rect, outVirtualAddress) : -1;
}

int AHardwareBuffer_unlock(AHardwareBuffer* buffer, int32_t* fence) {
    return DroidDeckAhb_load() && ahb.unlock ? ahb.unlock(buffer, fence) : -1;
}

int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer* buffer, int socketFd) {
    return DroidDeckAhb_load() ? ahb.sendHandle(buffer, socketFd) : -1;
}

int AHardwareBuffer_recvHandleFromUnixSocket(int socketFd, AHardwareBuffer** outBuffer) {
    return DroidDeckAhb_load() ? ahb.recvHandle(socketFd, outBuffer) : -1;
}
