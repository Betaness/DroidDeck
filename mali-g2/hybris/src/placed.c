/*
 * VK_EXT_map_memory_placed, which the Mali lacks, for WoW64 Wine: 32-bit games need their mappings
 * below 4 GB. Without it winevulkan falls back to VK_EXT_external_memory_host, which fails on the
 * Mali (vkCreateImage -1000158000 with HOST_ALLOCATION_BIT_EXT, and mappings of the 0x7 types above
 * 4 GB; tests/wow64_memory_test.c). As the Vortek client and server do it: on a device that enables
 * the extension, host-visible memory is a dma-buf imported into the requested type, and a placed map
 * is an mmap of that dma-buf at the caller's address. Other memory and other devices go to the
 * driver untouched, and so does the memory of a dedicated allocation for an optimal image, which
 * cannot be mapped meaningfully (DXVK never does; this is what the Vortek client does too). A placed
 * map of such memory fails; DROIDDECK_HYBRIS_PLACED_OPTIMAL_IMAGES=1 backs it with a dma-buf as well,
 * for a 32-bit program that maps it anyway. DROIDDECK_HYBRIS_MAP_PLACED=0 hides the extension.
 *
 * The GPU does not snoop the CPU's caches (tests/placed_memory_test.c), so HOST_COHERENT types get
 * the system-uncached heap and nothing else. The HOST_CACHED, non-coherent types get the CPU-cached
 * system heap: the application flushes and invalidates them, and those calls clean or clean and
 * invalidate the CPU cache over the given range (DROIDDECK_HYBRIS_PLACED_CACHED=0: uncached too).
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#define _GNU_SOURCE
#include "icd.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

const char* const placed_extension = "VK_EXT_map_memory_placed";
const char* const placed_needs[] = {
    "VK_KHR_map_memory2",
    "VK_KHR_external_memory",
    "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf",
};
const uint32_t placed_need_count = ARRAY_LEN(placed_needs);

typedef struct PlacedMemory {
    VkDeviceMemory memory;
    int fd;
    VkDeviceSize size;
    void* at;
    bool cached; /* from the CPU-cached heap: flush and invalidate maintain the CPU cache */
    struct PlacedMemory* next;
} PlacedMemory;

bool placed_wanted(void) {
    return icd_env_int("DROIDDECK_HYBRIS_MAP_PLACED", 1) != 0;
}

void placed_fill_features(void* chain) {
    for (VkBaseOutStructure* s = chain; s; s = s->pNext) {
        if (s->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT) continue;
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT* f = (VkPhysicalDeviceMapMemoryPlacedFeaturesEXT*)s;
        f->memoryMapPlaced = VK_TRUE;
        f->memoryMapRangePlaced = VK_FALSE;
        f->memoryUnmapReserve = VK_TRUE;
    }
}

void placed_fill_properties(void* chain) {
    for (VkBaseOutStructure* s = chain; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT)
            ((VkPhysicalDeviceMapMemoryPlacedPropertiesEXT*)s)->minPlacedMemoryMapAlignment = (VkDeviceSize)sysconf(_SC_PAGESIZE);
}

enum { HEAP_UNCACHED, HEAP_CACHED, HEAP_COUNT };
static const char* const heapPaths[HEAP_COUNT] = {"/dev/dma_heap/system-uncached", "/dev/dma_heap/system"};
static int heapFds[HEAP_COUNT] = {-1, -1};
static bool cachedWanted;
static pthread_once_t heapsOnce = PTHREAD_ONCE_INIT;

/* Opened once: under proot every open() is a round trip through the tracer. */
static void openHeaps(void) {
    cachedWanted = icd_env_int("DROIDDECK_HYBRIS_PLACED_CACHED", 1) != 0;
    for (int i = 0; i < HEAP_COUNT; i++) {
        heapFds[i] = open(heapPaths[i], O_RDONLY | O_CLOEXEC);
        if (heapFds[i] < 0) fprintf(stderr, "hybris-icd: %s: %s\n", heapPaths[i], strerror(errno));
    }
}

