/* t_extfd2.c — 带 DEDICATED 分配的 VK_KHR_external_memory_fd 验证
 *   OPAQUE_FD 在本 HAL 上是 DEDICATED_ONLY, 所以必须:
 *     buffer(带 VkExternalMemoryBufferCreateInfo)
 *       -> vkAllocateMemory(pNext: ExportMemoryAllocateInfo -> MemoryDedicatedAllocateInfo)
 *       -> vkGetMemoryFdKHR
 *       -> 自己 mmap 到低 4GB
 * 编译: gcc -O2 -o t_extfd2 t_extfd2.c -lvulkan
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

static void* reserve_low(size_t len) {
    for (unsigned long h = 0x20000000UL; h < 0xF0000000UL; h += 0x4000000UL) {
        void* r = mmap((void*)h, len, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (r == MAP_FAILED) continue;
        if (((unsigned long)r >> 32) == 0) return r;
        munmap(r, len);
    }
    return NULL;
}

int main(void) {
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
    const char* de[] = { "VK_KHR_external_memory", "VK_KHR_external_memory_fd",
                         "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2" };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 4, .ppEnabledExtensionNames = de };
    VkDevice dev;
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    printf("vkCreateDevice(+ext_mem_fd +dedicated) -> %d %s\n", r, r ? "❌" : "✅");
    if (r) return 2;

    PFN_vkGetMemoryFdKHR getFd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    printf("vkGetMemoryFdKHR = %p %s\n", (void*)getFd, getFd ? "✅" : "❌");
    if (!getFd) return 3;

    size_t len = 1 << 20;

    /* 1) 建一个声明了 external handle type 的 buffer */
    VkExternalMemoryBufferCreateInfo embci = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &embci, .size = len,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
               | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf;
    r = vkCreateBuffer(dev, &bci, NULL, &buf);
    printf("vkCreateBuffer(external) -> %d %s\n", r, r ? "❌" : "✅");
    if (r) return 4;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    printf("memReq: size=%llu align=%llu typeBits=0x%x\n",
           (unsigned long long)mr.size, (unsigned long long)mr.alignment, mr.memoryTypeBits);

    /* 2) 选 host-visible 且被 typeBits 允许的内存类型 */
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    int type = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(mr.memoryTypeBits & (1u << i))) continue;
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) { type = (int)i; break; }
    }
    printf("选中 memoryType = %d\n", type);
    if (type < 0) return 5;

    /* 3) 专用分配 + 可导出 */
    VkMemoryDedicatedAllocateInfo mdai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .buffer = buf };
    VkExportMemoryAllocateInfo emai = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &mdai,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &emai, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)type };
    VkDeviceMemory mem;
    r = vkAllocateMemory(dev, &mai, NULL, &mem);
    printf("vkAllocateMemory(dedicated+exportable) -> %d %s\n", r, r ? "❌" : "✅");
    if (r) return 6;
    r = vkBindBufferMemory(dev, buf, mem, 0);
    printf("vkBindBufferMemory -> %d %s\n", r, r ? "❌" : "✅");

    /* 4) 正常 map, 写标记 */
    void* hi = NULL;
    r = vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &hi);
    printf("vkMapMemory -> %d, HAL 地址=%p (%s)\n", r, hi,
           (hi && ((unsigned long)hi >> 32)) ? "高位, wow64 表示不了" : "低位");
    if (r) return 7;
    memcpy(hi, "PROTON11", 8);

    /* 5) 导出 fd */
    VkMemoryGetFdInfoKHR gfi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = mem, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    int fd = -1;
    r = getFd(dev, &gfi, &fd);
    printf("vkGetMemoryFdKHR -> %d, fd=%d %s\n", r, fd, (r == 0 && fd >= 0) ? "✅" : "❌");
    if (r || fd < 0) {
        printf("\n结论: 即使 dedicated 也导不出 fd, external_memory_fd 这条路堵死。\n");
        return 8;
    }
    char link[64], tgt[256];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t ln = readlink(link, tgt, sizeof(tgt) - 1);
    if (ln > 0) { tgt[ln] = 0; printf("fd 指向: %s\n", tgt); }

    /* 6) 关键: 把 fd 自己 mmap 到低 4GB */
    void* lo = reserve_low(len);
    printf("低位预留 -> %p\n", lo);
    if (!lo) return 9;
    void* p = mmap(lo, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (p == MAP_FAILED) {
        printf("MAP_SHARED 失败: errno=%d (%s), 试 MAP_PRIVATE\n", errno, strerror(errno));
        p = mmap(lo, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 0);
    }
    if (p == MAP_FAILED) {
        printf("mmap(导出的 fd) 失败: errno=%d (%s)\n", errno, strerror(errno));
        printf("\n结论: OPAQUE_FD 不可直接 mmap, 这条路堵死。\n");
        return 10;
    }
    printf("mmap(导出的 fd) -> %p %s\n", p, ((unsigned long)p >> 32) ? "❌高位" : "✅低位");
    int same = (memcmp(p, "PROTON11", 8) == 0);
    printf("内容校验: %s\n", same ? "一致 ✅ 同一批物理页" : "不一致 ❌ 不是同一块内存");

    memcpy(p, "LOWWRITE", 8);
    int both = (memcmp(hi, "LOWWRITE", 8) == 0);
    printf("低位写入后 HAL 地址读到: %.8s %s\n", (char*)hi, both ? "✅ 双向一致" : "❌ 不同步");

    printf("\n结论: %s\n", (same && both)
           ? "可行! 改用 export fd + 自行低位 mmap, 零拷贝成立。"
           : "不可行 (非同一物理页)。");
    return 0;
}
