/* [mali-g2] Regression test for the server's shared function table: create a device with
 * VK_KHR_push_descriptor, wait while another client (zink, which leaves the extension off)
 * creates its device, then push descriptors and dispatch. Before winlator-0005 the server
 * crashed calling a NULL vkCmdPushDescriptorSetKHR. Usage: push_after_zink_test <wait-seconds> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "m56_spv.h"
#define N 256
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d\n", #x, r_); return 1; } } while (0)
int main(int argc, char** argv) {
    int wait = argc > 1 ? atoi(argv[1]) : 5;
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "push", 1, "push", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai};
    VkInstance inst; CK(vkCreateInstance(&ii, 0, &inst));
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd);
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio};
    const char* exts[] = {"VK_KHR_push_descriptor"};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qi, 0, 0, 1, exts};
    VkDevice dev; CK(vkCreateDevice(pd, &di, 0, &dev));
    PFN_vkCmdPushDescriptorSetKHR push = (void*)vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR");
    if (!push) { printf("FAIL no vkCmdPushDescriptorSetKHR\n"); return 1; }
    printf("device ready, waiting %d s for the other client\n", wait); fflush(stdout);
    sleep(wait);
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buf; CK(vkCreateBuffer(dev, &bi, 0, &buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = 0; for (; mt < mp.memoryTypeCount; mt++) if ((mr.memoryTypeBits >> mt & 1) && (mp.memoryTypes[mt].propertyFlags & 6) == 6) break;
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mt};
    VkDeviceMemory mem; CK(vkAllocateMemory(dev, &ma, 0, &mem)); CK(vkBindBufferMemory(dev, buf, mem, 0));
    uint32_t* map; CK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void**)&map)); memset(map, 0, N * 4);
    VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, 1, &lb};
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &li, 0, &dsl));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl, 1, &pcr};
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, 0, &pl));
    VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(m56_spv), m56_spv};
    VkShaderModule sm; CK(vkCreateShaderModule(dev, &si, 0, &sm));
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main"}, pl};
    VkPipeline pipe; CK(vkCreateComputePipelines(dev, 0, 1, &ci, 0, &pipe));
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpi, 0, &cp));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CK(vkBeginCommandBuffer(cb, &cbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    VkDescriptorBufferInfo dbi = {buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, 0, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &dbi};
    push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &w);
    uint32_t add = 7; vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &add);
    vkCmdDispatch(cb, N / 64, 1, 1);
    CK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; CK(vkCreateFence(dev, &fci, 0, &fence));
    VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb};
    CK(vkQueueSubmit(q, 1, &sub, fence)); CK(vkWaitForFences(dev, 1, &fence, 1, 5000000000ull));
    int bad = 0; for (uint32_t i = 0; i < N; i++) if (map[i] != i * 3 + add) bad++;
    printf("%s (%d/%d wrong)\n", bad ? "PUSH-FAIL" : "PUSH-PASS", bad, N);
    return bad != 0;
}
