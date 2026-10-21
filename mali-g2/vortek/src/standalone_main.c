/*
 * vortek-server: standalone (JVM-free) host for Winlator's Vortek renderer.
 *
 * Replaces vortekrenderer/src/main.c (JNI entry points) and the Java side of
 * VortekRendererComponent (unix socket listener + per-connection dispatch).
 * Vulkan is loaded from the vendor HAL module through the exported "sphal"
 * linker namespace, so it also works inside proot where the system
 * libvulkan.so loader cannot see the driver.
 *
 * Copyright (C) 2026 mali-v15-bringup contributors.
 * Based on Winlator's vortekrenderer (C) brunodev85, LGPL-2.1.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version. See the LICENSE file of the upstream
 * vortek project for the full text.
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <android/dlext.h>
#include <android/hardware_buffer.h>

#include "vk_context.h"
#include "vortek_serializer.h"
#include "request_handler.h"
#include "vulkan_helper.h"

#define DEFAULT_SOCKET_PATH "/tmp/.vortek/V0"
#define DEFAULT_HAL_DRIVER "/vendor/lib64/hw/vulkan.mali.so"
#ifndef ANDROID_DLEXT_USE_NAMESPACE
#define ANDROID_DLEXT_USE_NAMESPACE 0x200
#endif

/* Globals normally defined by vortekrenderer/src/main.c */
VulkanWrapper vulkanWrapper = {0};
bool vortekSerializerCastVkObject = true;

static VortekOptions options;
static char socketPath[108];
static int listenFd = -1;

/* ---- replacements for winlator/src/{sysvshared_memory,gpu_image}.c ---- */

int createMemoryFd(const char* name, int64_t size) {
    int fd = syscall(__NR_memfd_create, name, 0);
    if (fd < 0) return -1;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* The ring buffers only need an mmap-able shared fd; memfd works in proot. */
int ashmemCreateRegion(const char* name, int64_t size) {
    return createMemoryFd(name, size);
}

typedef struct native_handle {
    int version;
    int numFds;
    int numInts;
    int data[0];
} native_handle_t;

/* AHardwareBuffer_getNativeHandle is exported by libnativewindow.so but is not
 * in the NDK stubs (it is a vendor/LL-NDK API), so resolve it at run time. */
typedef const native_handle_t* (*PFN_AHardwareBuffer_getNativeHandle)(const AHardwareBuffer* buffer);

int AHardwareBuffer_getFd(AHardwareBuffer* hardwareBuffer) {
    static PFN_AHardwareBuffer_getNativeHandle getNativeHandle;
    if (!getNativeHandle) {
        getNativeHandle = (PFN_AHardwareBuffer_getNativeHandle)dlsym(RTLD_DEFAULT, "AHardwareBuffer_getNativeHandle");
        if (!getNativeHandle) {
            void* lib = dlopen("libnativewindow.so", RTLD_NOW);
            if (lib) getNativeHandle = (PFN_AHardwareBuffer_getNativeHandle)dlsym(lib, "AHardwareBuffer_getNativeHandle");
        }
        if (!getNativeHandle) return -1;
    }
    const native_handle_t* nativeHandle = getNativeHandle(hardwareBuffer);
    return nativeHandle && nativeHandle->numFds > 0 ? nativeHandle->data[0] : -1;
}

/* ---- Vulkan driver loading (HAL module via sphal namespace) ---- */

typedef struct hw_module_methods {
    int (*open)(const void* module, const char* id, void** device);
} hw_module_methods_t;

typedef struct hw_module {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char* id;
    const char* name;
    const char* author;
    hw_module_methods_t* methods;
    void* dso;
    uint64_t reserved[32 - 7];
} hw_module_t;

typedef struct hw_device {
    uint32_t tag;
    uint32_t version;
    hw_module_t* module;
    uint64_t reserved[12];
    int (*close)(void* device);
} hw_device_t;

typedef struct hwvulkan_device {
    hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;

/* android_get_exported_namespace lives in libdl_android.so, which is not part of
 * the NDK: resolve it at run time instead of linking it, so the server builds with
 * a stock NDK and its only DT_NEEDED entries are public NDK libraries. */
typedef struct android_namespace_t* (*PFN_android_get_exported_namespace)(const char* name);

static struct android_namespace_t* getExportedNamespace(const char* name) {
    PFN_android_get_exported_namespace fn =
        (PFN_android_get_exported_namespace)dlsym(RTLD_DEFAULT, "android_get_exported_namespace");
    if (!fn) {
        void* lib = dlopen("libdl_android.so", RTLD_NOW | RTLD_GLOBAL);
        if (lib) fn = (PFN_android_get_exported_namespace)dlsym(lib, "android_get_exported_namespace");
    }
    if (!fn) {
        fprintf(stderr, "vortek-server: android_get_exported_namespace unavailable: %s\n", dlerror());
        return NULL;
    }
    return fn(name);
}

static bool loadHalDriver(const char* path) {
    struct android_namespace_t* ns = getExportedNamespace("sphal");
    void* handle = NULL;
    if (ns) {
        android_dlextinfo extinfo = {0};
        extinfo.flags = ANDROID_DLEXT_USE_NAMESPACE;
        extinfo.library_namespace = ns;
        handle = android_dlopen_ext(path, RTLD_NOW | RTLD_LOCAL, &extinfo);
        if (!handle) fprintf(stderr, "vortek-server: sphal dlopen %s: %s\n", path, dlerror());
    }
    else fprintf(stderr, "vortek-server: no exported sphal namespace\n");
    if (!handle) handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "vortek-server: cannot load %s: %s\n", path, dlerror());
        return false;
    }

    hw_module_t* module = dlsym(handle, "HMI");
    if (!module || !module->methods || !module->methods->open) {
        fprintf(stderr, "vortek-server: %s has no HMI\n", path);
        return false;
    }

    hwvulkan_device_t* device = NULL;
    int res = module->methods->open(module, "vk0", (void**)&device);
    if (res != 0 || !device) {
        fprintf(stderr, "vortek-server: HMI open(vk0) failed: %d\n", res);
        return false;
    }

    memset(&vulkanWrapper, 0, sizeof(VulkanWrapper));
    vulkanWrapper.vkCreateInstance = device->CreateInstance;
    vulkanWrapper.vkEnumerateInstanceExtensionProperties = device->EnumerateInstanceExtensionProperties;
    vulkanWrapper.vkGetInstanceProcAddr = device->GetInstanceProcAddr;
    fprintf(stderr, "vortek-server: loaded HAL driver %s (%s)\n", path, module->name ? module->name : "?");
    return true;
}

