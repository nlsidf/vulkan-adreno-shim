/*
 * hal_bgra_storage_probe.c — 经真实 Vulkan loader (/system/lib64/libvulkan.so)
 *   创建实例, 直接问 Adreno 540 真实 HAL: B8G8R8A8 能否作为 STORAGE image.
 *
 * 绕过 shim: 用 libvulkan.so 的 vkCreateInstance (不经由我们的 ICD), 得到真实驱动
 *   的能力. 用于判断 OP 黑屏是否因为 B8G8R8A8 不在 storage 白名单
 *   (shaderStorageImageExtendedFormats=0).
 *
 * 编译: gcc -O2 -o hal_bgra_storage_probe hal_bgra_storage_probe.c -ldl
 * 运行: ./hal_bgra_storage_probe
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

typedef void* (*gipa_t)(void*, const char*);
typedef int (*ci_t)(const void*, const void*, void**);
typedef int (*epd_t)(void*, uint32_t*, void**);
typedef int (*iffp_t)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void*);
typedef gipa_t (*gipa2_t)(void*, const char*);

#define VK_IMAGE_TYPE_2D 1
#define VK_IMAGE_TILING_OPTIMAL 0
#define VK_IMAGE_USAGE_SAMPLED_BIT 0x4u
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT 0x2u
#define VK_IMAGE_USAGE_STORAGE_BIT 0x8u

static const uint32_t fmts[] = { 44 /*B8G8R8A8*/, 37 /*R8G8B8A8*/, 43 /*B8G8R8A8_SRGB*/, 50 /*R8G8B8A8_SRGB*/ };
static const char* fmtname[] = { "B8G8R8A8_UNORM", "R8G8B8A8_UNORM", "B8G8R8A8_SRGB", "R8G8B8A8_SRGB" };

int main(void) {
    void* h = dlopen("/system/lib64/libvulkan.so", RTLD_NOW);
    if (!h) { printf("dlopen libvulkan 失败: %s\n", dlerror()); return 1; }
    /* 用标准 vkGetInstanceProcAddr 表项 (libvulkan 导出 vkCreateInstance 等) */
    ci_t vkCreateInstance = (ci_t)dlsym(h, "vkCreateInstance");
    gipa2_t vkGetInstanceProcAddr = (gipa2_t)dlsym(h, "vkGetInstanceProcAddr");
    if (!vkCreateInstance) { printf("无 vkCreateInstance\n"); return 1; }

    struct { uint32_t sType; const void* pNext; uint32_t flags; const void* pAppInfo;
             uint32_t layerCount; const char** ppLayerNames; uint32_t extCount; const char** ppExt; } ici = {0};
    ici.sType = 7;
    void* inst = NULL; int r = vkCreateInstance(&ici, NULL, &inst);
    printf("vkCreateInstance(libvulkan/real HAL) -> %d inst=%p\n", r, inst);
    if (r != 0 || !inst) return 1;

    gipa_t gipa = (gipa_t)vkGetInstanceProcAddr(inst, "vkGetInstanceProcAddr");
    epd_t vkEnumeratePhysicalDevices = (epd_t)vkGetInstanceProcAddr(inst, "vkEnumeratePhysicalDevices");
    iffp_t vkGetIPFP = (iffp_t)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceImageFormatProperties");
    void* pd = NULL; uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd);

    printf("\n== 真实 HAL: imageFormatProperties (OPTIMAL, STORAGE) ==\n");
    int bgra=-1, rgba=-1;
    for (int i = 0; i < 4; i++) {
        struct { uint32_t me[3]; uint32_t mip; uint32_t ly; uint32_t sc; uint32_t rs; } p = {0};
        int rr = vkGetIPFP(pd, fmts[i], VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                           0, &p);
        if (fmts[i]==44) bgra=rr; if (fmts[i]==37) rgba=rr;
        printf("  %-16s STORAGE -> %d (%s)\n", fmtname[i], rr, rr==0 ? "受支持" : "NOT_SUPPORTED");
    }
    printf("\n== 判定 ==\n");
    if (bgra != 0 && rgba == 0)
        printf(">>> Adreno 540 的 storage 白名单含 R8G8B8A8 但不含 B8G8R8A8.\n"
               "    DXVK 用 B8G8R8A8 作 YUV 转换目标会在 dispatch 级失败/黑屏 (UB, 不报 err).\n"
               "    修复: shim 把该 STORAGE 目标图格式 B8G8R8A8 -> R8G8B8A8.\n");
    else if (bgra == 0 && rgba == 0)
        printf(">>> B8G8R8A8 与 R8G8B8A8 都支持 storage, 则 OP 黑屏另有原因 (非格式白名单).\n");
    else
        printf(">>> 两者都不支持 storage, 问题更复杂, 需进一步分析.\n");
    return 0;
}
