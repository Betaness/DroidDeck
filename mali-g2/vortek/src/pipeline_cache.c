/*
 * [mali-g2] Persistent implicit pipeline cache of the Vortek server.
 *
 * DXVK passes VK_NULL_HANDLE as the VkPipelineCache and the Mali driver keeps no shader cache of
 * its own (in memory across devices or on disk), so every game start compiled every pipeline again
 * (a material-sized vertex or fragment shader takes ~15 ms on the Mali-G2; from a cache well under
 * 1 ms). Here every VkDevice gets a VkPipelineCache that is used whenever the application passes
 * none (pipeline libraries, optimized pipelines, compute pipelines; plain GPL fast links are left
 * out), loaded from a file per (application, driver build) at vkCreateDevice and written back:
 *   - by a saver thread, when new pipelines went in and compiling has paused for 2 s, at most every
 *     10 s, and at the latest VORTEK_PIPELINE_CACHE_SAVE_S (default 30) s after the first new one
 *     (the app kills the server with SIGKILL at the end of a session, so there is no exit hook);
 *   - when the device is destroyed, also by the cleanup after a client that is gone (killed game).
 * Writes go to a temporary file that is renamed over the old one. When the file changed on disk
 * since this device read it (two devices of the same game), the saved data is the union of both.
 *
 * File: <dir>/<application>.<driver key>.vkpc. The application is VkApplicationInfo's
 * pApplicationName (DXVK and vkd3d-proton: the .exe name; zink: the process name), else the
 * engine name. The driver key hashes vendorID, deviceID, driverVersion, pipelineCacheUUID and the
 * driver info string (Mali: "v1.r56p0-...", the build), so a driver update starts new files. A
 * 64-byte header carries the same identity, the data size and a hash of the data; a file that does
 * not match is ignored and replaced.
 *
 * Environment:
 *   VORTEK_PIPELINE_CACHE_DIR       directory of the cache files; unset = no implicit cache
 *   VORTEK_PIPELINE_CACHE=0         no implicit cache even with a directory
 *   VORTEK_PIPELINE_CACHE_MAX_MB    largest file written (default 64): a cache that grows past it
 *                                   keeps the file it started from
 *   VORTEK_PIPELINE_CACHE_TOTAL_MB  all files together (default 256): least recently used go first
 *   VORTEK_PIPELINE_CACHE_SAVE_S    longest time new pipelines stay unsaved (default 30)
 * Logs one line per device when it loads ("vortek: pipeline cache Game.exe: loaded 2345 KB ...")
 * and one when it goes, with the pipelines it created and how long they took ("... 412 pipelines in
 * 1234 ms, by time: <2 ms 300 ..."), which also counts pipeline compiles per game session.
 *
 * Copyright (C) 2026 mali-v15-bringup contributors.
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "vulkan_helper.h"
#include "pipeline_cache.h"

#define FILE_MAGIC "VTKPCv1"
#define FILE_HEADER_SIZE 64
#define IDLE_BEFORE_SAVE_NS 2000000000ull
#define MIN_SAVE_INTERVAL_NS 10000000000ull
#define STALE_TEMP_S 600
#define HIST_BUCKETS 5

typedef struct {
    uint32_t vendorID, deviceID, driverVersion;
    uint8_t uuid[VK_UUID_SIZE];
    uint64_t driverKey;
} DriverIdentity;

typedef struct CacheEntry {
    struct CacheEntry* next;
    VkDevice device;
    VkPipelineCache cache;
    DriverIdentity identity;
    char path[PATH_MAX];
    char name[80];
    /* what is on disk as far as this entry knows (written or read by it) */
    bool knownExists;
    struct timespec knownMtime;
    off_t knownSize;
    /* under tableLock */
    int busy;
    uint64_t dirty, firstDirtyNs, lastCreateNs, lastSaveNs;
    uint32_t pipelines, others, saves;
    uint64_t compileNs, othersNs, maxNs;
    uint32_t hist[HIST_BUCKETS];
    size_t loadedBytes, savedBytes;
    bool overCapLogged;
} CacheEntry;