static bool loadLibvulkan(const char* path) {
    void* libvulkan = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!libvulkan) {
        fprintf(stderr, "vortek-server: cannot load %s: %s\n", path, dlerror());
        return false;
    }
    initVulkanWrapper(&vulkanWrapper, libvulkan);
    fprintf(stderr, "vortek-server: loaded loader library %s\n", path);
    return vulkanWrapper.vkCreateInstance != NULL;
}

/* A HAL driver has no exported vkGetDeviceProcAddr; fetch it through a
 * short-lived instance (the driver's GDPA is instance independent). */
static bool resolveGlobalFuncs(void) {
    if (!vulkanWrapper.vkEnumerateInstanceVersion)
        vulkanWrapper.vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)vulkanWrapper.vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    if (vulkanWrapper.vkGetDeviceProcAddr) return true;

    VkApplicationInfo appInfo = {0};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "vortek-server";
    appInfo.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo createInfo = {0};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vulkanWrapper.vkCreateInstance(&createInfo, NULL, &instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vortek-server: probe vkCreateInstance failed: %d\n", result);
        return false;
    }

    vulkanWrapper.vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)vulkanWrapper.vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr");

    uint32_t count = 0;
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)vulkanWrapper.vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices");
    if (enumerate) enumerate(instance, &count, NULL);
    fprintf(stderr, "vortek-server: driver reports %u physical device(s)\n", count);

    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)vulkanWrapper.vkGetInstanceProcAddr(instance, "vkDestroyInstance");
    if (destroy) destroy(instance, NULL);
    return vulkanWrapper.vkGetDeviceProcAddr != NULL && count > 0;
}

/* ---- options ---- */

static void loadOptions(void) {
    options.vkMaxVersion = VK_MAKE_VERSION(1, 3, 128);
    options.imageCacheSize = 256;

    const char* value;
    if ((value = getenv("VORTEK_VK_MAX_VERSION")) && *value) {
        int major = 1, minor = 3;
        sscanf(value, "%d.%d", &major, &minor);
        options.vkMaxVersion = VK_MAKE_VERSION(major, minor, 128);
    }
    if ((value = getenv("VORTEK_MAX_DEVICE_MEMORY")) && *value) options.maxDeviceMemory = (short)atoi(value);
    if ((value = getenv("VORTEK_IMAGE_CACHE_SIZE")) && *value) options.imageCacheSize = (short)atoi(value);
    if ((value = getenv("VORTEK_RESOURCE_MEMORY_TYPE")) && *value) options.resourceMemoryType = (uint8_t)atoi(value);
    if ((value = getenv("VORTEK_EXPOSED_DEVICE_EXTENSIONS")) && *value && strcmp(value, "all") != 0) {
        options.exposedDeviceExtensions = calloc(1, sizeof(ArrayList));
        char* list = strdup(value);
        char* save = NULL;
        for (char* name = strtok_r(list, "|", &save); name; name = strtok_r(NULL, "|", &save))
            ArrayList_add(options.exposedDeviceExtensions, strdup(name));
        free(list);
    }
}

