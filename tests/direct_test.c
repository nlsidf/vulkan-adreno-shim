/*
 * direct_test.c — 绕过系统 loader, 直接 dlopen shim + vk_icdGetInstanceProcAddr 调用,
 * 精确复刻"游戏路径"传给 shim 的参数(20扩展/1queue/最小特性/pNext=NULL),
 * 判断 -1 是"扩展列表内容"还是"winevulkan 传递链路"引起。
 *
 * 编译: gcc -O2 -o direct_test direct_test.c -ldl
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
typedef int (*create_dev_t)(void*, const void*, const void*, void**);
typedef int (*destroy_inst_t)(void*, const void*);

/* 游戏路径 shim 收到的 20 个扩展 */
static const char* game_exts[] = {
    "VK_KHR_create_renderpass2",
    "VK_KHR_driver_properties",
    "VK_KHR_sampler_mirror_clamp_to_edge",
    "VK_KHR_external_semaphore_fd",
    "VK_KHR_external_fence",
    "VK_KHR_external_fence_fd",
    "VK_KHR_external_semaphore",
    "VK_KHR_external_semaphore_fd",
    "VK_KHR_external_memory",
    "VK_KHR_external_memory_fd",
    "VK_KHR_external_fence",
    "VK_KHR_external_fence_fd",
    "VK_KHR_external_semaphore",
    "VK_KHR_external_semaphore_fd",
    "VK_KHR_dedicated_allocation",
    "VK_KHR_get_memory_requirements2",
    "VK_KHR_sampler_ycbcr_conversion",
    "VK_EXT_queue_family_foreign",
    "VK_KHR_maintenance1",
    "VK_KHR_bind_memory2",
};
#define NGAME (sizeof(game_exts)/sizeof(game_exts[0]))

int main(int argc, char** argv) {
    const char* shim_path = getenv("SHIM") ? getenv("SHIM") :
        "/data/data/com.termux/files/home/proton11/.build/vulkan_adreno_icd.so";
    void* h = dlopen(shim_path, RTLD_NOW);
    if (!h) { printf("dlopen shim 失败: %s\n", dlerror()); return 1; }
    getip_t vkGetInstanceProcAddr = (getip_t)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr) { printf("找不到 vk_icdGetInstanceProcAddr\n"); return 1; }

    /* ---- 创建实例(模拟 winevulkan: 无扩展) ---- */
    create_inst_t vkCreateInstance = (create_inst_t)vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    enum_pd_t     vkEnumeratePhysicalDevices = (enum_pd_t)vkGetInstanceProcAddr(NULL, "vkEnumeratePhysicalDevices");
    create_dev_t  vkCreateDevice = (create_dev_t)vkGetInstanceProcAddr(NULL, "vkCreateDevice");
    destroy_inst_t vkDestroyInstance = (destroy_inst_t)vkGetInstanceProcAddr(NULL, "vkDestroyInstance");
    if (!vkCreateInstance || !vkEnumeratePhysicalDevices || !vkCreateDevice) {
        printf("接口解析失败\n"); return 1;
    }

    /* VkInstanceCreateInfo 精确布局 */
    struct { uint32_t sType; const void* pNext; uint32_t flags;
             const void* pAppInfo; uint32_t layerCount; const char** ppLayerNames;
             uint32_t extCount; const char** ppExt; } ici = {0};
    ici.sType = 7; /* VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO */
    void* inst = NULL;
    int r = vkCreateInstance(&ici, NULL, &inst);
    printf("vkCreateInstance -> %d inst=%p\n", r, inst);

    void* pd = NULL;
    uint32_t n = 1;
    r = vkEnumeratePhysicalDevices(inst, &n, &pd);
    printf("vkEnumeratePhysicalDevices -> %d pd=%p n=%u\n", r, pd, n);

    /* ---- 设备创建: 复刻游戏路径参数 ---- */
    float prio = 1.0f;
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t family; uint32_t count; float* p; } qci = {0};
    qci.sType = 4; /* VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO */
    qci.family = 0; qci.count = 1; qci.p = &prio;

    /* 最小特性集: 仅 vertexPipelineStoresAndAtomics (第5个VkBool32) */
    static VkBool32 feats[55]; memset(feats, 0, sizeof(feats)); feats[4] = 1;

    int ext_count = NGAME;
    const char** exts = game_exts;
    /* 可选项: --pad48 把扩展填充到 48 个(重复) */
    if (argc > 1 && !strcmp(argv[1], "--pad48")) {
        static const char* pad[48];
        for (int i = 0; i < 48; i++) pad[i] = game_exts[i % NGAME];
        exts = pad; ext_count = 48;
    }
    /* 可选项: --20raw 逐个剔除测试用 */

    /* VkDeviceCreateInfo 精确布局 */
    struct { uint32_t sType; const void* pNext; uint32_t flags;
             uint32_t qcount; const void* q;
             uint32_t layerCount; const char** ppLayerNames;
             uint32_t extCount; const char** exts; const VkBool32* feats; } dci = {0};
    dci.sType = 3; /* VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO */
    dci.qcount = 1; dci.q = &qci;
    dci.extCount = ext_count; dci.exts = exts;
    dci.feats = feats;

    void* dev = NULL;
    r = vkCreateDevice(pd, &dci, NULL, &dev);
    printf("[%s] %d ext -> %d (%s) dev=%p\n",
           (ext_count == 48) ? "PAD48" : "GAME20", ext_count, r,
           r == 0 ? "VK_SUCCESS" : (r == -7 ? "EXTENSION_NOT_PRESENT" : (r == -1 ? "OUT_OF_HOST_MEMORY" : "other")), dev);

    if (vkDestroyInstance) vkDestroyInstance(inst, NULL);
    return 0;
}
