/* [mali-g2] Fault injection for the Vortek client's reply handling and call lock (vortek-0014,
 * tools/harden_replies.py), without a GPU. Two parts:
 *
 * 1. In this process, the client's (bounds-checked) vt_unserialize_* against replies made by the
 *    serializer the server uses: VkPhysicalDeviceProperties2 / Features2 / MemoryProperties2 /
 *    FormatProperties2 / ImageFormatProperties2 / MemoryRequirements2 with every extension
 *    structure the serializer carries (tests/reply_fault_gen.py lists them), queue families,
 *    extension lists, physical devices and groups, surface formats and present modes, swapchain
 *    images, pipeline cache data, query results. For each reply: the serializer must write exactly
 *    vt_sizeof bytes (it wrote past them for every fixed string, e.g. deviceName, and the memory
 *    budget arrays went over as 16 bytes instead of 128); the reply must come back whole and
 *    unchanged (spot checks); every truncation must be reported bad; every byte overwritten with
 *    0, -1, INT_MAX, INT_MIN, 0x10000, 0x100 or the reply's size, and random garbage, must neither
 *    crash nor write outside the application's arrays nor hang (replies and the application's
 *    arrays end right before an inaccessible page; -fsanitize=address checks the rest where it runs).
 *
 * 2. The client library itself (dlopen, vk_icdGetInstanceProcAddr) against a fake server in a
 *    child process that speaks the protocol (socket, the two shared-memory rings) and answers each
 *    request as the test tells it: valid, truncated, oversized counts, a pNext structure nobody
 *    asked for, empty, a header with an impossible size, or no reply and the reply ring's memory
 *    cut (the client faults reading it). Each scenario runs in its own
 *    client process under an alarm, so a deadlock is a failure, not a hung test:
 *      - 8 threads calling at once: every reply reaches its call, the cached physical device and
 *        queue keep one handle each;
 *      - bad replies fail the call (VK_ERROR_INITIALIZATION_FAILED / VK_ERROR_DEVICE_LOST, or zeroed
 *        output for void calls), the application's arrays are not overrun, and the next call with a
 *        good reply works; the memory budget of heap 3 comes through (it did not with 16 bytes);
 *      - A.V.A.'s case: a fault while the client writes the reply into the application's
 *        structure (here a read-only page), unwound by siglongjmp the way Wine unwinds a Unix call,
 *        must not leave the call lock held: another thread's call and the same thread's next call
 *        must complete;
 *      - a fault inside the call lock (the reply ring's memory cut while the thread waits, SIGBUS,
 *        siglongjmp): the same thread's next call releases the lock and, its reply never read,
 *        loses the connection; it and another thread's call fail with VK_ERROR_DEVICE_LOST at once;
 *      - a reply header with an impossible size loses the connection: that call and every later one
 *        fail with VK_ERROR_DEVICE_LOST at once;
 *      - a thread that ends while it holds the call lock (it faulted inside it, as above, and its
 *        function returned) must not block the others: within about a second their calls fail with
 *        VK_ERROR_DEVICE_LOST instead of hanging;
 *      - the library unloaded (the Vulkan loader dlclose()s the ICD when the last instance is
 *        destroyed) and then a thread that had made calls exits: nothing of the library may run
 *        then (the first r12-vortek client crashed here: its thread-specific key's destructor).
 *    Run against the g2-060 client, the fault scenarios hang (alarm).
 *
 * Build and run (from mali-g2/vortek, after fetch.sh and build-client.sh <dir>):
 *   python3 tests/reply_fault_gen.py work/vortek/include/vortek_serializer.h > out/reply_fault_chains.h
 *   cc -O1 -g -Iout -Iwork/vortek/include \
 *      -Iout/build-client/vkinc -Isrc -include src/droiddeck_defaults.h -Wno-discarded-qualifiers \
 *      -o out/reply_fault_test tests/reply_fault_test.c work/vortek/src/ring_buffer.c \
 *      work/vortek/src/arrays.c work/vortek/src/vk_object.c -lpthread -ldl
 *   out/reply_fault_test <dir>/libvulkan_vortek.so
 * (out/build-client/vkinc is where build-client.sh puts the Vulkan headers; with BUILD=... use that.)
 * Prints REPLY-FAULT-PASS, or REPLY-FAULT-FAIL after the failed checks. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>

/* the client's helpers, renamed so that they cannot clash with the library's (part 2) */
#define vt_threadMemoryPool rft_threadMemoryPool
#define vt_markConnectionLost rft_markConnectionLost
#define vt_connectionLost rft_connectionLost
#define vt_inScratch rft_inScratch
#define serverFd rft_serverFd
#define maxClientRequestId rft_maxClientRequestId
#define serverRing rft_serverRing
#define clientRing rft_clientRing
#define droiddeck_trace_request rft_trace_request
#define droiddeck_trace_cmd rft_trace_cmd
#define vt_cmdChunkSize rft_cmdChunkSize
#define vt_sendCommandChunk rft_sendCommandChunk
#include "vortek.h"
#include "vortek_serializer.h"
#include "reply_fault_chains.h"

#if defined(__SANITIZE_ADDRESS__)
#define RFT_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define RFT_ASAN 1
#endif
#endif
#ifndef RFT_ASAN
#define RFT_ASAN 0
#endif

int serverFd = -1;
uint16_t maxClientRequestId = 1;
RingBuffer* serverRing;
RingBuffer* clientRing;
bool vt_connectionLost;
int vt_cmdChunkSize;
void vt_sendCommandChunk(CommandBatch* batch) { (void)batch; }
void droiddeck_trace_request(int requestCode, int size) { (void)requestCode; (void)size; }
void droiddeck_trace_cmd(const char* name) { (void)name; }
void vt_markConnectionLost(const char* why) { (void)why; }
static MemoryPool pool;
MemoryPool* vt_threadMemoryPool(void) { return &pool; }
static char scratch[VT_IN_SCRATCH_SIZE] __attribute__((aligned(16)));
void* vt_inScratch(void) { return scratch; }

/* Blocks that end right before an inaccessible page, so that reading or writing past them faults
 * even without AddressSanitizer (which does not run under every proot). */
static void* galloc(size_t size) {
    long page = sysconf(_SC_PAGESIZE);
    size_t span = (size + page - 1) / page * page;
    char* base = mmap(NULL, span + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) abort();
    mprotect(base + span, page, PROT_NONE);
    char* block = base + span - size;
    memset(block, 0, size);
    return block;
}
static void gfree(void* block, size_t size) {
    if (!block) return;
    long page = sysconf(_SC_PAGESIZE);
    size_t span = (size + page - 1) / page * page;
    munmap((char*)block + size - span, span + page);
}
#define GREPLACE(ptr, n) do { gfree(ptr, ptr##Size); ptr##Size = (n) * sizeof(*ptr); ptr = galloc(ptr##Size); } while (0)

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

/* ---------------------------------------------------------------- part 1: unserialisers ---- */

#define SLOT 4096 /* room for any extension structure */

typedef struct Case {
    const char* name;
    int (*size)(void);
    void (*serialize)(char* out);
    void (*unserialize)(char* in); /* into fresh output; sets pool.inCap itself */
    int (*verify)(void);           /* 1 when a clean unserialise gave the expected values */
} Case;

