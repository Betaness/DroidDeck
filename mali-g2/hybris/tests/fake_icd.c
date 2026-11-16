/*
 * A Vulkan driver of one device for tests/guards_icd_test.c, tests/event_sleep_test.c and
 * tests/load_skip_test.c (see tests/fake_icd.h): one queue family of FAKE_QUEUES queues, a
 * device-local heap of FAKE_HEAP_MB and a host heap, no extensions. Waits, idle waits, query reads
 * and event status queries play their FakeScript; vkEndCommandBuffer and pipeline creation fault
 * on demand. Objects are distinct handles; memory maps to host memory of its size; recorded
 * commands do nothing but leave a trace of passes.
 */
#include "fake_icd.h"

#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

EXPORT FakeIcd fake_icd;

typedef struct {
    uintptr_t loaderMagic;
} Object;

static Object instanceObject, physicalObject, deviceObject, queueObjects[FAKE_QUEUES];

static VkResult play(FakeScript* s) {
    s->calls++;
    return s->next < s->count ? s->results[s->next++] : s->otherwise;
}

static VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* out) {
    *out = (VkInstance)&instanceObject;
    return VK_SUCCESS;
}

static void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
}

static VkResult VKAPI_CALL EnumerateExtensions(const char* layer, uint32_t* count, VkExtensionProperties* properties) {
    *count = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL EnumerateDeviceExtensions(VkPhysicalDevice physical, const char* layer, uint32_t* count,
                                                     VkExtensionProperties* properties) {
    *count = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance instance, uint32_t* count, VkPhysicalDevice* physicals) {
    if (physicals && *count) physicals[0] = (VkPhysicalDevice)&physicalObject;
    *count = 1;
    return VK_SUCCESS;
}

static void VKAPI_CALL GetPhysicalDeviceProperties(VkPhysicalDevice physical, VkPhysicalDeviceProperties* properties) {
    memset(properties, 0, sizeof(*properties));
    properties->apiVersion = VK_API_VERSION_1_3;
    properties->vendorID = 0x13b5;
    properties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    strcpy(properties->deviceName, "fake Mali");
}

static void VKAPI_CALL GetPhysicalDeviceFeatures(VkPhysicalDevice physical, VkPhysicalDeviceFeatures* features) {
    memset(features, 0, sizeof(*features));
}

static void VKAPI_CALL GetQueueFamilies(VkPhysicalDevice physical, uint32_t* count, VkQueueFamilyProperties* families) {
    if (families && *count) {
        memset(families, 0, sizeof(*families));
        families[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        families[0].queueCount = FAKE_QUEUES;
    }
    *count = 1;
}

static void VKAPI_CALL GetMemoryProperties(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties* memory) {
    memset(memory, 0, sizeof(*memory));
    memory->memoryHeapCount = 2;
    memory->memoryHeaps[0].size = FAKE_HEAP_MB << 20;
    memory->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    memory->memoryHeaps[1].size = 2048ull << 20;
    memory->memoryTypeCount = 2;
    memory->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    memory->memoryTypes[1].heapIndex = 1;
    memory->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
}

static void VKAPI_CALL GetMemoryProperties2(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties2* memory) {
    GetMemoryProperties(physical, &memory->memoryProperties);
    for (VkBaseOutStructure* s = memory->pNext; s; s = s->pNext) {
        if (s->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) continue;
        VkPhysicalDeviceMemoryBudgetPropertiesEXT* budget = (VkPhysicalDeviceMemoryBudgetPropertiesEXT*)s;
        budget->heapBudget[0] = 9000ull << 20;
        budget->heapUsage[0] = 100ull << 20;
        budget->heapBudget[1] = 1500ull << 20;
        fake_icd.budgets++;
    }
}

static VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator,
                                        VkDevice* out) {
    *out = (VkDevice)&deviceObject;
    return VK_SUCCESS;
}

static void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
}

static void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    *queue = family == 0 && index < FAKE_QUEUES ? (VkQueue)&queueObjects[index] : VK_NULL_HANDLE;
}

static VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    fake_icd.submits++;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence) {
    fake_icd.submits2++;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL QueueWaitIdle(VkQueue queue) {
    fake_icd.lastIdleQueue = queue;
    return play(&fake_icd.queueIdle);
}

static VkResult VKAPI_CALL DeviceWaitIdle(VkDevice device) {
    return play(&fake_icd.deviceIdle);
}

static VkResult VKAPI_CALL WaitForFences(VkDevice device, uint32_t count, const VkFence* fences, VkBool32 all, uint64_t timeout) {
    return play(&fake_icd.fences);
}

static VkResult VKAPI_CALL WaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* info, uint64_t timeout) {
    return play(&fake_icd.semaphores);
}

static VkResult VKAPI_CALL CreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo* info, const VkAllocationCallbacks* allocator,
                                           VkQueryPool* pool) {
    *pool = (VkQueryPool)(uintptr_t)0x5000;
    return VK_SUCCESS;
}

static void VKAPI_CALL DestroyQueryPool(VkDevice device, VkQueryPool pool, const VkAllocationCallbacks* allocator) {
}

static void VKAPI_CALL ResetQueryPool(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count) {
}

static void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer commandBuffer, VkQueryPool pool, uint32_t first, uint32_t count) {
}

static VkResult VKAPI_CALL GetQueryPoolResults(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count, size_t size, void* data,
                                               VkDeviceSize stride, VkQueryResultFlags flags) {
    VkResult r = play(&fake_icd.queries);
    if (r == VK_SUCCESS && data && size >= 4) memset(data, 0, size);
    return r;
}

static VkResult VKAPI_CALL GetEventStatus(VkDevice device, VkEvent event) {
    fake_icd.lastEvent = event;
    return fake_icd.lastEventResult = play(&fake_icd.events);
}

static VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* info) {
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer commandBuffer) {
    fake_icd.ends++;
    if (fake_icd.endFault) return *(volatile int*)fake_icd.endFault;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags) {
    return VK_SUCCESS;
}

static VkResult createPipelines(uint32_t count, VkPipeline* pipelines) {
    fake_icd.pipelines++;
    if (fake_icd.pipelineFault) return *(volatile int*)fake_icd.pipelineFault;
    static uintptr_t next = 0x7000;
    for (uint32_t i = 0; i < count; i++) pipelines[i] = (VkPipeline)next++;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkComputePipelineCreateInfo* infos,
                                                  const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
    return createPipelines(count, pipelines);
}

static VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                   const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
                                                   VkPipeline* pipelines) {
    return createPipelines(count, pipelines);
}

static void VKAPI_CALL DestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator) {
}

static uint64_t nextHandle = 0x100000;
#define NEW(type) ((type)(uintptr_t)(nextHandle += 0x10))

#define MEMORIES 256
static struct {
    VkDeviceMemory memory;
    void* data;
} memories[MEMORIES];

static Object commandBufferObjects[1024];
static int commandBufferCount;

static void trace(char c) {
    if (fake_icd.traceLen < (int)sizeof fake_icd.trace - 1) fake_icd.trace[fake_icd.traceLen++] = c;
    fake_icd.trace[fake_icd.traceLen] = 0;
}

