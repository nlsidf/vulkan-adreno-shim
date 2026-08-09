/* t_shimlow.c — 端到端验证 shim 的低位 dmabuf 接管
 * 完全模仿 DXVK 的行为: 应用侧不知道 external memory 的存在,
 *   - 创建 device 时不开任何 external 扩展 (由 shim 自己追加)
 *   - vkAllocateMemory 不带任何 pNext
 *   - vkMapMemory(offset=0, size=VK_WHOLE_SIZE)
 * 期望: 拿到的指针 <4GB, 读写正常, 且能在这块内存上做 suballocation。
 * 编译: gcc -O2 -o t_shimlow t_shimlow.c -lvulkan
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &ai };
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("createInstance 失败\n"); return 1; }
    uint32_t n = 1; VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);

    uint32_t qc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, NULL);
    VkQueueFamilyProperties* qs = malloc(sizeof(*qs) * qc);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, qs);
    uint32_t gf = 0;
    for (uint32_t i = 0; i < qc; i++)
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gf = i; break; }
    free(qs);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = gf, .queueCount = 1, .pQueuePriorities = &prio };
    /* 关键: 一个扩展都不开, 看 shim 会不会自己补上 */
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev;
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    printf("vkCreateDevice(零扩展, 模仿 DXVK) -> %d %s\n", r, r ? "❌" : "✅");
    if (r) return 2;

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    int fails = 0;
    const VkDeviceSize sizes[] = { 1u << 20, 16u << 20, 64u << 20 };
    for (unsigned s = 0; s < 3; s++) {
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
            if (!(mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
            /* 应用侧一无所知的朴素分配 */
            VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = sizes[s], .memoryTypeIndex = t };
            VkDeviceMemory mem;
            r = vkAllocateMemory(dev, &mai, NULL, &mem);
            if (r) { printf("  %2lluMB type=%u alloc=%d ❌\n",
                            (unsigned long long)sizes[s] >> 20, t, r); fails++; continue; }

            void* p = NULL;
            r = vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &p);
            int low = (p && ((unsigned long)p >> 32) == 0);
            printf("  %2lluMB type=%u map=%d ptr=%p %s",
                   (unsigned long long)sizes[s] >> 20, t, r, p,
                   r ? "❌map失败" : (low ? "✅低位" : "❌高位"));
            if (!r && low) {
                /* 读写整块, 确认物理页真的在 */
                memset(p, 0xA5, (size_t)sizes[s]);
                unsigned char* b = p;
                int ok = (b[0] == 0xA5 && b[sizes[s] - 1] == 0xA5);
                memcpy(p, "SHIMLOW", 7);
                printf(" 读写=%s", ok ? "✅" : "❌");
                if (!ok) fails++;

                /* suballocation: 在这块内存上绑普通 buffer */
                VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .size = 4096, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
                VkBuffer b1;
                if (vkCreateBuffer(dev, &bci, NULL, &b1) == VK_SUCCESS) {
                    VkResult rb = vkBindBufferMemory(dev, b1, mem, 65536);
                    printf(" suballoc=%s", rb ? "❌" : "✅");
                    if (rb) fails++;
                    vkDestroyBuffer(dev, b1, NULL);
                }
            } else fails++;
            printf("\n");
            if (!r) vkUnmapMemory(dev, mem);
            vkFreeMemory(dev, mem, NULL);
        }
    }

    /* 反复 alloc/map/free, 查 fd 和低位地址空间会不会漏 */
    printf("\n压力: 200 轮 4MB alloc/map/unmap/free ...\n");
    for (int i = 0; i < 200; i++) {
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = 4u << 20, .memoryTypeIndex = 2 };
        VkDeviceMemory mem;
        if (vkAllocateMemory(dev, &mai, NULL, &mem) != VK_SUCCESS) {
            printf("  第 %d 轮 alloc 失败 ❌\n", i); fails++; break;
        }
        void* p = NULL;
        if (vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS ||
            ((unsigned long)p >> 32) != 0) {
            printf("  第 %d 轮 map 失败或高位 ptr=%p ❌\n", i, p); fails++;
            vkFreeMemory(dev, mem, NULL); break;
        }
        *(volatile int*)p = i;
        vkUnmapMemory(dev, mem);
        vkFreeMemory(dev, mem, NULL);
    }
    printf("压力测试结束\n");

    printf("\n总结: %s (fails=%d)\n", fails ? "有问题 ❌" : "全部通过 ✅", fails);
    return fails ? 1 : 0;
}