static void* chain(const ChainEntry* list, char* blocks) {
    void* head = NULL;
    int n = 0;
    while (list[n].name) n++;
    for (int i = n - 1; i >= 0; i--) {
        VkBaseOutStructure* s = (VkBaseOutStructure*)(blocks + (size_t)i * SLOT);
        memset(s, 0, SLOT);
        s->sType = list[i].sType;
        s->pNext = head;
        head = s;
    }
    return head;
}
#define CHAIN_BLOCKS 128

/* VkPhysicalDeviceProperties2 */
static VkPhysicalDeviceProperties2 propsIn, propsOut;
static char propsInBlocks[CHAIN_BLOCKS * SLOT], propsOutBlocks[CHAIN_BLOCKS * SLOT];
static void propsSetup(void) {
    propsIn = (VkPhysicalDeviceProperties2){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, chain(chain_VkPhysicalDeviceProperties2, propsInBlocks)};
    strcpy(propsIn.properties.deviceName, "Mali-G2-Ultra-NX MC16");
    propsIn.properties.vendorID = 0x13b5;
    propsIn.properties.limits.maxImageDimension2D = 16384;
    propsIn.properties.sparseProperties.residencyNonResidentStrict = VK_TRUE;
    VkPhysicalDeviceDriverProperties* driver = findNextVkStructure(propsIn.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
    if (driver) {
        strcpy(driver->driverName, "Mali-G2");
        strcpy(driver->driverInfo, "56.0.0");
        driver->conformanceVersion = (VkConformanceVersion){1, 4, 1, 0};
    }
    VkPhysicalDeviceVulkan12Properties* p12 = findNextVkStructure(propsIn.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES);
    if (p12) {
        p12->maxTimelineSemaphoreValueDifference = UINT64_MAX;
        p12->framebufferIntegerColorSampleCounts = 0x1f;
    }
}
static int propsSize(void) { propsSetup(); return vt_sizeof_VkPhysicalDeviceProperties2(&propsIn); }
static void propsSerialize(char* out) { vt_serialize_VkPhysicalDeviceProperties2(&propsIn, out); }
static void propsUnserialize(char* in) {
    propsOut = (VkPhysicalDeviceProperties2){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, chain(chain_VkPhysicalDeviceProperties2, propsOutBlocks)};
    vt_unserialize_VkPhysicalDeviceProperties2(&propsOut, in, &pool);
}
static int propsVerify(void) {
    VkPhysicalDeviceDriverProperties* driver = findNextVkStructure(propsOut.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
    VkPhysicalDeviceVulkan12Properties* p12 = findNextVkStructure(propsOut.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES);
    return strcmp(propsOut.properties.deviceName, "Mali-G2-Ultra-NX MC16") == 0 && propsOut.properties.vendorID == 0x13b5 &&
           propsOut.properties.limits.maxImageDimension2D == 16384 && propsOut.properties.sparseProperties.residencyNonResidentStrict &&
           (!driver || (strcmp(driver->driverInfo, "56.0.0") == 0 && driver->conformanceVersion.minor == 4)) &&
           (!p12 || (p12->maxTimelineSemaphoreValueDifference == UINT64_MAX && p12->framebufferIntegerColorSampleCounts == 0x1f));
}

/* VkPhysicalDeviceFeatures2 */
static VkPhysicalDeviceFeatures2 featIn, featOut;
static char featInBlocks[CHAIN_BLOCKS * SLOT], featOutBlocks[CHAIN_BLOCKS * SLOT];
static int featSize(void) {
    featIn = (VkPhysicalDeviceFeatures2){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain(chain_VkPhysicalDeviceFeatures2, featInBlocks)};
    featIn.features.textureCompressionBC = VK_TRUE;
    featIn.features.inheritedQueries = VK_TRUE; /* the last field */
    VkPhysicalDeviceVulkan13Features* f13 = findNextVkStructure(featIn.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    if (f13) f13->dynamicRendering = VK_TRUE;
    return vt_sizeof_VkPhysicalDeviceFeatures2(&featIn);
}
static void featSerialize(char* out) { vt_serialize_VkPhysicalDeviceFeatures2(&featIn, out); }
static void featUnserialize(char* in) {
    featOut = (VkPhysicalDeviceFeatures2){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain(chain_VkPhysicalDeviceFeatures2, featOutBlocks)};
    vt_unserialize_VkPhysicalDeviceFeatures2(&featOut, in, &pool);
}
static int featVerify(void) {
    VkPhysicalDeviceVulkan13Features* f13 = findNextVkStructure(featOut.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    return featOut.features.textureCompressionBC && featOut.features.inheritedQueries && (!f13 || f13->dynamicRendering);
}

/* VkPhysicalDeviceMemoryProperties2 + budget */
static VkPhysicalDeviceMemoryProperties2 memIn, memOut;
static VkPhysicalDeviceMemoryBudgetPropertiesEXT budgetIn, budgetOut;
static int memSize(void) {
    budgetIn = (VkPhysicalDeviceMemoryBudgetPropertiesEXT){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    memIn = (VkPhysicalDeviceMemoryProperties2){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budgetIn};
    memIn.memoryProperties.memoryTypeCount = 3;
    memIn.memoryProperties.memoryHeapCount = 4;
    for (int i = 0; i < 3; i++) memIn.memoryProperties.memoryTypes[i] = (VkMemoryType){1u << i, (uint32_t)i};
    for (int i = 0; i < 4; i++) {
        memIn.memoryProperties.memoryHeaps[i] = (VkMemoryHeap){(VkDeviceSize)(i + 1) << 30, i == 0 ? VK_MEMORY_HEAP_DEVICE_LOCAL_BIT : 0};
        budgetIn.heapBudget[i] = 0x100000000ull * (i + 1) + 0x1234;
        budgetIn.heapUsage[i] = 0x10000ull * (i + 1);
    }
    budgetIn.heapBudget[15] = 0xfeedfacecafebeefull; /* the last entry of the array */
    return vt_sizeof_VkPhysicalDeviceMemoryProperties2(&memIn);
}
static void memSerialize(char* out) { vt_serialize_VkPhysicalDeviceMemoryProperties2(&memIn, out); }
static void memUnserialize(char* in) {
    budgetOut = (VkPhysicalDeviceMemoryBudgetPropertiesEXT){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    memOut = (VkPhysicalDeviceMemoryProperties2){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budgetOut};
    vt_unserialize_VkPhysicalDeviceMemoryProperties2(&memOut, in, &pool);
}
static int memVerify(void) {
    if (getenv("RFT_VERBOSE")) printf("   mem: types %u heaps %u type2 heap %u heap3 %llx budget3 %llx usage3 %llx budget15 %llx pNext %d\n", memOut.memoryProperties.memoryTypeCount, memOut.memoryProperties.memoryHeapCount, memOut.memoryProperties.memoryTypes[2].heapIndex, (unsigned long long)memOut.memoryProperties.memoryHeaps[3].size, (unsigned long long)budgetOut.heapBudget[3], (unsigned long long)budgetOut.heapUsage[3], (unsigned long long)budgetOut.heapBudget[15], memOut.pNext == &budgetOut);
    return memOut.memoryProperties.memoryTypeCount == 3 && memOut.memoryProperties.memoryHeapCount == 4 &&
           memOut.memoryProperties.memoryTypes[2].heapIndex == 2 && memOut.memoryProperties.memoryHeaps[3].size == (4ull << 30) &&
           budgetOut.heapBudget[3] == 0x400001234ull && budgetOut.heapUsage[3] == 0x40000ull &&
           budgetOut.heapBudget[15] == 0xfeedfacecafebeefull && memOut.pNext == &budgetOut;
}

/* a pNext structure the application did not ask for must not end up linked into its struct */
static int memNoChainSize(void) { return memSize(); }
static void memNoChainUnserialize(char* in) {
    memOut = (VkPhysicalDeviceMemoryProperties2){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, NULL};
    vt_unserialize_VkPhysicalDeviceMemoryProperties2(&memOut, in, &pool);
}
static int memNoChainVerify(void) { return memOut.pNext == NULL && memOut.memoryProperties.memoryHeapCount == 4; }

/* the other *2 structures with their chains */
#define SIMPLE2(T, sType_, field, value)                                                                       \
    static T T##In, T##Out;                                                                                    \
    static char T##InBlocks[8 * SLOT], T##OutBlocks[8 * SLOT];                                                 \
    static int T##Size(void) {                                                                                 \
        T##In = (T){sType_, chain(chain_##T, T##InBlocks)};                                                    \
        T##In.field = value;                                                                                   \
        return vt_sizeof_##T(&T##In);                                                                          \
    }                                                                                                          \
    static void T##Serialize(char* out) { vt_serialize_##T(&T##In, out); }                                     \
    static void T##Unserialize(char* in) {                                                                     \
        T##Out = (T){sType_, chain(chain_##T, T##OutBlocks)};                                                  \
        vt_unserialize_##T(&T##Out, in, &pool);                                                                \
    }                                                                                                          \
    static int T##Verify(void) { return T##Out.field == value; }
SIMPLE2(VkFormatProperties2, VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, formatProperties.bufferFeatures, 0x55)
SIMPLE2(VkImageFormatProperties2, VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, imageFormatProperties.maxResourceSize, 0x123456789ull)
SIMPLE2(VkMemoryRequirements2, VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, memoryRequirements.memoryTypeBits, 0x7)

/* commands with arrays the application passes: the reply is built like the server does, for an
 * application array of CAP elements; the output goes into an array of exactly CAP elements */
#define CAP 5
static uint32_t countOut;

static VkQueueFamilyProperties qfIn[CAP], *qfOut;
static size_t qfOutSize;
static int qfSize(void) {
    for (int i = 0; i < CAP; i++) qfIn[i] = (VkQueueFamilyProperties){VK_QUEUE_GRAPHICS_BIT, (uint32_t)i + 1, 64, {1, 1, 1}};
    uint32_t n = CAP;
    return vt_sizeof_vkGetPhysicalDeviceQueueFamilyProperties(NULL, &n, qfIn);
}
static void qfSerialize(char* out) { uint32_t n = CAP; vt_serialize_vkGetPhysicalDeviceQueueFamilyProperties(NULL, &n, qfIn, out); }
static void qfUnserialize(char* in) {
    GREPLACE(qfOut, CAP);
    countOut = CAP;
    pool.inCap = CAP;
    vt_unserialize_vkGetPhysicalDeviceQueueFamilyProperties(NULL, &countOut, qfOut, in, &pool);
}
static int qfVerify(void) { return countOut == CAP && qfOut[CAP - 1].queueCount == CAP && qfOut[2].minImageTransferGranularity.depth == 1; }

static VkQueueFamilyProperties2 qf2In[CAP], *qf2Out;
static size_t qf2OutSize;
static int qf2Size(void) {
    for (int i = 0; i < CAP; i++) qf2In[i] = (VkQueueFamilyProperties2){VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2, NULL, {VK_QUEUE_COMPUTE_BIT, (uint32_t)i + 2}};
    uint32_t n = CAP;
    return vt_sizeof_vkGetPhysicalDeviceQueueFamilyProperties2(NULL, &n, qf2In);
}
static void qf2Serialize(char* out) { uint32_t n = CAP; vt_serialize_vkGetPhysicalDeviceQueueFamilyProperties2(NULL, &n, qf2In, out); }
static void qf2Unserialize(char* in) {
    GREPLACE(qf2Out, CAP);
    for (int i = 0; i < CAP; i++) qf2Out[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
    countOut = CAP;
    pool.inCap = CAP;
    vt_unserialize_vkGetPhysicalDeviceQueueFamilyProperties2(NULL, &countOut, qf2Out, in, &pool);
}
static int qf2Verify(void) { return countOut == CAP && qf2Out[CAP - 1].queueFamilyProperties.queueCount == CAP + 1 && qf2Out[0].pNext == NULL; }

static VkExtensionProperties extIn[CAP], *extOut;
static size_t extOutSize;
static int extSize(void) {
    for (int i = 0; i < CAP; i++) {
        memset(extIn[i].extensionName, 0, sizeof(extIn[i].extensionName));
        snprintf(extIn[i].extensionName, sizeof(extIn[i].extensionName), "VK_EXT_extension_number_%d", i);
        extIn[i].specVersion = 100 + i;
    }
    memset(extIn[3].extensionName, 'x', sizeof(extIn[3].extensionName) - 1); /* the longest a name can be */
    uint32_t n = CAP;
    return vt_sizeof_vkEnumerateDeviceExtensionProperties(NULL, NULL, &n, extIn);
}
static void extSerialize(char* out) { uint32_t n = CAP; vt_serialize_vkEnumerateDeviceExtensionProperties(NULL, NULL, &n, extIn, out); }
static void extUnserialize(char* in) {
    GREPLACE(extOut, CAP);
    countOut = CAP;
    pool.inCap = CAP;
    vt_unserialize_vkEnumerateDeviceExtensionProperties(NULL, NULL, &countOut, extOut, in, &pool);
}
static int extVerify(void) {
    return countOut == CAP && strcmp(extOut[4].extensionName, "VK_EXT_extension_number_4") == 0 && extOut[4].specVersion == 104 &&
           extOut[0].specVersion == 100 && strlen(extOut[3].extensionName) == VK_MAX_EXTENSION_NAME_SIZE - 1;
}

/* handles as the serializer in this (client) copy of the header expects them: VkObjects with the
 * server's id, which it writes into the reply like the server writes its own handles */
static void* idHandle(uint64_t id) { return VkObject_toHandle(VkObject_create(VK_OBJECT_TYPE_UNKNOWN, id)); }

static uint64_t pdIds[CAP] = {11, 12, 13, 14, 15};
static VkPhysicalDevice* pdOut;
static size_t pdOutSize;
static int pdSize(void) { uint32_t n = CAP; return vt_sizeof_vkEnumeratePhysicalDevices(NULL, &n, (VkPhysicalDevice*)pdIds); }
static void pdSerialize(char* out) {
    VkPhysicalDevice devices[CAP];
    for (int i = 0; i < CAP; i++) devices[i] = idHandle(pdIds[i]);
    uint32_t n = CAP;
    vt_serialize_vkEnumeratePhysicalDevices(NULL, &n, devices, out);
}
static void pdUnserialize(char* in) {
    GREPLACE(pdOut, CAP);
    countOut = CAP;
    pool.inCap = CAP;
    vt_unserialize_vkEnumeratePhysicalDevices(NULL, &countOut, pdOut, in, &pool);
}
static int pdVerify(void) { return countOut == CAP && VkObject_fromHandle(pdOut[4])->id == 15; }

static VkPhysicalDeviceGroupProperties grpIn[2], *grpOut;
static size_t grpOutSize;
static int grpSize(void) {
    for (int g = 0; g < 2; g++) {
        grpIn[g] = (VkPhysicalDeviceGroupProperties){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES, NULL, 2};
        for (int i = 0; i < 2; i++) grpIn[g].physicalDevices[i] = idHandle(pdIds[g * 2 + i]);
        grpIn[g].subsetAllocation = VK_TRUE;
    }
    uint32_t n = 2;
    return vt_sizeof_vkEnumeratePhysicalDeviceGroups(NULL, &n, grpIn);
}
static void grpSerialize(char* out) { uint32_t n = 2; vt_serialize_vkEnumeratePhysicalDeviceGroups(NULL, &n, grpIn, out); }
static void grpUnserialize(char* in) {
    GREPLACE(grpOut, 2);
    countOut = 2;
    pool.inCap = 2;
    vt_unserialize_vkEnumeratePhysicalDeviceGroups(NULL, &countOut, grpOut, in, &pool);
}
static int grpVerify(void) { return countOut == 2 && grpOut[1].physicalDeviceCount == 2 && VkObject_fromHandle(grpOut[1].physicalDevices[1])->id == 14 && grpOut[1].subsetAllocation; }

static VkSurfaceFormatKHR sfIn[CAP], *sfOut;
static size_t sfOutSize;
static int sfSize(void) {
    for (int i = 0; i < CAP; i++) sfIn[i] = (VkSurfaceFormatKHR){VK_FORMAT_B8G8R8A8_UNORM + i, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    uint32_t n = CAP;
    return vt_sizeof_vkGetPhysicalDeviceSurfaceFormatsKHR(NULL, NULL, &n, sfIn);
}
static void sfSerialize(char* out) { uint32_t n = CAP; vt_serialize_vkGetPhysicalDeviceSurfaceFormatsKHR(NULL, NULL, &n, sfIn, out); }
static void sfUnserialize(char* in) {
    GREPLACE(sfOut, CAP);
    countOut = CAP;
    pool.inCap = CAP;
    vt_unserialize_vkGetPhysicalDeviceSurfaceFormatsKHR(NULL, NULL, &countOut, sfOut, in, &pool);
}
static int sfVerify(void) { return countOut == CAP && sfOut[4].format == VK_FORMAT_B8G8R8A8_UNORM + 4; }

static VkPresentModeKHR pmIn[CAP] = {0, 1, 2, 3, 2}, *pmOut;
static size_t pmOutSize;
static int pmSize(void) { uint32_t n = CAP; return vt_sizeof_vkGetPhysicalDeviceSurfacePresentModesKHR(NULL, NULL, &n, pmIn); }
static void pmSerialize(char* out) { uint32_t n = CAP; vt_serialize_vkGetPhysicalDeviceSurfacePresentModesKHR(NULL, NULL, &n, pmIn, out); }
static void pmUnserialize(char* in) {
    GREPLACE(pmOut, CAP);
    countOut = CAP;
    pool.inCap = CAP;
    vt_unserialize_vkGetPhysicalDeviceSurfacePresentModesKHR(NULL, NULL, &countOut, pmOut, in, &pool);
}
static int pmVerify(void) { return countOut == CAP && pmOut[3] == 3 && pmOut[4] == 2; }

static uint64_t imgIds[3] = {21, 22, 23};
static VkImage* imgOut;
static size_t imgOutSize;
static int imgSize(void) { uint32_t n = 3; return vt_sizeof_vkGetSwapchainImagesKHR(NULL, NULL, &n, (VkImage*)imgIds); }
static void imgSerialize(char* out) {
    VkImage images[3];
    for (int i = 0; i < 3; i++) images[i] = idHandle(imgIds[i]);
    uint32_t n = 3;
    vt_serialize_vkGetSwapchainImagesKHR(NULL, NULL, &n, images, out);
}
static void imgUnserialize(char* in) {
    GREPLACE(imgOut, 3);
    countOut = 3;
    pool.inCap = 3;
    vt_unserialize_vkGetSwapchainImagesKHR(NULL, NULL, &countOut, imgOut, in, &pool);
}
static int imgVerify(void) { return countOut == 3 && VkObject_fromHandle(imgOut[2])->id == 23; }

#define DATA 300
static unsigned char dataIn[DATA], *dataOut;
static size_t dataOutSize;
static size_t dataSizeOut;
static int pcSize(void) {
    for (int i = 0; i < DATA; i++) dataIn[i] = (unsigned char)(i * 7 + 1);
    size_t n = DATA;
    return vt_sizeof_vkGetPipelineCacheData(NULL, NULL, &n, dataIn);
}
static void pcSerialize(char* out) { size_t n = DATA; vt_serialize_vkGetPipelineCacheData(NULL, NULL, &n, dataIn, out); }
static void pcUnserialize(char* in) {
    GREPLACE(dataOut, DATA);
    dataSizeOut = DATA;
    pool.inCap = DATA;
    vt_unserialize_vkGetPipelineCacheData(NULL, NULL, &dataSizeOut, dataOut, in, &pool);
}
static int pcVerify(void) { return dataSizeOut == DATA && memcmp(dataIn, dataOut, DATA) == 0; }

static int qrSize(void) {
    for (int i = 0; i < DATA; i++) dataIn[i] = (unsigned char)(i * 3 + 5);
    return vt_sizeof_vkGetQueryPoolResults(NULL, NULL, 0, 0, DATA, dataIn, 8, 0);
}
static void qrSerialize(char* out) { vt_serialize_vkGetQueryPoolResults(NULL, NULL, 0, 0, DATA, dataIn, 8, 0, out); }
static void qrUnserialize(char* in) {
    GREPLACE(dataOut, DATA);
    pool.inCap = DATA;
    vt_unserialize_vkGetQueryPoolResults(NULL, NULL, NULL, NULL, NULL, dataOut, NULL, NULL, in, &pool);
}
static int qrVerify(void) { return memcmp(dataIn, dataOut, DATA) == 0; }

#define CASE(n, p) {n, p##Size, p##Serialize, p##Unserialize, p##Verify}
static const Case cases[] = {
    CASE("VkPhysicalDeviceProperties2 + chain", props),
    CASE("VkPhysicalDeviceFeatures2 + chain", feat),
    CASE("VkPhysicalDeviceMemoryProperties2 + budget", mem),
    {"VkPhysicalDeviceMemoryProperties2, chain nobody asked for", memNoChainSize, memSerialize, memNoChainUnserialize, memNoChainVerify},
    CASE("VkFormatProperties2 + chain", VkFormatProperties2),
    CASE("VkImageFormatProperties2 + chain", VkImageFormatProperties2),
    CASE("VkMemoryRequirements2 + chain", VkMemoryRequirements2),
    CASE("vkGetPhysicalDeviceQueueFamilyProperties", qf),
    CASE("vkGetPhysicalDeviceQueueFamilyProperties2", qf2),
    CASE("vkEnumerateDeviceExtensionProperties", ext),
    CASE("vkEnumeratePhysicalDevices", pd),
    CASE("vkEnumeratePhysicalDeviceGroups", grp),
    CASE("vkGetPhysicalDeviceSurfaceFormatsKHR", sf),
    CASE("vkGetPhysicalDeviceSurfacePresentModesKHR", pm),
    CASE("vkGetSwapchainImagesKHR", img),
    CASE("vkGetPipelineCacheData", pc),
    CASE("vkGetQueryPoolResults", qr),
};

/* unserialise `len` bytes copied to a heap block of exactly that size */
static bool runOn(const Case* c, const char* bytes, int len) {
    char* in = galloc(len > 0 ? len : 1);
    if (len > 0) memcpy(in, bytes, len);
    pool.inStart = len > 0 ? in : NULL;
    pool.inEnd = len > 0 ? in + len : NULL;
    pool.inBad = false;
    pool.inCap = 0;
    c->unserialize(in);
    vt_free(&pool);
    gfree(in, len > 0 ? len : 1);
    return pool.inBad;
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)rng;
}

static void part1(void) {
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        const Case* c = &cases[k];
        if (getenv("RFT_VERBOSE")) printf("-- %s\n", c->name);
        int size = c->size();
        char* buf = malloc(size + 256);
        memset(buf, 0xa5, size + 256);
        c->serialize(buf);
        if (getenv("RFT_VERBOSE")) printf("   serialized %d bytes\n", size);
        int over = 0;
        for (int i = size; i < size + 256; i++) over += (unsigned char)buf[i] != 0xa5;
        CHECK(over == 0, "%s: the serializer wrote %d bytes past vt_sizeof (%d)", c->name, over, size);

        CHECK(!runOn(c, buf, size), "%s: a whole reply (%d bytes) was reported bad", c->name, size);
        if (getenv("RFT_VERBOSE")) printf("   whole reply unserialised\n");
        CHECK(c->verify(), "%s: values did not come through", c->name);

        int missed = 0;
        for (int len = 0; len < size; len++) {
            if (getenv("RFT_VERBOSE") && atoi(getenv("RFT_VERBOSE")) > 1) printf("   truncated to %d\n", len);
            missed += !runOn(c, buf, len);
        }
        CHECK(missed == 0, "%s: %d of %d truncations were not reported bad", c->name, missed, size);

        static const uint32_t values[] = {0, 0xffffffffu, 0x7fffffffu, 0x80000000u, 0x10000u, 0x100u, 0};
        char* mut = malloc(size);
        long mutations = 0, bad = 0;
        for (int off = 0; off + 4 <= size; off++) {
            for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
                uint32_t value = v == sizeof(values) / sizeof(values[0]) - 1 ? (uint32_t)size : values[v];
                memcpy(mut, buf, size);
                memcpy(mut + off, &value, 4);
                if (getenv("RFT_VERBOSE") && atoi(getenv("RFT_VERBOSE")) > 1) printf("   offset %d = 0x%x\n", off, value);
                bad += runOn(c, mut, size);
                mutations++;
            }
        }
        for (int r = 0; r < 3000; r++) {
            int len = (r & 1) ? (int)(rnd() % (size * 2 + 1)) : size;
            char* g = malloc(len + 1);
            if (r & 1) {
                for (int i = 0; i < len; i++) g[i] = (char)rnd();
            }
            else {
                memcpy(g, buf, size);
                for (int n = 1 + rnd() % 4; n > 0; n--) g[rnd() % size] = (char)rnd();
            }
            bad += runOn(c, g, len);
            mutations++;
            free(g);
        }
        printf("%-58s %5d bytes: whole ok, %d truncations bad, %ld mutations survived (%ld bad)\n", c->name, size, size - missed, mutations, bad);
        free(mut);
        free(buf);
    }
}

/* ---------------------------------------------------------------- part 2: fake server ------- */

enum Mode {
    REPLY_GOOD,
    REPLY_TRUNCATED,     /* half of the payload */
    REPLY_HEAP_COUNT,    /* memoryHeapCount 1000 */
    REPLY_ITEM_SIZE,     /* the budget item's size INT_MAX */
    REPLY_EXTRA_CHAIN,   /* a budget structure the application did not ask for */
    REPLY_EMPTY,         /* no payload at all */
    REPLY_COUNT_OVER,    /* more elements than the application's array holds */
    REPLY_LONG_NAME,     /* an extension name of 300 bytes */
    REPLY_BAD_HEADER,    /* a reply header with size -5 */
    REPLY_RING_GONE,     /* instead of answering, cut the reply ring's memory (the client faults reading it) */
};

typedef struct Shared {
    atomic_int mode;
    atomic_int requests;
} Shared;

static Shared* shared;
static int replyRingFd = -1;
static char socketPath[108];

static void sendReply(RingBuffer* ring, int result, const void* data, int size) {
    char header[HEADER_SIZE];
    *(int*)(header + 0) = result;
    *(int*)(header + 4) = size;
    RingBuffer_write(ring, header, HEADER_SIZE);
    if (size > 0) RingBuffer_write(ring, data, size);
}

static void replyMemoryProperties2(RingBuffer* ring, const char* request, int requestSize, int mode) {
    (void)request;
    (void)requestSize;
    /* always with the budget: the scenarios ask for it, except REPLY_EXTRA_CHAIN, which must ignore it */
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
    props.memoryProperties.memoryTypeCount = 2;
    props.memoryProperties.memoryHeapCount = 4;
    props.memoryProperties.memoryTypes[0] = (VkMemoryType){VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    props.memoryProperties.memoryTypes[1] = (VkMemoryType){VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 3};
    for (int i = 0; i < 4; i++) {
        props.memoryProperties.memoryHeaps[i].size = 1ull << 32;
        budget.heapBudget[i] = 0x100000000ull * (i + 1);
        budget.heapUsage[i] = 0x1000ull * (i + 1);
    }
    if (mode == REPLY_HEAP_COUNT) props.memoryProperties.memoryHeapCount = 1000;
    int size = vt_sizeof_VkPhysicalDeviceMemoryProperties2(&props);
    char* out = calloc(1, size + 64);
    vt_serialize_VkPhysicalDeviceMemoryProperties2(&props, out);
    if (mode == REPLY_HEAP_COUNT) size = vt_sizeof_VkPhysicalDeviceMemoryProperties2(&props); /* as written */
    if (mode == REPLY_ITEM_SIZE) *(int*)(out + 8) = INT_MAX; /* sType, pNext type, item size */
    if (mode == REPLY_TRUNCATED) size /= 2;
    if (mode == REPLY_EMPTY) size = 0;
    sendReply(ring, VK_SUCCESS, out, size);
    free(out);
}

static void replyQueueFamilies(RingBuffer* ring, const char* request, int requestSize, int mode) {
    uint64_t id = 0;
    uint32_t count = 0;
    pool.inStart = request;
    pool.inEnd = request + requestSize;
    pool.inBad = false;
    vt_unserialize_vkGetPhysicalDeviceQueueFamilyProperties((VkPhysicalDevice)&id, &count, NULL, (char*)request, &pool);
    vt_free(&pool);
    uint32_t n = mode == REPLY_COUNT_OVER && count ? count + 7 : (count ? (count < 2 ? count : 2) : 2);
    VkQueueFamilyProperties families[64] = {0};
    for (uint32_t i = 0; i < n && i < 64; i++) families[i] = (VkQueueFamilyProperties){VK_QUEUE_GRAPHICS_BIT, 1 + i, 64, {1, 1, 1}};
    int size = vt_sizeof_vkGetPhysicalDeviceQueueFamilyProperties(NULL, &n, count ? families : NULL);
    char* out = calloc(1, size);
    vt_serialize_vkGetPhysicalDeviceQueueFamilyProperties(NULL, &n, count ? families : NULL, out);
    sendReply(ring, VK_SUCCESS, out, size);
    free(out);
}

static void replyExtensions(RingBuffer* ring, const char* request, int requestSize, int mode) {
    uint64_t id = 0;
    uint32_t count = 0;
    pool.inStart = request;
    pool.inEnd = request + requestSize;
    pool.inBad = false;
    vt_unserialize_vkEnumerateDeviceExtensionProperties((VkPhysicalDevice)&id, NULL, &count, NULL, (char*)request, &pool);
    vt_free(&pool);
    VkExtensionProperties props[3] = {{"VK_KHR_swapchain", 70}, {"VK_KHR_maintenance1", 2}, {"VK_EXT_robustness2", 1}};
    uint32_t n = 3;
    int size = vt_sizeof_vkEnumerateDeviceExtensionProperties(NULL, NULL, &n, count ? props : NULL);
    char* out = calloc(1, size + 400);
    vt_serialize_vkEnumerateDeviceExtensionProperties(NULL, NULL, &n, count ? props : NULL, out);
    if (mode == REPLY_LONG_NAME && count) {
        /* the first name says it is 300 bytes long and has no NUL */
        /* no-device byte, layer name size, count size, count, array size, then the item's size */
        char* item = out + 1 + 4 + 4 + 4 + 4 + 4;
        *(int*)item = 300;
        size += 400;
    }
    sendReply(ring, VK_SUCCESS, out, size);
    free(out);
}

static void serveClient(int fd) {
    char header[HEADER_SIZE];
    if (read(fd, header, HEADER_SIZE) != HEADER_SIZE || *(int*)header != REQUEST_CODE_CREATE_CONTEXT) return;
    int fds[2] = {memfd_create("rft-server-ring", 0), memfd_create("rft-client-ring", 0)};
    if (ftruncate(fds[0], RingBuffer_getSHMemSize(SERVER_RING_BUFFER_SIZE)) || ftruncate(fds[1], RingBuffer_getSHMemSize(CLIENT_RING_BUFFER_SIZE))) return;
    RingBuffer* requests = RingBuffer_create(fds[0], SERVER_RING_BUFFER_SIZE);
    RingBuffer* replies = RingBuffer_create(fds[1], CLIENT_RING_BUFFER_SIZE);
    send_fds(fd, fds, 2, NULL, 0);
    close(fds[0]);
    replyRingFd = fds[1];
    uint64_t nextId = 0x1000;
    for (;;) {
        if (!RingBuffer_read(requests, header, HEADER_SIZE)) return;
        int code = *(int*)(header + 0), size = *(int*)(header + 4);
        char* payload = size > 0 ? malloc(size) : NULL;
        if (size > 0 && !RingBuffer_read(requests, payload, size)) return;
        atomic_fetch_add(&shared->requests, 1);
        int mode = atomic_load(&shared->mode);
        if (getenv("RFT_VERBOSE")) printf("   server: request %d (%d bytes), mode %d\n", code, size, mode);
        if (mode == REPLY_RING_GONE) {
            if (ftruncate(replyRingFd, 0)) {}
        }
        else if (mode == REPLY_BAD_HEADER) {
            char bad[HEADER_SIZE];
            *(int*)(bad + 0) = VK_SUCCESS;
            *(int*)(bad + 4) = -5;
            RingBuffer_write(replies, bad, HEADER_SIZE);
        }
        else switch (code) {
            case REQUEST_CODE_VK_CREATE_INSTANCE:
            case REQUEST_CODE_VK_CREATE_DEVICE: {
                uint64_t id = nextId++;
                sendReply(replies, VK_SUCCESS, &id, 8);
                break;
            }
            case REQUEST_CODE_VK_GET_DEVICE_QUEUE: {
                uint64_t id = 0x9999; /* the driver's queue: the same every time */
                sendReply(replies, VK_SUCCESS, &id, 8);
                break;
            }
            case REQUEST_CODE_VK_ENUMERATE_PHYSICAL_DEVICES: {
                VkPhysicalDevice device = idHandle(0x77);
                uint32_t n = 1;
                /* the client asks for the count first (no array), then for the array */
                pool.inStart = payload;
                pool.inEnd = payload + size;
                uint32_t asked = 0;
                uint64_t instanceId;
                vt_unserialize_vkEnumeratePhysicalDevices((VkInstance)&instanceId, &asked, NULL, payload, &pool);
                vt_free(&pool);
                int replySize = vt_sizeof_vkEnumeratePhysicalDevices(NULL, &n, asked ? &device : NULL);
                char* out = calloc(1, replySize);
                vt_serialize_vkEnumeratePhysicalDevices(NULL, &n, asked ? &device : NULL, out);
                sendReply(replies, VK_SUCCESS, out, replySize);
                free(out);
                break;
            }
            case REQUEST_CODE_VK_GET_PHYSICAL_DEVICE_PROPERTIES: {
                VkPhysicalDeviceProperties props = {VK_API_VERSION_1_3, 1, 0x13b5, 0x1234, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, "fake"};
                int replySize = vt_sizeof_VkPhysicalDeviceProperties(&props);
                char* out = calloc(1, replySize);
                vt_serialize_VkPhysicalDeviceProperties(&props, out);
                sendReply(replies, VK_SUCCESS, out, replySize);
                free(out);
                break;
            }
            case REQUEST_CODE_VK_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES2:
                replyMemoryProperties2(replies, payload, size, mode);
                break;
            case REQUEST_CODE_VK_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES:
                replyQueueFamilies(replies, payload, size, mode);
                break;
            case REQUEST_CODE_VK_ENUMERATE_DEVICE_EXTENSION_PROPERTIES:
                replyExtensions(replies, payload, size, mode);
                break;
            case REQUEST_CODE_VK_QUEUE_WAIT_IDLE:
                sendReply(replies, VK_SUCCESS, NULL, 0);
                break;
            default:
                break; /* requests without a reply */
        }
        free(payload);
    }
}

static pid_t startServer(void) {
    snprintf(socketPath, sizeof(socketPath), "/tmp/rft-%d.sock", getpid());
    unlink(socketPath);
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, socketPath, sizeof(addr.sun_path) - 1);
    if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(listener, 16) < 0) {
        perror("fake server");
        exit(2);
    }
    pid_t pid = fork();
    if (pid == 0) {
        for (;;) {
            int fd = accept(listener, NULL, NULL);
            if (fd < 0) continue;
            if (fork() == 0) {
                prctl(PR_SET_PDEATHSIG, SIGKILL);
                serveClient(fd);
                _exit(0);
            }
            close(fd);
        }
    }
    close(listener);
    setenv("VORTEK_SERVER_PATH", socketPath, 1);
    return pid;
}

/* the client side, in a child process per scenario */
static PFN_vkGetInstanceProcAddr gipa;
static VkInstance instance;
static VkPhysicalDevice physicalDevice;
static VkDevice device;
static VkQueue queue;
#define FN(name) ((PFN_##name)gipa(instance, #name))

static void setMode(int mode) { atomic_store(&shared->mode, mode); }

static void* clientLib;

static int clientSetup(const char* library) {
    void* lib = clientLib = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        printf("dlopen %s: %s\n", library, dlerror());
        return 0;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    setMode(REPLY_GOOD);
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (!gipa || ((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ii, NULL, &instance) != VK_SUCCESS) return 0;
    uint32_t n = 0;
    if (FN(vkEnumeratePhysicalDevices)(instance, &n, NULL) != VK_SUCCESS || n != 1) return 0;
    if (FN(vkEnumeratePhysicalDevices)(instance, &n, &physicalDevice) != VK_SUCCESS) return 0;
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &priority};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qi};
    if (FN(vkCreateDevice)(physicalDevice, &di, NULL, &device) != VK_SUCCESS) return 0;
    FN(vkGetDeviceQueue)(device, 0, 0, &queue);
    return queue != VK_NULL_HANDLE;
}

static int memoryGood(void) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
    setMode(REPLY_GOOD);
    FN(vkGetPhysicalDeviceMemoryProperties2)(physicalDevice, &props);
    return props.memoryProperties.memoryHeapCount == 4 && props.memoryProperties.memoryTypes[1].heapIndex == 3 &&
           budget.heapBudget[3] == 0x400000000ull && budget.heapUsage[3] == 0x4000ull && props.pNext == &budget;
}

