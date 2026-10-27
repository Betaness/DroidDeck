/* [mali-g2] Pipeline compile cost and the Vortek server's persistent pipeline cache.
 *
 * Builds PC_N (default 200) distinct graphics pipelines the way DXVK 3.x does with
 * VK_EXT_graphics_pipeline_library, always with VK_NULL_HANDLE as the VkPipelineCache:
 *   per pipeline i: a pre-rasterization library (vertex shader), a fragment shader library,
 *   a fast link of those with shared vertex input / fragment output libraries, and the
 *   "optimized" monolithic pipeline DXVK compiles in the background.
 * The shaders (pcache.vert/pcache.frag -> pcache_spv.h) are shaped like a skinned mesh and a
 * Unity standard-lit material and are compiled in full, but the lit path is skipped at run time,
 * so pipeline i draws a constant colour that the test patches into its SPIR-V (every pipeline is
 * a different module, so nothing can come from a cache keyed on an earlier run's module unless
 * it is the same i). PC_SALT (default 0) shifts the colours: another set of modules.
 * Each pipeline then draws one pixel (monolithic: row 0, fast-linked: row 1) of an N x 2 image,
 * which is read back and checked: a pipeline taken from a cache under the wrong key shows up as
 * a wrong colour.
 * Prints the client-side create times (they include the Vortek round trip):
 *   PC vslib|fslib|link|full: n N total T ms median M ms p90 P ms max X ms
 * and "PC verify: K/2N right", then PC-PASS or PC-FAIL. PC_APP sets VkApplicationInfo's
 * pApplicationName (default pcache_test; the server keys its cache on it). PC_MODE=full makes
 * only the monolithic pipelines (no GPL). PC_CACHE_FILE=<path> makes the test pass its own
 * VkPipelineCache, loaded from and saved to that file (what an application with its own cache
 * does; for comparing with the server's implicit cache). PC_NO_DESTROY=1 exits without
 * destroying anything (a killed game). PC_THREADS=<n> creates the sets from n threads. PC_HOLD_S=<s> keeps the device that long after the check.
 * PC_IDENT=<k> then asks for the vertex libraries of the first k sets again the way DXVK does
 * with VK_EXT_shader_module_identifier (only the identifier, FAIL_ON_PIPELINE_COMPILE_REQUIRED) and
 * prints "PC ident: H/k from the identifier" (the Vortek server keeps such calls off its cache).
 *
 * Build: cc -O2 -o pcache_test pcache_test.c -lvulkan -lpthread
 *   (shaders: glslangValidator -V --vn pcache_vert_spv pcache.vert, the same for pcache.frag, -> pcache_spv.h) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <vulkan/vulkan.h>
#include "pcache_spv.h"

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static VkDevice dev;
static VkPipelineCache appCache = VK_NULL_HANDLE; /* PC_CACHE_FILE: the application's own cache */
static VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
static VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, 1, &colorFormat};

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int envInt(const char* name, int def) {
    const char* v = getenv(name);
    return v && *v ? atoi(v) : def;
}

static void saltColor(int i, uint8_t rgb[3]) {
    rgb[0] = (uint8_t)(3 + i % 250);
    rgb[1] = (uint8_t)(3 + (i / 250) % 250);
    rgb[2] = (uint8_t)(3 + (i * 37) % 250);
}

/* copies the module and replaces the three float constants that hold the magic values */
static uint32_t* saltModule(const uint32_t* code, size_t size, const float magic[3], const float value[3]) {
    uint32_t* out = malloc(size);
    memcpy(out, code, size);
    for (size_t w = 5; w < size / 4;) {
        uint32_t op = out[w] & 0xffff, count = out[w] >> 16;
        if (count == 0) break;
        if (op == 43 /* OpConstant */ && count == 4) {
            for (int k = 0; k < 3; k++) {
                uint32_t bits;
                memcpy(&bits, &magic[k], 4);
                if (out[w + 3] == bits) memcpy(&out[w + 3], &value[k], 4);
            }
        }
        w += count;
    }
    return out;
}

typedef struct { double* t; int n; } Times;

static pthread_mutex_t timesLock = PTHREAD_MUTEX_INITIALIZER;

static void addTime(Times* times, double ms) {
    pthread_mutex_lock(&timesLock);
    times->t[times->n++] = ms;
    pthread_mutex_unlock(&timesLock);
}

