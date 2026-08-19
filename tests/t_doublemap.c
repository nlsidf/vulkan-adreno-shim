/* t_doublemap.c — P0 回归测试: 多次 vkMapMemory 后不配对 unmap 直接 vkFreeMemory
 * 验证 shim_free_memory 会把 outstanding 的 HAL map 全部配对还回去 (循环 unmap),
 * 不再只还一次导致 HAL 映射泄漏。
 * 编译: gcc -O2 -o t_doublemap t_doublemap.c -lvulkan
 * 运行: VK_ICD_FILENAMES=<json> ./t_doublemap
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VkDevice g_dev = 0;

static int setup(void) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) return 1;
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
    return vkCreateDevice(pd, &dci, NULL, &g_dev);
}

/* 场景: map N 次 (不 unmap), 直接 free — 应循环 unmap N 次, 不崩不泄漏 */
static int test_mapN_then_free(int n) {
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 4u << 20, .memoryTypeIndex = 2 };
    VkDeviceMemory mem;
    if (vkAllocateMemory(g_dev, &mai, NULL, &mem) != VK_SUCCESS) return 1;
    void* p[8];
    for (int i = 0; i < n; i++) {
        if (vkMapMemory(g_dev, mem, 0, VK_WHOLE_SIZE, 0, &p[i]) != VK_SUCCESS ||
            ((unsigned long)p[i] >> 32) != 0) {
            printf("  map[%d] 失败或高位\n", i);
            return 1;
        }
    }
    /* 两次 map 应返回同一低位基址 (缓存复用) */
    if (n >= 2 && p[0] != p[1]) { printf("  map 指针不一致 %p vs %p\n", p[0], p[1]); return 1; }
    memset(p[0], 0x5A, 4u << 20);
    vkFreeMemory(g_dev, mem, NULL);   /* 关键: 不 unmap 直接 free */
    return 0;
}

/* 场景: map N 次, unmap N-1 次 (剩 1), 再 free — 剩的 1 次应由 free 还掉 */
static int test_mapN_unmapN1_then_free(int n) {
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 4u << 20, .memoryTypeIndex = 2 };
    VkDeviceMemory mem;
    if (vkAllocateMemory(g_dev, &mai, NULL, &mem) != VK_SUCCESS) return 1;
    void* p = NULL;
    for (int i = 0; i < n; i++) {
        if (vkMapMemory(g_dev, mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return 1;
    }
    for (int i = 0; i < n - 1; i++) vkUnmapMemory(g_dev, mem);
    vkFreeMemory(g_dev, mem, NULL);
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (setup()) { printf("setup 失败\n"); return 1; }
    int fails = 0;

    for (int rep = 0; rep < 20; rep++) {
        if (test_mapN_then_free(2)) { printf("map2+free 失败 ❌\n"); fails++; }
        if (test_mapN_then_free(3)) { printf("map3+free 失败 ❌\n"); fails++; }
        if (test_mapN_unmapN1_then_free(2)) { printf("map2+unmap1+free 失败 ❌\n"); fails++; }
        if (test_mapN_unmapN1_then_free(4)) { printf("map4+unmap3+free 失败 ❌\n"); fails++; }
    }
    printf("双 map 回归: 20 轮 x 4 场景 => %s (fails=%d)\n", fails ? "有问题 ❌" : "全部通过 ✅", fails);
    return fails ? 1 : 0;
}
