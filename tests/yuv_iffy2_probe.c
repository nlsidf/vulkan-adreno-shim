/*
 * yuv_iffy2_probe.c — 用 vkGetPhysicalDeviceImageFormatProperties2 (DXVK 实际探测
 * 路径) 检查 NV12/YV12/YUY2/UYVY 的返回码。若驱动对这些格式 + SAMPLED usage 返回
 * VK_ERROR_FORMAT_NOT_SUPPORTED (而非 VK_SUCCESS), 则 DXVK 的 CheckImageSupport
 * 失败 —— 这与 FormatProperties 补位无关, 必须在 iffy2 路径放行为 YUV 格式。
 *
 * 简化版: 仅测 plain iffy2 (不带 ycbcr pNext), 避免 pNext 布局错导致驱动阻塞。
 *
 * 编译: gcc -O2 -o yuv_iffy2_probe yuv_iffy2_probe.c -ldl
 * 运行: ./yuv_iffy2_probe            (默认关 fix)
 *       VK_ICD_YUV_FIX=1 ./yuv_iffy2_probe
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
typedef int (*iffy2_t)(void*, const void*, void*);

#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 1000059003
#define VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 1000071000
#define VK_IMAGE_TYPE_2D 1
#define VK_IMAGE_TILING_OPTIMAL 0
#define VK_IMAGE_USAGE_SAMPLED_BIT 0x00000004u
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT 0x00000002u
#define VK_IMAGE_USAGE_TRANSFER_SRC_BIT 0x00000001u

static const struct { uint32_t fmt; const char* d3d9; } yuv[] = {
    { 148, "NV12" }, { 151, "YV12" }, { 43, "YUY2" }, { 44, "UYVY" },
};
#define NYUV (sizeof(yuv)/sizeof(yuv[0]))

/* 通用 VkImageFormatProperties2 占位 (前 16 字节 sType/pNext, 后接 ImageFormatProperties) */
struct ifp2 { uint32_t sType; void* pNext;
              uint32_t me[3]; uint32_t mip; uint32_t layers; uint32_t samples; uint32_t res; };

int main(void) {
    const char* shim_path = getenv("SHIM") ? getenv("SHIM") :
        "/data/data/com.termux/files/home/proton11/.build/vulkan_adreno_icd.so";
    void* h = dlopen(shim_path, RTLD_NOW);
    if (!h) { printf("dlopen shim 失败: %s\n", dlerror()); return 1; }
    getip_t gip = (getip_t)dlsym(h, "vk_icdGetInstanceProcAddr");
    void* inst = NULL;
    create_inst_t vkCreateInstance = (create_inst_t)gip(NULL, "vkCreateInstance");
    enum_pd_t vkEnumeratePhysicalDevices = (void*)gip(NULL, "vkEnumeratePhysicalDevices");
    destroy_inst_t vkDestroyInstance = (void*)gip(NULL, "vkDestroyInstance");
    vkCreateInstance(&(struct { uint32_t s; const void* n; uint32_t f; const void* ai; uint32_t lc; const char** pl; uint32_t ec; const char** pe; }){ .s = 7 }, NULL, &inst);
    void* pd = NULL; uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);

    iffy2_t vkIffy2 = (iffy2_t)gip(inst, "vkGetPhysicalDeviceImageFormatProperties2");
    if (!vkIffy2) { printf("无 vkGetPhysicalDeviceImageFormatProperties2\n"); return 1; }

    for (uint32_t i = 0; i < NYUV; i++) {
        struct ifp2 out = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
        struct { uint32_t sType; const void* pNext; uint32_t format; uint32_t imageType;
                 uint32_t tiling; uint32_t usage; uint32_t flags; } info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .format = yuv[i].fmt, .imageType = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                     | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .flags = 0, .pNext = NULL };
        int r1 = vkIffy2(pd, &info, &out);
        printf("%-5s fmt=%-3u  iffy2(plain, OPTIMAL, SAMPLED+XFER)=%d (0=VK_SUCCESS, -7=NOT_SUPPORTED)\n",
               yuv[i].d3d9, yuv[i].fmt, r1);
    }
    printf("\n若返回 -7 (VK_ERROR_FORMAT_NOT_SUPPORTED), DXVK 失败于此路径;\n");
    printf("与 FormatProperties 补位无关, 需在 shim_iffp2 对 YUV 放行为 VK_SUCCESS。\n");
    if (vkDestroyInstance) vkDestroyInstance(inst, NULL);
    return 0;
}
