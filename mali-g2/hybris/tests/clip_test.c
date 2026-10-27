/*
 * Builds graphics pipelines whose vertex shader writes ClipDistance / CullDistance (clip.vert), once
 * from a VkShaderModule and once with the code inline in the stage (VK_KHR_maintenance5), through
 * the Vulkan loader (run it with tests/run.sh). Without the ICD's rewrite the Mali shader compiler
 * crashes the process (VORTEK_KEEP_CLIP_DISTANCE=1 shows it). With VK_EXT_pipeline_robustness the
 * inline code also comes behind a VkPipelineRobustnessCreateInfoEXT in the stage's chain (valid in
 * any order; a rewrite that only looked at the first structure crashed there), and with
 * VK_EXT_shader_module_identifier the identifier of the create info must be the module's (the ICD
 * hashes the rewritten code for both). Prints CLIP-PASS / CLIP-FAIL.
 * Build: glslangValidator -V --vn clip_vert -o clip_vert.h clip.vert, then cc clip_test.c -lvulkan.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physical, NULL, &extCount, NULL);
    VkExtensionProperties* available = calloc(extCount, sizeof(*available));
    vkEnumerateDeviceExtensionProperties(physical, NULL, &extCount, available);
    int robustness = 0, identifier = 0;
    for (uint32_t i = 0; i < extCount; i++) {
        robustness |= !strcmp(available[i].extensionName, "VK_EXT_pipeline_robustness");
        identifier |= !strcmp(available[i].extensionName, "VK_EXT_shader_module_identifier");
    }
    free(available);
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &priority};
    VkPhysicalDeviceMaintenance5FeaturesKHR m5 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, NULL, VK_TRUE};
    VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT idf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT, &m5,
                                                             VK_TRUE};
    VkPhysicalDeviceVulkan13Features v13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, identifier ? (void*)&idf : (void*)&m5};
    v13.dynamicRendering = VK_TRUE;
    const char* extensions[3] = {VK_KHR_MAINTENANCE_5_EXTENSION_NAME};
    uint32_t enabled = 1;
    if (robustness) extensions[enabled++] = "VK_EXT_pipeline_robustness";
    if (identifier) extensions[enabled++] = "VK_EXT_shader_module_identifier";
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &v13, 0, 1, &qi, 0, NULL, enabled, extensions};
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
    VkPipeline pipelines[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &pipelines[0]));
    printf("pipeline from a shader module: created\n");

    VkShaderModuleCreateInfo inlineCode = code;
    stage.pNext = &inlineCode;
    stage.module = VK_NULL_HANDLE;
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &pipelines[1]));
    printf("pipeline with inline code: created\n");

    if (robustness) {
        VkPipelineRobustnessCreateInfoEXT robust = {VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT, &inlineCode};
        stage.pNext = &robust;
        CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &pipelines[2]));
        printf("pipeline with inline code second in the stage's chain: created\n");
    } else printf("no VK_EXT_pipeline_robustness: inline code second in the chain not tried\n");

    if (identifier) {
        PFN_vkGetShaderModuleIdentifierEXT moduleId = (PFN_vkGetShaderModuleIdentifierEXT)vkGetDeviceProcAddr(device, "vkGetShaderModuleIdentifierEXT");
        PFN_vkGetShaderModuleCreateInfoIdentifierEXT infoId =
            (PFN_vkGetShaderModuleCreateInfoIdentifierEXT)vkGetDeviceProcAddr(device, "vkGetShaderModuleCreateInfoIdentifierEXT");
        VkShaderModuleIdentifierEXT a = {VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT}, b = {VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT};
        moduleId(device, module, &a);
        infoId(device, &code, &b);
        if (a.identifierSize == 0 || a.identifierSize != b.identifierSize || memcmp(a.identifier, b.identifier, a.identifierSize)) {
            printf("CLIP-FAIL the create info's identifier (%u bytes) is not the module's (%u bytes)\n", b.identifierSize, a.identifierSize);
            return 1;
        }
        printf("shader module identifier of the create info is the module's (%u bytes)\n", a.identifierSize);
    } else printf("no VK_EXT_shader_module_identifier: identifiers not compared\n");

    for (int i = 0; i < 3; i++) vkDestroyPipeline(device, pipelines[i], NULL);
    vkDestroyShaderModule(device, module, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("CLIP-PASS\n");
    return 0;
}