static pthread_once_t configOnce = PTHREAD_ONCE_INIT;
static char cacheDir[PATH_MAX];
static bool enabled;
static size_t maxFileBytes, maxTotalBytes;
static uint64_t maxUnsavedNs;

static pthread_mutex_t tableLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idleCond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t saverCond = PTHREAD_COND_INITIALIZER;
static CacheEntry* entries;
static bool saverStarted;

static uint64_t nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static long envLong(const char* name, long def) {
    const char* value = getenv(name);
    if (!value || !*value) return def;
    char* end;
    long result = strtol(value, &end, 10);
    return end != value && result >= 0 ? result : def;
}

static void loadConfig(void) {
    const char* dir = getenv("VORTEK_PIPELINE_CACHE_DIR");
    const char* toggle = getenv("VORTEK_PIPELINE_CACHE");
    if (!dir || !*dir || (toggle && strcmp(toggle, "0") == 0)) return;
    if (strlen(dir) >= sizeof(cacheDir) - 128) return;
    strcpy(cacheDir, dir);
    if (mkdir(cacheDir, 0700) != 0 && errno != EEXIST) {
        println("vortek: pipeline cache: cannot create %s (%s), off", cacheDir, strerror(errno));
        return;
    }
    maxFileBytes = (size_t)envLong("VORTEK_PIPELINE_CACHE_MAX_MB", 64) << 20;
    maxTotalBytes = (size_t)envLong("VORTEK_PIPELINE_CACHE_TOTAL_MB", 256) << 20;
    maxUnsavedNs = (uint64_t)envLong("VORTEK_PIPELINE_CACHE_SAVE_S", 30) * 1000000000ull;
    if (maxUnsavedNs < MIN_SAVE_INTERVAL_NS) maxUnsavedNs = MIN_SAVE_INTERVAL_NS;
    enabled = maxFileBytes > 0;
    if (enabled)
        println("vortek: pipeline cache in %s (file limit %zu MB, all files %zu MB, saved within %llu s)", cacheDir, maxFileBytes >> 20,
                maxTotalBytes >> 20, (unsigned long long)(maxUnsavedNs / 1000000000ull));
}

/* not cryptographic: detects truncated or damaged files */
static uint64_t hashBytes(const void* data, size_t size, uint64_t h) {
    const uint8_t* p = data;
    h ^= size * 0x9e3779b97f4a7c15ull;
    for (; size >= 8; size -= 8, p += 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h = (h ^ w) * 0xff51afd7ed558ccdull;
        h ^= h >> 32;
    }
    for (; size > 0; size--, p++) h = (h ^ *p) * 0x100000001b3ull;
    h ^= h >> 29;
    h *= 0xc4ceb9fe1a85ec53ull;
    return h ^ (h >> 32);
}

static void driverIdentity(VkPhysicalDevice physicalDevice, DriverIdentity* id) {
    VkPhysicalDeviceDriverProperties driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    vulkanWrapper.vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
    memset(id, 0, sizeof(*id));
    id->vendorID = properties.properties.vendorID;
    id->deviceID = properties.properties.deviceID;
    id->driverVersion = properties.properties.driverVersion;
    memcpy(id->uuid, properties.properties.pipelineCacheUUID, VK_UUID_SIZE);
    uint64_t h = hashBytes(&id->vendorID, 12, 0x6a09e667f3bcc908ull);
    h = hashBytes(id->uuid, VK_UUID_SIZE, h);
    driver.driverInfo[VK_MAX_DRIVER_INFO_SIZE - 1] = 0;
    id->driverKey = hashBytes(driver.driverInfo, strlen(driver.driverInfo), h);
}

/* header: magic[8] vendor device version uuid[16] key(8) dataSize(8) dataHash(8) pad */
static void writeHeader(uint8_t* header, const DriverIdentity* id, const void* data, uint64_t size) {
    memset(header, 0, FILE_HEADER_SIZE);
    memcpy(header, FILE_MAGIC, 8);
    memcpy(header + 8, &id->vendorID, 4);
    memcpy(header + 12, &id->deviceID, 4);
    memcpy(header + 16, &id->driverVersion, 4);
    memcpy(header + 20, id->uuid, VK_UUID_SIZE);
    memcpy(header + 36, &id->driverKey, 8);
    memcpy(header + 44, &size, 8);
    uint64_t h = hashBytes(data, size, id->driverKey);
    memcpy(header + 52, &h, 8);
}