static int scenarioBadReplies(void) {
    int ok = 1;
    if (!memoryGood()) { printf("  good memory properties (budget of heap 3) did not come through\n"); ok = 0; }
    static const struct { int mode; const char* name; } bad[] = {
        {REPLY_TRUNCATED, "truncated"}, {REPLY_HEAP_COUNT, "memoryHeapCount 1000"}, {REPLY_ITEM_SIZE, "budget item size INT_MAX"},
        {REPLY_EMPTY, "empty"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
        setMode(bad[i].mode);
        FN(vkGetPhysicalDeviceMemoryProperties2)(physicalDevice, &props);
        if (props.memoryProperties.memoryHeapCount != 0 || props.memoryProperties.memoryTypeCount != 0 || props.pNext != &budget) {
            printf("  %s memory properties: heaps %u types %u (want a zeroed result)\n", bad[i].name, props.memoryProperties.memoryHeapCount, props.memoryProperties.memoryTypeCount);
            ok = 0;
        }
        if (!memoryGood()) { printf("  the call after a %s reply did not work\n", bad[i].name); ok = 0; }
    }

    VkPhysicalDeviceMemoryProperties2 bare = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, NULL};
    setMode(REPLY_EXTRA_CHAIN);
    FN(vkGetPhysicalDeviceMemoryProperties2)(physicalDevice, &bare);
    if (bare.pNext != NULL || bare.memoryProperties.memoryHeapCount != 4) {
        printf("  a budget structure nobody asked for: pNext %p heaps %u (want NULL, 4)\n", bare.pNext, bare.memoryProperties.memoryHeapCount);
        ok = 0;
    }

    /* more queue families than the application's array holds; a guard element after it */
    VkQueueFamilyProperties families[3];
    memset(families, 0x5a, sizeof(families));
    uint32_t count = 2;
    setMode(REPLY_COUNT_OVER);
    FN(vkGetPhysicalDeviceQueueFamilyProperties)(physicalDevice, &count, families);
    unsigned char guard[sizeof(families[2])];
    memset(guard, 0x5a, sizeof(guard));
    if (count != 0 || memcmp(&families[2], guard, sizeof(guard)) != 0) {
        printf("  9 queue families for an array of 2: count %u, guard %s\n", count, memcmp(&families[2], guard, sizeof(guard)) ? "overwritten" : "intact");
        ok = 0;
    }
    setMode(REPLY_GOOD);
    count = 2;
    FN(vkGetPhysicalDeviceQueueFamilyProperties)(physicalDevice, &count, families);
    if (count != 2 || families[1].queueCount != 2) { printf("  queue families after the bad reply: count %u\n", count); ok = 0; }

    VkExtensionProperties ext[3];
    count = 3;
    setMode(REPLY_LONG_NAME);
    VkResult r = FN(vkEnumerateDeviceExtensionProperties)(physicalDevice, NULL, &count, ext);
    if (r != VK_ERROR_INITIALIZATION_FAILED) { printf("  300-byte extension name: result %d (want -3)\n", r); ok = 0; }
    setMode(REPLY_GOOD);
    count = 3;
    r = FN(vkEnumerateDeviceExtensionProperties)(physicalDevice, NULL, &count, ext);
    if (r != VK_SUCCESS || count != 3 || strcmp(ext[2].extensionName, "VK_EXT_robustness2") != 0 || ext[0].specVersion != 70) {
        printf("  extensions after the bad reply: result %d count %u spec %u\n", r, count, count ? ext[0].specVersion : 0);
        ok = 0;
    }
    return ok;
}

