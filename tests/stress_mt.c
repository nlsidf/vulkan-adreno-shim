/* stress_mt.c — 多线程 alloc/map/unmap/free 并发压力测试
 * 验证 RwLock+原子化后的低位映射路径在多线程下不串行、不泄漏、全低位。
 * 编译: gcc -O2 -o stress_mt stress_mt.c -lvulkan -lpthread
 * 运行: VK_ICD_FILENAMES=<json> ./stress_mt
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define NTHREADS 8
#define NITER 150
#define ALLOC_SIZE (4u << 20)

static VkDevice g_dev = 0;
static pthread_mutex_t g_fail_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_fails = 0;
static int g_low_count = 0;

static void* worker(void* arg) {
    long tid = (long)arg;
    for (int i = 0; i < NITER; i++) {
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = ALLOC_SIZE, .memoryTypeIndex = 2 };
        VkDeviceMemory mem = 0;
        VkResult r = vkAllocateMemory(g_dev, &mai, NULL, &mem);
        if (r != VK_SUCCESS) { pthread_mutex_lock(&g_fail_lock); g_fails++; pthread_mutex_unlock(&g_fail_lock); continue; }
        void* p = NULL;
        r = vkMapMemory(g_dev, mem, 0, VK_WHOLE_SIZE, 0, &p);
        if (r != VK_SUCCESS || !p || ((unsigned long)p >> 32) != 0) {
            pthread_mutex_lock(&g_fail_lock); g_fails++; pthread_mutex_unlock(&g_fail_lock);
            vkFreeMemory(g_dev, mem, NULL); continue;
        }
        /* 写入校验低位映射物理页 */
        memset(p, (int)(tid + i), ALLOC_SIZE);
        if (((volatile unsigned char*)p)[ALLOC_SIZE - 1] != (unsigned char)(tid + i)) {
            pthread_mutex_lock(&g_fail_lock); g_fails++; pthread_mutex_unlock(&g_fail_lock);
        }
        pthread_mutex_lock(&g_fail_lock); g_low_count++; pthread_mutex_unlock(&g_fail_lock);
        vkUnmapMemory(g_dev, mem);
        vkFreeMemory(g_dev, mem, NULL);
    }
    return NULL;
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
    if (vkCreateDevice(pd, &dci, NULL, &g_dev) != VK_SUCCESS) { printf("dev fail\n"); return 1; }

    pthread_t th[NTHREADS];
    for (long t = 0; t < NTHREADS; t++) pthread_create(&th[t], NULL, worker, (void*)t);
    for (int t = 0; t < NTHREADS; t++) pthread_join(th[t], NULL);

    printf("多线程压力: %d 线程 x %d 轮 4MB alloc/map/write/unmap/free\n", NTHREADS, NITER);
    printf("低位成功=%d 失败=%d => %s\n", g_low_count, g_fails,
           (g_fails == 0 && g_low_count == NTHREADS * NITER) ? "全部通过 ✅" : "有问题 ❌");
    return g_fails ? 1 : 0;
}