static int cmpDouble(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static void report(const char* what, Times* times) {
    if (!times->n) return;
    double total = 0;
    for (int i = 0; i < times->n; i++) total += times->t[i];
    qsort(times->t, times->n, sizeof(double), cmpDouble);
    printf("PC %s: n %d total %.1f ms median %.2f ms p90 %.2f ms max %.2f ms\n", what, times->n, total, times->t[times->n / 2],
           times->t[times->n * 9 / 10], times->t[times->n - 1]);
}

typedef struct {
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[2];
    VkPipelineDynamicStateCreateInfo dy;
} States;

static void initStates(States* s) {
    memset(s, 0, sizeof(*s));
    s->vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    s->ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    s->ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    s->vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    s->vp.viewportCount = 1;
    s->vp.scissorCount = 1;
    s->rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    s->rs.polygonMode = VK_POLYGON_MODE_FILL;
    s->rs.lineWidth = 1.0f;
    s->ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    s->ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    s->ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    s->cba.colorWriteMask = 0xf;
    s->cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    s->cb.attachmentCount = 1;
    s->cb.pAttachments = &s->cba;
    s->dyn[0] = VK_DYNAMIC_STATE_VIEWPORT;
    s->dyn[1] = VK_DYNAMIC_STATE_SCISSOR;
    s->dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    s->dy.dynamicStateCount = 2;
    s->dy.pDynamicStates = s->dyn;
}

static VkPipeline create(const VkGraphicsPipelineCreateInfo* info, Times* times) {
    VkPipeline p = VK_NULL_HANDLE;
    double t0 = now();
    CK(vkCreateGraphicsPipelines(dev, appCache, 1, info, 0, &p));
    if (times) addTime(times, (now() - t0) * 1e3);
    if (!p) { printf("FAIL null pipeline\n"); exit(1); }
    return p;
}

/* a library holding one part; stages/code only for the shader parts (inline SPIR-V, like DXVK) */
static VkPipeline createLibrary(const States* s, VkGraphicsPipelineLibraryFlagsEXT part, VkPipelineLayout layout,
                                const uint32_t* code, size_t size, Times* times) {
    VkGraphicsPipelineLibraryCreateInfoEXT lib = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, (void*)&rendering, part};
    VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &lib};
    info.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    info.basePipelineIndex = -1;
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, size, code};
    VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &mi};
    stage.pName = "main";
    if (part == VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT) {
        info.pVertexInputState = &s->vi;
        info.pInputAssemblyState = &s->ia;
    } else if (part == VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT) {
        stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        info.stageCount = 1;
        info.pStages = &stage;
        info.pViewportState = &s->vp;
        info.pRasterizationState = &s->rs;
        info.pDynamicState = &s->dy;
        info.layout = layout;
    } else if (part == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT) {
        stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        info.stageCount = 1;
        info.pStages = &stage;
        info.pDepthStencilState = &s->ds;
        info.pMultisampleState = &s->ms;
        info.layout = layout;
    } else {
        info.pMultisampleState = &s->ms;
        info.pColorBlendState = &s->cb;
    }
    return create(&info, times);
}

/* PC_IDENT: vertex libraries of sets 0..k-1 from their module identifier only; how many the
 * driver could create without compiling, or -1 without the extension */
static int identifierProbe(const States* s, VkPipelineLayout layout, int k, int salt) {
    PFN_vkGetShaderModuleCreateInfoIdentifierEXT getIdentifier =
        (PFN_vkGetShaderModuleCreateInfoIdentifierEXT)vkGetDeviceProcAddr(dev, "vkGetShaderModuleCreateInfoIdentifierEXT");
    if (!getIdentifier) return -1;
    int hits = 0;
    for (int i = 0; i < k; i++) {
        uint8_t rgb[3];
        saltColor(i + salt, rgb);
        float value[3] = {rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f};
        static const float magic[3] = {0.4444444f, 0.5555555f, 0.6666666f};
        uint32_t* vs = saltModule(pcache_vert_spv, sizeof(pcache_vert_spv), magic, value);
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(pcache_vert_spv), vs};
        VkShaderModuleIdentifierEXT id = {VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT};
        getIdentifier(dev, &mi, &id);
        free(vs);
        VkPipelineShaderStageModuleIdentifierCreateInfoEXT idInfo = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT, 0,
                                                                      id.identifierSize, id.identifier};
        VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &idInfo, 0, VK_SHADER_STAGE_VERTEX_BIT, 0, "main"};
        VkGraphicsPipelineLibraryCreateInfoEXT lib = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, (void*)&rendering,
                                                      VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT};
        VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &lib};
        info.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        info.stageCount = 1;
        info.pStages = &stage;
        info.pViewportState = &s->vp;
        info.pRasterizationState = &s->rs;
        info.pDynamicState = &s->dy;
        info.layout = layout;
        info.basePipelineIndex = -1;
        VkPipeline p = VK_NULL_HANDLE;
        VkResult r = vkCreateGraphicsPipelines(dev, appCache, 1, &info, 0, &p);
        if (r == VK_SUCCESS && p) {
            hits++;
            vkDestroyPipeline(dev, p, 0);
        } else if (r != VK_PIPELINE_COMPILE_REQUIRED) {
            printf("PC ident: set %d: %d\n", i, r);
        }
    }
    return hits;
}