/* A dma-buf for a memory type with these flags, or -1; *cached tells which heap it came from. */
static int dmabufAlloc(VkDeviceSize size, VkMemoryPropertyFlags flags, bool* cached) {
    pthread_once(&heapsOnce, openHeaps);
    int heap = HEAP_UNCACHED;
    /* Coherent memory is never flushed: only the uncached heap will do. */
    if (!(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) && (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) &&
        cachedWanted && heapFds[HEAP_CACHED] >= 0)
        heap = HEAP_CACHED;
    if (heapFds[heap] < 0) {
        errno = ENODEV;
        return -1;
    }
    struct dma_heap_allocation_data data = {.len = size, .fd_flags = O_RDWR | O_CLOEXEC};
    int result;
    do result = ioctl(heapFds[heap], DMA_HEAP_IOCTL_ALLOC, &data);
    while (result < 0 && (errno == EINTR || errno == EAGAIN));
    if (result) return -1;
    *cached = heap == HEAP_CACHED;
    return (int)data.fd;
}

typedef struct PlacedImage {
    VkImage image;
    struct PlacedImage* next;
} PlacedImage;

static VkResult VKAPI_CALL CreateImage(VkDevice handle, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocator,
                                       VkImage* image) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    VkResult result = device->CreateImage(handle, info, allocator, image);
    if (result != VK_SUCCESS || info->tiling == VK_IMAGE_TILING_OPTIMAL) return result;
    PlacedImage* linear = malloc(sizeof(*linear));
    if (!linear) {
        device->DestroyImage(handle, *image, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    linear->image = *image;
    pthread_mutex_lock(&device->placedLock);
    linear->next = device->placedLinear;
    device->placedLinear = linear;
    pthread_mutex_unlock(&device->placedLock);
    return result;
}

static void VKAPI_CALL DestroyImage(VkDevice handle, VkImage image, const VkAllocationCallbacks* allocator) {
    Device* device = icd_device(handle);
    if (!device) return;
    if (image && __atomic_load_n(&device->placedLinear, __ATOMIC_RELAXED)) {
        PlacedImage* gone = NULL;
        pthread_mutex_lock(&device->placedLock);
        for (PlacedImage** link = &device->placedLinear; *link; link = &(*link)->next) {
            if ((*link)->image == image) {
                gone = *link;
                *link = gone->next;
                break;
            }
        }
        pthread_mutex_unlock(&device->placedLock);
        free(gone);
    }
    device->DestroyImage(handle, image, allocator);
}

/* A dedicated allocation for an optimal image. */
static bool optimalImageMemory(Device* device, const void* chain) {
    VkImage image = VK_NULL_HANDLE;
    for (const VkBaseInStructure* s = chain; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) image = ((const VkMemoryDedicatedAllocateInfo*)s)->image;
    if (!image) return false;
    bool linear = false;
    pthread_mutex_lock(&device->placedLock);
    for (PlacedImage* i = device->placedLinear; i && !linear; i = i->next) linear = i->image == image;
    pthread_mutex_unlock(&device->placedLock);
    return !linear;
}

static PlacedMemory** slot(Device* device, VkDeviceMemory memory) {
    uint64_t key = (uint64_t)memory;
    PlacedMemory** link = &device->placedTable[(key * 0x9E3779B97F4A7C15ull) >> (64 - PLACED_TABLE_BITS)];
    while (*link && (*link)->memory != memory) link = &(*link)->next;
    return link;
}

static PlacedMemory* find(Device* device, VkDeviceMemory memory) {
    pthread_mutex_lock(&device->placedLock);
    PlacedMemory* found = *slot(device, memory);
    pthread_mutex_unlock(&device->placedLock);
    return found;
}

static bool importsOrExports(const void* chain) {
    for (const VkBaseInStructure* s = chain; s; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR:
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT:
        case VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID:
        case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO:
            return true;
        default:
            break;
        }
    }
    return false;
}

static void logOnce(int* flag, const char* message, int value) {
    if (!__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED)) fprintf(stderr, "hybris-icd: %s (%d)\n", message, value);
}

