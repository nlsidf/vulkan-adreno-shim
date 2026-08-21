/*
 * disp_readback_probe.c — 决定性验证: Adreno 540 能否真正对 R8G8B8A8 storage image
 *   做 compute 写入并读回. 用 glslang 编译的合法 rgba8 compute shader 写 (1,0,0,1).
 *   (DXVK-Sarek 的 YUV 转换正是 storage image + compute 写 RGBA8, 同构)
 *
 * 若读回 (255,0,0,255) -> 改写 R8G8B8A8 方案成立, shim 侧已修好, OP 跳过必在别处;
 * 若 device lost / 读回全 0 -> storage 转换在 Adreno 上根本不可用, 方案需换.
 *
 * 编译: gcc -O2 -o disp_readback_probe disp_readback_probe.c -ldl
 * 运行: ./disp_readback_probe
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "disp_spv.inc"

typedef uint32_t VkBool32;
typedef void* (*gipa_t)(void*, const char*);
typedef int (*ci_t)(const void*, const void*, void**);
typedef int (*epd_t)(void*, uint32_t*, void**);
typedef int (*cd_t)(void*, const void*, const void*, void**);
typedef void (*dd_t)(void*, const void*);
typedef int (*ci_img_t)(void*, const void*, const void*, void**);
typedef void (*di_img_t)(void*, void*, const void*);
typedef int (*civ_t)(void*, const void*, const void*, void**);
typedef void (*divf_t)(void*, void*, const void*);
typedef int (*csmd_t)(void*, const void*, const void*, void**);
typedef void (*dsmd_t)(void*, void*, const void*);
typedef int (*cp_t)(void*, const void*, const void*, void**);
typedef void (*dp_t)(void*, void*, const void*);
typedef int (*cb_t)(void*, const void*, const void*, void**);
typedef void (*db_t)(void*, void*, const void*);
typedef int (*cmdp_t)(void*, const void*, const void*, void**);
typedef void (*cmdeb_t)(void*, void*, const void*);
typedef int (*cmdbi_t)(void*, const void*, uint32_t, const void*, void**);
typedef void (*cmdbif_t)(void*, void*, const void*);
typedef int (*begincmd_t)(void*, const void*, const void*, void*);
typedef int (*endcmd_t)(void*, const void*, void*);
typedef int (*qsb_t)(void*, void*, const void*);
typedef int (*dw_t)(void*, const void*);
typedef void (*allocmem_t)(void*, const void*, const void*, void**);
typedef void (*freemem_t)(void*, void*, const void*);
typedef int (*bindimg_t)(void*, void*, void*, uint64_t);
typedef int (*ci_buf_t)(void*, const void*, const void*, void**);
typedef int (*bindbuf_t)(void*, void*, void*, uint64_t);
typedef int (*bindpipeline_t)(void*, void*, uint32_t, void*);
typedef int (*binddesc_t)(void*, void*, uint32_t, void*, uint32_t, uint32_t, const void*, uint32_t, const void*);
typedef int (*dispatch_t)(void*, void*, uint32_t, uint32_t, uint32_t);
typedef int (*imgbarriercmd_t)(void*, void*, uint32_t, const void*);
typedef int (*bufcopycmd_t)(void*, void*, uint32_t, const void*);
typedef void (*vkDestroyBuffer_t)(void*, void*, const void*);
typedef int (*mapmem_t)(void*, void*, const void*, uint64_t, uint64_t, uint32_t, void**);
typedef void (*unmapmem_t)(void*, void*, void*);
typedef void (*upbuf_t)(void*, const void*, const void*, const void*);
typedef int (*getimgmemreq_t)(void*, void*, const void*, void*);

#define VK_IMAGE_TYPE_2D 1
#define VK_IMAGE_TILING_OPTIMAL 0
#define VK_IMAGE_USAGE_SAMPLED_BIT 0x4u
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT 0x2u
#define VK_IMAGE_USAGE_TRANSFER_SRC_BIT 0x1u
#define VK_IMAGE_USAGE_STORAGE_BIT 0x8u
#define VK_IMAGE_VIEW_TYPE_2D 1
#define VK_FORMAT_R8G8B8A8_UNORM 37
#define VK_IMAGE_ASPECT_COLOR_BIT 0x1u
#define VK_IMAGE_LAYOUT_GENERAL 0
#define VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL 5
#define VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT 0x00000800u
#define VK_PIPELINE_STAGE_TRANSFER_BIT 0x00000040u
#define VK_ACCESS_SHADER_WRITE_BIT 0x00000200u
#define VK_ACCESS_TRANSFER_READ_BIT 0x00000020u
#define VK_ACCESS_TRANSFER_WRITE_BIT 0x00000040u
#define VK_BUFFER_USAGE_TRANSFER_DST_BIT 0x2u
#define VK_SHARING_MODE_EXCLUSIVE 0
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT 0x2u
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 0x4u
#define VK_DESCRIPTOR_TYPE_STORAGE_IMAGE 0x00000005u
#define VK_SHADER_STAGE_COMPUTE_BIT 0x00000020u
#define VK_PIPELINE_BIND_POINT_COMPUTE 1u
#define VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO 0x15u
#define VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO 0x17u
#define VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO 0x18u
#define VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO 0x21u
#define VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO 0x22u
#define VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO 0x25u
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO 0x26u
#define VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO 0x29u
#define VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET 0x2bu
#define VK_STRUCTURE_TYPE_DESCRIPTOR_IMAGE_INFO 0x21u /* actually 0x21? use correct */
#define VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO 0x28u
#define VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO 0x19u
#define VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO 0xbu
#define VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO 0x13u
#define VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER 0x33u
#define VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY 0x3au
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO 0x30u
#define VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER 0x36u