/* many threads at once: every reply reaches the call that asked for it (the lock is now held only
 * from request to reply), and the cached physical device and queue keep one handle each */
#define THREADS 8
static void* concurrentCalls(void* arg) {
    (void)arg;
    for (int i = 0; i < 300; i++) {
        uint32_t n = 1;
        VkPhysicalDevice pd = VK_NULL_HANDLE;
        VkQueue qu = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties props = {0};
        if (FN(vkEnumeratePhysicalDevices)(instance, &n, &pd) != VK_SUCCESS || pd != physicalDevice) return (void*)1;
        FN(vkGetDeviceQueue)(device, 0, 0, &qu);
        if (qu != queue) return (void*)2;
        FN(vkGetPhysicalDeviceProperties)(physicalDevice, &props);
        if (strcmp(props.deviceName, "fake") != 0) return (void*)3;
        if (i % 3 == 0 && !memoryGood()) return (void*)4;
    }
    return NULL;
}

static int scenarioConcurrent(void) {
    setMode(REPLY_GOOD);
    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&threads[i], NULL, concurrentCalls, NULL);
    int ok = 1;
    for (int i = 0; i < THREADS; i++) {
        void* r = NULL;
        pthread_join(threads[i], &r);
        if (r) { printf("  thread %d: check %d failed\n", i, (int)(intptr_t)r); ok = 0; }
    }
    return ok;
}