static bool headerMatches(const uint8_t* header, const DriverIdentity* id, const void* data, uint64_t size) {
    uint8_t want[FILE_HEADER_SIZE];
    writeHeader(want, id, data, size);
    return memcmp(header, want, FILE_HEADER_SIZE) == 0;
}

/* The pipeline cache data of a valid file (malloc'd, *size bytes), or NULL; *st gets its stat. */
static void* readCacheFile(const char* path, const DriverIdentity* id, size_t* size, struct stat* st, bool* exists) {
    *size = 0;
    *exists = false;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    void* data = NULL;
    uint8_t header[FILE_HEADER_SIZE];
    *exists = fstat(fd, st) == 0;
    if (*exists && st->st_size > FILE_HEADER_SIZE && (size_t)st->st_size <= maxFileBytes + FILE_HEADER_SIZE &&
        pread(fd, header, FILE_HEADER_SIZE, 0) == FILE_HEADER_SIZE) {
        size_t dataSize = (size_t)st->st_size - FILE_HEADER_SIZE;
        data = malloc(dataSize);
        size_t done = 0;
        while (data && done < dataSize) {
            ssize_t n = pread(fd, (uint8_t*)data + done, dataSize - done, FILE_HEADER_SIZE + done);
            if (n <= 0) break;
            done += n;
        }
        if (data && done == dataSize && headerMatches(header, id, data, dataSize)) *size = dataSize;
        else {
            free(data);
            data = NULL;
        }
    }
    close(fd);
    return data;
}

static void sanitize(char* out, size_t outSize, const char* in) {
    size_t n = 0;
    for (; in && *in && n + 1 < outSize; in++) {
        char c = *in;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '+';
        out[n++] = ok ? c : '_';
    }
    out[n] = 0;
    if (n == 0 || out[0] == '.') snprintf(out, outSize, "unknown");
}

typedef struct { time_t t; long ns; off_t size; char name[256]; } DirItem;

static int compareMtime(const void* a, const void* b) {
    const DirItem *x = a, *y = b;
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    return (x->ns > y->ns) - (x->ns < y->ns);
}

/* Drops temporary files left by a killed server and the least recently used caches beyond the
 * total limit (never `keep`). */
static void trimDirectory(const char* keep) {
    DIR* dir = opendir(cacheDir);
    if (!dir) return;
    DirItem* items = NULL;
    size_t count = 0, capacity = 0, total = 0;
    time_t now = time(NULL);
    struct dirent* d;
    while ((d = readdir(dir))) {
        struct stat st;
        if (d->d_name[0] == '.' || fstatat(dirfd(dir), d->d_name, &st, 0) != 0 || !S_ISREG(st.st_mode)) continue;
        if (strstr(d->d_name, ".tmp-")) {
            if (now - st.st_mtime > STALE_TEMP_S) unlinkat(dirfd(dir), d->d_name, 0);
            continue;
        }
        size_t len = strlen(d->d_name);
        if (len < 5 || strcmp(d->d_name + len - 5, ".vkpc") != 0 || len >= sizeof(items[0].name)) continue;
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 32;
            DirItem* grown = realloc(items, capacity * sizeof(DirItem));
            if (!grown) break;
            items = grown;
        }
        items[count].t = st.st_mtim.tv_sec;
        items[count].ns = st.st_mtim.tv_nsec;
        items[count].size = st.st_size;
        strcpy(items[count].name, d->d_name);
        total += st.st_size;
        count++;
    }
    if (total > maxTotalBytes && count > 0) {
        qsort(items, count, sizeof(DirItem), compareMtime);
        const char* keepName = keep ? strrchr(keep, '/') : NULL;
        keepName = keepName ? keepName + 1 : "";
        for (size_t i = 0; i < count && total > maxTotalBytes; i++) {
            if (strcmp(items[i].name, keepName) == 0) continue;
            if (unlinkat(dirfd(dir), items[i].name, 0) == 0) {
                total -= items[i].size;
                println("vortek: pipeline cache: removed %s (%lld KB, least recently used beyond %zu MB)", items[i].name,
                        (long long)items[i].size >> 10, maxTotalBytes >> 20);
            }
        }
    }
    free(items);
    closedir(dir);
}