typedef struct {
    int first, step, n, salt, useGpl;
    const States* s;
    VkPipelineLayout layoutLib, layoutFull;
    VkPipeline viLib, foLib, *libs, *linked, *full;
    Times *tv, *tf, *tl, *tm;
} Work;

static const float magicV[3] = {0.4444444f, 0.5555555f, 0.6666666f}, magicF[3] = {0.1111111f, 0.2222222f, 0.3333333f};

/* pipeline sets first, first + step, ... (PC_THREADS threads, like DXVK's compiler workers) */
static void* createSets(void* param) {
    Work* w = param;
    for (int i = w->first; i < w->n; i += w->step) {
        uint8_t rgb[3];
        saltColor(i + w->salt, rgb);
        float value[3] = {rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f};
        uint32_t* vs = saltModule(pcache_vert_spv, sizeof(pcache_vert_spv), magicV, value);
        uint32_t* fs = saltModule(pcache_frag_spv, sizeof(pcache_frag_spv), magicF, value);
        if (w->useGpl) {
            w->libs[2 * i] = createLibrary(w->s, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, w->layoutLib, vs, sizeof(pcache_vert_spv), w->tv);
            w->libs[2 * i + 1] = createLibrary(w->s, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, w->layoutLib, fs, sizeof(pcache_frag_spv), w->tf);
            VkPipeline parts[4] = {w->viLib, w->libs[2 * i], w->libs[2 * i + 1], w->foLib};
            VkPipelineLibraryCreateInfoKHR li = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, 0, 4, parts};
            VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &li};
            info.layout = w->layoutLib;
            info.basePipelineIndex = -1;
            w->linked[i] = create(&info, w->tl);
        }
        VkShaderModuleCreateInfo mi[2] = {{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(pcache_vert_spv), vs},
                                          {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof(pcache_frag_spv), fs}};
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &mi[0], 0, VK_SHADER_STAGE_VERTEX_BIT, 0, "main"},
                                                     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &mi[1], 0, VK_SHADER_STAGE_FRAGMENT_BIT, 0, "main"}};
        VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering, 0, 2, stages, &w->s->vi, &w->s->ia, 0, &w->s->vp, &w->s->rs,
                                             &w->s->ms, &w->s->ds, &w->s->cb, &w->s->dy, w->layoutFull};
        info.basePipelineIndex = -1;
        w->full[i] = create(&info, w->tm);
        free(vs);
        free(fs);
    }
    return NULL;
}

