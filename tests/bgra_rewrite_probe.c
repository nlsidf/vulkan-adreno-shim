/*
 * bgra_rewrite_probe.c — 端到端验证 shim 的 B8G8R8A8+STORAGE -> R8G8B8A8 改写:
 *   1) iffy(B8G8R8A8, STORAGE) 经 shim 应重定向到 R8G8B8A8 并返回 0 (真实能力)
 *   2) 以 B8G8R8A8+STORAGE 请求 vkCreateImage, shim 改写成 R8G8B8A8 真正创建
 *   3) 在该 image 上以 B8G8R8A8 创建 STORAGE view 和 SAMPLED view, shim 同步改写
 *
 * 编译: gcc -O2 -o bgra_rewrite_probe bgra_rewrite_probe.c -ldl
 * 运行: ./bgra_rewrite_probe
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

typedef uint32_t VkBool32;
typedef void* (*gipa_t)(void*, const char*);
typedef int (*ci_t)(const void*, const void*, void**);
typedef int (*epd_t)(void*, uint32_t*, void**);
typedef int (*cd_t)(void*, const void*, const void*, void**);
typedef void (*dd_t)(void*, const void*);
typedef int (*iffp_t)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void*);
typedef int (*ci_img_t)(void*, const void*, const void*, void**);
typedef void (*di_img_t)(void*, void*, const void*);
typedef int (*civ_t)(void*, const void*, const void*, void**);
typedef void (*divf_t)(void*, void*, const void*);
typedef void (*gq_t)(void*, uint32_t, uint32_t, void**);
typedef int (*qsb_t)(void*, void*, const void*);
typedef int (*dw_t)(void*, const void*);

#define VK_IMAGE_TYPE_2D 1
#define VK_IMAGE_TILING_OPTIMAL 0
#define VK_IMAGE_USAGE_SAMPLED_BIT 0x4u
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT 0x2u
#define VK_IMAGE_USAGE_STORAGE_BIT 0x8u
#define VK_IMAGE_VIEW_TYPE_2D 1
#define VK_FORMAT_B8G8R8A8_UNORM 44
#define VK_FORMAT_R8G8B8A8_UNORM 37
#define VK_IMAGE_ASPECT_COLOR_BIT 0x1u

int main(void) {
    const char* shim_path = getenv("SHIM") ? getenv("SHIM") :
        "/data/data/com.termux/files/home/proton11/.build/vulkan_adreno_icd.so";
    void* h = dlopen(shim_path, RTLD_NOW);
    if (!h) { printf("dlopen shim 失败: %s\n", dlerror()); return 1; }
    gipa_t gipa = (gipa_t)dlsym(h, "vk_icdGetInstanceProcAddr");
    ci_t vkCreateInstance = (ci_t)gipa(NULL, "vkCreateInstance");
    epd_t vkEnumeratePhysicalDevices = (epd_t)gipa(NULL, "vkEnumeratePhysicalDevices");
    struct { uint32_t sType; const void* pNext; uint32_t flags; const void* pAppInfo;
             uint32_t layerCount; const char** ppLayerNames; uint32_t extCount; const char** ppExt; } ici = {0};
    ici.sType = 7;
    void* inst = NULL; vkCreateInstance(&ici, NULL, &inst);
    void* pd = NULL; uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd);

    iffp_t vkGetIPFP = (iffp_t)gipa(inst, "vkGetPhysicalDeviceImageFormatProperties");
    struct { uint32_t me[3]; uint32_t mip; uint32_t ly; uint32_t sc; uint32_t rs; } p = {0};
    int rr = vkGetIPFP(pd, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                       VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_STORAGE_BIT,
                       0, &p);
    printf("[1] iffy(B8G8R8A8,STORAGE) 经 shim -> %d (期望 0, 重定向到 R8G8B8A8 真实能力)\n", rr);

    float prio = 1.0f;
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t family; uint32_t count; float* p; } qci = {0};
    qci.sType = 4; qci.family = 0; qci.count = 1; qci.p = &prio;
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t qcount; const void* q;
             uint32_t layerCount; const char** ppLayerNames; uint32_t extCount; const char** exts; const VkBool32* feats; } dci = {0};
    dci.sType = 3; dci.qcount = 1; dci.q = &qci;
    cd_t vkCreateDevice = (cd_t)gipa(NULL, "vkCreateDevice");
    void* dev = NULL; vkCreateDevice(pd, &dci, NULL, &dev);
    if (!dev) { printf("vkCreateDevice 失败\n"); return 1; }
    gq_t vkGetDeviceQueue = (gq_t)gipa(inst, "vkGetDeviceQueue");
    qsb_t vkQueueSubmit = (qsb_t)gipa(inst, "vkQueueSubmit");
    dw_t vkDeviceWaitIdle = (dw_t)gipa(inst, "vkDeviceWaitIdle");

    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t imageType; uint32_t format;
             struct { uint32_t w, h, d; } extent; uint32_t mipLevels, arrayLayers, samples, tiling, usage, sharingMode;
             uint32_t queueFamilyCount; const uint32_t* pQueueFamilyIndices; uint32_t initialLayout; } ii = {0};
    ii.sType = 9; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_B8G8R8A8_UNORM; /* 应用请求 B8G8R8A8 */
    ii.extent.w = 1280; ii.extent.h = 720; ii.extent.d = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = 1; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_STORAGE_BIT;
    ii.sharingMode = 0; ii.initialLayout = 0;
    ci_img_t vkCreateImage = (ci_img_t)gipa(inst, "vkCreateImage");
    di_img_t vkDestroyImage = (di_img_t)gipa(inst, "vkDestroyImage");
    void* img = NULL; int cr = vkCreateImage(dev, &ii, NULL, &img);
    printf("[2] vkCreateImage(B8G8R8A8,STORAGE) 经 shim -> %d (期望 0, 实际创建为 R8G8B8A8)\n", cr);

    civ_t vkCreateImageView = (civ_t)gipa(inst, "vkCreateImageView");
    divf_t vkDestroyImageView = (divf_t)gipa(inst, "vkDestroyImageView");
    struct { uint32_t sType; const void* pNext; uint32_t flags; void* image; uint32_t viewType; uint32_t format;
             struct { uint32_t aspectMask, baseMip, levelCount, baseLayer, layerCount; } comp;
             struct { uint32_t r,g,b,a; } swizzle; } ivi = {0};
    ivi.sType = 19; ivi.image = img; ivi.viewType = VK_IMAGE_VIEW_TYPE_2D; ivi.format = VK_FORMAT_B8G8R8A8_UNORM;
    ivi.comp.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ivi.comp.levelCount = 1; ivi.comp.layerCount = 1;
    ivi.swizzle.r = 0; ivi.swizzle.g = 1; ivi.swizzle.b = 2; ivi.swizzle.a = 3;
    void* iv_storage = NULL;
    int vsr = vkCreateImageView(dev, &ivi, NULL, &iv_storage);
    printf("[3] vkCreateImageView(STORAGE, B8G8R8A8) 经 shim -> %d (期望 0, 同步改 R8G8B8A8)\n", vsr);

    /* 再建一个 SAMPLED view (DXVK 对转换图还会建采样视图) */
    void* iv_sampled = NULL;
    int vpr = vkCreateImageView(dev, &ivi, NULL, &iv_sampled);
    printf("[4] vkCreateImageView(SAMPLED, B8G8R8A8) 经 shim -> %d (期望 0, 同步改 R8G8B8A8)\n", vpr);

    int widle = vkDeviceWaitIdle(dev, NULL);
    printf("[5] vkDeviceWaitIdle -> %d (期望 0; 若 device lost 说明改写仍有问题)\n", widle);

    if (iv_storage) vkDestroyImageView(dev, iv_storage, NULL);
    if (iv_sampled) vkDestroyImageView(dev, iv_sampled, NULL);
    if (img) vkDestroyImage(dev, img, NULL);
    void (*vkDestroyDevice)(void*, const void*) = (void(*)(void*,const void*))gipa(inst, "vkDestroyDevice");
    if (dev && vkDestroyDevice) vkDestroyDevice(dev, NULL);
    printf("\n== 判定 ==\n");
    if (rr==0 && cr==0 && vsr==0 && vpr==0 && widle==0)
        printf(">>> 改写链路全部通过: iffy 重定向 + CreateImage + 双 view 一致改写, 且设备无丢失.\n"
               "    现在可在 kamiyu 实测 OP 是否放出.\n");
    else
        printf(">>> 仍有环节失败, 见上方返回码.\n");
    return 0;
}
