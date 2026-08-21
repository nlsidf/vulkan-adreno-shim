/*
 * ycbcr_probe.c — 验证 Adreno 540 真驱动是否实现了 VK_KHR_sampler_ycbcr_conversion:
 *   1. 枚举设备扩展, 看驱动是否暴露 VK_KHR_sampler_ycbcr_conversion;
 *   2. 直接请求该扩展创建 device, 若返回 VK_ERROR_EXTENSION_NOT_PRESENT
 *      则驱动根本不支持 (DXVK 的能力门必然过不去);
 *   3. 若 device 创建成功, 进一步实际调用 vkCreateSamplerYcbcrConversion
 *      (NV12) 看是否返回 VK_SUCCESS.
 *
 * 经 shim (dlopen .so) 跑也行: shim 对 ycbcr 扩展既不 keep 也不 strip,
 * 直接透传给真驱动, 因此结果即真驱动的实情.
 *
 * 编译: gcc -O2 -o ycbcr_probe ycbcr_probe.c -ldl
 * 运行: ./ycbcr_probe
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;

typedef void* (*getip_t)(void*, const char*);
typedef int (*create_inst_t)(const void*, const void*, void**);
typedef int (*enum_pd_t)(void*, uint32_t*, void**);
typedef int (*destroy_inst_t)(void*, const void*);
typedef int (*enum_ext_t)(void*, const char*, uint32_t*, void*);
typedef int (*create_dev_t)(void*, const void*, const void*, void**);
typedef void (*destroy_dev_t)(void*, const void*);
typedef void* (*getdp_t)(void*, const char*);

/* VkExtensionProperties (name[256] + u32 specVersion) */
struct extp { char name[256]; uint32_t spec; };

/* 枚举/结构枚举值 */
#define VK_ERROR_EXTENSION_NOT_PRESENT (-7)
#define VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO 1000156000
/* VkSamplerYcbcrModelConversion */
#define VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709 2
/* VkSamplerYcbcrRange */
#define VK_SAMPLER_YCBCR_RANGE_ITU_NARROW 1
/* VkComponentSwizzle */
#define VK_COMPONENT_SWIZZLE_IDENTITY 0
#define VK_COMPONENT_SWIZZLE_R 1
#define VK_COMPONENT_SWIZZLE_G 2
#define VK_COMPONENT_SWIZZLE_B 3
#define VK_COMPONENT_SWIZZLE_A 4
/* VkChromaLocation */
#define VK_CHROMA_LOCATION_MIDPOINT 1
/* VkFilter */
#define VK_FILTER_LINEAR 1

/* VkComponentMapping: 4 个 swizzle (uint32) */
struct compmap { uint32_t r,g,b,a; };
/* VkSamplerYcbcrConversionCreateInfo (精简布局, 字段顺序与 vulkan.h 一致) */
struct ycbcrc { uint32_t sType; const void* pNext; uint32_t format; uint32_t ycbcrModel;
                uint32_t ycbcrRange; struct compmap components; uint32_t xChromaOffset;
                uint32_t yChromaOffset; uint32_t chromaFilter; uint32_t forceExplicit; };

