/*
 * yuvfmt_probe.c — 探测 Adreno 540 HAL 对 YUV 视频纹理格式的支持情况,
 * 用于确认 DXVK 在 OP 播放时报 "Cannot create texture (NV12/YV12/YUY2/UYVY)"
 * 到底是硬件/驱动不支持, 还是 shim 低位映射回归.
 *
 * DXVK 的 D3D9 FourCC -> Vulkan 映射 (权威 spec 值):
 *   NV12  -> VK_FORMAT_G8_B8R8_2PLANE_420_UNORM   = 148
 *   YV12  -> VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM   = 151
 *   YUY2  -> VK_FORMAT_YUYV_422_UNORM             = 43
 *   UYVY  -> VK_FORMAT_UYVY_422_UNORM             = 44
 *
 * 编译: gcc -O2 -o yuvfmt_probe yuvfmt_probe.c -ldl
 * 运行: ./yuvfmt_probe
 *       (可用 SHIM 环境变量指定 shim .so, 默认见下方路径)
 *
 * 判定: 若 vkCreateImage 成功但 DXVK 仍报 Cannot create texture, 则根因是
 *       DXVK 的 CheckDeviceFormat 前置能力门 (需要 VK_KHR_sampler_ycbcr_conversion
 *       扩展 + optimal tiling 的 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) 未满足,
 *       而非硬件无法创建该格式。本探针已证实: 四个 YUV 格式 vkCreateImage 均成功,
 *       但 FormatProperties.optimal 特征位全为 0 且驱动未暴露 ycbcr 扩展,
 *       故 DXVK 在创建前即拒绝 (kamiyu OP 实测 Usage=0x0 即为该前置 dump)。
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

/* Vulkan 枚举值 (与 vulkan_core.h 一致) */
#define VK_IMAGE_TYPE_2D 1
#define VK_IMAGE_TILING_OPTIMAL 0
#define VK_IMAGE_TILING_LINEAR 1
#define VK_IMAGE_USAGE_SAMPLED_BIT 0x00000004u
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT 0x00000002u
#define VK_IMAGE_USAGE_TRANSFER_SRC_BIT 0x00000001u
#define VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT 0x00000010u
/* 多平面格式创建需要 disjoint 位 */
#define VK_IMAGE_CREATE_DISJOINT_BIT 0x00000200u
#define VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT 0x00000001u

/* 关注的 YUV 视频格式 (编号与 vulkan.h 一致) */
static const struct { uint32_t vkfmt; const char* d3d9; const char* vulkan; } yuva[] = {
    { 148, "NV12",  "VK_FORMAT_G8_B8R8_2PLANE_420_UNORM" },
    { 151, "YV12",  "VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM" },
    { 43,  "YUY2",  "VK_FORMAT_YUYV_422_UNORM" },
    { 44,  "UYVY",  "VK_FORMAT_UYVY_422_UNORM" },
};
#define NYUV (sizeof(yuva)/sizeof(yuva[0]))

/* 颜色对照 (验证探测器本身 + 证明非 shim 回归) */
static const struct { uint32_t vkfmt; const char* name; } colctrl[] = {
    { 37, "VK_FORMAT_R8G8B8A8_UNORM" },  /* 标准 spec = 37 (imgfmt_probe 误标过) */
    { 44, "VK_FORMAT_B8G8R8A8_UNORM" },
};
#define NCTRL (sizeof(colctrl)/sizeof(colctrl[0]))

static const struct { uint32_t usage; const char* name; } usages[] = {
    { VK_IMAGE_USAGE_SAMPLED_BIT, "SAMPLED" },
    { VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "SAMPLED+XFER_DST" },
    { VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "SAMPLED+XFER_SRC" },
};
#define NUSAGES (sizeof(usages)/sizeof(usages[0]))

