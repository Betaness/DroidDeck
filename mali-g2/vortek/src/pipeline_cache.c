/*
 * [mali-g2] Persistent implicit pipeline cache of the Vortek server, also built into the in-process
 * libhybris ICD (mali-g2/hybris/src/pipeline_cache_hybris.c defines PCACHE_LOG, PCACHE_VK, PCACHE_ENV
 * and includes this file: the driver is reached through its device table, the variables are
 * DROIDDECK_HYBRIS_PIPELINE_CACHE_*, logs go to stderr).
 *
 * DXVK passes VK_NULL_HANDLE as the VkPipelineCache and the Mali driver keeps no shader cache of
 * its own (in memory across devices or on disk), so every game start compiled every pipeline again
 * (a material-sized vertex or fragment shader takes ~15 ms on the Mali-G2; from a cache well under
 * 1 ms). Here every VkDevice gets a VkPipelineCache that is used whenever the application passes
 * none (pipeline libraries, optimized pipelines, compute pipelines; plain GPL fast links are left
 * out), loaded from a file per (application, driver build) when the device first creates a pipeline
 * with it (devices that never do, like probes, cost nothing) and written back:
 *   - by a saver thread, when new pipelines went in and compiling has paused for 2 s, at most every
 *     10 s, and at the latest VORTEK_PIPELINE_CACHE_SAVE_S (default 30) s after the first new one
 *     (the app kills the server with SIGKILL at the end of a session, so there is no exit hook);
 *   - when the device is destroyed, also by the cleanup after a client that is gone (killed game).
 * Writes go to a temporary file that is renamed over the old one; one save at a time in the
 * directory (a mutex in the process, flock() of the directory between processes: game processes
 * of the in-process path share it). When the file changed on disk since this device read it (two
 * devices of the same game), the saved data is the union of both, and nothing is written when that
 * union adds nothing.
 * Calls that pass a shader module identifier (VK_EXT_shader_module_identifier, DXVK's library
 * recompiles) do not use the cache: the identifier is the driver's hash of the application's
 * SPIR-V, which the client may have rewritten (spec composites, clip distances) differently in the
 * session that stored the pipeline.
 *
 * File: <dir>/<application>.<driver key>.vkpc. The application is VkApplicationInfo's
 * pApplicationName (DXVK and vkd3d-proton: the .exe name; zink: the process name), else the engine
 * name, followed by -<Steam game id> where the process has one (SteamGameId, else SteamAppId: the
 * in-process path). The driver key hashes vendorID, deviceID, driverVersion, pipelineCacheUUID and
 * the driver info string (Mali: "v1.r56p0-...", the build), so a driver update starts new files. A
 * 64-byte header carries the same identity, the data size and a hash of the data; a file that does
 * not match, or whose data does not start with this driver's VkPipelineCacheHeaderVersionOne, is
 * ignored and replaced. Data that passes those checks can still crash the driver (a bug of the
 * driver's own output): <file>.loading exists while the driver parses a file, flock()ed by the
 * parsing process, which removes it before it lets go. A marker nobody holds is what a process that
 * died there left behind; it counts the deaths in a row, and at the second the file is deleted
 * instead of being loaded again (and crashing every start of that game). One death is read once
 * more: game processes die in a parse for other reasons too. A marker a live process holds is
 * waited for (up to 10 s, else no load). exit() waits up to 2 s for a parse of another thread.
 *
 * Environment:
 *   VORTEK_PIPELINE_CACHE_DIR       directory of the cache files (made with its parents); unset = no
 *                                   implicit cache
 *   VORTEK_PIPELINE_CACHE=0         no implicit cache even with a directory
 *   VORTEK_PIPELINE_CACHE_MAX_MB    largest file written (default 64): a cache that grows past it
 *                                   keeps the file it started from
 *   VORTEK_PIPELINE_CACHE_TOTAL_MB  all files together (default 256): least recently used go first
 *   VORTEK_PIPELINE_CACHE_SAVE_S    longest time new pipelines stay unsaved (default 30)
 * Logs one line per device when it loads (at its first pipeline: "vortek: pipeline cache Game.exe: loaded 2345 KB ...")
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef PCACHE_LOG
#include "vulkan_helper.h"
#define PCACHE_LOG(fmt, ...) println("vortek: " fmt, ##__VA_ARGS__)
#define PCACHE_VK(device, name) vulkanWrapper.vk##name
#define PCACHE_VK_PHYSICAL(physicalDevice, name) vulkanWrapper.vk##name
#define PCACHE_ENV "VORTEK_PIPELINE_CACHE"
#endif
#include "pipeline_cache.h"

#define FILE_MAGIC "VTKPCv1"
#define FILE_HEADER_SIZE 64
#define IDLE_BEFORE_SAVE_NS 2000000000ull
#define MIN_SAVE_INTERVAL_NS 10000000000ull
#define STALE_TEMP_S 600
#define MARKER_WAIT_MS 10000
/* a parse holds parseLock for the marker wait plus the driver's parse of at most a 64 MB file */
#define PARSE_LOCK_WAIT_MS (MARKER_WAIT_MS + 5000)
#define EXIT_WAIT_MS 2000
/* deaths during a parse of the same file before it is taken for one that crashes the driver */
#define MARKER_DEATHS 2
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
    char name[104];
    /* what is on disk as far as this entry knows (written or read by it) */
    bool knownExists;
    struct timespec knownMtime;
    off_t knownSize;
    /* under tableLock */
    int busy; /* saving or loading: the entry stays */
    bool loadTried;
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
/* one driver parse of file data (load or merge) at a time, so the .loading marker is unambiguous;
 * taken with a time limit (lockParse): its holder may be a thread that is gone */
