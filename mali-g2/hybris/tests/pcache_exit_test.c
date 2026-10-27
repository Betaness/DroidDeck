/*
 * exit() while a thread of the game's process is inside a pipeline cache parse that never finishes
 * (pcache_test.sh step 12). Wine ends a thread (NtTerminateThread, and NtTerminateProcess for the
 * other threads) with a signal that unwinds it to its start and pthread_exit()s it, wherever it is:
 * a thread killed while it held the cache's parse lock never lets go. Here a worker creates a compute
 * pipeline without a cache of its own, so the ICD loads the implicit cache, and waits there for a
 * <file>.loading marker another process holds (pcache_test.sh holds it); the main thread kills the
 * worker that way and calls exit(), which must return within a few seconds (it hung until killed).
 *   PE_FIRST=1  makes the cache file (one pipeline, the device destroyed) and exits
 * Prints PE-EXIT just before exit(0); the caller times the process.
 * Build: glslangValidator -V --vn pcache_exit_comp -o out/tests/pcache_exit_comp.h tests/pcache_exit.comp
 *        cc -Iout/tests -o out/tests/pcache_exit_test tests/pcache_exit_test.c -lvulkan -lpthread
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pcache_exit_comp.h"

static VkDevice device;
static VkPipelineLayout layout;
static VkShaderModule module;
static __thread sigjmp_buf threadStart;
static volatile int inCall;

static void createPipeline(void) {
    VkComputePipelineCreateInfo info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                                                   module, "main"};
    info.layout = layout;
    VkPipeline pipeline;
    inCall = 1;
    VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline);
    inCall = 0;
    printf("PE pipeline %d\n", result);
    fflush(stdout);
}

static void onQuit(int signal) {
    (void)signal;
    siglongjmp(threadStart, 1);
}

static void* worker(void* unused) {
    (void)unused;
    if (sigsetjmp(threadStart, 1)) {
        printf("PE worker killed inside vkCreateComputePipelines\n");
        fflush(stdout);
        pthread_exit(NULL);
    }
    createPipeline();
    return NULL;
}

int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "pcache_exit.exe", 1, "test", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance instance;
    if (vkCreateInstance(&instanceInfo, NULL, &instance) != VK_SUCCESS) return printf("PE-FAIL vkCreateInstance\n"), 1;
    uint32_t count = 1;
    VkPhysicalDevice physical;
    vkEnumeratePhysicalDevices(instance, &count, &physical);
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &priority};
    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &queue};
    if (vkCreateDevice(physical, &deviceInfo, NULL, &device) != VK_SUCCESS) return printf("PE-FAIL vkCreateDevice\n"), 1;
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo setInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &binding};
    VkDescriptorSetLayout setLayout;
    vkCreateDescriptorSetLayout(device, &setInfo, NULL, &setLayout);
    VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &setLayout};
    vkCreatePipelineLayout(device, &layoutInfo, NULL, &layout);
    VkShaderModuleCreateInfo moduleInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(pcache_exit_comp), pcache_exit_comp};
    vkCreateShaderModule(device, &moduleInfo, NULL, &module);
    if (getenv("PE_FIRST")) {
        createPipeline();
        vkDestroyDevice(device, NULL);
        printf("PE first run done\n");
        return 0;
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = onQuit;
    sigaction(SIGQUIT, &action, NULL);
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    sleep(2);
    printf("PE worker in the call: %d; killing it, then exit()\n", inCall);
    fflush(stdout);
    pthread_kill(thread, SIGQUIT);
    pthread_join(thread, NULL);
    printf("PE-EXIT\n");
    fflush(stdout);
    exit(0);
}