static VkResult VKAPI_CALL CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo* i, const VkAllocationCallbacks* a, VkShaderModule* m) {
    *m = NEW(VkShaderModule);
    return VK_SUCCESS;
}
static void VKAPI_CALL DestroyShaderModule(VkDevice d, VkShaderModule m, const VkAllocationCallbacks* a) {
}
static VkResult VKAPI_CALL CreateImage(VkDevice d, const VkImageCreateInfo* i, const VkAllocationCallbacks* a, VkImage* image) {
    *image = NEW(VkImage);
    return VK_SUCCESS;
}
static void VKAPI_CALL DestroyImage(VkDevice d, VkImage i, const VkAllocationCallbacks* a) {
}
static VkResult VKAPI_CALL CreateImageView(VkDevice d, const VkImageViewCreateInfo* i, const VkAllocationCallbacks* a, VkImageView* v) {
    *v = NEW(VkImageView);
    return VK_SUCCESS;
}
static void VKAPI_CALL DestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks* a) {
}
static VkResult VKAPI_CALL CreateBuffer(VkDevice d, const VkBufferCreateInfo* i, const VkAllocationCallbacks* a, VkBuffer* b) {
    *b = NEW(VkBuffer);
    return VK_SUCCESS;
}
static void VKAPI_CALL DestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks* a) {
}
static VkResult VKAPI_CALL BindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) {
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL AllocateMemory(VkDevice d, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* a, VkDeviceMemory* m) {
    for (int i = 0; i < MEMORIES; i++) {
        if (memories[i].memory) continue;
        memories[i].data = calloc(1, info->allocationSize ? info->allocationSize : 1);
        if (!memories[i].data) return VK_ERROR_OUT_OF_HOST_MEMORY;
        memories[i].memory = *m = NEW(VkDeviceMemory);
        return VK_SUCCESS;
    }
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
static void VKAPI_CALL FreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks* a) {
    for (int i = 0; i < MEMORIES; i++)
        if (m && memories[i].memory == m) {
            free(memories[i].data);
            memories[i].memory = VK_NULL_HANDLE;
        }
}
static VkResult VKAPI_CALL MapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize s, VkMemoryMapFlags f, void** data) {
    for (int i = 0; i < MEMORIES; i++)
        if (memories[i].memory == m) {
            *data = (char*)memories[i].data + o;
            return VK_SUCCESS;
        }
    return VK_ERROR_MEMORY_MAP_FAILED;
}
static void VKAPI_CALL UnmapMemory(VkDevice d, VkDeviceMemory m) {
}
static VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out) {
    for (uint32_t i = 0; i < info->commandBufferCount; i++) {
        if (commandBufferCount == 1024) return VK_ERROR_OUT_OF_HOST_MEMORY;
        out[i] = (VkCommandBuffer)&commandBufferObjects[commandBufferCount++];
    }
    return VK_SUCCESS;
}
static void VKAPI_CALL FreeCommandBuffers(VkDevice d, VkCommandPool p, uint32_t n, const VkCommandBuffer* b) {
}
static VkResult VKAPI_CALL CreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo* i, const VkAllocationCallbacks* a, VkCommandPool* p) {
    *p = NEW(VkCommandPool);
    return VK_SUCCESS;
}
static void VKAPI_CALL DestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks* a) {
}
static VkResult VKAPI_CALL ResetCommandPool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) {
    return VK_SUCCESS;
}
static void VKAPI_CALL CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* info) {
    fake_icd.renderings++;
    fake_icd.lastColorLoad = info && info->colorAttachmentCount ? info->pColorAttachments[0].loadOp : VK_ATTACHMENT_LOAD_OP_MAX_ENUM;
    trace('B');
}
static void VKAPI_CALL CmdEndRendering(VkCommandBuffer cb) {
    trace('E');
}
static void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer* b) {
    trace('X');
}
static void VKAPI_CALL CmdDraw(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    trace('D');
}
static void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e) {
    trace('D');
}
static void VKAPI_CALL CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
}
static void VKAPI_CALL CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
}
static void VKAPI_CALL CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkBuffer* b, const VkDeviceSize* o) {
}
static void VKAPI_CALL CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkBuffer* b, const VkDeviceSize* o,
                                             const VkDeviceSize* s, const VkDeviceSize* st) {
}
static void VKAPI_CALL CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t) {
}
static void VKAPI_CALL CmdSetViewport(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkViewport* v) {
}
static void VKAPI_CALL CmdSetScissor(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkRect2D* s) {
}
static void VKAPI_CALL CmdSetViewportWithCount(VkCommandBuffer cb, uint32_t n, const VkViewport* v) {
}
static void VKAPI_CALL CmdSetScissorWithCount(VkCommandBuffer cb, uint32_t n, const VkRect2D* s) {
}
static void VKAPI_CALL CmdSetCullMode(VkCommandBuffer cb, VkCullModeFlags m) {
}
static void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* i) {
}
static void VKAPI_CALL CmdCopyBuffer(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy* r) {
}
static void VKAPI_CALL CmdClearAttachments(VkCommandBuffer cb, uint32_t n, const VkClearAttachment* a, uint32_t rn, const VkClearRect* r) {
}