static pthread_mutex_t parseLock = PTHREAD_MUTEX_INITIALIZER;
static int exiting;          /* atomic: exit() has begun, no parse starts */
static uintptr_t parseOwner; /* atomic: the thread holding parseLock, 0 for none */
/* one save at a time: two writers of one file would each rename over the other's data */
static pthread_mutex_t saveLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idleCond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t saverCond = PTHREAD_COND_INITIALIZER;
static CacheEntry* entries;
static bool saverStarted;

static uint64_t nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* PCACHE_ENV + suffix, else (PCACHE_ENV_FALLBACK, not for the directory) the fallback's variable. */
static const char* pcacheEnv(const char* suffix) {
    char name[96];
    snprintf(name, sizeof(name), "%s%s", PCACHE_ENV, suffix);
    const char* value = getenv(name);
#ifdef PCACHE_ENV_FALLBACK
    if ((!value || !*value) && strcmp(suffix, "_DIR") != 0) {
        snprintf(name, sizeof(name), "%s%s", PCACHE_ENV_FALLBACK, suffix);
        value = getenv(name);
    }
#endif
    return value && *value ? value : NULL;
}

static long envLong(const char* suffix, long def) {
    const char* value = pcacheEnv(suffix);
    if (!value) return def;
    char* end;
    long result = strtol(value, &end, 10);
    return end != value && result >= 0 ? result : def;
}

