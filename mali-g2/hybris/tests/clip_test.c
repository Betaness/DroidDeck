/*
 * Builds graphics pipelines whose vertex shader writes ClipDistance / CullDistance (clip.vert), once
 * from a VkShaderModule and once with the code inline in the stage (VK_KHR_maintenance5), through
 * the Vulkan loader (run it with tests/run.sh). Without the ICD's rewrite the Mali shader compiler
 * crashes the process (VORTEK_KEEP_CLIP_DISTANCE=1 shows it). Prints CLIP-PASS / CLIP-FAIL.
 * Build: glslangValidator -V --vn clip_vert -o clip_vert.h clip.vert, then cc clip_test.c -lvulkan.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include "clip_vert.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("CLIP-FAIL %s -> %d\n", #x, r_); return 1; } } while (0)

int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "clip_test", 1, "test", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance instance;
    CHECK(vkCreateInstance(&ii, NULL, &instance));
    uint32_t n = 1;
    VkPhysicalDevice physical;
    CHECK(vkEnumeratePhysicalDevices(instance, &n, &physical));
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &priority};
    VkPhysicalDeviceMaintenance5FeaturesKHR m5 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, NULL, VK_TRUE};
    VkPhysicalDeviceVulkan13Features v13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &m5};
    v13.dynamicRendering = VK_TRUE;
    const char* extensions[] = {VK_KHR_MAINTENANCE_5_EXTENSION_NAME};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &v13, 0, 1, &qi, 0, NULL, 1, extensions};
    VkDevice device;
    CHECK(vkCreateDevice(physical, &di, NULL, &device));

    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &li, NULL, &layout));
    VkShaderModuleCreateInfo code = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(clip_vert), clip_vert};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(device, &code, NULL, &module));

    VkVertexInputBindingDescription binding = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vertexInput = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &binding, 1, &attribute};
    VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                       VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.rasterizerDiscardEnable = VK_TRUE;
    raster.lineWidth = 1;
    VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dynamic};
    VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};

    VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, module, "main"};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    pi.stageCount = 1;
    pi.pStages = &stage;
    pi.pVertexInputState = &vertexInput;
    pi.pInputAssemblyState = &assembly;
    pi.pViewportState = &viewport;
    pi.pRasterizationState = &raster;
    pi.pDynamicState = &dynamicState;
    pi.layout = layout;
    VkPipeline pipelines[2];
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &pipelines[0]));
    printf("pipeline from a shader module: created\n");

    VkShaderModuleCreateInfo inlineCode = code;
    stage.pNext = &inlineCode;
    stage.module = VK_NULL_HANDLE;
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &pipelines[1]));
    printf("pipeline with inline code: created\n");

    vkDestroyPipeline(device, pipelines[0], NULL);
    vkDestroyPipeline(device, pipelines[1], NULL);
    vkDestroyShaderModule(device, module, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("CLIP-PASS\n");
    return 0;
}