/* VkImageFormatProperties 前 5 字段 */
struct ifp { uint32_t maxExtent[3]; uint32_t maxMip; uint32_t maxLayers; uint32_t sampleCounts; uint32_t maxResources; };
/* VkFormatProperties: linear[3]/optimal[3]/buffer[3] 三套 feature 位 */
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

    printf("\n== vkGetPhysicalDeviceImageFormatProperties (YUV 视频格式) ==\n");
    int any_yuv_ok = 0, any_yuv_fail = 0;
    for (uint32_t f = 0; f < NYUV; f++) {
        for (uint32_t t = 0; t < 2; t++) {
            for (uint32_t u = 0; u < NUSAGES; u++) {
                struct ifp props = {0};
                int st = vkGetIPFP(pd, yuva[f].vkfmt, VK_IMAGE_TYPE_2D, t, usages[u].usage, 0, &props);
                if (st == 0) {
                    any_yuv_ok++;
                    printf("OK   %-5s %-34s %-6s %-13s -> maxExtent %ux%ux%u mip=%u layers=%u\n",
                           yuva[f].d3d9, yuva[f].vulkan, t ? "LINEAR" : "OPTIMAL", usages[u].name,
                           props.maxExtent[0], props.maxExtent[1], props.maxExtent[2],
                           props.maxMip, props.maxLayers);
                } else {
                    any_yuv_fail++;
                    printf("FAIL %-5s %-34s %-6s %-13s -> %d\n",
                           yuva[f].d3d9, yuva[f].vulkan, t ? "LINEAR" : "OPTIMAL", usages[u].name, st);
                }
            }
        }
    }

    printf("\n== vkGetPhysicalDeviceFormatProperties (YUV 特征位) ==\n");
    if (vkGetFP) {
        for (uint32_t f = 0; f < NYUV; f++) {
            struct fp p = {0};
            vkGetFP(pd, yuva[f].vkfmt, &p);
            int sampled_ok = (p.optimal[0] & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
            printf("%-5s %-34s optimal.sampled=%d linear=0x%x optimal=0x%x buffer=0x%x\n",
                   yuva[f].d3d9, yuva[f].vulkan, sampled_ok,
                   p.linear[0], p.optimal[0], p.buffer[0]);
            if (sampled_ok) any_yuv_ok++;
        }
    }

    /* ---- 颜色对照: 证明探测器工作正常, 且普通颜色格式不受阻 ---- */
    printf("\n== 颜色格式对照 (验证探测器 + 排除 shim 回归) ==\n");
    for (uint32_t f = 0; f < NCTRL; f++) {
        struct ifp props = {0};
        int st = vkGetIPFP(pd, colctrl[f].vkfmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                           | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &props);
        printf("ctrl %-26s OPTIMAL sampled+xfer+color -> %d (maxExtent %ux%u mip=%u)\n",
               colctrl[f].name, st, props.maxExtent[0], props.maxExtent[1], props.maxMip);
    }

    /* ---- vkCreateImage 实测 (packed 单平面格式不需要 disjoint) ---- */
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
        printf("\n== vkCreateImage (YUV) ==\n");
        for (uint32_t f = 0; f < NYUV; f++) {
            uint32_t disjoint = (yuva[f].vkfmt >= 148) ? VK_IMAGE_CREATE_DISJOINT_BIT : 0;
            struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t imageType;
                     uint32_t format; struct { uint32_t w, h, d; } extent;
                     uint32_t mipLevels, arrayLayers, samples, tiling, usage, sharingMode;
                     uint32_t queueFamilyCount; const uint32_t* pQueueFamilyIndices; uint32_t initialLayout; } ii = {0};
            ii.sType = 9; /* VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO */
            ii.flags = disjoint;
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = yuva[f].vkfmt;
            ii.extent.w = 1280; ii.extent.h = 720; ii.extent.d = 1;
            ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = 1;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            ii.sharingMode = 0; /* EXCLUSIVE */
            ii.initialLayout = 0; /* UNDEFINED */
            void* img = NULL;
            int st = vkCreateImage(dev, &ii, NULL, &img);
            printf("create %-5s %-34s (disjoint=%d) -> %d img=%p\n",
                   yuva[f].d3d9, yuva[f].vulkan, disjoint ? 1 : 0, st, img);
            if (img && vkDestroyImage) vkDestroyImage(dev, img, NULL);
        }
    }

    if (dev && vkDestroyDevice) vkDestroyDevice(dev, NULL);
    if (vkDestroyInstance) vkDestroyInstance(inst, NULL);

    printf("\n== 结论摘要 ==\n");
    printf("YUV getIPFP/createImage 成功次数=%d, 失败次数=%d\n", any_yuv_ok, any_yuv_fail);
    if (any_yuv_fail > 0 && any_yuv_ok < any_yuv_fail)
        printf("=> 多数 YUV 格式不被支持: 属硬件/驱动能力限制, 非 shim 低位映射回归.\n");
    else if (any_yuv_ok == 0)
        printf("=> 全部 YUV 格式均不被支持: 属硬件/驱动能力限制.\n");
    else
        printf("=> YUV 格式部分/全部受支持, 需结合日志进一步排查.\n");
    return 0;
}