int main(void) {
    int n = envInt("PC_N", 200), salt = envInt("PC_SALT", 0);
    const char* mode = getenv("PC_MODE") && *getenv("PC_MODE") ? getenv("PC_MODE") : "gpl";
    const char* appName = getenv("PC_APP") && *getenv("PC_APP") ? getenv("PC_APP") : "pcache_test";
    int useGpl = strcmp(mode, "full") != 0;
    if (n < 1) n = 1;
    if (n > 4096) n = 4096;

    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, appName, 1, "pcache", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai};
    VkInstance inst;
    CK(vkCreateInstance(&ii, 0, &inst));
    uint32_t count = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &count, &pd);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    VkPhysicalDeviceVulkan13Features e13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.dynamicRendering = VK_TRUE;
    int identK = useGpl ? envInt("PC_IDENT", 0) : 0;
    if (identK > n) identK = n;
    VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT eid = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT, 0, VK_TRUE};
    if (identK > 0) {
        e13.pNext = &eid;
        e13.pipelineCreationCacheControl = VK_TRUE;
    }
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT egpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, &e13, VK_TRUE};
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio};
    const char* exts[] = {VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, useGpl ? (void*)&egpl : (void*)&e13, 0, 1, &qi, 0, 0, useGpl ? (identK > 0 ? 3 : 2) : 0, exts};
    CK(vkCreateDevice(pd, &di, 0, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    const char* cacheFile = getenv("PC_CACHE_FILE");
    if (cacheFile && *cacheFile) {
        void* data = NULL;
        long size = 0;
        FILE* f = fopen(cacheFile, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            size = ftell(f);
            fseek(f, 0, SEEK_SET);
            data = malloc(size > 0 ? size : 1);
            if (fread(data, 1, size, f) != (size_t)size) size = 0;
            fclose(f);
        }
        VkPipelineCacheCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, 0, 0, (size_t)size, data};
        CK(vkCreatePipelineCache(dev, &pci, 0, &appCache));
        printf("PC app cache: %ld bytes loaded from %s\n", size, cacheFile);
        free(data);
    }

    /* resources the lit path would read: bound for validity, never sampled at run time */
    VkDescriptorSetLayoutBinding bindings[7];
    for (int b = 0; b < 7; b++) {
        bindings[b] = (VkDescriptorSetLayoutBinding){b, b == 0 || b == 6 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                     b == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT};
    }
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 7, bindings};
    VkDescriptorSetLayout dsl;
    CK(vkCreateDescriptorSetLayout(dev, &dli, 0, &dsl));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl, 1, &pcr};
    VkPipelineLayout layoutFull, layoutLib = VK_NULL_HANDLE;
    CK(vkCreatePipelineLayout(dev, &pli, 0, &layoutFull));
    if (useGpl) {
        pli.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
        CK(vkCreatePipelineLayout(dev, &pli, 0, &layoutLib));
    }

    /* memory: one host-visible coherent type for everything (UMA) */
    uint32_t memType = 0;
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
        VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((mp.memoryTypes[t].propertyFlags & want) == want) { memType = t; break; }
    }
    int width = n;
    VkImageCreateInfo imi = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, colorFormat, {width, 2, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage target, tex;
    CK(vkCreateImage(dev, &imi, 0, &target));
    imi.extent = (VkExtent3D){1, 1, 1};
    imi.format = VK_FORMAT_R8G8B8A8_UNORM;
    imi.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    CK(vkCreateImage(dev, &imi, 0, &tex));
    VkImage shadowTex;
    imi.format = VK_FORMAT_D16_UNORM;
    CK(vkCreateImage(dev, &imi, 0, &shadowTex));
    VkImage images[3] = {target, tex, shadowTex};
    VkDeviceMemory imageMem[3];
    for (int k = 0; k < 3; k++) {
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, images[k], &req);
        uint32_t type = 0;
        while (!(req.memoryTypeBits & (1u << type))) type++;
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, req.size, type};
        CK(vkAllocateMemory(dev, &mai, 0, &imageMem[k]));
        CK(vkBindImageMemory(dev, images[k], imageMem[k], 0));
    }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer ubo, readback;
    CK(vkCreateBuffer(dev, &bci, 0, &ubo));
    bci.size = (VkDeviceSize)width * 2 * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CK(vkCreateBuffer(dev, &bci, 0, &readback));
    VkBuffer buffers[2] = {ubo, readback};
    VkDeviceMemory bufferMem[2];
    void* mapped[2];
    for (int k = 0; k < 2; k++) {
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, buffers[k], &req);
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, req.size, memType};
        CK(vkAllocateMemory(dev, &mai, 0, &bufferMem[k]));
        CK(vkBindBufferMemory(dev, buffers[k], bufferMem[k], 0));
        CK(vkMapMemory(dev, bufferMem[k], 0, VK_WHOLE_SIZE, 0, &mapped[k]));
    }
    memset(mapped[0], 0, 65536);
    VkImageViewCreateInfo ivi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, target, VK_IMAGE_VIEW_TYPE_2D, colorFormat, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView targetView, texView, shadowView;
    CK(vkCreateImageView(dev, &ivi, 0, &targetView));
    ivi.image = tex;
    ivi.format = VK_FORMAT_R8G8B8A8_UNORM;
    CK(vkCreateImageView(dev, &ivi, 0, &texView));
    ivi.image = shadowTex;
    ivi.format = VK_FORMAT_D16_UNORM;
    ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    CK(vkCreateImageView(dev, &ivi, 0, &shadowView));
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler sampler, shadowSampler;
    CK(vkCreateSampler(dev, &sci, 0, &sampler));
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS;
    CK(vkCreateSampler(dev, &sci, 0, &shadowSampler));
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 1, 2, sizes};
    VkDescriptorPool pool;
    CK(vkCreateDescriptorPool(dev, &dpi, 0, &pool));
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, pool, 1, &dsl};
    VkDescriptorSet set;
    CK(vkAllocateDescriptorSets(dev, &dsai, &set));
    VkDescriptorBufferInfo uboInfo[2] = {{ubo, 0, 16384}, {ubo, 16384, 1024}};
    VkDescriptorImageInfo texInfo = {sampler, texView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo shadowInfo = {shadowSampler, shadowView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[7];
    for (int b = 0; b < 7; b++) {
        writes[b] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, set, b, 0, 1, bindings[b].descriptorType};
        if (b == 0) writes[b].pBufferInfo = &uboInfo[0];
        else if (b == 6) writes[b].pBufferInfo = &uboInfo[1];
        else writes[b].pImageInfo = b == 5 ? &shadowInfo : &texInfo;
    }
    vkUpdateDescriptorSets(dev, 7, writes, 0, 0);

    States s;
    initStates(&s);
    rendering.colorAttachmentCount = 1;
    Times tv = {calloc(n, sizeof(double))}, tf = {calloc(n, sizeof(double))}, tl = {calloc(n, sizeof(double))}, tm = {calloc(n, sizeof(double))};
    VkPipeline viLib = VK_NULL_HANDLE, foLib = VK_NULL_HANDLE;
    if (useGpl) {
        viLib = createLibrary(&s, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, layoutLib, 0, 0, 0);
        foLib = createLibrary(&s, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, layoutLib, 0, 0, 0);
    }
    VkPipeline* full = calloc(n, sizeof(VkPipeline));
    VkPipeline* linked = calloc(n, sizeof(VkPipeline));
    VkPipeline* libs = calloc((size_t)n * 2, sizeof(VkPipeline));
    int threads = envInt("PC_THREADS", 1);
    if (threads < 1) threads = 1;
    if (threads > 16) threads = 16;
    pthread_t workers[16];
    Work work[16];
    double t0 = now();
    for (int t = 0; t < threads; t++) {
        work[t] = (Work){t, threads, n, salt, useGpl, &s, layoutLib, layoutFull, viLib, foLib, libs, linked, full, &tv, &tf, &tl, &tm};
        if (pthread_create(&workers[t], NULL, createSets, &work[t]) != 0) { printf("FAIL pthread_create\n"); return 1; }
    }
    for (int t = 0; t < threads; t++) pthread_join(workers[t], NULL);
    double createTotal = (now() - t0) * 1e3;
    report("vslib", &tv);
    report("fslib", &tf);
    report("link", &tl);
    report("full", &tm);
    printf("PC all: %d pipeline sets in %.1f ms (%d threads)\n", n, createTotal, threads);
    if (appCache) {
        size_t size = 0;
        CK(vkGetPipelineCacheData(dev, appCache, &size, NULL));
        void* data = malloc(size ? size : 1);
        CK(vkGetPipelineCacheData(dev, appCache, &size, data));
        FILE* f = fopen(cacheFile, "wb");
        if (f) { fwrite(data, 1, size, f); fclose(f); }
        printf("PC app cache: %zu bytes saved\n", size);
        free(data);
    }

    /* draw pixel (i, 0) with full[i] and (i, 1) with linked[i] */
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool cmdPool;
    CK(vkCreateCommandPool(dev, &cpi, 0, &cmdPool));
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cmdPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd;
    CK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CK(vkBeginCommandBuffer(cmd, &cbi));
    VkImageMemoryBarrier toGeneral[3];
    for (int k = 0; k < 3; k++) {
        toGeneral[k] = (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, images[k],
                                              {k == 2 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, 0, 0, 0, 3, toGeneral);
    VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, targetView, VK_IMAGE_LAYOUT_GENERAL};
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, {{0, 0}, {width, 2}}, 1, 0, 1, &att};
    vkCmdBeginRendering(cmd, &ri);
    float pc[8] = {0};
    for (int row = 0; row < (useGpl ? 2 : 1); row++) {
        VkPipelineLayout layout = row == 0 ? layoutFull : layoutLib;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, 0);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
        for (int i = 0; i < n; i++) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, row == 0 ? full[i] : linked[i]);
            VkViewport viewport = {(float)i, (float)row, 1, 1, 0, 1};
            VkRect2D scissor = {{i, row}, {1, 1}};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
    }
    vkCmdEndRendering(cmd);
    VkImageMemoryBarrier toCopy = toGeneral[0];
    toCopy.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toCopy.oldLayout = toCopy.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &toCopy);
    VkBufferImageCopy region = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {width, 2, 1}};
    vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_GENERAL, readback, 1, &region);
    VkMemoryBarrier toHost = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, 0, 0, 0);
    CK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cmd};
    CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CK(vkQueueWaitIdle(queue));

    const uint8_t* px = mapped[1];
    int right = 0, rows = useGpl ? 2 : 1, shown = 0;
    for (int row = 0; row < rows; row++) {
        for (int i = 0; i < n; i++) {
            uint8_t rgb[3];
            saltColor(i + salt, rgb);
            const uint8_t* p = px + ((size_t)row * width + i) * 4;
            int ok = 1;
            for (int c = 0; c < 3; c++) if (abs((int)p[c] - rgb[c]) > 1) ok = 0;
            if (ok) right++;
            else if (shown++ < 5) printf("wrong pixel %d,%d: %u %u %u want %u %u %u\n", i, row, p[0], p[1], p[2], rgb[0], rgb[1], rgb[2]);
        }
    }
    printf("PC verify: %d/%d right\n", right, rows * n);
    if (identK > 0) printf("PC ident: %d/%d from the identifier\n", identifierProbe(&s, layoutLib, identK, salt), identK);
    fflush(stdout);
    int hold = envInt("PC_HOLD_S", 0);
    if (hold > 0) sleep(hold);

    for (int i = 0; i < n; i++) {
        vkDestroyPipeline(dev, full[i], 0);
        if (useGpl) {
            vkDestroyPipeline(dev, linked[i], 0);
            vkDestroyPipeline(dev, libs[2 * i], 0);
            vkDestroyPipeline(dev, libs[2 * i + 1], 0);
        }
    }
    if (useGpl) {
        vkDestroyPipeline(dev, viLib, 0);
        vkDestroyPipeline(dev, foLib, 0);
    }
    if (getenv("PC_NO_DESTROY") && atoi(getenv("PC_NO_DESTROY"))) {
        /* leave the device to the server's disconnect cleanup, like a game that is killed */
        printf(right == rows * n ? "PC-PASS\n" : "PC-FAIL\n");
        fflush(stdout);
        _exit(right == rows * n ? 0 : 1);
    }
    vkDestroyCommandPool(dev, cmdPool, 0);
    vkDestroyDescriptorPool(dev, pool, 0);
    vkDestroySampler(dev, sampler, 0);
    vkDestroySampler(dev, shadowSampler, 0);
    vkDestroyImageView(dev, targetView, 0);
    vkDestroyImageView(dev, texView, 0);
    vkDestroyImageView(dev, shadowView, 0);
    for (int k = 0; k < 3; k++) {
        vkDestroyImage(dev, images[k], 0);
        vkFreeMemory(dev, imageMem[k], 0);
    }
    for (int k = 0; k < 2; k++) {
        vkDestroyBuffer(dev, buffers[k], 0);
        vkFreeMemory(dev, bufferMem[k], 0);
    }
    if (appCache) vkDestroyPipelineCache(dev, appCache, 0);
    vkDestroyPipelineLayout(dev, layoutFull, 0);
    if (layoutLib) vkDestroyPipelineLayout(dev, layoutLib, 0);
    vkDestroyDescriptorSetLayout(dev, dsl, 0);
    vkDestroyDevice(dev, 0);
    vkDestroyInstance(inst, 0);
    printf(right == rows * n ? "PC-PASS\n" : "PC-FAIL\n");
    return right == rows * n ? 0 : 1;
}