static VkResult VKAPI_CALL AllocateMemory(VkDevice handle, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* allocator,
                                          VkDeviceMemory* memory) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    if (!device->placed || info->memoryTypeIndex >= device->memory.memoryTypeCount ||
        !(device->memory.memoryTypes[info->memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
        importsOrExports(info->pNext))
        return device->AllocateMemory(handle, info, allocator, memory);
    static int okLogged, failLogged, importLogged, optimalLogged;
    if (!device->placedOptimal && optimalImageMemory(device, info->pNext)) {
        logOnce(&optimalLogged,
                "dedicated memory of an optimal image stays the driver's, a placed map of it fails "
                "(DROIDDECK_HYBRIS_PLACED_OPTIMAL_IMAGES=1 backs it with a dma-buf), first type",
                (int)info->memoryTypeIndex);
        return device->AllocateMemory(handle, info, allocator, memory);
    }

    long page = sysconf(_SC_PAGESIZE);
    VkMemoryPropertyFlags flags = device->memory.memoryTypes[info->memoryTypeIndex].propertyFlags;
    VkDeviceSize size = (info->allocationSize + page - 1) & ~(VkDeviceSize)(page - 1);
    PlacedMemory* record = calloc(1, sizeof(*record));
    if (!record) return VK_ERROR_OUT_OF_HOST_MEMORY;
    /* Without a dma-buf the memory could not be placed-mapped. A failed allocation is what an
     * application expects and handles (DXVK tries again elsewhere); a failed map is not. */
    int fd = dmabufAlloc(size, flags, &record->cached);
    int driverFd = fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;
    if (driverFd < 0) {
        logOnce(&failLogged, "no dma-buf for host-visible memory, allocation fails", errno);
        if (fd >= 0) close(fd);
        free(record);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    VkImportMemoryFdInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = info->pNext,
                                      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = driverFd};
    VkMemoryAllocateInfo copy = *info;
    copy.pNext = &import;
    VkResult result = device->AllocateMemory(handle, &copy, allocator, memory);
    if (result != VK_SUCCESS) {
        logOnce(&importLogged, "importing a dma-buf as host-visible memory failed, allocation fails", result);
        close(driverFd);
        close(fd);
        free(record);
        return result == VK_ERROR_OUT_OF_HOST_MEMORY ? result : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    logOnce(&okLogged, "host-visible memory is dma-buf backed for placed maps, first type", (int)info->memoryTypeIndex);
    static int cachedLogged;
    if (record->cached) logOnce(&cachedLogged, "HOST_CACHED memory from the cached dma-buf heap, first type", (int)info->memoryTypeIndex);
    record->memory = *memory;
    record->fd = fd;
    record->size = size;
    pthread_mutex_lock(&device->placedLock);
    PlacedMemory** link = slot(device, *memory);
    record->next = *link;
    *link = record;
    pthread_mutex_unlock(&device->placedLock);
    return VK_SUCCESS;
}

static void unmapPlaced(PlacedMemory* record, bool reserve) {
    if (!record->at) return;
    if (!reserve || mmap(record->at, record->size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == MAP_FAILED)
        munmap(record->at, record->size);
    record->at = NULL;
}

static void VKAPI_CALL FreeMemory(VkDevice handle, VkDeviceMemory memory, const VkAllocationCallbacks* allocator) {
    Device* device = icd_device(handle);
    if (!device) return;
    if (memory) {
        pthread_mutex_lock(&device->placedLock);
        PlacedMemory** link = slot(device, memory);
        PlacedMemory* record = *link;
        if (record) *link = record->next;
        pthread_mutex_unlock(&device->placedLock);
        if (record) {
            unmapPlaced(record, false);
            close(record->fd);
            free(record);
        }
    }
    device->FreeMemory(handle, memory, allocator);
}

static VkResult mapMemory2(VkDevice handle, const VkMemoryMapInfoKHR* info, void** data, bool core) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    PFN_vkMapMemory2KHR next = core && device->MapMemory2 ? device->MapMemory2 : device->MapMemory2KHR;
    if (!next) next = device->MapMemory2;
    if (!(info->flags & VK_MEMORY_MAP_PLACED_BIT_EXT)) return next ? next(handle, info, data) : VK_ERROR_MEMORY_MAP_FAILED;

    const VkMemoryMapPlacedInfoEXT* where = NULL;
    for (const VkBaseInStructure* s = info->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT) where = (const VkMemoryMapPlacedInfoEXT*)s;
    PlacedMemory* record = find(device, info->memory);
    static int notBacked;
    if (!record)
        logOnce(&notBacked, "placed map of memory without a dma-buf (imported, exportable or an optimal image's), fails", 0);
    if (!record || !where || !where->pPlacedAddress || info->offset || record->at) return VK_ERROR_MEMORY_MAP_FAILED;
    void* at = mmap(where->pPlacedAddress, record->size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, record->fd, 0);
    if (at == MAP_FAILED) return VK_ERROR_MEMORY_MAP_FAILED;
    record->at = at;
    *data = at;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL MapMemory2KHR(VkDevice handle, const VkMemoryMapInfoKHR* info, void** data) {
    return mapMemory2(handle, info, data, false);
}

static VkResult VKAPI_CALL MapMemory2(VkDevice handle, const VkMemoryMapInfoKHR* info, void** data) {
    return mapMemory2(handle, info, data, true);
}

static VkResult unmapMemory2(VkDevice handle, const VkMemoryUnmapInfoKHR* info, bool core) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    PlacedMemory* record = find(device, info->memory);
    if (record && record->at) {
        unmapPlaced(record, info->flags & VK_MEMORY_UNMAP_RESERVE_BIT_EXT);
        return VK_SUCCESS;
    }
    PFN_vkUnmapMemory2KHR next = core && device->UnmapMemory2 ? device->UnmapMemory2 : device->UnmapMemory2KHR;
    if (!next) next = device->UnmapMemory2;
    if (next) return next(handle, info);
    device->UnmapMemory(handle, info->memory);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL UnmapMemory2KHR(VkDevice handle, const VkMemoryUnmapInfoKHR* info) {
    return unmapMemory2(handle, info, false);
}

static VkResult VKAPI_CALL UnmapMemory2(VkDevice handle, const VkMemoryUnmapInfoKHR* info) {
    return unmapMemory2(handle, info, true);
}

static void VKAPI_CALL UnmapMemory(VkDevice handle, VkDeviceMemory memory) {
    Device* device = icd_device(handle);
    if (!device) return;
    PlacedMemory* record = find(device, memory);
    if (record && record->at) unmapPlaced(record, false);
    else device->UnmapMemory(handle, memory);
}

/* Cache maintenance by virtual address over [begin, end): clean (flush: the CPU's writes reach
 * memory for the GPU) or clean and invalidate (invalidate: later reads miss and fetch what the GPU
 * wrote). Linux lets user space do both (SCTLR_EL1.UCI), and unlike DMA_BUF_IOCTL_SYNC, which always
 * covers the whole dma-buf, it costs only the range. */
static void cacheMaintain(uintptr_t begin, uintptr_t end, bool invalidate) {
#if defined(__aarch64__)
    static uintptr_t line;
    if (!line) {
        uint64_t ctr;
        __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
        line = (uintptr_t)4 << ((ctr >> 16) & 0xf); /* DminLine: the smallest data cache line */
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (invalidate)
        for (uintptr_t a = begin & ~(line - 1); a < end; a += line) __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    else
        for (uintptr_t a = begin & ~(line - 1); a < end; a += line) __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#else
#error "cache maintenance for this architecture"
#endif
}

/* The driver flushes and invalidates through its own mapping, which placed memory does not have (it
 * dereferences a null one). Uncached memory needs only its write buffers drained; cached memory gets
 * cache maintenance over the range. */
static VkResult syncRanges(VkDevice handle, uint32_t count, const VkMappedMemoryRange* ranges, bool flush) {
    Device* device = icd_device(handle);
    if (!device) return VK_ERROR_DEVICE_LOST;
    VkMappedMemoryRange* rest = NULL;
    uint32_t restCount = 0;
    bool drain = false;
    for (uint32_t i = 0; i < count; i++) {
        PlacedMemory* record = find(device, ranges[i].memory);
        if (record && record->at) {
            if (!record->cached || ranges[i].offset >= record->size) {
                drain = true;
                continue;
            }
            VkDeviceSize size = record->size - ranges[i].offset;
            if (ranges[i].size != VK_WHOLE_SIZE && ranges[i].size < size) size = ranges[i].size;
            uintptr_t begin = (uintptr_t)record->at + ranges[i].offset;
            cacheMaintain(begin, begin + size, !flush);
            continue;
        }
        if (!rest && !(rest = malloc(count * sizeof(*rest)))) return VK_ERROR_OUT_OF_HOST_MEMORY;
        rest[restCount++] = ranges[i];
    }
#if defined(__aarch64__)
    if (drain) __asm__ volatile("dsb sy" ::: "memory");
#endif
    VkResult result = VK_SUCCESS;
    if (restCount)
        result = flush ? device->FlushMappedMemoryRanges(handle, restCount, rest) : device->InvalidateMappedMemoryRanges(handle, restCount, rest);
    free(rest);
    return result;
}

static VkResult VKAPI_CALL FlushMappedMemoryRanges(VkDevice handle, uint32_t count, const VkMappedMemoryRange* ranges) {
    return syncRanges(handle, count, ranges, true);
}

static VkResult VKAPI_CALL InvalidateMappedMemoryRanges(VkDevice handle, uint32_t count, const VkMappedMemoryRange* ranges) {
    return syncRanges(handle, count, ranges, false);
}

void placed_device_created(Device* device) {
    pthread_mutex_init(&device->placedLock, NULL);
    device->placedOptimal = icd_env_int("DROIDDECK_HYBRIS_PLACED_OPTIMAL_IMAGES", 0) != 0;
    device->MapMemory2 = (PFN_vkMapMemory2KHR)device->instance->GetDeviceProcAddr(device->handle, "vkMapMemory2");
    device->UnmapMemory2 = (PFN_vkUnmapMemory2KHR)device->instance->GetDeviceProcAddr(device->handle, "vkUnmapMemory2");
}

void placed_device_destroyed(Device* device) {
    for (uint32_t i = 0; i < ARRAY_LEN(device->placedTable); i++) {
        while (device->placedTable[i]) {
            PlacedMemory* record = device->placedTable[i];
            device->placedTable[i] = record->next;
            unmapPlaced(record, false);
            close(record->fd);
            free(record);
        }
    }
    while (device->placedLinear) {
        PlacedImage* image = device->placedLinear;
        device->placedLinear = image->next;
        free(image);
    }
    pthread_mutex_destroy(&device->placedLock);
}

PFN_vkVoidFunction placed_device_proc(Device* device, const char* name) {
    if (!device || !device->placed) return NULL;
    if (!strcmp(name, "vkAllocateMemory")) return (PFN_vkVoidFunction)AllocateMemory;
    if (!strcmp(name, "vkFreeMemory")) return (PFN_vkVoidFunction)FreeMemory;
    if (!strcmp(name, "vkCreateImage")) return (PFN_vkVoidFunction)CreateImage;
    if (!strcmp(name, "vkDestroyImage")) return (PFN_vkVoidFunction)DestroyImage;
    if (!strcmp(name, "vkMapMemory2KHR")) return (PFN_vkVoidFunction)MapMemory2KHR;
    if (!strcmp(name, "vkMapMemory2")) return (PFN_vkVoidFunction)MapMemory2;
    if (!strcmp(name, "vkUnmapMemory2KHR")) return (PFN_vkVoidFunction)UnmapMemory2KHR;
    if (!strcmp(name, "vkUnmapMemory2")) return (PFN_vkVoidFunction)UnmapMemory2;
    if (!strcmp(name, "vkUnmapMemory")) return (PFN_vkVoidFunction)UnmapMemory;
    if (!strcmp(name, "vkFlushMappedMemoryRanges")) return (PFN_vkVoidFunction)FlushMappedMemoryRanges;
    if (!strcmp(name, "vkInvalidateMappedMemoryRanges")) return (PFN_vkVoidFunction)InvalidateMappedMemoryRanges;
    return NULL;
}