/* A.V.A.: a fault while the reply is written into the application's structure */
static sigjmp_buf faultJump;
static volatile sig_atomic_t faults;
static void onFault(int sig) {
    (void)sig;
    faults++;
    siglongjmp(faultJump, 1);
}

static void* otherThreadCall(void* arg) {
    (void)arg;
    VkPhysicalDeviceProperties props = {0};
    FN(vkGetPhysicalDeviceProperties)(physicalDevice, &props);
    return (void*)(uintptr_t)(strcmp(props.deviceName, "fake") == 0 && props.vendorID == 0x13b5);
}

static int scenarioFault(void) {
    struct sigaction sa = {0};
    sa.sa_handler = onFault;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    long page = sysconf(_SC_PAGESIZE);
    VkPhysicalDeviceMemoryProperties2* ro = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    *ro = (VkPhysicalDeviceMemoryProperties2){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, NULL};
    mprotect(ro, page, PROT_READ);
    setMode(REPLY_GOOD);
    if (sigsetjmp(faultJump, 1) == 0) FN(vkGetPhysicalDeviceMemoryProperties2)(physicalDevice, ro);
    if (faults != 1) { printf("  no fault writing into a read-only structure (%d)\n", (int)faults); return 0; }
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);

    pthread_t thread;
    void* other = NULL;
    pthread_create(&thread, NULL, otherThreadCall, NULL);
    pthread_join(thread, &other); /* hangs (alarm) while the call lock is still held */
    if (!other) { printf("  another thread's call after the fault failed\n"); return 0; }
    if (!memoryGood()) { printf("  the faulting thread's next call failed\n"); return 0; }
    return 1;
}