static const char* YCBCR_EXT = "VK_KHR_sampler_ycbcr_conversion";

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
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    printf("vkEnumeratePhysicalDevices -> %d pd=%p n=%u\n", r, pd, n);

    /* ---- 步骤 1: 枚举设备扩展 ---- */
    enum_ext_t vkEnumDevExt = (enum_ext_t)gip(inst, "vkEnumerateDeviceExtensionProperties");
    int found_in_enum = 0;
    if (vkEnumDevExt) {
        uint32_t cnt = 0;
        int st = vkEnumDevExt(pd, NULL, &cnt, NULL);
        printf("\nvkEnumerateDeviceExtensionProperties: count=%u st=%d\n", cnt, st);
        if (st == 0 && cnt > 0) {
            struct extp* list = calloc(cnt, sizeof(struct extp));
            int st2 = vkEnumDevExt(pd, NULL, &cnt, list);
            printf("  扩展列表 (关注 ycbcr):\n");
            for (uint32_t i = 0; i < cnt; i++) {
                if (strstr(list[i].name, "ycbcr")) {
                    printf("    [YCBCR] %s (spec %u)\n", list[i].name, list[i].spec);
                    found_in_enum = 1;
                }
            }
            if (!found_in_enum)
                printf("    (无包含 'ycbcr' 的扩展)\n");
            free(list);
        }
    } else {
        printf("找不到 vkEnumerateDeviceExtensionProperties\n");
    }

    /* ---- 步骤 2+3: 请求 ycbcr 扩展创建 device 并实际调用 ---- */
    float prio = 1.0f;
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t family; uint32_t count; float* p; } qci = {0};
    qci.sType = 4; qci.family = 0; qci.count = 1; qci.p = &prio;
    const char* exts[2] = { YCBCR_EXT, NULL };
    struct { uint32_t sType; const void* pNext; uint32_t flags;
             uint32_t qcount; const void* q;
             uint32_t layerCount; const char** ppLayerNames;
             uint32_t extCount; const char** exts; const VkBool32* feats; } dci = {0};
    dci.sType = 3; dci.qcount = 1; dci.q = &qci;
    dci.extCount = 1; dci.exts = exts;

    create_dev_t vkCreateDevice = (create_dev_t)gip(NULL, "vkCreateDevice");
    destroy_dev_t vkDestroyDevice = (destroy_dev_t)gip(inst, "vkDestroyDevice");
    void* dev = NULL;
    int dr = vkCreateDevice(pd, &dci, NULL, &dev);
    printf("\n[vkCreateDevice 请求 %s]\n", YCBCR_EXT);
    printf("  -> %d dev=%p\n", dr, dev);

    if (dr == VK_ERROR_EXTENSION_NOT_PRESENT) {
        printf("  => 驱动拒绝该扩展: VK_ERROR_EXTENSION_NOT_PRESENT.\n");
        printf("  => 结论: Adreno 540 真驱动未实现 VK_KHR_sampler_ycbcr_conversion.\n");
        printf("     DXVK 的 ycbcr 能力门无法通过, OP 视频跳过与 shim 无关.\n");
        if (vkDestroyInstance) vkDestroyInstance(inst, NULL);
        return 0;
    }
    if (dr != 0 || !dev) {
        printf("  => 设备创建失败 (非扩展缺失, 可能是其它原因 r=%d). 无法继续.\n", dr);
        if (vkDestroyInstance) vkDestroyInstance(inst, NULL);
        return 0;
    }

    /* device 成功创建 (扩展被接受) -> 步骤 3: 实际调用 vkCreateSamplerYcbcrConversion */
    getdp_t vkGetDP = (getdp_t)gip(inst, "vkGetDeviceProcAddr");
    void* (*vkCreateYcbcr)(void*, const void*, const void*, void**) =
        vkGetDP ? (void*)vkGetDP(dev, "vkCreateSamplerYcbcrConversion") : NULL;
    void (*vkDestroyYcbcr)(void*, void*, const void*) =
        vkGetDP ? (void*)vkGetDP(dev, "vkDestroySamplerYcbcrConversion") : NULL;
    printf("  vkCreateSamplerYcbcrConversion ptr=%p\n", (void*)vkCreateYcbcr);

    if (!vkCreateYcbcr) {
        printf("  => 函数指针为空: 即使扩展被接受, 驱动未提供实现.\n");
    } else {
        struct ycbcrc ci = {0};
        ci.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
        ci.format = 148; /* NV12 = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM */
        ci.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
        ci.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
        ci.components.r = VK_COMPONENT_SWIZZLE_R;
        ci.components.g = VK_COMPONENT_SWIZZLE_G;
        ci.components.b = VK_COMPONENT_SWIZZLE_B;
        ci.components.a = VK_COMPONENT_SWIZZLE_A;
        ci.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        ci.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        ci.chromaFilter = VK_FILTER_LINEAR;
        ci.forceExplicit = 0;
        void* conv = NULL;
        int cr = (int)(intptr_t)vkCreateYcbcr(dev, &ci, NULL, &conv);
        printf("  vkCreateSamplerYcbcrConversion(NV12) -> %d conv=%p\n", cr, conv);
        if (cr == 0) {
            printf("  => 驱动真正实现了 ycbcr 转换! 之前 DXVK 失败另有原因.\n");
            if (conv && vkDestroyYcbcr) vkDestroyYcbcr(dev, conv, NULL);
        } else {
            printf("  => 扩展被接受但转换创建失败 (r=%d): 驱动实现不完整/格式不受支持.\n", cr);
        }
    }

    if (vkDestroyDevice) vkDestroyDevice(dev, NULL);
    if (vkDestroyInstance) vkDestroyInstance(inst, NULL);
    return 0;
}
