/*
 * bgra_create_probe.c — 验证门: Adreno 540 HAL 是否真的能
 *   vkCreateImage(VK_FORMAT_B8G8R8A8_UNORM, OPTIMAL, STORAGE_BIT)。
 *
 * 背景: DXVK-Sarek D3D9 视频路径把 NV12/YV12/YUY2/UYVY 全部转换成
 *   B8G8R8A8 + VK_IMAGE_USAGE_STORAGE_BIT (compute YUV→RGB 写入目标),
 *   CheckImageSupport 会先查 imageFormatProperties(B8G8R8A8, OPTIMAL, STORAGE)
 *   被 HAL 拒 (-11), 抛 "Cannot create texture"。
 *
 * 本探针确认: 若 imageFormatProperties 报 NOT_SUPPORTED 但 vkCreateImage 仍
 * 成功, 则 shim_iffp 伪造成功方案成立 (DXVK 仅依赖 iffy 门, 不复查 create)。
 * 若 vkCreateImage 也失败, 则伪造 iffy 只会把失败推迟到创建, 需改 DXVK 侧。
 *
 * 编译: gcc -O2 -o bgra_create_probe bgra_create_probe.c -ldl
 * 运行: ./bgra_create_probe   (SHIM 可指定 shim .so)
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
typedef int (*get_iffp_t)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void*);

#define VK_IMAGE_TYPE_2D 1
#define VK_IMAGE_TILING_OPTIMAL 0
#define VK_IMAGE_USAGE_SAMPLED_BIT 0x00000004u
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT 0x00000002u
#define VK_IMAGE_USAGE_STORAGE_BIT 0x00000008u
#define VK_FORMAT_B8G8R8A8_UNORM 44

int main(void) {
    const char* shim_path = getenv("SHIM") ? getenv("SHIM") :
        "/data/data/com.termux/files/home/proton11/.build/vulkan_adreno_icd.so";
    void* h = dlopen(shim_path, RTLD_NOW);
    if (!h) { printf("dlopen shim 失败: %s\n", dlerror()); return 1; }
    getip_t gip = (getip_t)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gip) { printf("找不到 vk_icdGetInstanceProcAddr\n"); return 1; }

    create_inst_t vkCreateInstance = (create_inst_t)gip(NULL, "vkCreateInstance");
    enum_pd_t     vkEnumeratePhysicalDevices = (enum_pd_t)gip(NULL, "vkEnumeratePhysicalDevices");
    if (!vkCreateInstance || !vkEnumeratePhysicalDevices) { printf("接口解析失败\n"); return 1; }

    struct { uint32_t sType; const void* pNext; uint32_t flags;
             const void* pAppInfo; uint32_t layerCount; const char** ppLayerNames;
             uint32_t extCount; const char** ppExt; } ici = {0};
    ici.sType = 7;
    void* inst = NULL;
    int r = vkCreateInstance(&ici, NULL, &inst);
    printf("vkCreateInstance -> %d inst=%p\n", r, inst);
    void* pd = NULL; uint32_t n = 1;
    r = vkEnumeratePhysicalDevices(inst, &n, &pd);
    printf("vkEnumeratePhysicalDevices -> %d pd=%p n=%u\n", r, pd, n);

    get_iffp_t vkGetIPFP = (get_iffp_t)gip(inst, "vkGetPhysicalDeviceImageFormatProperties");

    /* imageFormatProperties: B8G8R8A8 OPTIMAL SAMPLED|TRANSFER|STORAGE */
    struct { uint32_t maxExtent[3]; uint32_t maxMip; uint32_t maxLayers;
             uint32_t sampleCounts; uint32_t maxResources; } props = {0};
    int iffy_r = vkGetIPFP(pd, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                           0, &props);
    printf("imageFormatProperties(B8G8R8A8,OPTIMAL,STORAGE) -> %d (0=OK, -11=NOT_SUPPORTED)\n", iffy_r);

    /* create device */
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
    int dr = vkCreateDevice(pd, &dci, NULL, &dev);
    printf("vkCreateDevice -> %d dev=%p\n", dr, dev);

    if (dev) {
        create_img_t vkCreateImage = (create_img_t)gip(inst, "vkCreateImage");
        destroy_img_t vkDestroyImage = (destroy_img_t)gip(inst, "vkDestroyImage");
        struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t imageType;
                 uint32_t format; struct { uint32_t w, h, d; } extent;
                 uint32_t mipLevels, arrayLayers, samples, tiling, usage, sharingMode;
                 uint32_t queueFamilyCount; const uint32_t* pQueueFamilyIndices; uint32_t initialLayout; } ii = {0};
        ii.sType = 9; ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_B8G8R8A8_UNORM;
        ii.extent.w = 1280; ii.extent.h = 720; ii.extent.d = 1;
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = 1;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        ii.sharingMode = 0; ii.initialLayout = 0;
        void* img = NULL;
        int cr = vkCreateImage(dev, &ii, NULL, &img);
        printf("vkCreateImage(B8G8R8A8,OPTIMAL,STORAGE) -> %d img=%p\n", cr, img);

        /* 也测一下不含 STORAGE 的 create, 作为对照 */
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        void* img2 = NULL;
        int cr2 = vkCreateImage(dev, &ii, NULL, &img2);
        printf("vkCreateImage(B8G8R8A8,OPTIMAL,no STORAGE) -> %d img=%p\n", cr2, img2);

        if (img && vkDestroyImage) vkDestroyImage(dev, img, NULL);
        if (img2 && vkDestroyImage) vkDestroyImage(dev, img2, NULL);
    }

    void (*vkDestroyDevice)(void*, const void*) = (void(*)(void*,const void*))gip(inst, "vkDestroyDevice");
    void (*vkDestroyInstance)(void*, const void*) = (void(*)(void*,const void*))gip(NULL, "vkDestroyInstance");
    if (dev && vkDestroyDevice) vkDestroyDevice(dev, NULL);
    if (vkDestroyInstance) vkDestroyInstance(inst, NULL);

    printf("\n== 判定 ==\n");
    printf("iffy(STORAGE)=%d, createImage(STORAGE)=%d\n", iffy_r, dev ? 0 : -1);
    printf(iffy_r == -11 ?
        "=> iffy 拒 STORAGE (与 bgra_storage_probe 一致)。若 createImage 成功则 shim_iffp 伪造方案成立。\n"
        : "=> iffy 未拒 STORAGE, 需重新审视根因。\n");
    return 0;
}