#define F(name, fn) {"vk" name, (PFN_vkVoidFunction)fn}
static const struct {
    const char* name;
    PFN_vkVoidFunction fn;
} instanceFns[] = {
    F("CreateInstance", CreateInstance), F("DestroyInstance", DestroyInstance), F("EnumerateInstanceExtensionProperties", EnumerateExtensions),
    F("EnumeratePhysicalDevices", EnumeratePhysicalDevices), F("GetPhysicalDeviceProperties", GetPhysicalDeviceProperties),
    F("GetPhysicalDeviceFeatures", GetPhysicalDeviceFeatures), F("GetPhysicalDeviceQueueFamilyProperties", GetQueueFamilies),
    F("GetPhysicalDeviceMemoryProperties", GetMemoryProperties), F("GetPhysicalDeviceMemoryProperties2", GetMemoryProperties2),
    F("EnumerateDeviceExtensionProperties", EnumerateDeviceExtensions), F("CreateDevice", CreateDevice),
}, deviceFns[] = {
    F("DestroyDevice", DestroyDevice), F("GetDeviceQueue", GetDeviceQueue), F("QueueSubmit", QueueSubmit), F("QueueSubmit2", QueueSubmit2),
    F("QueueWaitIdle", QueueWaitIdle), F("DeviceWaitIdle", DeviceWaitIdle), F("WaitForFences", WaitForFences),
    F("WaitSemaphores", WaitSemaphores), F("CreateQueryPool", CreateQueryPool), F("DestroyQueryPool", DestroyQueryPool),
    F("ResetQueryPool", ResetQueryPool), F("CmdResetQueryPool", CmdResetQueryPool), F("GetQueryPoolResults", GetQueryPoolResults),
    F("BeginCommandBuffer", BeginCommandBuffer), F("EndCommandBuffer", EndCommandBuffer), F("ResetCommandBuffer", ResetCommandBuffer),
    F("CreateComputePipelines", CreateComputePipelines), F("CreateGraphicsPipelines", CreateGraphicsPipelines),
    F("DestroyPipeline", DestroyPipeline), F("GetEventStatus", GetEventStatus),
    F("CreateShaderModule", CreateShaderModule), F("DestroyShaderModule", DestroyShaderModule), F("CreateImage", CreateImage),
    F("DestroyImage", DestroyImage), F("CreateImageView", CreateImageView), F("DestroyImageView", DestroyImageView),
    F("CreateBuffer", CreateBuffer), F("DestroyBuffer", DestroyBuffer), F("BindBufferMemory", BindBufferMemory),
    F("AllocateMemory", AllocateMemory), F("FreeMemory", FreeMemory), F("MapMemory", MapMemory), F("UnmapMemory", UnmapMemory),
    F("AllocateCommandBuffers", AllocateCommandBuffers), F("FreeCommandBuffers", FreeCommandBuffers),
    F("CreateCommandPool", CreateCommandPool), F("DestroyCommandPool", DestroyCommandPool), F("ResetCommandPool", ResetCommandPool),
    F("CmdBeginRendering", CmdBeginRendering), F("CmdEndRendering", CmdEndRendering), F("CmdExecuteCommands", CmdExecuteCommands),
    F("CmdDraw", CmdDraw), F("CmdDrawIndexed", CmdDrawIndexed), F("CmdDispatch", CmdDispatch), F("CmdBindPipeline", CmdBindPipeline),
    F("CmdBindVertexBuffers", CmdBindVertexBuffers), F("CmdBindVertexBuffers2", CmdBindVertexBuffers2),
    F("CmdBindIndexBuffer", CmdBindIndexBuffer), F("CmdSetViewport", CmdSetViewport), F("CmdSetScissor", CmdSetScissor),
    F("CmdSetViewportWithCount", CmdSetViewportWithCount), F("CmdSetScissorWithCount", CmdSetScissorWithCount),
    F("CmdSetCullMode", CmdSetCullMode), F("CmdPipelineBarrier2", CmdPipelineBarrier2), F("CmdCopyBuffer", CmdCopyBuffer),
    F("CmdClearAttachments", CmdClearAttachments),
};

EXPORT PFN_vkVoidFunction fake_icd_device_proc(const char* name) {
    for (size_t i = 0; i < sizeof(deviceFns) / sizeof(deviceFns[0]); i++)
        if (!strcmp(name, deviceFns[i].name)) return deviceFns[i].fn;
    return NULL;
}

static PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)GetDeviceProcAddr;
    return fake_icd_device_proc(name);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* version) {
    if (*version > 5) *version = 5;
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char* name) {
    return NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)GetDeviceProcAddr;
    for (size_t i = 0; i < sizeof(instanceFns) / sizeof(instanceFns[0]); i++)
        if (!strcmp(name, instanceFns[i].name)) return instanceFns[i].fn;
    return instance ? fake_icd_device_proc(name) : NULL;
}
