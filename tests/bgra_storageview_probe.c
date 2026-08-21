/*
 * bgra_storageview_probe.c — 验证 Adreno 540 能否给 B8G8R8A8 创建
 *   STORAGE 用途的 image view (DXVK-Sarek YUV→RGB compute 转换的目标)。
 *
 * 关键: vkCreateImage 对 storage 格式校验宽松, 但 vkCreateImageView(STORAGE)
 * 才会真正检查该格式是否在硬件 storage 格式白名单中. Adreno 540 的
 * shaderStorageImageExtendedFormats=0, 通常只支持白名单格式, B8G8R8A8 可能
 * 不在其中 -> 这一步才是 OP 视频真正卡住的位置 (之前只测了 createImage 成功)。
 *
 * 编译: gcc -O2 -o bgra_storageview_probe bgra_storageview_probe.c -ldl
 * 运行: ./bgra_storageview_probe   (SHIM 可指定 shim .so)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

typedef uint32_t VkBool32;
typedef void* (*getip_t)(void*, const char*);
typedef int (*create_inst_t)(const void*, const void*, void**);
typedef int (*enum_pd_t)(void*, uint32_t*, void**);
typedef int (*create_dev_t)(void*, const void*, const void*, void**);
typedef void (*destroy_dev_t)(void*, const void*);
typedef void (*destroy_inst_t)(void*, const void*);
typedef int (*create_img_t)(void*, const void*, const void*, void**);
typedef void (*destroy_img_t)(void*, void*, const void*);
typedef int (*create_iv_t)(void*, const void*, const void*, void**);
typedef void (*destroy_iv_t)(void*, void*, const void*);

#define VK_IMAGE_TYPE_2D 1
#define VK_IMAGE_TILING_OPTIMAL 0
#define VK_IMAGE_USAGE_SAMPLED_BIT 0x00000004u
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT 0x00000002u
#define VK_IMAGE_USAGE_STORAGE_BIT 0x00000008u
#define VK_IMAGE_VIEW_TYPE_2D 1
#define VK_FORMAT_B8G8R8A8_UNORM 44
#define VK_FORMAT_R8G8B8A8_UNORM 37
#define VK_IMAGE_ASPECT_COLOR_BIT 0x00000001u

static void try_storage_view(getip_t gip, void* inst, void* dev, uint32_t fmt, const char* name) {
    /* create image */
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t imageType;
             uint32_t format; struct { uint32_t w, h, d; } extent;
             uint32_t mipLevels, arrayLayers, samples, tiling, usage, sharingMode;
             uint32_t queueFamilyCount; const uint32_t* pQueueFamilyIndices; uint32_t initialLayout; } ii = {0};
    ii.sType = 9; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent.w = 1280; ii.extent.h = 720; ii.extent.d = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = 1;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    ii.sharingMode = 0; ii.initialLayout = 0;
    create_img_t vkCreateImage = (create_img_t)gip(inst, "vkCreateImage");
    destroy_img_t vkDestroyImage = (destroy_img_t)gip(inst, "vkDestroyImage");
    void* img = NULL;
    int cr = vkCreateImage(dev, &ii, NULL, &img);
    if (cr != 0) { printf("  %s: vkCreateImage -> %d (跳过 view)\n", name, cr); return; }

    create_iv_t vkCreateImageView = (create_iv_t)gip(inst, "vkCreateImageView");
    destroy_iv_t vkDestroyImageView = (destroy_iv_t)gip(inst, "vkDestroyImageView");
    struct { uint32_t sType; const void* pNext; uint32_t flags;
             void* image; uint32_t viewType; uint32_t format;
             struct { uint32_t aspectMask, baseMip, levelCount, baseLayer, layerCount; } comp;
             struct { uint32_t r,g,b,a; } swizzle; } ivi = {0};
    ivi.sType = 19; /* VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO */
    ivi.image = img;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = fmt;
    ivi.comp.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivi.comp.levelCount = 1;
    ivi.comp.layerCount = 1;
    ivi.swizzle.r = 0; ivi.swizzle.g = 1; ivi.swizzle.b = 2; ivi.swizzle.a = 3; /* IDENTITY */
    void* iv = NULL;
    int vr = vkCreateImageView(dev, &ivi, NULL, &iv);
    printf("  %s (fmt=%u): vkCreateImage=%d, vkCreateImageView(STORAGE)=%d (%s)\n",
           name, fmt, cr, vr, vr==0 ? "OK" : "FAIL -- 这才是 OP 真正卡点");
    if (iv && vkDestroyImageView) vkDestroyImageView(dev, iv, NULL);
    if (img && vkDestroyImage) vkDestroyImage(dev, img, NULL);
}

int main(void) {
    const char* shim_path = getenv("SHIM") ? getenv("SHIM") :
        "/data/data/com.termux/files/home/proton11/.build/vulkan_adreno_icd.so";
    void* h = dlopen(shim_path, RTLD_NOW);
    if (!h) { printf("dlopen shim 失败: %s\n", dlerror()); return 1; }
    getip_t gip = (getip_t)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gip) { printf("找不到 vk_icdGetInstanceProcAddr\n"); return 1; }
    create_inst_t vkCreateInstance = (create_inst_t)gip(NULL, "vkCreateInstance");
    enum_pd_t vkEnumeratePhysicalDevices = (enum_pd_t)gip(NULL, "vkEnumeratePhysicalDevices");
    struct { uint32_t sType; const void* pNext; uint32_t flags;
             const void* pAppInfo; uint32_t layerCount; const char** ppLayerNames;
             uint32_t extCount; const char** ppExt; } ici = {0};
    ici.sType = 7;
    void* inst = NULL; int r = vkCreateInstance(&ici, NULL, &inst);
    printf("vkCreateInstance -> %d\n", r);
    void* pd = NULL; uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);

    float prio = 1.0f;
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t family; uint32_t count; float* p; } qci = {0};
    qci.sType = 4; qci.family = 0; qci.count = 1; qci.p = &prio;
    struct { uint32_t sType; const void* pNext; uint32_t flags;
             uint32_t qcount; const void* q;
             uint32_t layerCount; const char** ppLayerNames;
             uint32_t extCount; const char** exts; const VkBool32* feats; } dci = {0};
    dci.sType = 3; dci.qcount = 1; dci.q = &qci;
    create_dev_t vkCreateDevice = (create_dev_t)gip(NULL, "vkCreateDevice");
    void* dev = NULL;
    vkCreateDevice(pd, &dci, NULL, &dev);
    printf("vkCreateDevice -> %d dev=%p\n", (dev!=NULL), dev);

    if (dev) {
        printf("\n== 探测 storage image view 创建 (DXVK YUV 转换目标) ==\n");
        try_storage_view(gip, inst, dev, VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM");
        try_storage_view(gip, inst, dev, VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM");
    }
    void (*vkDestroyDevice)(void*, const void*) = (void(*)(void*,const void*))gip(inst, "vkDestroyDevice");
    void (*vkDestroyInstance)(void*, const void*) = (void(*)(void*,const void*))gip(NULL, "vkDestroyInstance");
    if (dev && vkDestroyDevice) vkDestroyDevice(dev, NULL);
    if (vkDestroyInstance) vkDestroyInstance(inst, NULL);
    return 0;
}
