/* t_extfd4.c — 每个组合 fork 一个子进程跑, HAL 崩了也不影响后续
 * 目标: 摸清"哪些 memoryTypeIndex + 哪种 buffer 声明"能走通 export-fd,
 *       以及 dedicated 内存能否被当成 suballoc 池用 (DXVK 的实际行为)。
 * 编译: gcc -O2 -o t_extfd4 t_extfd4.c -lvulkan
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

static VkDevice dev;
static VkPhysicalDevice pd;
static VkInstance inst;
static PFN_vkGetMemoryFdKHR getFd;
static VkPhysicalDeviceMemoryProperties mp;

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

static VkBuffer mkbuf(VkDeviceSize size, int external) {
    VkExternalMemoryBufferCreateInfo embci = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = external ? &embci : NULL, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
               | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
               | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer b = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev, &bci, NULL, &b) != VK_SUCCESS) return VK_NULL_HANDLE;
    return b;
}

static int setup(void) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &ai };
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) return 1;
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
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
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) return 2;
    getFd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    return getFd ? 0 : 3;
}

/* 子进程里跑的实际测试 */
static void body(int type, int external_buf, int dedicated, VkDeviceSize want, int rebind) {
    VkBuffer buf = mkbuf(want, external_buf);
    if (!buf) { printf("createBuffer❌ "); return; }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    printf("memReq=%llu bits=0x%x ", (unsigned long long)mr.size, mr.memoryTypeBits);

    VkMemoryDedicatedAllocateInfo mdai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = buf };
    VkExportMemoryAllocateInfo emai = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = dedicated ? (void*)&mdai : NULL,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &emai, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)type };
    VkDeviceMemory mem;
    VkResult r = vkAllocateMemory(dev, &mai, NULL, &mem);
    printf("alloc=%d ", r);
    if (r) { printf("❌"); return; }

    VkMemoryGetFdInfoKHR gfi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = mem, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    int fd = -1;
    r = getFd(dev, &gfi, &fd);
    printf("getFd=%d fd=%d ", r, fd);
    if (r || fd < 0) { printf("❌"); return; }

    void* hi = NULL;
    VkResult rm = vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &hi);
    if (rm == 0) memcpy(hi, "DEDICATE", 8);
    printf("map=%d hi=%p ", rm, hi);

    void* lo = reserve_low((size_t)mr.size);
    void* p = lo ? mmap(lo, (size_t)mr.size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_FIXED, fd, 0) : MAP_FAILED;
    if (p == MAP_FAILED) { printf("lowmmap=errno%d ❌", errno); return; }
    printf("lo=%p %s ", p, ((unsigned long)p >> 32) ? "高位❌" : "低位✅");
    int same = (rm == 0 && memcmp(p, "DEDICATE", 8) == 0);
    memcpy(p, "LOWWRITE", 8);
    int both = (rm == 0 && memcmp(hi, "LOWWRITE", 8) == 0);
    printf("零拷贝=%s ", (same && both) ? "✅" : "❌");

    if (rebind) {
        VkBuffer o1 = mkbuf(4096, 0);
        if (o1) printf("rebind@0=%d ", vkBindBufferMemory(dev, o1, mem, 0));
        VkBuffer o2 = mkbuf(4096, 0);
        if (o2) printf("rebind@64K=%d ", vkBindBufferMemory(dev, o2, mem, 65536));
    }
    printf("✅");
}

static void run(const char* name, int type, int external_buf, int dedicated,
                VkDeviceSize want, int rebind) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        /* 子进程: 静音 shim 的噪音 */
        freopen("/dev/null", "w", stderr);
        printf("  %-42s : ", name);
        body(type, external_buf, dedicated, want, rebind);
        printf("\n");
        fflush(NULL);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) printf("  %-42s : 💥 崩溃 signal=%d\n", name, WTERMSIG(st));
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int rc = setup();
    if (rc) { printf("setup 失败 rc=%d\n", rc); return rc; }

    printf("\n内存类型表:\n");
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        printf("  [%u] heap=%u%s%s%s%s\n", i, mp.memoryTypes[i].heapIndex,
               (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? " DEVICE_LOCAL" : "",
               (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " HOST_VISIBLE" : "",
               (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? " HOST_COHERENT" : "",
               (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? " HOST_CACHED" : "");
    }

    printf("\n[1] 逐个 memoryType 试 可导出+dedicated (buffer 声明 external):\n");
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        char nm[64]; snprintf(nm, sizeof(nm), "type=%u external_buf=1 dedicated=1", i);
        run(nm, (int)i, 1, 1, 1 << 20, 0);
    }

    printf("\n[2] buffer 不声明 external (DXVK 的实际做法):\n");
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        char nm[64]; snprintf(nm, sizeof(nm), "type=%u external_buf=0 dedicated=1", i);
        run(nm, (int)i, 0, 1, 1 << 20, 0);
    }

    printf("\n[3] 不带 dedicated:\n");
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        char nm[64]; snprintf(nm, sizeof(nm), "type=%u external_buf=1 dedicated=0", i);
        run(nm, (int)i, 1, 0, 1 << 20, 0);
    }

    printf("\n[4] dedicated 内存当 suballoc 池 (再绑别的 buffer):\n");
    run("type=4 rebind 测试", 4, 1, 1, 1 << 20, 1);
    run("type=3 rebind 测试", 3, 1, 1, 1 << 20, 1);

    printf("\n[5] 大块 (DXVK chunk 量级):\n");
    run("type=4 16MB", 4, 1, 1, 16 << 20, 0);
    run("type=4 64MB", 4, 1, 1, 64u << 20, 0);
    run("type=3 16MB", 3, 1, 1, 16 << 20, 0);

    printf("\n[6] DXVK 实际会挑的 host-visible 类型 rebind:\n");
    run("type=1 rebind (HOST_VISIBLE|CACHED)", 1, 1, 1, 1 << 20, 1);
    run("type=2 rebind (VISIBLE|COHERENT|CACHED)", 2, 1, 1, 1 << 20, 1);
    run("type=0 rebind (DEVICE_LOCAL only)", 0, 1, 1, 1 << 20, 1);

    printf("\n全部完成。\n");
    return 0;
}