static bool writeFileAtomically(CacheEntry* e, const void* data, size_t size) {
    char tmp[PATH_MAX + 32];
    snprintf(tmp, sizeof(tmp), "%s.tmp-%d-%lx", e->path, getpid(), (unsigned long)pthread_self());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    uint8_t header[FILE_HEADER_SIZE];
    writeHeader(header, &e->identity, data, size);
    bool ok = write(fd, header, FILE_HEADER_SIZE) == FILE_HEADER_SIZE;
    for (size_t done = 0; ok && done < size;) {
        ssize_t n = write(fd, (const uint8_t*)data + done, size - done);
        if (n <= 0) ok = false;
        else done += n;
    }
    ok = ok && fsync(fd) == 0;
    struct stat st;
    ok = ok && fstat(fd, &st) == 0;
    close(fd);
    if (ok && rename(tmp, e->path) == 0) {
        e->knownExists = true;
        e->knownMtime = st.st_mtim;
        e->knownSize = st.st_size;
        return true;
    }
    unlink(tmp);
    return false;
}

static void* getCacheData(VkDevice device, VkPipelineCache cache, size_t* size) {
    for (int attempt = 0; attempt < 4; attempt++) {
        *size = 0;
        if (vulkanWrapper.vkGetPipelineCacheData(device, cache, size, NULL) != VK_SUCCESS || *size == 0) return NULL;
        size_t want = *size + *size / 8 + 4096; /* room for pipelines added in between */
        void* data = malloc(want);
        if (!data) return NULL;
        *size = want;
        VkResult result = vulkanWrapper.vkGetPipelineCacheData(device, cache, size, data);
        if (result == VK_SUCCESS) return data;
        free(data);
        if (result != VK_INCOMPLETE) return NULL;
    }
    return NULL;
}

/* Writes the entry's cache (merged with a newer file another device wrote) to disk; no lock held,
 * the entry is busy (or private to the caller). False when it should be tried again later. */
static bool saveEntry(CacheEntry* e) {
    if (e->overCapLogged) return true;
    uint64_t t0 = nowNs();
    VkPipelineCache source = e->cache, merged = VK_NULL_HANDLE;
    struct stat st;
    bool changedOnDisk = stat(e->path, &st) == 0
                             ? !e->knownExists || st.st_size != e->knownSize || st.st_mtim.tv_sec != e->knownMtime.tv_sec ||
                                   st.st_mtim.tv_nsec != e->knownMtime.tv_nsec
                             : false;
    if (changedOnDisk) {
        /* union with what the other writer saved, built in a new cache: merging into the live one
         * would need it externally synchronized against the pipeline threads */
        size_t size;
        bool exists;
        void* disk = readCacheFile(e->path, &e->identity, &size, &st, &exists);
        if (disk) {
            VkPipelineCacheCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, size, disk};
            if (vulkanWrapper.vkCreatePipelineCache(e->device, &info, NULL, &merged) == VK_SUCCESS &&
                vulkanWrapper.vkMergePipelineCaches(e->device, merged, 1, &e->cache) == VK_SUCCESS)
                source = merged;
            free(disk);
        }
    }

    size_t size;
    void* data = getCacheData(e->device, source, &size);
    if (merged) vulkanWrapper.vkDestroyPipelineCache(e->device, merged, NULL);
    if (!data) return false;
    if (!changedOnDisk && e->knownExists && (off_t)size + FILE_HEADER_SIZE == e->knownSize) {
        /* every pipeline came from the cache. Mali's data differs from run to run (order) even then,
         * but entries are only ever added, so the same size means nothing new to write. */
        free(data);
        return true;
    }
    if (size > maxFileBytes) {
        if (!e->overCapLogged)
            println("vortek: pipeline cache %s: %zu KB is over the %zu MB limit, keeping the file it started from", e->name, size >> 10,
                    maxFileBytes >> 20);
        e->overCapLogged = true;
        free(data);
        return true;
    }
    bool ok = writeFileAtomically(e, data, size);
    free(data);
    if (!ok) {
        println("vortek: pipeline cache %s: could not write %s (%s)", e->name, e->path, strerror(errno));
        return false;
    }
    e->savedBytes = size;
    e->saves++;
    if (e->saves == 1 || getenv("VORTEK_PIPELINE_CACHE_LOG"))
        println("vortek: pipeline cache %s: saved %zu KB in %.1f ms%s", e->name, size >> 10, (nowNs() - t0) / 1e6,
                source == merged && merged ? " (merged with a newer file)" : "");
    trimDirectory(e->path);
    return true;
}

