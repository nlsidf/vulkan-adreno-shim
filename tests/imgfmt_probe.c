/*
 * imgfmt_probe.c — 直接探测 Adreno 540 HAL 对深度/模板格式的
 * vkGetPhysicalDeviceImageFormatProperties 支持情况, 排查 DXVK
 * "Cannot create texture (R32_FLOAT_X8X24_TYPELESS / D32_SFLOAT_S8_UINT)".
 *
 * 编译: gcc -O2 -o imgfmt_probe imgfmt_probe.c -ldl
 * 运行: LD_PRELOAD=$HOME/fake_machineid.so:$HOME/proton11/.build/vulkan_gpu.so \
 *       FAKE_MACHINE_ID=85536ceb47e8aa768973fe1c6a227604 ./imgfmt_probe
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

typedef uint32_t VkBool32;

typedef void* (*getip_t)(void*, const char*);
typedef int (*create_inst_t)(const void*, const void*, void**);
typedef int (*enum_pd_t)(void*, uint32_t*, void**);
typedef int (*destroy_inst_t)(void*, const void*);
typedef int (*get_iffp_t)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void*);
typedef void (*get_fmtp_t)(void*, uint32_t, void*);
typedef int (*create_dev_t)(void*, const void*, const void*, void**);
typedef void (*destroy_dev_t)(void*, const void*);
typedef int (*create_img_t)(void*, const void*, const void*, void**);
typedef void (*destroy_img_t)(void*, void*, const void*);
typedef int (*alloc_mem_t)(void*, const void*, const void*, void**);

/* Vulkan 枚举值 (与 vulkan.h 一致) */
#define VK_IMAGE_TYPE_2D 1
#define VK_IMAGE_TILING_OPTIMAL 0
#define VK_IMAGE_TILING_LINEAR 1
#define VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT 0x00000020u  /* 0x20, 不是 0x200! */
#define VK_IMAGE_USAGE_SAMPLED_BIT 0x00000004u
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT 0x00000002u
#define VK_IMAGE_USAGE_TRANSFER_SRC_BIT 0x00000001u

/* 颜色格式 (验证探测器本身 + 对照) */
static const struct { uint32_t vkfmt; const char* name; } colfmts[] = {
    { 44, "VK_FORMAT_R8G8B8A8_UNORM" },
    { 50, "VK_FORMAT_B8G8R8A8_UNORM" },
    { 37, "VK_FORMAT_R32G8X24_TYPELESS" },
    { 9,  "VK_FORMAT_R32G32B32A32_SFLOAT" },
};

/* 关注的深度格式 (编号与 vulkan.h 一致) */
static const struct { uint32_t vkfmt; const char* name; } fmts[] = {
    { 130, "VK_FORMAT_D32_SFLOAT_S8_UINT" },   /* D32_FLOAT_S8X24 家族 */
    { 126, "VK_FORMAT_D32_SFLOAT" },
    { 129, "VK_FORMAT_D24_UNORM_S8_UINT" },
    { 124, "VK_FORMAT_D16_UNORM" },
    { 128, "VK_FORMAT_D16_UNORM_S8_UINT" },
    { 37,  "VK_FORMAT_R32G8X24_TYPELESS" },    /* 若 HAL 直接暴露 typeless */
    { 38,  "VK_FORMAT_R32_FLOAT_X8X24_TYPELESS" },
    { 39,  "VK_FORMAT_X32_TYPELESS_G8X24_UINT" },
};
#define NFMTS (sizeof(fmts)/sizeof(fmts[0]))

static const struct { uint32_t usage; const char* name; } usages[] = {
    { VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, "DS_ATTACH" },
    { VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "DS+SAMPLED" },
    { VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "DS+XFER_DST" },
    { VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "DS+XFER_SRC" },
    { VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
      | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "DS_ALL" },
};
#define NUSAGES (sizeof(usages)/sizeof(usages[0]))

/* VkImageFormatProperties 前 5 个字段足以判断 */
struct ifp { uint32_t maxExtent[3]; uint32_t maxMip; uint32_t maxLayers; uint32_t sampleCounts; uint32_t maxResources; };
struct fp  { uint32_t linear[3]; uint32_t optimal[3]; uint32_t buffer[3]; };