static int scenarioBadHeader(void) {
    setMode(REPLY_BAD_HEADER);
    uint32_t n = 0;
    VkResult r = FN(vkEnumeratePhysicalDevices)(instance, &n, NULL);
    setMode(REPLY_GOOD);
    int before = atomic_load(&shared->requests);
    VkResult r2 = FN(vkEnumeratePhysicalDevices)(instance, &n, NULL);
    int sent = atomic_load(&shared->requests) - before;
    if (r != VK_ERROR_DEVICE_LOST || r2 != VK_ERROR_DEVICE_LOST || sent != 0) {
        printf("  reply size -5: result %d, next call %d with %d requests sent (want -4, -4, 0)\n", r, r2, sent);
        return 0;
    }
    return 1;
}

/* a thread that faults inside the call lock and then ends (its function returns after the
 * siglongjmp): it never releases the lock, the others must not wait for it forever */
static __thread sigjmp_buf threadJump;
static void onThreadFault(int sig) {
    (void)sig;
    siglongjmp(threadJump, 1);
}

static void* faultThenEnd(void* arg) {
    (void)arg;
    if (sigsetjmp(threadJump, 1) == 0) FN(vkQueueWaitIdle)(queue);
    return NULL;
}

static int scenarioThreadEnds(void) {
    struct sigaction sa = {0};
    sa.sa_handler = onThreadFault;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    setMode(REPLY_RING_GONE);
    pthread_t thread;
    pthread_create(&thread, NULL, faultThenEnd, NULL);
    pthread_join(thread, NULL);
    setMode(REPLY_GOOD);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t n = 0;
    VkResult r = FN(vkEnumeratePhysicalDevices)(instance, &n, NULL); /* hangs (alarm) while nobody releases the lock */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("  the next call returned %d after %.1f s\n", r, s);
    return r == VK_ERROR_DEVICE_LOST;
}