static bool dueForSave(const CacheEntry* e, uint64_t now) {
    if (e->busy || e->dirty == 0) return false;
    if (now - e->firstDirtyNs >= maxUnsavedNs) return true;
    return now - e->lastCreateNs >= IDLE_BEFORE_SAVE_NS && now - e->lastSaveNs >= MIN_SAVE_INTERVAL_NS;
}

static void* saverThread(void* unused) {
    (void)unused;
    pthread_mutex_lock(&tableLock);
    for (;;) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 1;
        pthread_cond_timedwait(&saverCond, &tableLock, &until);
        uint64_t now = nowNs();
        for (CacheEntry* e = entries; e; e = e->next) {
            if (!dueForSave(e, now)) continue;
            e->busy = 1;
            uint64_t dirty = e->dirty;
            pthread_mutex_unlock(&tableLock);
            bool saved = saveEntry(e);
            pthread_mutex_lock(&tableLock);
            e->busy = 0;
            if (saved) e->dirty -= dirty;
            e->lastSaveNs = nowNs();
            if (e->dirty) e->firstDirtyNs = e->lastSaveNs;
            pthread_cond_broadcast(&idleCond);
            break; /* the list may have changed while unlocked */
        }
    }
    return NULL;
}

void PipelineCache_deviceCreated(VkPhysicalDevice physicalDevice, VkDevice device, const char* appName, const char* engineName) {
    pthread_once(&configOnce, loadConfig);
    if (!enabled || !device) return;
    CacheEntry* e = calloc(1, sizeof(CacheEntry));
    if (!e) return;
    e->device = device;
    driverIdentity(physicalDevice, &e->identity);
    sanitize(e->name, sizeof(e->name), appName && *appName ? appName : engineName);
    snprintf(e->path, sizeof(e->path), "%s/%s.%016llx.vkpc", cacheDir, e->name, (unsigned long long)e->identity.driverKey);

    uint64_t t0 = nowNs();
    size_t size;
    struct stat st;
    bool exists;
    void* data = readCacheFile(e->path, &e->identity, &size, &st, &exists);
    VkPipelineCacheCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, size, data};
    VkResult result = vulkanWrapper.vkCreatePipelineCache(device, &info, NULL, &e->cache);
    if (result != VK_SUCCESS && data) {
        info.initialDataSize = 0;
        info.pInitialData = NULL;
        result = vulkanWrapper.vkCreatePipelineCache(device, &info, NULL, &e->cache);
        size = 0;
    }
    free(data);
    if (result != VK_SUCCESS) {
        println("vortek: pipeline cache %s: vkCreatePipelineCache failed (%d)", e->name, result);
        free(e);
        return;
    }
    if (size) {
        /* recently used: the last to go when the directory is over its limit */
        utimensat(AT_FDCWD, e->path, NULL, 0);
        if (stat(e->path, &st) == 0) {
            e->knownExists = true;
            e->knownMtime = st.st_mtim;
            e->knownSize = st.st_size;
        }
    }
    e->loadedBytes = size;
    println("vortek: pipeline cache %s: %s%zu KB in %.1f ms (%s)", e->name, size ? "loaded " : exists ? "unusable file, new; " : "new, ",
            size >> 10, (nowNs() - t0) / 1e6, e->path);

    pthread_mutex_lock(&tableLock);
    e->next = entries;
    entries = e;
    if (!saverStarted) {
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        saverStarted = pthread_create(&thread, &attr, saverThread, NULL) == 0;
        pthread_attr_destroy(&attr);
    }
    pthread_mutex_unlock(&tableLock);
}

