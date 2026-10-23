/* [mali-g2] Exercise the Vortek client's maintenance5/6 emulation the way DXVK 3.x does:
 * enable both features, create a buffer with VkBufferUsageFlags2CreateInfoKHR and a compute
 * pipeline with VkPipelineCreateFlags2CreateInfoKHR, bind with vkCmdBindDescriptorSets2KHR,
 * push with vkCmdPushConstants2KHR, dispatch, check the result. Prints MAINT56-PASS. */
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "m56_spv.h"
#define N 256
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); return 1; } } while (0)
int main(void) {
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "m56", 1, "m56", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai};
    VkInstance inst; CK(vkCreateInstance(&ii, 0, &inst));
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceMaintenance6FeaturesKHR f6 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
    VkPhysicalDeviceMaintenance5FeaturesKHR f5 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, &f6};
    VkPhysicalDeviceFeatures2 ff = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f5};
    vkGetPhysicalDeviceFeatures2(pd, &ff);
    printf("features: maintenance5=%u maintenance6=%u\n", f5.maintenance5, f6.maintenance6);
    if (!f5.maintenance5 || !f6.maintenance6) { printf("FAIL features\n"); return 1; }
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio};
    const char* exts[] = {"VK_KHR_maintenance5", "VK_KHR_maintenance6"};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f5, 0, 1, &qi, 0, 0, 2, exts};
    VkDevice dev; CK(vkCreateDevice(pd, &di, 0, &dev));
    PFN_vkCmdBindDescriptorSets2KHR bind2 = (void*)vkGetDeviceProcAddr(dev, "vkCmdBindDescriptorSets2KHR");
    PFN_vkCmdPushConstants2KHR push2 = (void*)vkGetDeviceProcAddr(dev, "vkCmdPushConstants2KHR");
    PFN_vkCmdBindIndexBuffer2KHR bindIndex2 = (void*)vkGetDeviceProcAddr(dev, "vkCmdBindIndexBuffer2KHR");
    PFN_vkGetDeviceImageSubresourceLayoutKHR devLayout = (void*)vkGetDeviceProcAddr(dev, "vkGetDeviceImageSubresourceLayoutKHR");
    if (!bind2 || !push2 || !bindIndex2 || !devLayout) { printf("FAIL entry points missing\n"); return 1; }
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

    VkBufferUsageFlags2CreateInfoKHR u2 = {VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR, 0,
        VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR | VK_BUFFER_USAGE_2_INDEX_BUFFER_BIT_KHR};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &u2, 0, N * 4, 0 /* usage comes from u2 */};
    VkBuffer buf; CK(vkCreateBuffer(dev, &bi, 0, &buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = 0; for (; mt < mp.memoryTypeCount; mt++) if ((mr.memoryTypeBits >> mt & 1) && (mp.memoryTypes[mt].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mt};
    VkDeviceMemory mem; CK(vkAllocateMemory(dev, &ma, 0, &mem)); CK(vkBindBufferMemory(dev, buf, mem, 0));
    uint32_t* map; CK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void**)&map)); memset(map, 0, N * 4);

    VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &lb};
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &li, 0, &dsl));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl, 1, &pcr};
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, 0, &pl));
    VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(m56_spv), m56_spv};
    VkShaderModule sm; CK(vkCreateShaderModule(dev, &si, 0, &sm));
    VkPipelineCreateFlags2CreateInfoKHR pf2 = {VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, 0, 0};
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, &pf2, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main"}, pl};
    VkPipeline pipe; CK(vkCreateComputePipelines(dev, 0, 1, &ci, 0, &pipe));

    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 1, 1, &ps};
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, 0, &dp));
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dp, 1, &dsl};
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dai, &ds));
    VkDescriptorBufferInfo dbi = {buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &dbi};
    vkUpdateDescriptorSets(dev, 1, &w, 0, 0);

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, 0};
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpi, 0, &cp));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CK(vkBeginCommandBuffer(cb, &cbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    VkBindDescriptorSetsInfoKHR bdi = {VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO_KHR, 0, VK_SHADER_STAGE_COMPUTE_BIT, pl, 0, 1, &ds, 0, 0};
    bind2(cb, &bdi);
    uint32_t add = 1000;
    VkPushConstantsInfoKHR pci = {VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO_KHR, 0, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &add};
    push2(cb, &pci);
    bindIndex2(cb, buf, 0, VK_WHOLE_SIZE, VK_INDEX_TYPE_UINT32); /* recorded only; checks the call path */
    vkCmdDispatch(cb, N / 64, 1, 1);
    CK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; CK(vkCreateFence(dev, &fci, 0, &fence));
    VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb};
    CK(vkQueueSubmit(q, 1, &sub, fence)); CK(vkWaitForFences(dev, 1, &fence, 1, 5000000000ull));

    int bad = 0; for (uint32_t i = 0; i < N; i++) if (map[i] != i * 3 + add) bad++;
    printf("compute: %d/%d wrong (o[5]=%u, want %u)\n", bad, N, map[5], 5 * 3 + add);

    VkImageCreateInfo imi = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {64, 64, 1}, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImageSubresource2KHR isr = {VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2_KHR, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}};
    VkDeviceImageSubresourceInfoKHR dsi = {VK_STRUCTURE_TYPE_DEVICE_IMAGE_SUBRESOURCE_INFO_KHR, 0, &imi, &isr};
    VkSubresourceLayout2KHR sl = {VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2_KHR};
    devLayout(dev, &dsi, &sl);
    printf("linear 64x64 RGBA8 rowPitch=%llu size=%llu\n", (unsigned long long)sl.subresourceLayout.rowPitch, (unsigned long long)sl.subresourceLayout.size);

    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, 0); vkDestroyInstance(inst, 0);
    if (bad || sl.subresourceLayout.rowPitch < 256) { printf("MAINT56-FAIL\n"); return 1; }
    printf("MAINT56-PASS\n");
    return 0;
}
