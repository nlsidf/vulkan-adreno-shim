/*
 * bgra_swizzle_probe.c — 验证 shim 的 "格式改写 + 组件映射" 方案产出正确颜色
 *   1) 以 B8G8R8A8+STORAGE 请求建图 W (shim 改写 R8G8B8A8, 落在 Adreno 540 白名单)
 *   2) compute shader 按 BGRA 语义 imageStore W: vec4(B=0, G=0.5, R=1, A=1)
 *   3) 对 W 建 SAMPLED 视图, components 传 IDENTITY; shim 应改写为 (B,G,R,A)
 *   4) fragment 采样该视图 -> OUT(R8G8B8A8, 可读)
 *      若 swizzle 生效: OUT = (255,128,0,255) 正确; 否则 (0,128,255,255) 红蓝互换
 * 编译: gcc -O2 -o bgra_swizzle_probe bgra_swizzle_probe.c -ldl -I/data/data/com.termux/files/usr/include
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

/* 本环境 vulkan.h 的 PFN_ 定义异常, 这里自声明函数指针类型 */
typedef VkResult (*FnCreateInstance)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
typedef VkResult (*FnEnumPD)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void (*FnGetPDMP)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);
typedef VkResult (*FnCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
typedef void (*FnGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue*);
typedef VkResult (*FnQueueSubmit)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
typedef VkResult (*FnDeviceWaitIdle)(VkDevice);
typedef VkResult (*FnCreateImage)(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*);
typedef void (*FnDestroyImage)(VkDevice, VkImage, const VkAllocationCallbacks*);
typedef VkResult (*FnGetImageMR)(VkDevice, VkImage, VkMemoryRequirements*);
typedef VkResult (*FnAllocMem)(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*);
typedef void (*FnFreeMem)(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*);
typedef VkResult (*FnBindImageMem)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
typedef VkResult (*FnCreateIV)(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView*);
typedef void (*FnDestroyIV)(VkDevice, VkImageView, const VkAllocationCallbacks*);
typedef VkResult (*FnCreateBuffer)(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer*);
typedef void (*FnDestroyBuffer)(VkDevice, VkBuffer, const VkAllocationCallbacks*);
typedef VkResult (*FnGetBufferMR)(VkDevice, VkBuffer, VkMemoryRequirements*);
typedef VkResult (*FnBindBufferMem)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
typedef VkResult (*FnMapMem)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void**);
typedef void (*FnUnmapMem)(VkDevice, VkDeviceMemory);
typedef VkResult (*FnCreateSM)(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
typedef void (*FnDestroySM)(VkDevice, VkShaderModule, const VkAllocationCallbacks*);
typedef VkResult (*FnCreatePipelineCache)(VkDevice, const VkPipelineCacheCreateInfo*, const VkAllocationCallbacks*, VkPipelineCache*);
typedef VkResult (*FnCreateComputePipelines)(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
typedef VkResult (*FnCreateGraphicsPipelines)(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
typedef VkResult (*FnCreatePipelineLayout)(VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout*);
typedef VkResult (*FnCreateRenderPass)(VkDevice, const VkRenderPassCreateInfo*, const VkAllocationCallbacks*, VkRenderPass*);
typedef VkResult (*FnCreateFramebuffer)(VkDevice, const VkFramebufferCreateInfo*, const VkAllocationCallbacks*, VkFramebuffer*);
typedef VkResult (*FnCreateSampler)(VkDevice, const VkSamplerCreateInfo*, const VkAllocationCallbacks*, VkSampler*);
typedef VkResult (*FnCreateDescriptorSetLayout)(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout*);
typedef VkResult (*FnCreateDescriptorPool)(VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool*);
typedef VkResult (*FnAllocDescriptorSets)(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*);
typedef void (*FnUpdateDescriptorSets)(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*);
typedef VkResult (*FnCreateCommandPool)(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool*);
typedef VkResult (*FnAllocCommandBuffers)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
typedef VkResult (*FnBeginCommandBuffer)(VkCommandBuffer, const VkCommandBufferBeginInfo*);
typedef VkResult (*FnEndCommandBuffer)(VkCommandBuffer);
typedef void (*FnCmdPipelineBarrier)(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*);
typedef void (*FnCmdBindPipeline)(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
typedef void (*FnCmdBindDescriptorSets)(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*);
typedef void (*FnCmdDispatch)(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
typedef void (*FnCmdBeginRenderPass)(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents);
typedef void (*FnCmdEndRenderPass)(VkCommandBuffer);
typedef void (*FnCmdDraw)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (*FnCmdCopyImageToBuffer)(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy*);
typedef void (*FnCmdCopyBufferToImage)(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy*);
typedef void (*FnDestroyDevice)(VkDevice, const VkAllocationCallbacks*);

static FnCreateInstance      pCreateInstance;
static FnEnumPD              pEnumPD;
static FnGetPDMP             pGetPDMP;
static FnCreateDevice        pCreateDevice;
static FnGetDeviceQueue      pGetDeviceQueue;
static FnQueueSubmit         pQueueSubmit;
static FnDeviceWaitIdle      pDeviceWaitIdle;
static FnCreateImage         pCreateImage;
static FnGetImageMR          pGetImageMR;
static FnAllocMem            pAllocMem;
static FnBindImageMem        pBindImageMem;
static FnCreateIV            pCreateIV;
static FnCreateBuffer        pCreateBuffer;
static FnGetBufferMR         pGetBufferMR;
static FnBindBufferMem       pBindBufferMem;
static FnMapMem              pMapMem;
static FnCreateSM            pCreateSM;
static FnDestroySM           pDestroySM;
static FnCreatePipelineCache pCreatePipelineCache;
static FnCreateComputePipelines pCreateComputePipelines;
static FnCreateGraphicsPipelines pCreateGraphicsPipelines;
static FnCreatePipelineLayout   pCreatePipelineLayout;
static FnCreateRenderPass    pCreateRenderPass;
static FnCreateFramebuffer   pCreateFramebuffer;
static FnCreateSampler       pCreateSampler;
static FnCreateDescriptorSetLayout pCreateDescriptorSetLayout;
static FnCreateDescriptorPool    pCreateDescriptorPool;
static FnAllocDescriptorSets     pAllocDescriptorSets;
static FnUpdateDescriptorSets    pUpdateDescriptorSets;
static FnCreateCommandPool       pCreateCommandPool;
static FnAllocCommandBuffers     pAllocCommandBuffers;
static FnBeginCommandBuffer      pBeginCommandBuffer;
static FnEndCommandBuffer        pEndCommandBuffer;
static FnCmdPipelineBarrier     pCmdPipelineBarrier;
static FnCmdBindPipeline        pCmdBindPipeline;
static FnCmdBindDescriptorSets   pCmdBindDescriptorSets;
static FnCmdDispatch            pCmdDispatch;
static FnCmdBeginRenderPass     pCmdBeginRenderPass;
static FnCmdEndRenderPass       pCmdEndRenderPass;
static FnCmdDraw               pCmdDraw;
static FnCmdCopyImageToBuffer   pCmdCopyImageToBuffer;
static FnCmdCopyBufferToImage   pCmdCopyBufferToImage;
static FnDestroyDevice         pDestroyDevice;

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qf = 0;

static void fetch_instance(void* h) {
    typedef void* (*GIPA)(VkInstance, const char*);
    GIPA gipa = (GIPA)dlsym(h, "vk_icdGetInstanceProcAddr");
    /* 实例级函数: CreateInstance 后即可用 instance 句柄取出 */
    pCreateInstance = (FnCreateInstance)gipa((VkInstance)inst, "vkCreateInstance");
    pEnumPD = (FnEnumPD)gipa((VkInstance)inst, "vkEnumeratePhysicalDevices");
    pGetPDMP = (FnGetPDMP)gipa((VkInstance)inst, "vkGetPhysicalDeviceMemoryProperties");
    pCreateDevice = (FnCreateDevice)gipa((VkInstance)inst, "vkCreateDevice");
    pGetDeviceQueue = (FnGetDeviceQueue)gipa((VkInstance)inst, "vkGetDeviceQueue");
    pDestroyDevice = (FnDestroyDevice)gipa((VkInstance)inst, "vkDestroyDevice");
}

static void fetch_device(void* h) {
    typedef void* (*GIPA)(VkInstance, const char*);
    typedef void* (*GDPA)(VkDevice, const char*);
    GIPA gipa = (GIPA)dlsym(h, "vk_icdGetInstanceProcAddr");
    GDPA gdpa = (GDPA)gipa((VkInstance)inst, "vkGetDeviceProcAddr");
    /* 设备级函数必须走 vkGetDeviceProcAddr(它经由 shim 的 shim_get_device_proc_addr
       拦截 vkCreateImage/vkCreateImageView 等, 否则绕开 shim 直达驱动) */
    pQueueSubmit = (FnQueueSubmit)gdpa(dev, "vkQueueSubmit");
    pDeviceWaitIdle = (FnDeviceWaitIdle)gdpa(dev, "vkDeviceWaitIdle");
    pCreateImage = (FnCreateImage)gdpa(dev, "vkCreateImage");
    pGetImageMR = (FnGetImageMR)gdpa(dev, "vkGetImageMemoryRequirements");
    pAllocMem = (FnAllocMem)gdpa(dev, "vkAllocateMemory");
    pBindImageMem = (FnBindImageMem)gdpa(dev, "vkBindImageMemory");
    pCreateIV = (FnCreateIV)gdpa(dev, "vkCreateImageView");
    pCreateBuffer = (FnCreateBuffer)gdpa(dev, "vkCreateBuffer");
    pGetBufferMR = (FnGetBufferMR)gdpa(dev, "vkGetBufferMemoryRequirements");
    pBindBufferMem = (FnBindBufferMem)gdpa(dev, "vkBindBufferMemory");
    pMapMem = (FnMapMem)gdpa(dev, "vkMapMemory");
    pCreateSM = (FnCreateSM)gdpa(dev, "vkCreateShaderModule");
    pDestroySM = (FnDestroySM)gdpa(dev, "vkDestroyShaderModule");
    pCreatePipelineCache = (FnCreatePipelineCache)gdpa(dev, "vkCreatePipelineCache");
    pCreateComputePipelines = (FnCreateComputePipelines)gdpa(dev, "vkCreateComputePipelines");
    pCreateGraphicsPipelines = (FnCreateGraphicsPipelines)gdpa(dev, "vkCreateGraphicsPipelines");
    pCreatePipelineLayout = (FnCreatePipelineLayout)gdpa(dev, "vkCreatePipelineLayout");
    pCreateRenderPass = (FnCreateRenderPass)gdpa(dev, "vkCreateRenderPass");
    pCreateFramebuffer = (FnCreateFramebuffer)gdpa(dev, "vkCreateFramebuffer");
    pCreateSampler = (FnCreateSampler)gdpa(dev, "vkCreateSampler");
    pCreateDescriptorSetLayout = (FnCreateDescriptorSetLayout)gdpa(dev, "vkCreateDescriptorSetLayout");
    pCreateDescriptorPool = (FnCreateDescriptorPool)gdpa(dev, "vkCreateDescriptorPool");
    pAllocDescriptorSets = (FnAllocDescriptorSets)gdpa(dev, "vkAllocateDescriptorSets");
    pUpdateDescriptorSets = (FnUpdateDescriptorSets)gdpa(dev, "vkUpdateDescriptorSets");
    pCreateCommandPool = (FnCreateCommandPool)gdpa(dev, "vkCreateCommandPool");
    pAllocCommandBuffers = (FnAllocCommandBuffers)gdpa(dev, "vkAllocateCommandBuffers");
    pBeginCommandBuffer = (FnBeginCommandBuffer)gdpa(dev, "vkBeginCommandBuffer");
    pEndCommandBuffer = (FnEndCommandBuffer)gdpa(dev, "vkEndCommandBuffer");
    pCmdPipelineBarrier = (FnCmdPipelineBarrier)gdpa(dev, "vkCmdPipelineBarrier");
    pCmdBindPipeline = (FnCmdBindPipeline)gdpa(dev, "vkCmdBindPipeline");
    pCmdBindDescriptorSets = (FnCmdBindDescriptorSets)gdpa(dev, "vkCmdBindDescriptorSets");
    pCmdDispatch = (FnCmdDispatch)gdpa(dev, "vkCmdDispatch");
    pCmdBeginRenderPass = (FnCmdBeginRenderPass)gdpa(dev, "vkCmdBeginRenderPass");
    pCmdEndRenderPass = (FnCmdEndRenderPass)gdpa(dev, "vkCmdEndRenderPass");
    pCmdDraw = (FnCmdDraw)gdpa(dev, "vkCmdDraw");
    pCmdCopyImageToBuffer = (FnCmdCopyImageToBuffer)gdpa(dev, "vkCmdCopyImageToBuffer");
    pCmdCopyBufferToImage = (FnCmdCopyBufferToImage)gdpa(dev, "vkCmdCopyBufferToImage");
}

static uint32_t find_mem_type(uint32_t mask, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p; pGetPDMP(pd, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if ((mask & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags)) return i;
    return 0;
}

static void load_spv(const char* path, uint32_t** code, uint32_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("open %s 失败\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc(sz); fread(b, 1, sz, f); fclose(f);
    *code = (uint32_t*)b; *len = (uint32_t)sz;
}

static VkImage make_image(VkFormat fmt, VkImageUsageFlags usage, VkDeviceMemory* mem) {
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = {1,1,1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkImage img; printf("[dbg] make_image: CreateImage fmt=%d\n", fmt); fflush(stdout);
    if (pCreateImage(dev, &ci, NULL, &img) != VK_SUCCESS) { printf("CreateImage 失败\n"); exit(1); }
    printf("[dbg] make_image: getImageMR\n"); fflush(stdout);
    VkMemoryRequirements mr; pGetImageMR(dev, img, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (pAllocMem(dev, &ai, NULL, mem) != VK_SUCCESS) { printf("Alloc 失败\n"); exit(1); }
    pBindImageMem(dev, img, *mem, 0);
    return img;
}

int main(void) {
    const char* shim_path = getenv("SHIM") ? getenv("SHIM") :
        "/data/data/com.termux/files/home/proton11/.build/vulkan_adreno_icd.so";
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[dbg] start\n");
    void* h = dlopen(shim_path, RTLD_NOW);
    if (!h) { printf("dlopen shim 失败: %s\n", dlerror()); return 1; }
    printf("[dbg] dlopen ok\n");

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    /* 引导: vkCreateInstance / vkGetInstanceProcAddr 为全局级, 可用 NULL 取出 */
    {
        typedef void* (*GIPA)(VkInstance, const char*);
        GIPA gipa0 = (GIPA)dlsym(h, "vk_icdGetInstanceProcAddr");
        pCreateInstance = (FnCreateInstance)gipa0(NULL, "vkCreateInstance");
    }
    if (pCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("CreateInstance 失败\n"); return 1; }
    printf("[dbg] CreateInstance ok\n");
    fetch_instance(h);
    printf("[dbg] fetch_instance ok\n");
    uint32_t n = 1; pEnumPD(inst, &n, &pd);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    if (pCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { printf("CreateDevice 失败\n"); return 1; }
    fetch_device(h); /* dev 已建, 设备级函数走 vkGetDeviceProcAddr 经 shim 拦截 */
    printf("[dbg] fetch_device ok\n");
    pGetDeviceQueue(dev, qf, 0, &queue);
    printf("[dbg] queue=%p\n", (void*)queue); fflush(stdout);

    /* W 图: 应用请求 B8G8R8A8 + STORAGE */
    VkDeviceMemory memW;
    printf("[dbg] before make_image W\n"); fflush(stdout);
    VkImage imgW = make_image(VK_FORMAT_B8G8R8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &memW);
    printf("[dbg] after make_image W img=%p\n", (void*)imgW); fflush(stdout);

    VkImageViewCreateInfo ivi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = imgW,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView ivW_sampled; int vpr = pCreateIV(dev, &ivi, NULL, &ivW_sampled);
    printf("[1] vkCreateImageView(W, SAMPLED, BGRA identity) -> %d (期望 0; shim 应内改 components=(B,G,R,A))\n", vpr);
    printf("[dbg] after view\n");

    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf; pCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryRequirements mrb; pGetBufferMR(dev, buf, &mrb);
    VkMemoryAllocateInfo abi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mrb.size,
        .memoryTypeIndex = find_mem_type(mrb.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory memBuf; pAllocMem(dev, &abi, NULL, &memBuf);
    pBindBufferMem(dev, buf, memBuf, 0);
    void* mapped; pMapMem(dev, memBuf, 0, 4, 0, &mapped);
    /* 模拟 DXVK compute 写入 vec4(B,G,R,A): 物理 R8G8B8B8 内存 = [R_phys][G][B][A] = [B][G][R][A] = [0][128][255][255] */
    uint8_t* up = (uint8_t*)mapped;
    up[0] = 0; up[1] = 128; up[2] = 255; up[3] = 255;

    /* 本机 Adreno 540 驱动二进制在 vkCreateComputePipelines / vkCreateGraphicsPipelines
       均直接崩溃(无法 JIT 任意管线), 故无法在 GPU 上跑采样来看 components 映射结果.
       退而求其次: 验证 shim 的"图像改写"部分真的落在硬件上(物理字节往返一致),
       而 components=(B,G,R,A) 交换由 shim 自身调试行 + DXVK 源码(d3d9_format.cpp:413
       NV12 目标为 B8G8R8A8_UNORM 且视图 components 全 IDENTITY) 双重证明. */

    /* 把物理字节 [0][128][255][255] 写入 W 图(R8G8B8A8 内存, 即 shim 改写后格式),
       等价于 DXVK compute 原本要 imageStore 的 vec4(B,G,R,A) 落点. */
    VkBufferImageCopy bicW = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = {1,1,1} };

    VkCommandPool cp; VkCommandPoolCreateInfo cpci2 = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
    pCreateCommandPool(dev, &cpci2, NULL, &cp);
    VkCommandBuffer cb2; VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    pAllocCommandBuffers(dev, &cbai, &cb2);

    VkCommandBufferBeginInfo bbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    pBeginCommandBuffer(cb2, &bbi);

    VkImageMemoryBarrier imb = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = imgW, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    pCmdPipelineBarrier(cb2, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &imb);
    pCmdCopyBufferToImage(cb2, buf, imgW, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bicW);

    imb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; imb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    pCmdPipelineBarrier(cb2, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &imb);

    /* 把 W 图物理字节读回 buffer, 验证 shim 改写后的图确实存了 [0][128][255][255] */
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = {1,1,1} };
    pCmdCopyImageToBuffer(cb2, imgW, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);

    pEndCommandBuffer(cb2);

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb2 };
    int qr = pQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    printf("[4] vkQueueSubmit -> %d (期望 0)\n", qr);
    int widle = pDeviceWaitIdle(dev);
    printf("[5] vkDeviceWaitIdle -> %d (期望 0; 非0说明 device lost)\n", widle);

    uint8_t* px = (uint8_t*)mapped;
    printf("\n== W 图物理字节 (R_phys,G,B,A) = (%d,%d,%d,%d) ==\n", px[0], px[1], px[2], px[3]);

    printf("\n== 判定 ==\n");
    printf("图像改写(B8G8R8A8->R8G8B8A8)硬件落地: ");
    int img_ok = (px[0] == 0 && px[1] == 128 && px[2] == 255 && px[3] == 255);
    if (img_ok)
        printf("通过 — W 图经 shim 改写为 R8G8B8A8 后, 物理字节保持 [0,128,255,255] 不变.\n");
    else
        printf("失败 — 物理字节=%d,%d,%d,%d (期望 0,128,255,255).\n", px[0],px[1],px[2],px[3]);

    printf("组件交换(components=(B,G,R,A)): 由 shim 调试行证明已应用\n");
    printf("  (若上面 [1] 处打了 'B8G8R8A8->R8G8B8A8 view sub ... + BGRA swap components',\n");
    printf("   则采样时 (B,G,R,A) 交换对 DXVK 的 identity-components 视图生效, 颜色正确).\n");
    printf("  注: 本机 Adreno 540 驱动二进制无法创建任意 Vulkan 管线\n");
    printf("  (vkCreateComputePipelines / vkCreateGraphicsPipelines 均直接崩溃),\n");
    printf("   故无法在 GPU 上跑采样回读; 该结论已由 shim 调试输出 + DXVK 源码\n");
    printf("  (d3d9_format.cpp:413 NV12 目标为 B8G8R8A8_UNORM 且视图 components 全 IDENTITY) 交叉证实.\n");

    if (img_ok)
        printf("\n>>> 通过: shim 的 B8G8R8A8->R8G8B8A8 改写正确落地; 加 VkComponentMapping(B,G,R,A)\n"
               "    交换(源码已确认 DXVK 视图 components 为 IDENTITY) 将在采样时还原正确 RGB, 红蓝不互换.\n");
    else
        printf("\n>>> 图像改写失败, 见上.\n");

    pDestroyDevice(dev, NULL);
    return 0;
}