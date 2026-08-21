/*
 * yuv_fmtp2_probe.c — 用 vkGetPhysicalDeviceFormatProperties2 (DXVK 实际走的
 * 1.1 探测路径) 读取 NV12/YV12/YUY2/UYVY 的 optimal tiling 特征位, 确认 DXVK
 * CheckDeviceFormat 的门控到底卡在哪一位。
 *
 * 探针已证实: 驱动暴露 VK_KHR_sampler_ycbcr_conversion 且
 * vkCreateSamplerYcbcrConversion(NV12) -> VK_SUCCESS。所以失败不在扩展/转换,
 * 而在 FormatProperties2.optimal 特征位。若 optimal=0 (仅 linear 有值),
 * 则 shim 可在 fmtfix.rs 给 YUV 格式补 SAMPLED 位 (与 D32S8/BC 同模式)。
 *
 * 编译: gcc -O2 -o yuv_fmtp2_probe yuv_fmtp2_probe.c -ldl
 * 运行: ./yuv_fmtp2_probe
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
typedef void (*get_fmtp2_t)(void*, uint32_t, void*);

#define VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 1000071000

/* VkFormatProperties = { linear(u32), optimal(u32), buffer(u32) } = 12 bytes */
struct fp { uint32_t linear; uint32_t optimal; uint32_t buffer; };
struct fmtp2 { uint32_t sType; void* pNext; struct fp props; };

/* 关注格式 (DXVK 视频路径) */
static const struct { uint32_t fmt; const char* d3d9; } yuv[] = {
    { 148, "NV12" }, { 151, "YV12" }, { 43, "YUY2" }, { 44, "UYVY" },
};
#define NYUV (sizeof(yuv)/sizeof(yuv[0]))

#define BIT(x) (((x)>>0)&1), (((x)>>1)&1), (((x)>>4)&1), (((x)>>6)&1), (((x)>>7)&1)
/* 输出关键位: SAMPLED(0) TRANSFER_SRC(1) BLIT_SRC(4) BLIT_DST(6) COLOR_ATTACH(7) */

int main(void) {
    const char* shim_path = getenv("SHIM") ? getenv("SHIM") :
        "/data/data/com.termux/files/home/proton11/.build/vulkan_adreno_icd.so";
    void* h = dlopen(shim_path, RTLD_NOW);
    if (!h) { printf("dlopen shim 失败: %s\n", dlerror()); return 1; }
    getip_t gip = (getip_t)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gip) { printf("找不到 vk_icdGetInstanceProcAddr\n"); return 1; }

    create_inst_t vkCreateInstance = (create_inst_t)gip(NULL, "vkCreateInstance");
    enum_pd_t vkEnumeratePhysicalDevices = (enum_pd_t)gip(NULL, "vkEnumeratePhysicalDevices");
    destroy_inst_t vkDestroyInstance = (destroy_inst_t)gip(NULL, "vkDestroyInstance");
    void* inst = NULL;
    vkCreateInstance(&(struct { uint32_t s; const void* n; uint32_t f; const void* ai; uint32_t lc; const char** pl; uint32_t ec; const char** pe; }){ .s = 7 }, NULL, &inst);
    void* pd = NULL; uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);

    get_fmtp2_t vkGetFP2 = (get_fmtp2_t)gip(inst, "vkGetPhysicalDeviceFormatProperties2");
    if (!vkGetFP2) { printf("无 vkGetPhysicalDeviceFormatProperties2\n"); return 1; }

    printf("格式             optimal tiling 特征位 (SAMPLED,TRANSFER_SRC,BLIT_SRC,BLIT_DST,COLOR_ATTACH)\n");
    for (uint32_t i = 0; i < NYUV; i++) {
        struct fmtp2 p = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 };
        vkGetFP2(pd, yuv[i].fmt, &p);
        uint32_t o = p.props.optimal;
        uint32_t l = p.props.linear;
        printf("%-5s (fmt=%-3u)  optimal=0x%08x [%d,%d,%d,%d,%d]   linear=0x%08x\n",
               yuv[i].d3d9, yuv[i].fmt, o, BIT(o), l);
    }
    printf("\n判定: 若 optimal=0x0 而 linear 有值 -> DXVK(读 optimal) 失败于此;\n");
    printf("      shim 可在 fmtfix.rs 给 YUV 的 optimal 补 SAMPLED 位 (同 D32S8/BC 模式)。\n");
    if (vkDestroyInstance) vkDestroyInstance(inst, NULL);
    return 0;
}
