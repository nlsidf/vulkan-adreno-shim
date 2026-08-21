/*
 * yuv_devp_probe.c — 用 vkGetDeviceProcAddr 解析 vkGetPhysicalDeviceFormatProperties2
 * (完全模拟 DXVK 的取函数方式), 验证 shim 现在是否已拦截该路径并施加 YUV 补位。
 * 若经设备 proc addr 取到的函数返回的 optimal 带 SAMPLED 位 (0x...31),
 * 说明 shim_get_device_proc_addr 路由生效。
 *
 * 编译: gcc -O2 -o yuv_devp_probe yuv_devp_probe.c -ldl
 * 运行: VK_ICD_YUV_FIX=1 ./yuv_devp_probe
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

typedef void* (*getip_t)(void*, const char*);
typedef int (*create_inst_t)(const void*, const void*, void**);
typedef int (*enum_pd_t)(void*, uint32_t*, void**);
typedef int (*destroy_inst_t)(void*, const void*);
typedef void* (*getdp_t)(void*, const char*);
typedef int (*create_dev_t)(void*, const void*, const void*, void**);
typedef void (*destroy_dev_t)(void*, const void*);

#define VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 1000071000
struct fp { uint32_t linear; uint32_t optimal; uint32_t buffer; };
struct fmtp2 { uint32_t sType; void* pNext; struct fp props; };

int main(void) {
    const char* shim_path = getenv("SHIM") ? getenv("SHIM") :
        "/data/data/com.termux/files/home/proton11/.build/vulkan_adreno_icd.so";
    void* h = dlopen(shim_path, RTLD_NOW);
    getip_t gip = (getip_t)dlsym(h, "vk_icdGetInstanceProcAddr");
    void* inst = NULL;
    create_inst_t vkCreateInstance = (create_inst_t)gip(NULL, "vkCreateInstance");
    enum_pd_t vkEnumeratePhysicalDevices = (enum_pd_t)gip(NULL, "vkEnumeratePhysicalDevices");
    destroy_inst_t vkDestroyInstance = (destroy_inst_t)gip(NULL, "vkDestroyInstance");
    create_dev_t vkCreateDevice = (create_dev_t)gip(NULL, "vkCreateDevice");
    destroy_dev_t vkDestroyDevice = (destroy_dev_t)gip(NULL, "vkDestroyDevice");
    vkCreateInstance(&(struct { uint32_t s; const void* n; uint32_t f; const void* ai; uint32_t lc; const char** pl; uint32_t ec; const char** pe; }){ .s = 7 }, NULL, &inst);
    void* pd = NULL; uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    void* dev = NULL;
    float prio = 1.0f;
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t family; uint32_t count; float* p; } qci = { .sType = 4, .family = 0, .count = 1, .p = &prio };
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t qc; const void* q; uint32_t lc; const char** pl; uint32_t ec; const char** pe; const void* ft; } dci = { .sType = 3, .qc = 1, .q = &qci };
    int dr = vkCreateDevice(pd, &dci, NULL, &dev);
    printf("vkCreateDevice -> %d dev=%p\n", dr, dev);
    if (!dev) { printf("设备创建失败\n"); return 1; }

    getdp_t vkGetDP = (getdp_t)gip(inst, "vkGetDeviceProcAddr");
    void (*fn)(void*, uint32_t, void*) = (void*)vkGetDP(dev, "vkGetPhysicalDeviceFormatProperties2");
    printf("经 vkGetDeviceProcAddr 取 vkGetPhysicalDeviceFormatProperties2 -> %p\n", (void*)fn);
    if (!fn) { printf("未解析到函数\n"); return 1; }

    uint32_t fmts[] = { 148, 151, 43, 44 };
    const char* names[] = { "NV12", "YV12", "YUY2", "UYVY" };
    for (int i = 0; i < 4; i++) {
        struct fmtp2 p = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 };
        fn(pd, fmts[i], &p);
        printf("%-5s optimal=0x%08x (SAMPLED位=%d)\n", names[i], p.props.optimal,
               (p.props.optimal & 1));
    }
    printf("\n若 optimal 的 SAMPLED 位=1 (0x...31), 说明设备 proc addr 路径已拦截并补位。\n");
    if (vkDestroyDevice) vkDestroyDevice(dev, NULL);
    if (vkDestroyInstance) vkDestroyInstance(inst, NULL);
    return 0;
}