static int test_r8g8b8a8(gipa_t gipa, void* inst, void* pd, void* dev) {
    printf("\n=== 测试 R8G8B8A8_UNORM storage image compute 写入 + 读回 ===\n");
    ci_img_t vkCreateImage = (ci_img_t)gipa(inst, "vkCreateImage");
    di_img_t vkDestroyImage = (di_img_t)gipa(inst, "vkDestroyImage");
    allocmem_t vkAllocateMemory = (allocmem_t)gipa(inst, "vkAllocateMemory");
    freemem_t vkFreeMemory = (freemem_t)gipa(inst, "vkFreeMemory");
    bindimg_t vkBindImageMemory = (bindimg_t)gipa(inst, "vkBindImageMemory");
    civ_t vkCreateImageView = (civ_t)gipa(inst, "vkCreateImageView");
    divf_t vkDestroyImageView = (divf_t)gipa(inst, "vkDestroyImageView");
    csmd_t vkCreateShaderModule = (csmd_t)gipa(inst, "vkCreateShaderModule");
    dsmd_t vkDestroyShaderModule = (dsmd_t)gipa(inst, "vkDestroyShaderModule");
    cp_t vkCreatePipelineLayout = (cp_t)gipa(inst, "vkCreatePipelineLayout");
    dp_t vkDestroyPipelineLayout = (dp_t)gipa(inst, "vkDestroyPipelineLayout");
    cb_t vkCreateComputePipelines = (cb_t)gipa(inst, "vkCreateComputePipelines");
    db_t vkDestroyPipeline = (db_t)gipa(inst, "vkDestroyPipeline");
    cmdp_t vkCreateCommandPool = (cmdp_t)gipa(inst, "vkCreateCommandPool");
    cmdeb_t vkDestroyCommandPool = (cmdeb_t)gipa(inst, "vkDestroyCommandPool");
    cmdbi_t vkAllocateCommandBuffers = (cmdbi_t)gipa(inst, "vkAllocateCommandBuffers");
    cmdbif_t vkFreeCommandBuffers = (cmdbif_t)gipa(inst, "vkFreeCommandBuffers");
    begincmd_t vkBeginCommandBuffer = (begincmd_t)gipa(inst, "vkBeginCommandBuffer");
    endcmd_t vkEndCommandBuffer = (endcmd_t)gipa(inst, "vkEndCommandBuffer");
    qsb_t vkQueueSubmit = (qsb_t)gipa(inst, "vkQueueSubmit");
    dw_t vkDeviceWaitIdle = (dw_t)gipa(inst, "vkDeviceWaitIdle");
    ci_buf_t vkCreateBuffer = (ci_buf_t)gipa(inst, "vkCreateBuffer");
    vkDestroyBuffer_t vkDestroyBuffer = (vkDestroyBuffer_t)gipa(inst, "vkDestroyBuffer");
    bindbuf_t vkBindBufferMemory = (bindbuf_t)gipa(inst, "vkBindBufferMemory");
    mapmem_t vkMapMemory = (mapmem_t)gipa(inst, "vkMapMemory");
    unmapmem_t vkUnmapMemory = (unmapmem_t)gipa(inst, "vkUnmapMemory");
    bindpipeline_t vkCmdBindPipeline = (bindpipeline_t)gipa(inst, "vkCmdBindPipeline");
    binddesc_t vkCmdBindDescriptorSets = (binddesc_t)gipa(inst, "vkCmdBindDescriptorSets");
    dispatch_t vkCmdDispatch = (dispatch_t)gipa(inst, "vkCmdDispatch");
    imgbarriercmd_t vkCmdPipelineBarrier = (imgbarriercmd_t)gipa(inst, "vkCmdPipelineBarrier");
    bufcopycmd_t vkCmdCopyImageToBuffer = (bufcopycmd_t)gipa(inst, "vkCmdCopyImageToBuffer");
    getimgmemreq_t vkGetImageMemoryRequirements = (getimgmemreq_t)gipa(inst, "vkGetImageMemoryRequirements");

    int W = 64, H = 64;
    /* image */
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t imageType; uint32_t format;
             struct { uint32_t w, h, d; } extent; uint32_t mipLevels, arrayLayers, samples, tiling, usage, sharingMode;
             uint32_t queueFamilyCount; const uint32_t* pQueueFamilyIndices; uint32_t initialLayout; } ii = {0};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.w = W; ii.extent.h = H; ii.extent.d = 1; ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = 1;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_STORAGE_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    void* img = NULL; int cr = vkCreateImage(dev, &ii, NULL, &img);
    if (cr != 0) { printf("  vkCreateImage -> %d\n", cr); return -1; }

    struct { uint32_t sType; void* pNext; uint64_t size; uint32_t memTypeCount; const void* pMemTypes; } mri = {0};
    mri.sType = 0x23u; /* GetImageMemoryRequirementsInfo2? use 0x23 fallback */ mri.sType = 0x23u;
    /* 直接用 vkGetImageMemoryRequirements 旧结构 0x16? 简化: 申请 4MB type 0 */
    struct { uint32_t sType; void* pNext; uint64_t size; uint32_t memTypeIndex; const void* pMappedData; } mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mai.allocationSize = 4*1024*1024; mai.memTypeIndex = 0;
    void* mem = NULL; int amr = vkAllocateMemory(dev, &mai, NULL, &mem);
    if (amr != 0) { printf("  vkAllocateMemory(type0) -> %d (类型0可能不可用)\n", amr); vkDestroyImage(dev, img, NULL); return -1; }
    int bmr = vkBindImageMemory(dev, img, mem, 0);
    if (bmr != 0) { printf("  vkBindImageMemory -> %d\n", bmr); goto fail_img; }

    struct { uint32_t sType; const void* pNext; uint32_t flags; void* image; uint32_t viewType; uint32_t format;
             struct { uint32_t aspectMask, baseMip, levelCount, baseLayer, layerCount; } comp;
             struct { uint32_t r,g,b,a; } swizzle; } ivi = {0};
    ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; ivi.image = img; ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = VK_FORMAT_R8G8B8A8_UNORM; ivi.comp.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ivi.comp.levelCount = 1; ivi.comp.layerCount = 1;
    ivi.swizzle.r = 0; ivi.swizzle.g = 1; ivi.swizzle.b = 2; ivi.swizzle.a = 3;
    void* iv = NULL; int vr = vkCreateImageView(dev, &ivi, NULL, &iv);
    if (vr != 0) { printf("  vkCreateImageView -> %d\n", vr); goto fail_img; }

    struct { uint32_t sType; const void* pNext; const uint32_t* code; uint32_t