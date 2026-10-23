#ifndef VK_PRESENT_DEV_H
#define VK_PRESENT_DEV_H
#include "vk_loader.h"
#include "vk_present.h"

/* The compositor's Vulkan device, for modules that record their own work on it (event-loop thread). */
enum {
    VKP_CAP_AHB = 1 << 0,          /* VK_ANDROID_external_memory_android_hardware_buffer */
    VKP_CAP_SEMAPHORE_FD = 1 << 1, /* VK_KHR_external_semaphore_fd */
    VKP_CAP_FENCE_FD = 1 << 2,     /* VK_KHR_external_fence_fd */
    VKP_CAP_CALIBRATED = 1 << 3,   /* VK_EXT_calibrated_timestamps */
    VKP_CAP_SIDE_QUEUE = 1 << 4,   /* side_queue: a second queue of qfam */
    VKP_CAP_TIMESTAMPS = 1 << 5,   /* qfam writes timestamps */
};

struct vkp_device_info {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    uint32_t qfam;
    VkQueue queue, side_queue;
    unsigned caps;
    uint32_t ts_bits;
    float ts_period;
    const VkPhysicalDeviceMemoryProperties *memprops;
};
/* 0 while the device is up. */
int vkp_device_info(struct vkp_device_info *out);
void vkp_note_device_lost(const char *where);

VkImage vkp_image_handle(const struct vkp_image *img);
VkFormat vkp_image_format(const struct vkp_image *img);
int vkp_image_renderable(const struct vkp_image *img);
/* Created on first use and destroyed with the image. */
VkImageView vkp_image_view(struct vkp_image *img);
VkFramebuffer vkp_image_framebuffer(struct vkp_image *img, VkRenderPass rp);
/* Called first thing in vkp_image_destroy, so work recorded elsewhere can finish with the image. */
void vkp_set_image_destroy_hook(void (*hook)(struct vkp_image *img));

#endif
