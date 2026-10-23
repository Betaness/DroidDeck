/* [mali-g2] vkGetSemaphoreCounterValue must return the counter (upstream Vortek replied with the
 * semaphore handle), and a count-only vkEnumerateInstanceExtensionProperties must not corrupt the
 * server. Prints TIMELINE-PASS. */
#include <stdio.h>
#include <vulkan/vulkan.h>
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d\n", #x, r_); return 1; } } while (0)
int main(void) {
    uint32_t n = 0;
    for (int i = 0; i < 20; i++) CK(vkEnumerateInstanceExtensionProperties(NULL, &n, NULL));
    VkExtensionProperties props[64]; uint32_t m = n < 64 ? n : 64;
    VkResult er = vkEnumerateInstanceExtensionProperties(NULL, &m, props);
    printf("instance extensions: %u (%s, first %s)\n", n, er == VK_SUCCESS ? "ok" : "incomplete", m ? props[0].extensionName : "-");
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "tl", 1, "tl", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai};
    VkInstance inst; CK(vkCreateInstance(&ii, 0, &inst));
    uint32_t c = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &c, &pd);
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES}; f12.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &qi};
    VkDevice dev; CK(vkCreateDevice(pd, &di, 0, &dev));
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, 0, VK_SEMAPHORE_TYPE_TIMELINE, 5};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    VkSemaphore sem; CK(vkCreateSemaphore(dev, &si, 0, &sem));
    uint64_t v0 = 0, v1 = 0;
    CK(vkGetSemaphoreCounterValue(dev, sem, &v0));
    VkSemaphoreSignalInfo sg = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, 0, sem, 9};
    CK(vkSignalSemaphore(dev, &sg));
    CK(vkGetSemaphoreCounterValue(dev, sem, &v1));
    printf("counter: initial %llu (want 5), after signal %llu (want 9)\n", (unsigned long long)v0, (unsigned long long)v1);
    vkDestroySemaphore(dev, sem, 0); vkDestroyDevice(dev, 0); vkDestroyInstance(inst, 0);
    int ok = v0 == 5 && v1 == 9 && n > 0;
    printf("%s\n", ok ? "TIMELINE-PASS" : "TIMELINE-FAIL");
    return !ok;
}