static CacheEntry* findEntry(VkDevice device) {
    for (CacheEntry* e = entries; e; e = e->next) {
        if (e->device == device) return e;
    }
    return NULL;
}

VkPipelineCache PipelineCache_get(VkDevice device) {
    pthread_once(&configOnce, loadConfig);
    if (!enabled) return VK_NULL_HANDLE;
    pthread_mutex_lock(&tableLock);
    CacheEntry* e = findEntry(device);
    VkPipelineCache cache = e ? e->cache : VK_NULL_HANDLE;
    pthread_mutex_unlock(&tableLock);
    return cache;
}

void PipelineCache_noteCreated(VkDevice device, uint32_t count, uint64_t elapsedNs, bool withImplicitCache) {
    if (!enabled || count == 0) return;
    pthread_mutex_lock(&tableLock);
    CacheEntry* e = findEntry(device);
    if (e) {
        uint64_t now = nowNs();
        if (withImplicitCache) {
            uint64_t each = elapsedNs / count;
            static const uint64_t limitsMs[HIST_BUCKETS - 1] = {2, 10, 50, 200};
            int bucket = 0;
            while (bucket < HIST_BUCKETS - 1 && each >= limitsMs[bucket] * 1000000ull) bucket++;
            e->hist[bucket] += count;
            e->pipelines += count;
            e->compileNs += elapsedNs;
            if (each > e->maxNs) e->maxNs = each;
            if (e->dirty == 0) e->firstDirtyNs = now;
            e->dirty += count;
            e->lastCreateNs = now;
        } else {
            e->others += count;
            e->othersNs += elapsedNs;
        }
    }
    pthread_mutex_unlock(&tableLock);
}

void PipelineCache_deviceDestroyed(VkDevice device) {
    if (!enabled) return;
    pthread_mutex_lock(&tableLock);
    CacheEntry** link = &entries;
    while (*link && (*link)->device != device) link = &(*link)->next;
    CacheEntry* e = *link;
    if (e) {
        while (e->busy) pthread_cond_wait(&idleCond, &tableLock);
        *link = e->next;
    }
    pthread_mutex_unlock(&tableLock);
    if (!e) return;

    if (e->dirty) saveEntry(e);
    println("vortek: pipeline cache %s: %u pipelines in %.0f ms (longest %.1f ms; by time: <2 ms %u, <10 ms %u, <50 ms %u, <200 ms %u, "
            "more %u), %u fast links/own-cache in %.0f ms; loaded %zu KB, saved %zu KB",
            e->name, e->pipelines, e->compileNs / 1e6, e->maxNs / 1e6, e->hist[0], e->hist[1], e->hist[2], e->hist[3], e->hist[4], e->others,
            e->othersNs / 1e6, e->loadedBytes >> 10, e->savedBytes >> 10);
    vulkanWrapper.vkDestroyPipelineCache(device, e->cache, NULL);
    free(e);
}

bool PipelineCache_isFastLink(const VkGraphicsPipelineCreateInfo* createInfo) {
    const VkPipelineLibraryCreateInfoKHR* libraries = NULL;
    VkPipelineCreateFlags2KHR flags = createInfo->flags;
    for (const VkBaseInStructure* next = createInfo->pNext; next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) libraries = (const VkPipelineLibraryCreateInfoKHR*)next;
        else if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR)
            flags = ((const VkPipelineCreateFlags2CreateInfoKHR*)next)->flags;
    }
    return libraries && libraries->libraryCount > 0 && !(flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR) &&
           !(flags & VK_PIPELINE_CREATE_2_LINK_TIME_OPTIMIZATION_BIT_EXT);
}