/* a fault inside the call lock: the reply ring's memory is gone while the thread waits for its
 * reply (SIGBUS), unwound by siglongjmp; the same thread's next call must release the lock (and,
 * the reply never read, lose the connection), and another thread must not wait for it */
static void* otherThreadEnumerate(void* arg) {
    (void)arg;
    uint32_t n = 0;
    return (void*)(intptr_t)FN(vkEnumeratePhysicalDevices)(instance, &n, NULL);
}

static int scenarioFaultInLock(void) {
    struct sigaction sa = {0};
    sa.sa_handler = onFault;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    setMode(REPLY_RING_GONE);
    if (sigsetjmp(faultJump, 1) == 0) FN(vkQueueWaitIdle)(queue);
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
    if (faults != 1) { printf("  no fault reading the cut reply ring (%d)\n", (int)faults); return 0; }
    setMode(REPLY_GOOD);
    uint32_t n = 0;
    VkResult r = FN(vkEnumeratePhysicalDevices)(instance, &n, NULL); /* self-deadlock without the recovery */
    pthread_t thread;
    void* other = NULL;
    pthread_create(&thread, NULL, otherThreadEnumerate, NULL);
    pthread_join(thread, &other);
    if (r != VK_ERROR_DEVICE_LOST || (VkResult)(intptr_t)other != VK_ERROR_DEVICE_LOST) {
        printf("  after the fault: same thread %d, other thread %d (want -4, -4)\n", r, (int)(intptr_t)other);
        return 0;
    }
    return 1;
}