static bool makeDirs(char* path) {
    for (char* slash = strchr(path + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        bool ok = mkdir(path, 0700) == 0 || errno == EEXIST;
        *slash = '/';
        if (!ok) return false;
    }
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static void waitForParsesAtExit(void);

static void loadConfig(void) {
    const char* dir = pcacheEnv("_DIR");
    const char* toggle = pcacheEnv("");
    if (!dir || (toggle && strcmp(toggle, "0") == 0)) return;
    if (strlen(dir) >= sizeof(cacheDir) - 128) return;
    strcpy(cacheDir, dir);
    if (!makeDirs(cacheDir)) {
        PCACHE_LOG("pipeline cache: cannot create %s (%s), off", cacheDir, strerror(errno));
        return;
    }
    maxFileBytes = (size_t)envLong("_MAX_MB", 64) << 20;
    maxTotalBytes = (size_t)envLong("_TOTAL_MB", 256) << 20;
    maxUnsavedNs = (uint64_t)envLong("_SAVE_S", 30) * 1000000000ull;
    if (maxUnsavedNs < MIN_SAVE_INTERVAL_NS) maxUnsavedNs = MIN_SAVE_INTERVAL_NS;
    enabled = maxFileBytes > 0;
    if (enabled) {
        atexit(waitForParsesAtExit);
        PCACHE_LOG("pipeline cache in %s (file limit %zu MB, all files %zu MB, saved within %llu s)", cacheDir, maxFileBytes >> 20,
                   maxTotalBytes >> 20, (unsigned long long)(maxUnsavedNs / 1000000000ull));
    }
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
    PCACHE_VK_PHYSICAL(physicalDevice, GetPhysicalDeviceProperties2)(physicalDevice, &properties);
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

/* The data starts with the VkPipelineCacheHeaderVersionOne of this driver. */
static bool vulkanHeaderMatches(const DriverIdentity* id, const uint8_t* data, size_t size) {
    uint32_t headerSize, version, vendorID, deviceID;
    if (size < 16 + VK_UUID_SIZE) return false;
    memcpy(&headerSize, data, 4);
    memcpy(&version, data + 4, 4);
    memcpy(&vendorID, data + 8, 4);
    memcpy(&deviceID, data + 12, 4);
    return headerSize >= 16 + VK_UUID_SIZE && headerSize <= size && version == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
           vendorID == id->vendorID && deviceID == id->deviceID && memcmp(data + 16, id->uuid, VK_UUID_SIZE) == 0;
}

static bool sameFile(const struct stat* a, const struct stat* b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

/* <path>.loading for this process, created under a temporary name, locked and renamed into place,
 * so no other process sees it unlocked; -1 when it could not be made (the parse goes ahead). It
 * holds the number of parses of the file in a row that this one makes (`deaths` died before it). */
static int publishMarker(const char* marker, int deaths) {
    char tmp[PATH_MAX + 48];
    snprintf(tmp, sizeof(tmp), "%s.tmp-%d", marker, getpid());
    int fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    char text[16];
    int length = snprintf(text, sizeof(text), "%d\n", deaths + 1);
    if (write(fd, text, length) != length || flock(fd, LOCK_EX) != 0 || rename(tmp, marker) != 0) {
        unlink(tmp);
        close(fd);
        return -1;
    }
    return fd;
}

typedef enum { MARKER_NONE, MARKER_STALE, MARKER_BUSY } MarkerState;

/* Whether <path>.loading is there and nobody holds it (left by a process that died while the driver
 * parsed the file: *deaths gets how many parses in a row did), or a live process parses it now
 * (waited for up to MARKER_WAIT_MS first, less once exit() has begun). The parser removes the
 * marker before it unlocks, so a marker that is still in place once its lock is free is stale. */
static MarkerState checkMarker(const char* marker, int* deaths) {
    for (int waited = 0;; waited += 20) {
        int fd = open(marker, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return MARKER_NONE;
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            struct stat held, now;
            bool stale = fstat(fd, &held) == 0 && stat(marker, &now) == 0 && sameFile(&held, &now);
            char text[16] = {0};
            if (stale && pread(fd, text, sizeof(text) - 1, 0) < 0) text[0] = 0;
            close(fd);
            if (stale) {
                *deaths = atoi(text); /* a marker of an older build is empty: one death */
                if (*deaths < 1) *deaths = 1;
                return MARKER_STALE;
            }
            continue;
        }
        close(fd);
        if (waited >= MARKER_WAIT_MS || __atomic_load_n(&exiting, __ATOMIC_ACQUIRE)) return MARKER_BUSY;
        usleep(20000);
    }
}

/* parseLock, waiting at most `ms`: false when its holder takes longer (or is a thread that was
 * killed in a parse, Wine's way of ending threads) */
static bool lockParse(int ms) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (long)(ms % 1000) * 1000000;
    if (until.tv_nsec >= 1000000000) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000;
    }
    if (pthread_mutex_timedlock(&parseLock, &until) != 0) return false;
    __atomic_store_n(&parseOwner, (uintptr_t)pthread_self(), __ATOMIC_RELEASE);
    return true;
}

static void unlockParse(void) {
    __atomic_store_n(&parseOwner, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&parseLock);
}

/* vkCreatePipelineCache from file data with <path>.loading present while the driver parses it.
 * False (and the file deleted) when processes died there MARKER_DEATHS times in a row: a process
 * also dies in a parse for other reasons (killed at the end of a session, another thread crashed,
 * the game quit), so the first death only makes this parse count the second. Also false, without
 * parsing, when another one has been parsing it for too long or this process is exiting. */
static bool createFromFileData(VkDevice device, const char* path, const char* name, const void* data, size_t size, VkPipelineCache* cache,
                               VkResult* result) {
    char marker[PATH_MAX + 16];
    snprintf(marker, sizeof(marker), "%s.loading", path);
    if (__atomic_load_n(&exiting, __ATOMIC_ACQUIRE)) return false;
    if (!lockParse(PARSE_LOCK_WAIT_MS)) {
        PCACHE_LOG("pipeline cache %s: another thread has been reading a cache file for %d s, not read", name, PARSE_LOCK_WAIT_MS / 1000);
        return false;
    }
    if (__atomic_load_n(&exiting, __ATOMIC_ACQUIRE)) {
        unlockParse();
        return false;
    }
    int deaths = 0;
    MarkerState state = checkMarker(marker, &deaths);
    if (state == MARKER_BUSY || (state == MARKER_STALE && deaths >= MARKER_DEATHS)) {
        if (state == MARKER_STALE) {
            unlink(path);
            unlink(marker);
        }
        unlockParse();
        if (state == MARKER_STALE)
            PCACHE_LOG("pipeline cache %s: processes died while the driver read this file the last %d times, deleted it", name, deaths);
        else if (!__atomic_load_n(&exiting, __ATOMIC_ACQUIRE))
            PCACHE_LOG("pipeline cache %s: another process has been reading this file for %d s, not loaded", name, MARKER_WAIT_MS / 1000);
        return false;
    }
    if (state == MARKER_STALE)
        PCACHE_LOG("pipeline cache %s: a process died while the driver read this file last time, reading it once more", name);
    int fd = publishMarker(marker, deaths);
    VkPipelineCacheCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, size, data};
    *result = PCACHE_VK(device, CreatePipelineCache)(device, &info, NULL, cache);
    if (fd >= 0) {
        struct stat mine, now;
        if (fstat(fd, &mine) == 0 && stat(marker, &now) == 0 && sameFile(&mine, &now)) unlink(marker);
        close(fd);
    }
    unlockParse();
    return true;
}

/* exit() while another thread has the driver parse a file would leave a marker nobody holds, which
 * the next start counts as a death in the driver: exit waits for a parse under way (a marker wait
 * gives up), and none starts after it. Not for long, and not at all from inside the parse: the
 * holder may be a thread that will never let go (Wine kills a thread by unwinding it to its start,
 * a crash in the driver ends in exit() on the parsing thread). */
static void waitForParsesAtExit(void) {
    __atomic_store_n(&exiting, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&parseOwner, __ATOMIC_ACQUIRE) == (uintptr_t)pthread_self()) return;
    if (lockParse(EXIT_WAIT_MS)) unlockParse();
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
        if (data && done == dataSize && headerMatches(header, id, data, dataSize) && vulkanHeaderMatches(id, data, dataSize))
            *size = dataSize;
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

/* The Steam game this process belongs to (Steam sets SteamGameId and SteamAppId for what it starts,
 * Proton reads them), or NULL: games whose .exe has the same name (game.exe, Launcher.exe) would
 * share a file, which keeps growing as the union of both until the size limit stops all saves.
 * Only seen in the game's process (the in-process path), not by the Vortek server. */
static const char* steamGameId(void) {
    static const char* const names[] = {"SteamGameId", "SteamAppId"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char* id = getenv(names[i]);
        if (!id || !*id || strlen(id) > 20 || strspn(id, "0123456789") != strlen(id) || strspn(id, "0") == strlen(id)) continue;
        return id;
    }
    return NULL;
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
                PCACHE_LOG("pipeline cache: removed %s (%lld KB, least recently used beyond %zu MB)", items[i].name,
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
        if (PCACHE_VK(device, GetPipelineCacheData)(device, cache, size, NULL) != VK_SUCCESS || *size == 0) return NULL;
        size_t want = *size + *size / 8 + 4096; /* room for pipelines added in between */
        void* data = malloc(want);
        if (!data) return NULL;
        *size = want;
        VkResult result = PCACHE_VK(device, GetPipelineCacheData)(device, cache, size, data);
        if (result == VK_SUCCESS) return data;
        free(data);
        if (result != VK_INCOMPLETE) return NULL;
    }
    return NULL;
}

static void noteOnDisk(CacheEntry* e, const struct stat* st) {
    e->knownExists = true;
    e->knownMtime = st->st_mtim;
    e->knownSize = st->st_size;
}

/* Writes the entry's cache (merged with a newer file another device wrote) to disk; saveLock held,
 * the entry is busy (or private to the caller). False when it should be tried again later. */
static bool saveLocked(CacheEntry* e) {
    uint64_t t0 = nowNs();
    VkPipelineCache source = e->cache, merged = VK_NULL_HANDLE;
    struct stat st;
    bool changedOnDisk = false;
    if (stat(e->path, &st) != 0) e->knownExists = false; /* removed (size limit): write it again */
    else changedOnDisk = !e->knownExists || st.st_size != e->knownSize || st.st_mtim.tv_sec != e->knownMtime.tv_sec ||
                         st.st_mtim.tv_nsec != e->knownMtime.tv_nsec;
    size_t diskSize = 0;
    if (changedOnDisk) {
        /* union with what the other writer saved, built in a new cache: merging into the live one
         * would need it externally synchronized against the pipeline threads */
        bool exists;
        void* disk = readCacheFile(e->path, &e->identity, &diskSize, &st, &exists);
        if (disk) {
            VkResult result = VK_ERROR_UNKNOWN;
            if (!createFromFileData(e->device, e->path, e->name, disk, diskSize, &merged, &result) || result != VK_SUCCESS)
                merged = VK_NULL_HANDLE;
            else if (PCACHE_VK(e->device, MergePipelineCaches)(e->device, merged, 1, &e->cache) == VK_SUCCESS)
                source = merged;
            free(disk);
        }
    }

    /* Nothing new when the data has the size of the file (entries are only ever added; Mali's data
     * differs from run to run in order even then): when every pipeline came from the cache, or the
     * other writer's file already holds everything of this device. The size alone is asked first,
     * so a warm start does not copy the whole cache out after every batch of hits. */
    bool haveExpected = source == merged || (!changedOnDisk && e->knownExists);
    size_t expected = source == merged ? diskSize : (size_t)(e->knownSize - FILE_HEADER_SIZE);
    size_t size = 0;
    void* data = NULL;
    if (!haveExpected || PCACHE_VK(e->device, GetPipelineCacheData)(e->device, source, &size, NULL) != VK_SUCCESS || size != expected)
        data = getCacheData(e->device, source, &size);
    if (merged) PCACHE_VK(e->device, DestroyPipelineCache)(e->device, merged, NULL);
    if (haveExpected && size == expected) {
        if (source == merged) noteOnDisk(e, &st);
        free(data);
        return true;
    }
    if (!data) return false;
    if (size > maxFileBytes) {
        if (!e->overCapLogged)
            PCACHE_LOG("pipeline cache %s: %zu KB is over the %zu MB limit, keeping the file it started from", e->name, size >> 10,
                       maxFileBytes >> 20);
        e->overCapLogged = true;
        free(data);
        return true;
    }
    bool ok = writeFileAtomically(e, data, size);
    free(data);
    if (!ok) {
        PCACHE_LOG("pipeline cache %s: could not write %s (%s)", e->name, e->path, strerror(errno));
        return false;
    }
    e->savedBytes = size;
    e->saves++;
    if (e->saves == 1 || pcacheEnv("_LOG"))
        PCACHE_LOG("pipeline cache %s: saved %zu KB in %.1f ms%s", e->name, size >> 10, (nowNs() - t0) / 1e6,
                   source == merged ? " (merged with a newer file)" : "");
    trimDirectory(e->path);
    return true;
}

static bool saveEntry(CacheEntry* e) {
    if (e->overCapLogged || !e->cache) return true;
    pthread_mutex_lock(&saveLock);
    int dirFd = open(cacheDir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd >= 0) flock(dirFd, LOCK_EX);
    bool ok = saveLocked(e);
    if (dirFd >= 0) close(dirFd);
    pthread_mutex_unlock(&saveLock);
    return ok;
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
            e->busy++;
            uint64_t dirty = e->dirty;
            pthread_mutex_unlock(&tableLock);
            bool saved = saveEntry(e);
            pthread_mutex_lock(&tableLock);
            e->busy--;
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
    const char* game = steamGameId();
    if (game) {
        size_t length = strlen(e->name);
        snprintf(e->name + length, sizeof(e->name) - length, "-%s", game);
    }
    if (snprintf(e->path, sizeof(e->path), "%s/%s.%016llx.vkpc", cacheDir, e->name, (unsigned long long)e->identity.driverKey) >=
        (int)sizeof(e->path)) {
        free(e);
        return;
    }

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

/* Reads the entry's file into a new VkPipelineCache (the entry is busy). */
static VkPipelineCache loadEntry(CacheEntry* e) {
    uint64_t t0 = nowNs();
    size_t size;
    struct stat st;
    bool exists;
    void* data = readCacheFile(e->path, &e->identity, &size, &st, &exists);
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkResult result = VK_ERROR_UNKNOWN;
    if (data && !createFromFileData(e->device, e->path, e->name, data, size, &cache, &result)) exists = false;
    if (result != VK_SUCCESS) {
        VkPipelineCacheCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        cache = VK_NULL_HANDLE;
        result = PCACHE_VK(e->device, CreatePipelineCache)(e->device, &info, NULL, &cache);
        size = 0;
    }
    free(data);
    if (result != VK_SUCCESS) {
        PCACHE_LOG("pipeline cache %s: vkCreatePipelineCache failed (%d)", e->name, result);
        return VK_NULL_HANDLE;
    }
    if (size) {
        /* recently used: the last to go when the directory is over its limit */
        utimensat(AT_FDCWD, e->path, NULL, 0);
        if (stat(e->path, &st) == 0) noteOnDisk(e, &st);
    }
    e->loadedBytes = size;
    PCACHE_LOG("pipeline cache %s: %s%zu KB in %.1f ms (%s)", e->name, size ? "loaded " : exists ? "unusable file, new; " : "new, ",
               size >> 10, (nowNs() - t0) / 1e6, e->path);
    return cache;
}

static CacheEntry* findEntry(VkDevice device) {
    for (CacheEntry* e = entries; e; e = e->next) {
        if (e->device == device) return e;
    }
    return NULL;
}

VkPipelineCache PipelineCache_get(VkDevice device) {
    static pthread_mutex_t loadLock = PTHREAD_MUTEX_INITIALIZER;
    pthread_once(&configOnce, loadConfig);
    if (!enabled) return VK_NULL_HANDLE;
    pthread_mutex_lock(&tableLock);
    CacheEntry* e = findEntry(device);
    if (e && !e->loadTried) {
        /* the first pipeline of the device loads the file; pipelines of other threads wait for it */
        e->busy++;
        pthread_mutex_unlock(&tableLock);
        pthread_mutex_lock(&loadLock);
        pthread_mutex_lock(&tableLock);
        bool load = !e->loadTried;
        pthread_mutex_unlock(&tableLock);
        VkPipelineCache cache = load ? loadEntry(e) : VK_NULL_HANDLE;
        pthread_mutex_lock(&tableLock);
        if (load) {
            e->cache = cache;
            e->loadTried = true;
        }
        pthread_mutex_unlock(&loadLock);
        e->busy--;
        pthread_cond_broadcast(&idleCond);
    }
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
    if (e->loadTried || e->others)
        PCACHE_LOG("pipeline cache %s: %u pipelines in %.0f ms (longest %.1f ms; by time: <2 ms %u, <10 ms %u, <50 ms %u, <200 ms %u, "
                   "more %u), %u fast links/own-cache/identifier in %.0f ms; loaded %zu KB, saved %zu KB",
                   e->name, e->pipelines, e->compileNs / 1e6, e->maxNs / 1e6, e->hist[0], e->hist[1], e->hist[2], e->hist[3], e->hist[4],
                   e->others, e->othersNs / 1e6, e->loadedBytes >> 10, e->savedBytes >> 10);
    if (e->cache) PCACHE_VK(device, DestroyPipelineCache)(device, e->cache, NULL);
    free(e);
}

bool PipelineCache_usesModuleIdentifier(const VkPipelineShaderStageCreateInfo* stages, uint32_t stageCount) {
    for (uint32_t i = 0; stages && i < stageCount; i++) {
        for (const VkBaseInStructure* next = stages[i].pNext; next; next = next->pNext) {
            if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT &&
                ((const VkPipelineShaderStageModuleIdentifierCreateInfoEXT*)next)->identifierSize > 0)
                return true;
        }
    }
    return false;
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

VkPipelineCache PipelineCache_forGraphics(VkDevice device, VkPipelineCache cache, const VkGraphicsPipelineCreateInfo* createInfos,
                                          uint32_t count) {
    if (cache) return cache;
    bool allFastLinks = true;
    for (uint32_t i = 0; i < count && allFastLinks; i++) allFastLinks = PipelineCache_isFastLink(&createInfos[i]);
    if (allFastLinks) return VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; i++) {
        if (PipelineCache_usesModuleIdentifier(createInfos[i].pStages, createInfos[i].stageCount)) return VK_NULL_HANDLE;
    }
    return PipelineCache_get(device);
}

VkPipelineCache PipelineCache_forCompute(VkDevice device, VkPipelineCache cache, const VkComputePipelineCreateInfo* createInfos,
                                         uint32_t count) {
    if (cache) return cache;
    for (uint32_t i = 0; i < count; i++) {
        if (PipelineCache_usesModuleIdentifier(&createInfos[i].stage, 1)) return VK_NULL_HANDLE;
    }
    return PipelineCache_get(device);
}