/* ---- connection handling (replaces VortekRendererComponent.java) ---- */

static bool readFully(int fd, void* buffer, int size) {
    char* data = buffer;
    while (size > 0) {
        ssize_t n = read(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        size -= n;
    }
    return true;
}

static void* connectionThread(void* param) {
    int clientFd = (int)(intptr_t)param;
    VkContext* context = NULL;

    for (;;) {
        int header[2];
        if (!readFully(clientFd, header, sizeof(header))) break;
        int requestCode = header[0];
        int requestLength = header[1];

        if (requestCode == REQUEST_CODE_CREATE_CONTEXT) {
            if (context) continue;
            context = createVkContext(clientFd, &options);
            if (!context) {
                fprintf(stderr, "vortek-server: createVkContext failed (fd=%d)\n", clientFd);
                break;
            }
            fprintf(stderr, "vortek-server: context created (fd=%d)\n", clientFd);
        }
        else if (requestCode > INT16_MAX && (requestCode >> 16) == REQUEST_CODE_SEND_EXTRA_DATA) {
            if (!context || !handleExtraDataRequest(context, requestCode & 0xffff, requestLength)) {
                fprintf(stderr, "vortek-server: extra data request failed (fd=%d)\n", clientFd);
                break;
            }
        }
        else {
            fprintf(stderr, "vortek-server: unknown request %d (fd=%d)\n", requestCode, clientFd);
            break;
        }
    }

    if (context) destroyVkContext(context);
    close(clientFd);
    fprintf(stderr, "vortek-server: connection closed (fd=%d)\n", clientFd);
    return NULL;
}

static int mkdirs(const char* path) {
    char buffer[PATH_MAX];
    snprintf(buffer, sizeof(buffer), "%s", path);
    for (char* p = buffer + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(buffer, 0755) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(buffer, 0755) < 0 && errno != EEXIST) return -1;
    return 0;
}

static void onSignal(int sig) {
    if (socketPath[0]) unlink(socketPath);
    _exit(128 + sig);
}

static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [socket-path]\n"
            "  socket path: argv[1], else $VORTEK_SERVER_PATH, else " DEFAULT_SOCKET_PATH "\n"
            "  VORTEK_HAL_DRIVER  vendor HAL module (default " DEFAULT_HAL_DRIVER ")\n"
            "  VORTEK_LIBVULKAN   use this Vulkan loader library instead of the HAL module\n"
            "  VORTEK_VK_MAX_VERSION, VORTEK_MAX_DEVICE_MEMORY (MiB), VORTEK_IMAGE_CACHE_SIZE,\n"
            "  VORTEK_RESOURCE_MEMORY_TYPE, VORTEK_EXPOSED_DEVICE_EXTENSIONS (a|b|c)\n"
            "  VORTEK_HEADLESS_SIZE (WxH), VORTEK_DUMP_PPM (path of last presented frame)\n",
            argv0);
}

int main(int argc, char** argv) {
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        usage(argv[0]);
        return 0;
    }

    const char* path = argc > 1 ? argv[1] : getenv("VORTEK_SERVER_PATH");
    if (!path || !*path) path = DEFAULT_SOCKET_PATH;
    if (strlen(path) >= sizeof(socketPath)) {
        fprintf(stderr, "vortek-server: socket path too long\n");
        return 1;
    }

    setvbuf(stderr, NULL, _IOLBF, 0);
    loadOptions();

    const char* libvulkan = getenv("VORTEK_LIBVULKAN");
    const char* halDriver = getenv("VORTEK_HAL_DRIVER");
    bool loaded = libvulkan && *libvulkan ? loadLibvulkan(libvulkan) : loadHalDriver(halDriver && *halDriver ? halDriver : DEFAULT_HAL_DRIVER);
    if (!loaded || !resolveGlobalFuncs()) {
        fprintf(stderr, "vortek-server: no usable Vulkan driver\n");
        return 1;
    }

    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char* slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        if (mkdirs(dir) < 0) {
            fprintf(stderr, "vortek-server: mkdir %s: %s\n", dir, strerror(errno));
            return 1;
        }
    }

    listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    unlink(path);
    if (bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(listenFd, 16) < 0) {
        fprintf(stderr, "vortek-server: bind/listen %s: %s\n", path, strerror(errno));
        return 1;
    }
    chmod(path, 0777);
    snprintf(socketPath, sizeof(socketPath), "%s", path);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    fprintf(stderr, "vortek-server: listening on %s\n", path);

    for (;;) {
        int clientFd = accept4(listenFd, NULL, NULL, SOCK_CLOEXEC);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "vortek-server: accept: %s\n", strerror(errno));
            break;
        }

        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thread, &attr, connectionThread, (void*)(intptr_t)clientFd) != 0) close(clientFd);
        pthread_attr_destroy(&attr);
    }

    unlink(socketPath);
    return 1;
}