int main(void) {
    const char* shim_path = getenv("SHIM") ? getenv("SHIM") :
        "/data/data/com.termux/files/home/proton11/.build/vulkan_adreno_icd.so";
    void* h = dlopen(shim_path, RTLD_NOW);
    if (!h) { printf("dlopen shim 失败: %s\n", dlerror()); return 1; }
    getip_t gip = (getip_t)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gip) { printf("找不到 vk_icdGetInstanceProcAddr\n"); return 1; }

    create_inst_t vkCreateInstance = (create_inst_t)gip(NULL, "vkCreateInstance");
    enum_pd_t     vkEnumeratePhysicalDevices = (enum_pd_t)gip(NULL, "vkEnumeratePhysicalDevices");
    destroy_inst_t vkDestroyInstance = (destroy_inst_t)gip(NULL, "vkDestroyInstance");
    if (!vkCreateInstance || !vkEnumeratePhysicalDevices) { printf("接口解析失败\n"); return 1; }

    struct { uint32_t sType; const void* pNext; uint32_t flags;
             const void* pAppInfo; uint32_t layerCount; const char** ppLayerNames;
             uint32_t extCount; const char** ppExt; } ici = {0};
    ici.sType = 7;
    void* inst = NULL;
    int r = vkCreateInstance(&ici, NULL, &inst);
    printf("vkCreateInstance -> %d inst=%p\n", r, inst);

    void* pd = NULL;
    uint32_t n = 1;
    r = vkEnumeratePhysicalDevices(inst, &n, &pd);
    printf("vkEnumeratePhysicalDevices -> %d pd=%p n=%u\n", r, pd, n);

    get_iffp_t vkGetIPFP = (get_iffp_t)gip(inst, "vkGetPhysicalDeviceImageFormatProperties");
    get_fmtp_t vkGetFP = (get_fmtp_t)gip(inst, "vkGetPhysicalDeviceFormatProperties");
    if (!vkGetIPFP) { printf("找不到 vkGetPhysicalDeviceImageFormatProperties\n"); return 1; }

    printf("\n== vkGetPhysicalDeviceImageFormatProperties (核心 1.0) ==\n");
    for (uint32_t f = 0; f < NFMTS; f++) {
        for (uint32_t t = 0; t < 2; t++) {
            for (uint32_t u = 0; u < NUSAGES; u++) {
                struct ifp props = {0};
                int st = vkGetIPFP(pd, fmts[f].vkfmt, VK_IMAGE_TYPE_2D, t, usages[u].usage, 0, &props);
                if (st == 0) {
                    printf("OK  %-28s %-6s %-9s -> maxExtent %ux%ux%u mip=%u layers=%u samples=0x%x\n",
                           fmts[f].name, t ? "LINEAR" : "OPTIMAL", usages[u].name,
                           props.maxExtent[0], props.maxExtent[1], props.maxExtent[2],
                           props.maxMip, props.maxLayers, props.sampleCounts);
                } else {
                    printf("FAIL %-28s %-6s %-9s -> %d\n",
                           fmts[f].name, t ? "LINEAR" : "OPTIMAL", usages[u].name, st);
                }
            }
        }
    }

    printf("\n== 颜色格式对照 (验证探测器) ==\n");
    for (uint32_t f = 0; f < sizeof(colfmts)/sizeof(colfmts[0]); f++) {
        struct ifp props = {0};
        int st = vkGetIPFP(pd, colfmts[f].vkfmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &props);
        printf("color %-26s OPTIMAL sampled+xfer -> %d (maxExtent %ux%u mip=%u)\n",
               colfmts[f].name, st, props.maxExtent[0], props.maxExtent[1], props.maxMip);
    }

    /* ---- 创建设备 + vkCreateImage 实测深度格式 ---- */
    float prio = 1.0f;
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t family; uint32_t count; float* p; } qci = {0};
    qci.sType = 4; qci.family = 0; qci.count = 1; qci.p = &prio;
    struct { uint32_t sType; const void* pNext; uint32_t flags;
             uint32_t qcount; const void* q;
             uint32_t layerCount; const char** ppLayerNames;
             uint32_t extCount; const char** exts; const VkBool32* feats; } dci = {0};
    dci.sType = 3; dci.qcount = 1; dci.q = &qci;

    create_dev_t vkCreateDevice = (create_dev_t)gip(NULL, "vkCreateDevice");
    destroy_dev_t vkDestroyDevice = (destroy_dev_t)gip(inst, "vkDestroyDevice");
    void* dev = NULL;
    int dr = vkCreateDevice(pd, &dci, NULL, &dev);
    printf("\n== vkCreateDevice -> %d dev=%p ==\n", dr, dev);

    create_img_t vkCreateImage = (create_img_t)gip(inst, "vkCreateImage");
    destroy_img_t vkDestroyImage = (destroy_img_t)gip(inst, "vkDestroyImage");
    if (dev && vkCreateImage) {
        static const uint32_t depth_fmts[] = {
            130 /*D32_SFLOAT_S8_UINT*/, 126 /*D32_SFLOAT*/, 129 /*D24_UNORM_S8_UINT*/, 124 /*D16_UNORM*/
        };
        for (uint32_t f = 0; f < 4; f++) {
            struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t imageType;
                     uint32_t format; struct { uint32_t w, h, d; } extent;
                     uint32_t mipLevels, arrayLayers, samples, tiling, usage, sharingMode;
                     uint32_t queueFamilyCount; const uint32_t* pQueueFamilyIndices; uint32_t initialLayout; } ii = {0};
            ii.sType = 9; /* VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO */
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = depth_fmts[f];
            ii.extent.w = 1280; ii.extent.h = 669; ii.extent.d = 1;
            ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = 1;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            ii.sharingMode = 0; /* EXCLUSIVE */
            ii.initialLayout = 0; /* UNDEFINED */
            void* img = NULL;
            int st = vkCreateImage(dev, &ii, NULL, &img);
            const char* names[] = {"D32_SFLOAT_S8_UINT", "D32_SFLOAT", "D24_UNORM_S8_UINT", "D16_UNORM"};
            printf("vkCreateImage %-18s -> %d img=%p\n", names[f], st, img);
            if (img && vkDestroyImage) vkDestroyImage(dev, img, NULL);
        }
    }
    if (dev && vkDestroyDevice) vkDestroyDevice(dev, NULL);

    if (vkGetFP) {
        printf("\n== vkGetPhysicalDeviceFormatProperties ==\n");
        for (uint32_t f = 0; f < NFMTS; f++) {
            struct fp p = {0};
            vkGetFP(pd, fmts[f].vkfmt, &p);
            printf("%-28s linear={%x,%x,%x} optimal={%x,%x,%x} buffer={%x,%x,%x}\n",
                   fmts[f].name, p.linear[0], p.linear[1], p.linear[2],
                   p.optimal[0], p.optimal[1], p.optimal[2], p.buffer[0], p.buffer[1], p.buffer[2]);
        }
    }

    if (vkDestroyInstance) vkDestroyInstance(inst, NULL);
    return 0;
}