/* the Vulkan loader dlclose()s the ICD when its last instance is destroyed: a thread that made calls
 * and exits after that must not run anything the library left behind (a thread-specific key's
 * destructor would jump into the unmapped library) */
static atomic_int unloadStep;
static void* callThenExitAfterUnload(void* arg) {
    (void)arg;
    uint32_t n = 0;
    VkResult r = FN(vkEnumeratePhysicalDevices)(instance, &n, NULL);
    atomic_store(&unloadStep, r == VK_SUCCESS ? 1 : -1);
    while (atomic_load(&unloadStep) == 1) usleep(1000);
    return NULL; /* the thread exits here, after the library is gone */
}

static const char* unloadLibrary;
static int scenarioUnloaded(void) {
    pthread_t thread;
    pthread_create(&thread, NULL, callThenExitAfterUnload, NULL);
    while (atomic_load(&unloadStep) == 0) usleep(1000);
    if (atomic_load(&unloadStep) < 0) { printf("  the call before unloading failed\n"); return 0; }
    dlclose(clientLib);
    void* still = dlopen(unloadLibrary, RTLD_NOW | RTLD_NOLOAD);
    if (still) printf("  (the library stayed loaded after dlclose, nothing to check)\n");
    atomic_store(&unloadStep, 2);
    pthread_join(thread, NULL); /* crashes here if the exiting thread called into the unloaded library */
    return 1;
}

static int runScenario(const char* library, const char* name, int (*scenario)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(30);
        int ok = clientSetup(library);
        if (!ok) printf("  client setup against the fake server failed\n");
        else ok = scenario();
        fflush(stdout);
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    const char* why = WIFSIGNALED(status) ? (WTERMSIG(status) == SIGALRM ? " (hung: alarm)" : " (crashed)") : "";
    printf("%-58s %s%s\n", name, ok ? "ok" : "FAIL", why);
    CHECK(ok, "%s", name);
    return ok;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("== part 1: unserialisers (replies and application arrays end at a guard page%s)\n", RFT_ASAN ? ", AddressSanitizer" : "");
    part1();
    if (argc > 1) {
        printf("== part 2: %s against a fake server\n", argv[1]);
        shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        pid_t server = startServer();
        runScenario(argv[1], "8 threads calling at once", scenarioConcurrent);
        runScenario(argv[1], "bad replies fail the call, the next one works", scenarioBadReplies);
        runScenario(argv[1], "fault writing the reply (A.V.A.), siglongjmp out", scenarioFault);
        runScenario(argv[1], "fault inside the call lock (reply ring gone)", scenarioFaultInLock);
        runScenario(argv[1], "reply header size -5 loses the connection", scenarioBadHeader);
        runScenario(argv[1], "thread ends holding the call lock", scenarioThreadEnds);
        unloadLibrary = argv[1];
        runScenario(argv[1], "library unloaded, then a thread that called it exits", scenarioUnloaded);
        kill(server, SIGKILL);
        waitpid(server, NULL, 0);
        unlink(socketPath);
    }
    else printf("== part 2 skipped (no client library given)\n");
    printf(failures ? "REPLY-FAULT-FAIL (%d)\n" : "REPLY-FAULT-PASS\n", failures);
    return failures ? 1 : 0;
}
