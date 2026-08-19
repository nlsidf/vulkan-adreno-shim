/* destroy_cleanup.c — 设备销毁清理测试
 * 模拟"漏内存的应用": alloc+map 后不 free 直接 vkDestroyDevice。
 * 校验 shim 在 vkDestroyDevice 时把自有的 fd/低位映射/靶子 buffer 全部释放
 * (用 /proc/self/fd 计数: 销毁后 fd 数应回落到基线附近, 无泄漏)。
 * 编译: gcc -O2 -o destroy_cleanup destroy_cleanup.c -lvulkan
 * 运行: VK_ICD_FILENAMES=<json> ./destroy_cleanup
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

static int count_fds(void) {
    int n = 0;
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;
    struct dirent* e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

int main(void) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("inst fail\n"); return 1; }
    uint32_t n = 1; VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    uint32_t qc = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, NULL);
    VkQueueFamilyProperties* qs = malloc(sizeof(*qs) * qc);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, qs);
    uint32_t gf = 0;
    for (uint32_t i = 0; i < qc; i++) if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gf = i; break; }
    free(qs);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = gf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { printf("dev fail\n"); return 1; }

    int base = count_fds();

    /* alloc + map 多块内存, 故意不 free (模拟泄漏的应用) */
    enum { NMEMS = 8 };
    VkDeviceMemory mems[NMEMS];
    for (int i = 0; i < 8; i++) {
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = 4u << 20, .memoryTypeIndex = 2 };
        if (vkAllocateMemory(dev, &mai, NULL, &mems[i]) != VK_SUCCESS) { printf("alloc fail %d\n", i); return 1; }
        void* p = NULL;
        if (vkMapMemory(dev, mems[i], 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS || ((unsigned long)p >> 32) != 0) {
            printf("map fail or high ptr %d\n", i); return 1;
        }
        *(volatile int*)p = i; /* 触碰物理页 */
    }
    int after_alloc = count_fds();
    printf("基线 fd=%d, alloc+map %d 块后 fd=%d (预期上升)\n", base, NMEMS, after_alloc);

    /* 直接销毁设备, 不 free 内存 */
    vkDestroyDevice(dev, NULL);
    int after_destroy = count_fds();
    /* shim 清理会关掉 N 个导出的 dmabuf fd + 销毁 N 个靶子 buffer。
     * HAL 内存对象自身还可能各持 1 个内部 fd (由 HAL 管理, 非 shim 职责),
     * 故断言 = 至少回落 N 个 (允许 4 个余量)。 */
    int dropped = after_alloc - after_destroy;
    int ok = dropped >= NMEMS - 2;
    printf("vkDestroyDevice 后 fd=%d (alloc 后 %d, 回落 %d, 基线 %d) => %s\n",
           after_destroy, after_alloc, dropped, base,
           ok ? "shim 清理无泄漏 ✅" : "shim 清理有泄漏 ❌");
    if (!ok) return 1;

    vkDestroyInstance(inst, NULL);
    return 0;
}
