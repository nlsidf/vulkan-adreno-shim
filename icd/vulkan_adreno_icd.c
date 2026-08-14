/*
 * vulkan_adreno_icd.so — Adreno Vulkan ICD 桥接 shim
 *
 * 问题: Adreno 的 Vulkan 驱动 (/vendor/lib64/hw/vulkan.msm8998.so) 是
 *       Android Vulkan HAL, 只导出 C++ 修饰名 qglinternal::vkGetInstanceProcAddr,
 *       没有标准 ICD 入口 vk_icdGetInstanceProcAddr, 所以 Mesa loader 无法直接加载。
 *
 * 解决: 本 shim 在构造函数中
 *        1) 链接 namespace (default<->sphal), 放行 /vendor 依赖
 *        2) dlopen Adreno Vulkan 驱动
 *        3) 导出标准 C 符号 vk_icdGetInstanceProcAddr (及 physical device 变体)
 *           转调驱动的 qglinternal::vkGetInstanceProcAddr
 *   使 Mesa loader 能把它当普通 ICD 加载, 从而枚举 Adreno 真实 Vulkan 能力。
 *
 * 编译:
 *   gcc -shared -fPIC -o vulkan_adreno_icd.so vulkan_adreno_icd.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/mman.h>
#include <errno.h>
#include <dirent.h>
#include <vulkan/vulkan.h>

#define ADRENO_VK_DRIVER "/vendor/lib64/hw/vulkan.msm8998.so"
/* qglinternal::vkGetInstanceProcAddr(VkInstance_T*, const char*) */
#define ADRENO_GETPROC_MANGLED "_ZN11qglinternal21vkGetInstanceProcAddrEP12VkInstance_TPKc"

/* 驱动 dispatcher 函数指针类型 (与 PFN_vkGetInstanceProcAddr 兼容) */
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_adreno_get_proc)(VkInstance, const char*);

static void*            g_drv = NULL;
static PFN_adreno_get_proc g_get_proc = NULL;
/* 实验: 最小特性集 — 仅 EXPERIMENT_MIN_FEATURES / EXPERIMENT_REBUILD 编译时启用,
 * 故声明与用法一并用同样的条件包裹, 避免正式构建时产生 unused-variable 警告。 */
#if defined(EXPERIMENT_MIN_FEATURES) || defined(EXPERIMENT_REBUILD)
static VkPhysicalDeviceFeatures g_fake_feats;
#endif

/* ---- vkCreateDevice 扩展过滤 (修复 DXVK-Sarek) ----
 * DXVK 请求的扩展含 VK_KHR_external_semaphore_win32, Adreno 540 HAL 不支持,
 * 导致 vkCreateDevice 返回 VK_ERROR_EXTENSION_NOT_PRESENT (-7).
 * 我们拦截 vkCreateDevice, 过滤掉真实驱动不支持的扩展后再转发.
 * 注意: WSI 扩展 (surface/swapchain 等) 由 Sky1 layer 实现, 真实驱动不报告,
 *       但它们必须在 enabled 列表里, 否则 WSI 会断 — 故保留白名单。
 */
static PFN_vkCreateInstance g_real_create_instance = NULL;
static PFN_vkCreateDevice   g_real_create_device   = NULL;
static VkInstance           g_real_icd_instance    = NULL; /* 真实驱动的实例句柄 */

/* ---- 多实例安全 (修复 Unity threaded=1 竞态) ----
 * DXVK/Unity 会并发创建多个 VkInstance (每个 D3D11 device 一个)。原来用单个
 * 全局 g_real_icd_instance + 惰性 g_get_proc 解析, 线程 B 创建新实例时覆盖全局,
 * 线程 A 再用它去调 HAL vkGetInstanceProcAddr → 在 HAL 内部 SIGSEGV (实测 exit 139)。
 * 方案:
 *   1) 构造器里用 instance=NULL 提前解析 iffp/fmtp (若 HAL 对 NULL 也返回 PD 级函数);
 *   2) 否则按 PD→实例 映射 + 互斥锁, 从 PD 所属的(已建成的)实例解析并缓存。
 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* 测试开关: 设 VK_TEST_RAW=1 时, 本 shim 不做任何 D32S8 修复/替换,
 * 让测试程序能观察 Adreno 硬件对 D32S8 的真实行为。默认关闭, 不影响游戏。 */
static int g_raw_test = 0;

/* 日志分级: 默认只打印初始化/错误等一次性日志; 设 VK_ICD_VERBOSE>=2 才打印
 * 热路径(persistent)诊断(iffp/fmtp/深度替换等)。这些日志每次格式探测/每个深度
 * 资源都会打一行, 不关的话一局游戏能写到几 MB, 故默认静默, 需调试时再开。 */
static int g_shim_verbose = 0;
#define SHIM_DBG(...) do { if (g_shim_verbose >= 2) fprintf(stderr, __VA_ARGS__); } while (0)

#define MAX_INST 16
#define MAX_PD   32
typedef struct { VkInstance inst; PFN_vkGetPhysicalDeviceImageFormatProperties iffp; PFN_vkGetPhysicalDeviceFormatProperties fmtp; } InstEntry;
static InstEntry g_insts[MAX_INST];
static int  g_inst_cnt = 0;
typedef struct { VkPhysicalDevice pd; VkInstance inst; } PDEntry;
static PDEntry g_pds[MAX_PD];
static int  g_pd_cnt = 0;

static InstEntry* find_or_add_inst(VkInstance inst) {
    for (int i = 0; i < g_inst_cnt; i++)
        if (g_insts[i].inst == inst) return &g_insts[i];
    if (g_inst_cnt < MAX_INST) {
        InstEntry* e = &g_insts[g_inst_cnt++];
        e->inst = inst; e->iffp = NULL; e->fmtp = NULL;
        return e;
    }
    return NULL;
}

static VkInstance inst_for_pd(VkPhysicalDevice pd) {
    for (int i = 0; i < g_pd_cnt; i++)
        if (g_pds[i].pd == pd) return g_pds[i].inst;
    if (g_inst_cnt > 0) return g_insts[g_inst_cnt - 1].inst;
    return g_real_icd_instance;
}

/* 崩溃回溯 (VK_ICD_BT=1 启用). 不用 execinfo 的 backtrace(): bionic/loader
 * 上下文解析不到该符号会导致 ICD dlopen 失败. 只打印故障地址+调用者符号。 */
static void crash_handler(int sig, siginfo_t* si, void* uc) {
    fprintf(stderr, "\n[VK_ICD] 信号 %d (SIGSEGV=%d) 访问地址 %p\n",
            sig, SIGSEGV, si ? si->si_addr : NULL);
    void* ra = __builtin_return_address(0);
    Dl_info info;
    if (dladdr(ra, &info) && info.dli_sname)
        fprintf(stderr, "  caller: %s (%s+0x%lx)\n", info.dli_sname,
                info.dli_fname ? info.dli_fname : "?",
                (unsigned long)((uintptr_t)ra - (uintptr_t)info.dli_fbase));
    fflush(stderr);
    _exit(139);
}
static void install_crash_handler(void) {
    if (getenv("VK_ICD_BT")) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = crash_handler;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGABRT, &sa, NULL);
    }
}

static const char* wsi_keep_exts[] = {
    "VK_KHR_surface", "VK_KHR_swapchain", "VK_KHR_display",
    "VK_KHR_xcb_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface",
    "VK_KHR_get_surface_capabilities2", "VK_KHR_display_swapchain",
    "VK_ANDROID_native_buffer",
    "VK_ANDROID_external_memory_android_hardware_buffer",
    "VK_EXT_swapchain_maintenance1",
};

/* win32 平台扩展: Adreno HAL 枚举里偶有出现但 vkCreateDevice 会拒绝, 一律剥离 */
static const char* strip_blacklist[] = {
    "VK_KHR_external_semaphore_win32",
    "VK_KHR_external_memory_win32",
    "VK_KHR_external_fence_win32",
    "VK_KHR_win32_keyed_mutex",
    "VK_KHR_win32_surface",
    "VK_EXT_external_memory_win32",
};

static int ext_in_wsi_keep(const char* name) {
    for (uint32_t i = 0; i < sizeof(wsi_keep_exts)/sizeof(wsi_keep_exts[0]); i++)
        if (!strcmp(wsi_keep_exts[i], name)) return 1;
    return 0;
}
static int ext_in_strip_blacklist(const char* name) {
    for (uint32_t i = 0; i < sizeof(strip_blacklist)/sizeof(strip_blacklist[0]); i++)
        if (!strcmp(strip_blacklist[i], name)) return 1;
    return 0;
}

/* ================= 原生分配器注入 =================================================
 * 【2026-08-08 证伪记录 — 勿再走回头路】
 *   曾经的判断是: 32 位下 HAL 内部分配器 alloc = pd->[0x8]->[0xcc0] 的
 *   "ctx" (alloc->[0x8]) = 0x100000000 被 wow64 写坏, 其 pfnAllocation
 *   (ctx->[0x10]) = 0x1003e0003 是垃圾 → HAL 返回 -1。
 *   给 PDDUMP 加上探页保护后重跑 64 位原生对照 (gpu_dev), 实测:
 *       64 位: alloc->[0x8] = 0x100000000, 结果 VK_SUCCESS
 *       32 位: alloc->[0x8] = 0x100000000, 结果 -1
 *   两边完全相同 → 该字段是常量, 不是被写坏的 ctx 指针, 此路不通。
 *   32 位那个 "pfnAlloc=0x1003e0003" 只是 box64 恰好在 0x100000010 映射了页,
 *   读到的随机数据; 64 位该页未映射, 所以现在会被 icd_readable 判为不可读。
 *   因此不要再提供 "改写 alloc->[0x8]" 的补丁 —— 那是在改一个认错了的字段。
 *
 *   真正的差异在 pd->[0x8] 本身: 32 位 = 0x45be1000 (低 4GB), 64 位 = 0x7871aa7000。
 *   成因见 box64/src/custommmap.c:39 —— box64 从可执行文件导出 mmap/mmap64,
 *   劫持了整个进程 (含原生 Adreno HAL); 当 running32bits && BOX64_MMAP32 时,
 *   所有未指定 MAP_32BIT 的匿名 mmap 都被强制塞进低 4GB。64 位游戏
 *   running32bits==0 故不受影响, 这才是 "64 位能跑 / 32 位 -1" 的真正机制。
 *   对策在启动脚本侧: BOX64_MMAP32=0 (guest 自己的 32 位分配在
 *   wrapped32/wrappedlibc.c:3142、threads32.c:202、custommem.c 里都显式硬编码了
 *   MAP_32BIT, 不依赖这个开关, 所以关掉对 guest 无损)。
 *
 * 保留下来的开关:
 *   VK_ICD_INJECT_ALLOC=1   向真实 vkCreateInstance / vkCreateDevice 传入本 shim
 *                           的原生 (LP64) VkAllocationCallbacks。与上面的证伪无关,
 *                           这是合规做法, 可排除 "上游传进来的 pAllocator 已损坏"。
 */

/* 指针可读探测。0xcc0 这条链只在 32 位形态下成立, 64 位裸解引用会 SIGSEGV
 * (PDDUMP 实测 exit 139), 所以所有裸偏移访问一律先过这里。
 * msync 对未映射区间返回 -1/ENOMEM, 是不触发信号的探页手段。 */
static int icd_readable(const void* p, size_t len) {
    if (!p) return 0;
    static size_t ps;
    if (!ps) ps = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t s = (uintptr_t)p & ~(uintptr_t)(ps - 1);
    uintptr_t e = ((uintptr_t)p + len + ps - 1) & ~(uintptr_t)(ps - 1);
    return msync((void*)s, (size_t)(e - s), MS_ASYNC) == 0;
}

/* 分配头内联在返回指针之前: 无容量上限、无锁、realloc 能拿到精确旧长度。
 * (不用哈希表 —— 满表会死循环, 且删除置空会切断线性探测链导致旧长度丢失) */
typedef struct { void* raw; size_t size; } IcdHdr;

static void* icd_raw_alloc(size_t size, size_t alignment) {
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    if (alignment & (alignment - 1)) return NULL;   /* 规范要求 2 的幂 */
    void* raw = malloc(size + alignment + sizeof(IcdHdr));
    if (!raw) return NULL;
    uintptr_t base    = (uintptr_t)raw + sizeof(IcdHdr);
    uintptr_t aligned = (base + alignment - 1) & ~(uintptr_t)(alignment - 1);
    IcdHdr* h = (IcdHdr*)(aligned - sizeof(IcdHdr));  /* aligned>=raw+16, 恒不越界 */
    h->raw = raw;
    h->size = size;
    return (void*)aligned;
}
static void VKAPI_PTR icd_cb_free(void* u, void* p) {
    (void)u;
    if (!p) return;
    free(((IcdHdr*)((uintptr_t)p - sizeof(IcdHdr)))->raw);
}
static void* VKAPI_PTR icd_cb_alloc(void* u, size_t size, size_t alignment,
                                    VkSystemAllocationScope scope) {
    (void)u; (void)scope;
    return icd_raw_alloc(size, alignment);
}
static void* VKAPI_PTR icd_cb_realloc(void* u, void* orig, size_t size, size_t alignment,
                                      VkSystemAllocationScope scope) {
    (void)scope;
    if (!orig) return icd_raw_alloc(size, alignment);
    if (!size) { icd_cb_free(u, orig); return NULL; }
    size_t old = ((IcdHdr*)((uintptr_t)orig - sizeof(IcdHdr)))->size;
    void* np = icd_raw_alloc(size, alignment);
    if (!np) return NULL;                            /* 失败时 orig 必须保持有效 */
    memcpy(np, orig, old < size ? old : size);
    icd_cb_free(u, orig);
    return np;
}

/* HAL 读取偏移 (反汇编 0xfa334 起): ctx->[0x10]=pfnAllocation,
 * ctx->[0x18]=pfnReallocation, ctx->[0x20]=pfnFree
 * —— 等价于 "8 字节前缀 + 标准 VkAllocationCallbacks"。 */
typedef struct { void* hal_prefix; VkAllocationCallbacks cb; } HalAllocCtx;
_Static_assert(offsetof(HalAllocCtx, cb) == 0x8, "HalAllocCtx 前缀必须 8 字节");
_Static_assert(offsetof(VkAllocationCallbacks, pfnAllocation) == 0x8, "VkAllocationCallbacks 布局异常");

static HalAllocCtx g_hal_ctx = {
    .hal_prefix = NULL,
    .cb = {
        .pUserData             = NULL,
        .pfnAllocation         = icd_cb_alloc,
        .pfnReallocation       = icd_cb_realloc,
        .pfnFree               = icd_cb_free,
        .pfnInternalAllocation = NULL,   /* 规范允许两者同时为 NULL */
        .pfnInternalFree       = NULL,
    },
};
#define ICD_NATIVE_ALLOC (&g_hal_ctx.cb)

/* 若开启注入, 返回原生分配器替换调用方传来的 (可能已被 wow64 转译坏的) 那个 */
static const VkAllocationCallbacks* icd_pick_alloc(const VkAllocationCallbacks* in,
                                                   const char* where) {
    if (!getenv("VK_ICD_INJECT_ALLOC")) return in;
    fprintf(stderr, "[VK_ICD] INJECT_ALLOC: %s 用原生分配器 %p (原 %p)\n",
            where, (const void*)ICD_NATIVE_ALLOC, (const void*)in);
    return ICD_NATIVE_ALLOC;
}

/* 拦截 vkCreateInstance: 记录真实驱动创建的实例句柄, 供后续解析 vkCreateDevice。
 * HAL 的内部分配器对象 (pd->[0x8]->[0xcc0]) 大概率在此期建立, 所以注入必须覆盖这里。 */
static VkResult VKAPI_CALL shim_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkInstance* pInstance) {
    if (!g_real_create_instance) {
        g_real_create_instance = (PFN_vkCreateInstance)g_get_proc(NULL, "vkCreateInstance");
        if (!g_real_create_instance) {
            fprintf(stderr, "[VK_ICD] 无法解析真实 vkCreateInstance\n");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    VkResult r = g_real_create_instance(pCreateInfo,
                                        icd_pick_alloc(pAllocator, "vkCreateInstance"),
                                        pInstance);
    if (r == VK_SUCCESS && pInstance) {
        pthread_mutex_lock(&g_lock);
        g_real_icd_instance = *pInstance;
        find_or_add_inst(*pInstance);
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "[VK_ICD] shim_vkCreateInstance -> 实例=%p (pNext=%p, 扩展数=%u, alloc=%p)\n",
                (void*)g_real_icd_instance, (const void*)pCreateInfo->pNext,
                (unsigned)pCreateInfo->enabledExtensionCount, (const void*)pAllocator);
    }
    return r;
}

/* 拦截 vkCreateDevice: 剥离 win32 平台扩展(Adreno HAL 枚举里有但创建时拒绝)
 * 后转发给真实驱动. WSI 扩展(surface/swapchain)由 Sky1 layer 实现, 原样保留。
 */
static PFN_vkEnumeratePhysicalDevices g_real_enum_pds = NULL;

static VkResult VKAPI_CALL shim_vkEnumeratePhysicalDevices(VkInstance instance,
                                                           uint32_t* pPhysicalDeviceCount,
                                                           VkPhysicalDevice* pPhysicalDevices) {
    if (!g_real_enum_pds)
        g_real_enum_pds = (PFN_vkEnumeratePhysicalDevices)g_get_proc(instance, "vkEnumeratePhysicalDevices");
    VkResult r = g_real_enum_pds(instance, pPhysicalDeviceCount, pPhysicalDevices);
    if (r == VK_SUCCESS && pPhysicalDevices && *pPhysicalDeviceCount) {
        /* 记录 PD→实例 归属 (供 iffp 按正确实例解析), 互斥保护 */
        pthread_mutex_lock(&g_lock);
        find_or_add_inst(instance);
        for (uint32_t i = 0; i < *pPhysicalDeviceCount && g_pd_cnt < MAX_PD; i++) {
            int found = 0;
            for (int j = 0; j < g_pd_cnt; j++)
                if (g_pds[j].pd == pPhysicalDevices[i]) { found = 1; break; }
            if (!found) {
                g_pds[g_pd_cnt].pd = pPhysicalDevices[i];
                g_pds[g_pd_cnt].inst = instance;
                g_pd_cnt++;
            }
        }
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "[VK_ICD] real enumPD n=%u:", *pPhysicalDeviceCount);
        for (uint32_t i = 0; i < *pPhysicalDeviceCount; i++)
            fprintf(stderr, " [%u]=%p", i, (void*)pPhysicalDevices[i]);
        fprintf(stderr, "\n");
    }
    return r;
}

/* ---- vkGetPhysicalDeviceImageFormatProperties 深度格式修正 ----
 * Adreno 540 HAL 对深度/模板格式的 imageFormatProperties 查询总是返回
 * VK_ERROR_FORMAT_NOT_SUPPORTED (-11), 连 D16_UNORM 都失败 (实测 imgfmt_probe),
 * 但 vkCreateImage 对这些格式实际全部成功。DXVK 的
 * D3D11CommonTexture::CheckImageSupport (d3d11_texture.cpp:453) 依赖此查询,
 * 误判为不支持 → 抛 "Cannot create texture" → Unity 游戏崩溃。
 * 这里拦截: 对深度/模板格式 (格式值在白名单 或 formatProperties 声称支持
 * DEPTH_STENCIL_ATTACHMENT) 且 usage 需要深度/模板时, 在 HAL 报错时
 * 返回宽松的 VK_SUCCESS + 保守能力值。
 */
static PFN_vkGetPhysicalDeviceImageFormatProperties g_real_iffp = NULL;
static PFN_vkGetPhysicalDeviceFormatProperties g_real_fmtp = NULL;

static int fmt_is_depth_stencil(VkFormat format) {
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_S8_UINT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return 1;
        default:
            return 0;
    }
}

static VkResult VKAPI_CALL shim_vkGetPhysicalDeviceImageFormatProperties(
        VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type,
        VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
        VkImageFormatProperties* pProperties) {
    if (!g_real_iffp || !g_real_fmtp) {
        /* 构造器 NULL 解析没成功 → 按 PD 所属实例解析 (互斥锁保护, 避免多实例竞态) */
        pthread_mutex_lock(&g_lock);
        VkInstance owner = inst_for_pd(physicalDevice);
        InstEntry* e = find_or_add_inst(owner);
        if (e) {
            if (!g_real_iffp && !e->iffp)
                e->iffp = (PFN_vkGetPhysicalDeviceImageFormatProperties)
                    g_get_proc(owner, "vkGetPhysicalDeviceImageFormatProperties");
            if (!g_real_fmtp && !e->fmtp)
                e->fmtp = (PFN_vkGetPhysicalDeviceFormatProperties)
                    g_get_proc(owner, "vkGetPhysicalDeviceFormatProperties");
            if (!g_real_iffp) g_real_iffp = e->iffp;
            if (!g_real_fmtp) g_real_fmtp = e->fmtp;
            fprintf(stderr, "[VK_ICD] iffp per-instance resolve owner=%p iffp=%p fmtp=%p\n",
                    (void*)owner, (void*)e->iffp, (void*)e->fmtp);
        }
        pthread_mutex_unlock(&g_lock);
        fflush(stderr);
    }
    if (!g_real_iffp) {
        fprintf(stderr, "[VK_ICD] 无法解析真实 vkGetPhysicalDeviceImageFormatProperties\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult r = g_real_iffp(physicalDevice, format, type, tiling, usage, flags, pProperties);
    if (r == VK_SUCCESS)
        return r;

    if (g_raw_test)
        return r;  /* 测试模式: 如实返回硬件结果, 不做放宽 */

    /* 深度/模板格式放宽。
     * 注意: DXVK 的 D3D11CommonTexture::CheckImageSupport 会分别用
     *   usage=DEPTH_STENCIL_ATTACHMENT(0x20)  -> 能否当深度附件
     *   usage=SAMPLED(0x4)                    -> 深度纹理能否当 SRV 采样
     * 两种 usage 各探测一次, 任一失败就把该 DXGI 格式整体标记为不支持并缓存,
     * 之后 Unity 每次建 RenderTexture 都直接从缓存返回失败 (实测 5594 次
     * "RenderTexture.Create failed" 而 vkCreateImage 一次都没被调到)。
     * 所以这里必须把 SAMPLED/TRANSFER 也一起放宽, 只挡住真正没意义的 usage
     * (STORAGE / COLOR_ATTACHMENT 等)。 */
    const VkImageUsageFlags ds_ok_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                        | VK_IMAGE_USAGE_SAMPLED_BIT
                                        | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
                                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                        | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fmt_is_depth_stencil(format) && (usage & ds_ok_usage) && !(usage & ~ds_ok_usage)) { /* 深度放宽: SPECIMEN 需要 */
        pProperties->maxExtent.width  = 8192;
        pProperties->maxExtent.height = 8192;
        pProperties->maxExtent.depth  = 1;
        pProperties->maxMipLevels     = 15;
        pProperties->maxArrayLayers   = 256;
        pProperties->sampleCounts     = VK_SAMPLE_COUNT_1_BIT
                                      | VK_SAMPLE_COUNT_2_BIT
                                      | VK_SAMPLE_COUNT_4_BIT;
        pProperties->maxResourceSize  = 0x80000000ull;
        SHIM_DBG("[VK_ICD] iffp 深度格式放宽: fmt=%d tiling=%d usage=0x%x -> VK_SUCCESS\n",
                (int)format, (int)tiling, (unsigned)usage);
        return VK_SUCCESS;
    }
    /* BC 压缩格式曾做过放宽(谎报 SAMPLED/TRANSFER 能力), 实测为负优化:
     * 让 Unity/DXVK 选中 Adreno 540 实际无法正确渲染的压缩格式做 RenderTexture,
     * 导致整屏黑(星白列车 VN 即因此黑屏)。Adreno 540 对 BC 报 0 能力位是如实
     * 行为, 不再谎报。 */
    return r;
}

/* ---- vkGetPhysicalDeviceFormatProperties 深度格式修正 ----
 * 与 iffp 同理: Adreno 540 HAL 的 vkGetPhysicalDeviceFormatProperties(/2) 对
 * 深度/模板格式(如 D32_SFloat_S8_UInt=94 / D32_SFLOAT_S8_UINT=130)不报
 * DEPTH_STENCIL_ATTACHMENT 特性位, Unity 据此判定 RenderTexture 的 depth/stencil
 * 格式不支持 -> RenderTexture.Create 失败 -> 游戏场景(渲染到 RenderTexture 的相机)
 * 整片黑屏, 仅 UI 文字可见。这里对深度/模板格式强制补上
 * DEPTH_STENCIL_ATTACHMENT(+SAMPLED)位 (D32S8 直接顶替为 D24S8 真实能力)。
 * 注意: BC 压缩格式与颜色格式的能力谎报均已移除, 实测对 Adreno 540 是负优化
 * (让 DXVK 选中无法正确渲染的格式 -> 整屏黑)。 */
static void fmtp_fix_depth(VkPhysicalDevice pd, VkFormat format, VkFormatProperties* p) {
    if (!p) return;
    if (g_raw_test) return;  /* 测试模式: 如实暴露硬件能力, 不做任何修补 */
    if (fmt_is_depth_stencil(format)) { /* 深度谎报: SPECIMEN 需要 */
        if (format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
            VkFormatProperties good;
            memset(&good, 0, sizeof(good));
            if (g_real_fmtp) g_real_fmtp(pd, VK_FORMAT_D24_UNORM_S8_UINT, &good);
            *p = good;
            return;
        }
        p->optimalTilingFeatures |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                                 | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        p->linearTilingFeatures  |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                                 | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        return;
    }
    /* 曾对"可采样但缺 COLOR_ATTACHMENT"的格式补 COLOR_ATTACHMENT 位(颜色格式谎报),
     * 实测是负优化: Unity/DXVK 会选中 Adreno 540 实际不能当 color attachment 渲染的
     * 格式做 RenderTexture, 整屏黑 (星白列车 VN 即因此黑屏)。HAL 真正支持的 RT 颜色
     * 格式本来就带 COLOR_ATTACHMENT 位, 无需谎报, 故此处仅如实回传能力。
     * (BC 压缩格式谎报亦已移除, 同理。) */
    /* 诊断: 真实 HAL 报告完全不支持 (opt/lin 均为 0) 的格式 */
    if (format >= 40 && format <= 220 &&
        p->optimalTilingFeatures == 0 && p->linearTilingFeatures == 0)
        SHIM_DBG("[VK_ICD] fmtp UNSUPPORTED fmt=%d\n", (int)format);
}

/* 顺便修正 pNext 链里的 VkFormatProperties3KHR(2KHR 特性位, 64-bit) */
static void fmtp_fix_depth_pnext(VkFormat format, VkFormatProperties2* p) {
    if (!p) return;
    if (!fmt_is_depth_stencil(format)) return;  /* 只修深度/模板格式; 颜色/BC 不再谎报 */
    /* 2KHR 位: SAMPLED=0x1, DEPTH_STENCIL_ATTACHMENT=0x200 */
    VkFlags64 add = 0x200ULL | 0x1ULL;
    struct VkBaseOutStruct { VkStructureType sType; struct VkBaseOutStruct* pNext; };
    struct VkBaseOutStruct* it = (struct VkBaseOutStruct*)p->pNext;
    while (it) {
        if (it->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR) {
            typedef struct { VkStructureType sType; void* pNext;
                             VkFlags64 optimalTilingFeatures;
                             VkFlags64 linearTilingFeatures;
                             VkFlags64 bufferFeatures; } Fmt3;
            Fmt3* p3 = (Fmt3*)it;
            p3->optimalTilingFeatures |= add;
            p3->linearTilingFeatures  |= add;
        }
        it = it->pNext;
    }
}

static void VKAPI_CALL shim_vkGetPhysicalDeviceFormatProperties(
        VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties* pProperties) {
    if (!g_real_fmtp) {
        pthread_mutex_lock(&g_lock);
        VkInstance owner = inst_for_pd(physicalDevice);
        InstEntry* e = find_or_add_inst(owner);
        if (e && !e->fmtp)
            e->fmtp = (PFN_vkGetPhysicalDeviceFormatProperties)
                g_get_proc(owner, "vkGetPhysicalDeviceFormatProperties");
        if (!g_real_fmtp) g_real_fmtp = e ? e->fmtp : NULL;
        pthread_mutex_unlock(&g_lock);
    }
    if (!g_real_fmtp) return;
    g_real_fmtp(physicalDevice, format, pProperties);
    fmtp_fix_depth(physicalDevice, format, pProperties);
}

static PFN_vkGetPhysicalDeviceFormatProperties2 g_real_fmtp2 = NULL;
static void VKAPI_CALL shim_vkGetPhysicalDeviceFormatProperties2(
        VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties2* pProperties) {
    if (!g_real_fmtp2) {
        pthread_mutex_lock(&g_lock);
        VkInstance owner = inst_for_pd(physicalDevice);
        g_real_fmtp2 = (PFN_vkGetPhysicalDeviceFormatProperties2)
            g_get_proc(owner, "vkGetPhysicalDeviceFormatProperties2");
        pthread_mutex_unlock(&g_lock);
    }
    if (!g_real_fmtp2) { /* 退化到 v1 */
        shim_vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &pProperties->formatProperties);
        return;
    }
    g_real_fmtp2(physicalDevice, format, pProperties);
    fmtp_fix_depth(physicalDevice, format, &pProperties->formatProperties);
    fmtp_fix_depth_pnext(format, pProperties);
}

/* 缓存物理设备的内存类型表: vkAllocateMemory 里只有 VkDevice, 拿不到 PD,
 * 而判断"这块内存要不要走低位 dmabuf"必须看 HOST_VISIBLE 位。 */
static VkPhysicalDeviceMemoryProperties g_mem_props;
static int g_mem_props_ok = 0;

static void icd_cache_mem_props(VkPhysicalDevice pd) {
    if (g_mem_props_ok) return;
    PFN_vkGetPhysicalDeviceMemoryProperties f =
        (PFN_vkGetPhysicalDeviceMemoryProperties)g_get_proc(inst_for_pd(pd),
                                                            "vkGetPhysicalDeviceMemoryProperties");
    if (!f) {
        fprintf(stderr, "[VK_ICD] 取不到 vkGetPhysicalDeviceMemoryProperties, 低位 dmabuf 关闭\n");
        return;
    }
    f(pd, &g_mem_props);
    g_mem_props_ok = 1;
    fprintf(stderr, "[VK_ICD] 内存类型表 (%u 种):", g_mem_props.memoryTypeCount);
    for (uint32_t i = 0; i < g_mem_props.memoryTypeCount; i++)
        fprintf(stderr, " [%u]heap%u%s", i, g_mem_props.memoryTypes[i].heapIndex,
                (g_mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
                    ? "*HV" : "");
    fprintf(stderr, "\n");
    fflush(stderr);
}

static VkResult VKAPI_CALL shim_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                               const VkDeviceCreateInfo* pCreateInfo,
                                               const VkAllocationCallbacks* pAllocator,
                                               VkDevice* pDevice) {
    if (!g_real_create_device) {
        VkInstance inst = g_real_icd_instance ? g_real_icd_instance : (VkInstance)physicalDevice;
        g_real_create_device = (PFN_vkCreateDevice)g_get_proc(inst, "vkCreateDevice");
        if (!g_real_create_device) {
            fprintf(stderr, "[VK_ICD] 无法解析真实 vkCreateDevice\n");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    if (getenv("VK_ICD_DIAG")) {
        VkInstance owner = inst_for_pd(physicalDevice);
        PFN_vkCreateDevice per_pd = (PFN_vkCreateDevice)g_get_proc(owner, "vkCreateDevice");
        fprintf(stderr, "[VK_ICD] diag: g_real_icd_instance=%p pd_owner=%p create_cached=%p create_perpd=%p %s\n",
                (void*)g_real_icd_instance, (void*)owner, (void*)g_real_create_device, (void*)per_pd,
                (g_real_create_device == per_pd) ? "SAME" : "**DIFF**");
    }

    fprintf(stderr, "[VK_ICD] vkCreateDevice pd=%p g_inst=%p alloc=%p real_create=%p sType=%u flags=0x%x layerCount=%u\n",
            (void*)physicalDevice, (void*)g_real_icd_instance, (const void*)pAllocator,
            (void*)g_real_create_device, (unsigned)pCreateInfo->sType, (unsigned)pCreateInfo->flags,
            (unsigned)pCreateInfo->enabledLayerCount);
    if (getenv("VK_ICD_PD_DUMP")) {
        /* 确认 real_create 在 HAL 内的精确偏移 + 首条指令 */
        {
            Dl_info di;
            if (dladdr((void*)g_real_create_device, &di)) {
                fprintf(stderr, "[VK_ICD] real_create dladdr: fbase=%p fname=%s sname=%s off=0x%lx\n",
                        di.dli_fbase, di.dli_fname ? di.dli_fname : "?",
                        di.dli_sname ? di.dli_sname : "?",
                        (unsigned long)((uintptr_t)g_real_create_device - (uintptr_t)di.dli_fbase));
            }
            const uint32_t* code = (const uint32_t*)g_real_create_device;
            fprintf(stderr, "[VK_ICD] real_create code: %08x %08x %08x %08x | %08x %08x %08x %08x\n",
                    code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7]);
            fflush(stderr);
        }
        /* 复现 HAL vkCreateDevice 内部读取链(2026-08-08 反汇编修正):
         *   9a2f0: bl 0xf907c  → x20 = pd->[0x8]        (0xf907c = ldr x0,[x0,#0x8]; ret)
         *   be204 trampoline  → temp->[0x30] = x20
         *   b6064: ldr x8,[x0,#0x30]                    (x8 = 内部 pd)
         *   b6070: ldr x0,[x8,#0xcc0]                   (alloc = 内部pd->[0xcc0])
         *   b6074: bl 0xb5ee0 (vtable[0x40])            → b5ef4
         *   b5f00: ldr x1,[x0,#0x8]                     (ctx = alloc->[0x8])
         *   b5f04: ldr w2,[x0,#0x10]                    (内存类型索引)
         *   → fa6b0 → fa234: x0=ctx, x1=0x108, w3=内存类型
         *   fa264: cbz ctx → calloc 路径 (应成功)
         *   fa334: ldr x9,[ctx,#0x10] (pfnAllocation)
         *   fa338: cbz x9 → 返回 NULL → HAL 返回 -1
         * 若 ctx!=NULL 且 pfn==NULL → 0xfa234 返回 NULL → HAL 返回 -1 */
        volatile uintptr_t pd = (uintptr_t)physicalDevice;
        volatile uintptr_t pd8 = 0, alloc = 0, ctx = 0, pfn = 0, pfn_realloc = 0, pfn_free = 0;
        volatile uintptr_t vtbl = 0, slot40 = 0, mtype = 0;
        /* 交叉验证: pd 是否与 shim 枚举到的 HAL 物理设备对象一致 */
        {
            int in_enum = 0;
            for (int j = 0; j < g_pd_cnt; j++)
                if ((uintptr_t)g_pds[j].pd == pd) { in_enum = 1; break; }
            fprintf(stderr, "[VK_ICD] PDDUMP pd==enumPD? %s (g_pd_cnt=%d)\n",
                    in_enum ? "SAME" : "**DIFF**", g_pd_cnt);
        }
        /* 每一跳都先探页: 0xcc0 这条链只在 32 位形态下成立, 64 位裸读会 SIGSEGV */
        if (icd_readable((void*)pd, 0x10)) {
            pd8 = *(volatile uintptr_t*)(pd + 0x8);
            if (icd_readable((void*)(pd8 + 0xcc0), 0x8))
                alloc = *(volatile uintptr_t*)(pd8 + 0xcc0);
            else if (pd8)
                fprintf(stderr, "[VK_ICD] PDDUMP pd8=%p 的 +0xcc0 不可读 (非 32 位布局), 跳过后续解引用\n",
                        (void*)pd8);
            if (icd_readable((void*)alloc, 0x18)) {
                ctx = *(volatile uintptr_t*)(alloc + 0x8);
                mtype = *(volatile uint32_t*)(alloc + 0x10);
                vtbl = *(volatile uintptr_t*)alloc;
                if (icd_readable((void*)vtbl, 0x48)) slot40 = *(volatile uintptr_t*)(vtbl + 0x40);
                fprintf(stderr, "[VK_ICD] PDDUMP pd=%p pd8=%p alloc=%p alloc->[0x10]=%u vtable=%p slot40=%p\n",
                        (void*)pd, (void*)pd8, (void*)alloc, (uint32_t)mtype,
                        (void*)vtbl, (void*)slot40);
            }
            if (icd_readable((void*)ctx, 0x28)) {
                pfn = *(volatile uintptr_t*)(ctx + 0x10);
                pfn_realloc = *(volatile uintptr_t*)(ctx + 0x18);
                pfn_free = *(volatile uintptr_t*)(ctx + 0x20);
            } else if (ctx) {
                fprintf(stderr, "[VK_ICD] PDDUMP ctx=%p 不可读 (guest 侧残留指针?)\n", (void*)ctx);
            }
        } else {
            fprintf(stderr, "[VK_ICD] PDDUMP pd=%p 不可读, 跳过\n", (void*)pd);
        }
        fprintf(stderr, "[VK_ICD] PDDUMP pd->[0x8]=%p ->[0xcc0](alloc)=%p ctx(alloc->[0x8])=%p pfnAlloc(ctx->[0x10])=%p pfnRealloc=%p pfnFree=%p\n",
                (void*)pd8, (void*)alloc, (void*)ctx, (void*)pfn, (void*)pfn_realloc, (void*)pfn_free);
        if (pd8 == 0)
            fprintf(stderr, "[VK_ICD] PDDUMP -> pd->[0x8]==NULL → 9a2f4 cbz → 返回 -4\n");
        else if (alloc == 0)
            fprintf(stderr, "[VK_ICD] PDDUMP -> alloc==NULL → b6074 传入 x0=0 → vtable 读 [0] 崩溃 或 b5ee0 段错误\n");
        else if (ctx == 0)
            fprintf(stderr, "[VK_ICD] PDDUMP -> ctx==NULL → 0xfa234 fa41c calloc 路径 (应成功!)\n");
        else if (pfn == 0)
            fprintf(stderr, "[VK_ICD] PDDUMP -> ctx!=NULL 且 pfnAllocation==NULL → 0xfa234 fa338 返回 NULL → -1 (根因锁定!)\n");
        else
            fprintf(stderr, "[VK_ICD] PDDUMP -> ctx!=NULL 且 pfnAllocation!=NULL → 走 custom 分配器 pfn=%p\n", (void*)pfn);
        fflush(stderr);
    }
    const VkAllocationCallbacks* dev_alloc = icd_pick_alloc(pAllocator, "vkCreateDevice");
    {
        const VkBaseInStructure* s = (const VkBaseInStructure*)pCreateInfo->pNext;
        while (s) {
            fprintf(stderr, "  pNext sType=%d\n", (int)s->sType);
            s = s->pNext;
        }
        for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
            const VkDeviceQueueCreateInfo* q = &pCreateInfo->pQueueCreateInfos[i];
            fprintf(stderr, "  queue[%u] family=%u count=%u prio=%f\n", i,
                    q->queueFamilyIndex, q->queueCount,
                    (q->pQueuePriorities ? (double)q->pQueuePriorities[0] : -1.0));
        }
    }

    uint32_t n = pCreateInfo->enabledExtensionCount;
    /* +8: 后面要追加 external_memory_fd 一族 (低位 dmabuf 映射用) */
    const char** keep = calloc(n + 8, sizeof(char*));
    uint32_t keep_count = 0;
    fprintf(stderr, "[VK_ICD] shim_vkCreateDevice pd=%p n=%u qf=%u:\n",
            (void*)physicalDevice, n, pCreateInfo->queueCreateInfoCount);
    for (uint32_t i = 0; i < n; i++) {
        const char* name = pCreateInfo->ppEnabledExtensionNames[i];
        fprintf(stderr, "  [%u] %s\n", i, name);
        if (ext_in_wsi_keep(name)) {
            /* WSI 扩展由 Sky1 layer 实现, 直接放行 */
            keep[keep_count++] = name;
        } else if (ext_in_strip_blacklist(name)) {
            /* win32 平台扩展: Adreno HAL 不支持, 剥离 */
            fprintf(stderr, "[VK_ICD] 剥离 win32 平台扩展: %s\n", name);
        } else {
            keep[keep_count++] = name;
        }
    }

    /* 追加低位 dmabuf 方案所需的扩展。DXVK 不会主动开这些, 但 shim 内部要用:
     * vkAllocateMemory 里注入 export+dedicated, vkMapMemory 里 getFd 后自己
     * 把 dmabuf 映射到 <4GB。四个都已确认 Adreno 540 HAL 支持。 */
    {
        static const char* need[] = {
            "VK_KHR_external_memory", "VK_KHR_external_memory_fd",
            "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2",
        };
        for (unsigned k = 0; k < sizeof(need) / sizeof(need[0]); k++) {
            int have = 0;
            for (uint32_t i = 0; i < keep_count; i++)
                if (!strcmp(keep[i], need[k])) { have = 1; break; }
            if (!have) {
                keep[keep_count++] = need[k];
                fprintf(stderr, "[VK_ICD] 追加扩展: %s\n", need[k]);
            }
        }
    }
    icd_cache_mem_props(physicalDevice);

    VkDeviceCreateInfo ci = *pCreateInfo;
    ci.enabledExtensionCount = keep_count;
    ci.ppEnabledExtensionNames = keep;
    /* pNext 清理: 上游(winevulkan/loader)把 DXVK 的 pNext 重写成了
     * [VkPhysicalDeviceFeatures2(48) ×2, ShaderDrawParameters], 重复的
     * Features2 违反规范(pEnabledFeatures 非空时 pNext 不应含 Features2),
     * Adreno HAL 对非法组合返回 OUT_OF_HOST_MEMORY。核心特性已由
     * pEnabledFeatures 启用, 附加扩展特性结构(transform feedback 等)
     * Adreno 540 均不支持(特性全 0), 故移除整个 pNext 链。
     */
    fprintf(stderr, "[VK_ICD] pEnabledFeatures=%p, 剥离 pNext\n", (void*)ci.pEnabledFeatures);
    ci.pNext = NULL;
    {
        const VkPhysicalDeviceFeatures* f = ci.pEnabledFeatures;
        fprintf(stderr, "[VK_ICD] layerCount=%u flags=0x%x", ci.enabledLayerCount, (unsigned)ci.flags);
        if (f) {
            const VkBool32* b = (const VkBool32*)f;
            const char* names[] = {
                "robustBufferAccess","fullDrawIndexUint32","imageCubeArray","independentBlend",
                "geometryShader","tessellationShader","sampleRateShading","dualSrcBlend",
                "logicOp","multiDrawIndirect","drawIndirectFirstInstance","depthClamp",
                "depthBiasClamp","fillModeNonSolid","depthBounds","wideLines",
                "largePoints","alphaToOne","multiViewport","samplerAnisotropy",
            };
            fprintf(stderr, " features:");
            for (int i = 0; i < 20; i++) fprintf(stderr, " %s=%u", names[i], (unsigned)b[i]);
        }
        fprintf(stderr, "\n");
    }
#ifdef EXPERIMENT_MIN_FEATURES
    /* 实验: 只启用 vertexPipelineStoresAndAtomics (repro 成功的形态) */
    if (ci.pEnabledFeatures) {
        memset(&g_fake_feats, 0, sizeof(g_fake_feats));
        g_fake_feats.vertexPipelineStoresAndAtomics = VK_TRUE;
        ci.pEnabledFeatures = &g_fake_feats;
        fprintf(stderr, "[VK_ICD] 实验: 替换 pEnabledFeatures 为仅 vertexPipelineStoresAndAtomics\n");
    }
#endif

#ifdef EXPERIMENT_REBUILD
    /* 实验2: 完全重建干净 VkDeviceCreateInfo, 排除 winevulkan 传参脏数据。
     * 只保留 queue(family/count 从传入读) 和扩展名指针, 其余全部零初始化。 */
    if (getenv("VK_ICD_REBUILD")) {
        static VkDeviceQueueCreateInfo g_clean_qci;
        static float g_clean_prio = 1.0f;
        const VkDeviceQueueCreateInfo* orig_q = pCreateInfo->pQueueCreateInfos;
        uint32_t orig_qn = pCreateInfo->queueCreateInfoCount;
        if (!orig_qn) { orig_q = NULL; }
        g_clean_qci = (VkDeviceQueueCreateInfo){0};
        g_clean_qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        g_clean_qci.queueFamilyIndex = orig_q ? orig_q[0].queueFamilyIndex : 0;
        g_clean_qci.queueCount = orig_q ? orig_q[0].queueCount : 1;
        g_clean_qci.pQueuePriorities = &g_clean_prio;
        static VkDeviceQueueCreateInfo g_clean_qs[2];
        g_clean_qs[0] = g_clean_qci;
        if (orig_qn > 1) {
            g_clean_qs[1] = (VkDeviceQueueCreateInfo){0};
            g_clean_qs[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            g_clean_qs[1].queueFamilyIndex = orig_q[1].queueFamilyIndex;
            g_clean_qs[1].queueCount = orig_q[1].queueCount;
            g_clean_qs[1].pQueuePriorities = &g_clean_prio;
        }
        VkDeviceCreateInfo rebuild = (VkDeviceCreateInfo){0};
        rebuild.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        rebuild.queueCreateInfoCount = (orig_qn > 2) ? 2 : orig_qn;
        rebuild.pQueueCreateInfos = g_clean_qs;
        rebuild.enabledExtensionCount = keep_count;
        rebuild.ppEnabledExtensionNames = keep;
        rebuild.pEnabledFeatures = &g_fake_feats;
        fprintf(stderr, "[VK_ICD] 实验2: 重建干净 VkDeviceCreateInfo (qf=%u ext=%u)\n",
                rebuild.queueCreateInfoCount, keep_count);
        if (getenv("VK_ICD_NOEXT")) {
            rebuild.enabledExtensionCount = 0;
            rebuild.ppEnabledExtensionNames = NULL;
            fprintf(stderr, "[VK_ICD] 实验3: 零扩展强制 (VK_ICD_NOEXT)\n");
        }
        VkResult r2 = g_real_create_device(physicalDevice, &rebuild, dev_alloc, pDevice);
        fprintf(stderr, "[VK_ICD] [REBUILD] real vkCreateDevice -> %d (%s)\n", r2,
                r2 == VK_SUCCESS ? "VK_SUCCESS" : (r2 == VK_ERROR_EXTENSION_NOT_PRESENT ? "EXTENSION_NOT_PRESENT" : "other"));
        return r2;
    }
#endif
    fprintf(stderr, "[VK_ICD] 转发: qf=%u ext=%u pEnabledFeatures=%p\n",
            ci.queueCreateInfoCount, keep_count, (void*)ci.pEnabledFeatures);

    VkResult r = g_real_create_device(physicalDevice, &ci, dev_alloc, pDevice);
    fprintf(stderr, "[VK_ICD] real vkCreateDevice -> %d (%s)\n", r,
            r == VK_SUCCESS ? "VK_SUCCESS" : (r == VK_ERROR_EXTENSION_NOT_PRESENT ? "EXTENSION_NOT_PRESENT" : "other"));
    if (keep) free(keep);
    return r;
}

/* ---- vkMapMemory 诊断 ----
 * BOX64_MMAP32=0 修好 vkCreateDevice 后, DXVK 在 DxvkMemoryAllocator 里报
 * "Mapping memory failed with VK_ERROR_OUT_OF_HOST_MEMORY"。需要区分两种可能:
 *   (a) HAL 自己返回 OOM                → 是驱动/低内存问题
 *   (b) HAL 返回 VK_SUCCESS 但指针 >4GB → 是 wine wow64 表示不了, 上层改写成 OOM
 * 这里记录 HAL 的真实返回值与指针高位, 直接判定。 */
static PFN_vkGetDeviceProcAddr g_real_get_dev_proc = NULL;
static PFN_vkMapMemory         g_real_map_memory   = NULL;
static unsigned g_map_ok = 0, g_map_hi = 0, g_map_fail = 0;

/* ==== 零拷贝低位映射: 导出 dmabuf, 自己 mmap 到 <4GB ====
 * 地址空间的两个要求互相矛盾:
 *   - HAL 内部结构 (上万个 kgsl/匿名映射) 必须留在高位, 否则 vkCreateDevice = -1
 *   - vkMapMemory 返回给 32 位 guest 的指针必须 <4GB, 否则 wine wow64 表示不了,
 *     会 unmap 并合成 VK_ERROR_OUT_OF_HOST_MEMORY → DXVK 报 Mapping memory failed → 白屏
 * BOX64_MMAP32 是全进程开关, 两者不可兼得; 曾试图在 box64 里加线程作用域开关,
 * 但 box64 跑 guest 时会换掉 TPIDR_EL0, 在 mmap 钩子里读 __thread 会拿到垃圾,
 * 误入 box_mmap 后在其内部互斥锁上自死锁 (实测 futex_wait, 1 tick CPU)。
 *
 * 直接搬 kgsl 映射的三条路 2026-08-08 已全部实测排除:
 *   1) mremap(old_size=0) 建别名 → EFAULT(14)。内核 4.4.194 本身支持该语义
 *      (t_mremap.c 里匿名/文件共享映射都成功), 但 kgsl 显存 VMA 由
 *      remap_pfn_range 建立, 带 VM_PFNMAP|VM_DONTEXPAND, 在 vma_to_resize()
 *      的 new_len > old_len 分支被 `goto Efault` 挡掉。
 *   2) 同 (fd,offset) 在低位再 mmap 一次 → EBUSY(16)。KGSL 是 SVM 驱动,
 *      一个显存对象只允许一个用户态映射 (kgsl_mmap: useraddr != 0 → EBUSY)。
 *   3) 先 munmap 高位再在低位重建 → EINVAL(22), 且连恢复原址都失败。
 *      SVM 下 GPU VA == CPU VA, 地址是锁死的, 根本搬不动。
 *
 * 能走通的是 external memory: dmabuf 不受 KGSL SVM 约束, 可以随便 mmap。
 * 2026-08-08 实测矩阵 (t_extfd4.c):
 *   - buffer 必须带 VkExternalMemoryBufferCreateInfo, 否则 HAL 段错误
 *   - 分配必须带 VkMemoryDedicatedAllocateInfo, 否则 vkGetMemoryFdKHR 返回 -1
 *     (HAL 报 OPAQUE_FD 是 DEDICATED_ONLY)
 *   - 满足以上两点后 memoryType 0..4 全部可导出, 类型 5 (heap1) 不行
 *   - 导出的 fd 是 anon_inode:dmabuf, mmap 到 0x20000000 成功, 双向读写一致
 *   - dedicated 内存上仍可再 vkBindBufferMemory 别的 buffer (offset 0 / 64K 都行),
 *     所以 DXVK 的 chunk suballocation 能直接跑在上面; 16MB / 64MB 大块也通过
 *
 * 于是策略: vkAllocateMemory 时对 HOST_VISIBLE 的内存偷偷造一个同尺寸的
 * "靶子 buffer" 做 dedicated 目标并声明可导出; vkMapMemory 时导出 fd 再自行
 * 映射到低位, 把低位指针交给 guest。HAL 手里的高位映射原封不动。 */

/* 在 <4GB 里挑一段长度为 len 的空闲区间。直接用 mmap(hint, PROT_NONE) 试探:
 * 内核给回的地址若仍在低位就采纳, 否则换下一个 hint。占位成功后由
 * mremap(MREMAP_FIXED) 覆盖 —— MREMAP_FIXED 会自动 unmap 目标区间。
 * 注意 box64 劫持了 mmap: 但 addr 非 NULL 时它直接走 InternalMmap 透传, hint 有效。*/
static void* icd_reserve_low(size_t len) {
    /* 避开 wine/box64 常用的低端区域, 从 512MB 往上按 64MB 步进探 */
    for (uintptr_t hint = 0x20000000UL; hint < 0xF0000000UL; hint += 0x4000000UL) {
        void* r = mmap((void*)hint, len, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (r == MAP_FAILED) continue;
        if (((uintptr_t)r >> 32) == 0 && (uintptr_t)r + len <= 0x100000000UL)
            return r;
        munmap(r, len);   /* 内核没听 hint, 落到高位, 丢掉重试 */
    }
    return NULL;
}

/* 每块被接管的 VkDeviceMemory 的记账。
 * buf  = 为满足 DEDICATED_ONLY 而造的靶子 buffer, 随内存一起销毁
 * fd   = vkGetMemoryFdKHR 导出的 dmabuf, 每块只导一次 (每次调用都会新建 fd, 会漏)
 * lo   = 我们自己映射到 <4GB 的地址, len 是其长度 */
#define ICD_ALIAS_MAX 4096
typedef struct {
    VkDeviceMemory mem;
    VkDevice       dev;
    VkBuffer       buf;
    VkDeviceSize   size;      /* 实际分配的字节数 (含靶子 buffer 的对齐膨胀) */
    int            fd;
    void*          lo;
    size_t         len;
    int            hal_mapped; /* 是否已对 HAL 调过 vkMapMemory, 决定要不要配对 unmap */
} IcdMemEntry;
static IcdMemEntry g_alias[ICD_ALIAS_MAX];
static unsigned g_alias_n = 0;
static pthread_mutex_t g_alias_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_alias_ok = 0, g_alias_fail = 0;
static int g_alias_enabled = 1;

static void icd_init_alias(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char* s = getenv("VK_ICD_MAP_LOW");
    if (s && !strcmp(s, "0")) {
        g_alias_enabled = 0;
        fprintf(stderr, "[VK_ICD] VK_ICD_MAP_LOW=0, 关闭低位 dmabuf 映射 (仅诊断)\n");
    } else if (g_raw_test) {
        g_alias_enabled = 0;
        fprintf(stderr, "[VK_ICD] VK_TEST_RAW=1, 关闭低位 dmabuf 映射 (仅测试用)\n");
    } else {
        fprintf(stderr, "[VK_ICD] 低位 dmabuf 映射已启用: 分配时注入 export+dedicated, "
                        "map 时导出 fd 自行映射到 <4GB\n");
    }
    fflush(stderr);
}

/* 需要用到的真实设备函数, 在 vkGetDeviceProcAddr 首次命中时惰性解析 */
static PFN_vkAllocateMemory              g_real_alloc_mem = NULL;
static PFN_vkFreeMemory                  g_real_free_mem  = NULL;
static PFN_vkCreateBuffer                g_real_create_buf = NULL;
static PFN_vkDestroyBuffer               g_real_destroy_buf = NULL;
static PFN_vkGetBufferMemoryRequirements g_real_buf_reqs = NULL;
static PFN_vkGetMemoryFdKHR              g_real_get_mem_fd = NULL;

static IcdMemEntry* icd_find_entry(VkDeviceMemory mem) {
    for (unsigned i = 0; i < g_alias_n; i++)
        if (g_alias[i].mem == mem) return &g_alias[i];
    return NULL;
}

/* pNext 链里是否已经有会跟我们打架的结构 (应用自己搞了 dedicated / external) */
static int icd_pnext_conflicts(const void* pNext) {
    const VkBaseInStructure* s = (const VkBaseInStructure*)pNext;
    while (s) {
        switch ((int)s->sType) {
            case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO:
            case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO:
            case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR:
                return 1;
            default: break;
        }
        s = s->pNext;
    }
    return 0;
}

/* vkAllocateMemory: 对 HOST_VISIBLE 的分配注入 export + dedicated,
 * 让它以后能导出 dmabuf。做不到就静默回退到原始分配, 不影响正确性。 */
static unsigned g_alloc_taken = 0, g_alloc_plain = 0, g_alloc_diag = 0;
static VkResult VKAPI_CALL shim_vkAllocateMemory(VkDevice device,
                                                 const VkMemoryAllocateInfo* pAllocateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkDeviceMemory* pMemory) {
    int diag = (g_alloc_diag++ < 16);
    uint32_t ti = pAllocateInfo->memoryTypeIndex;
    int host_visible = g_mem_props_ok && ti < g_mem_props.memoryTypeCount &&
        (g_mem_props.memoryTypes[ti].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    /* 类型 5 在 heap1, 实测带 export 分配必失败 (-3); 超大块也不碰 */
    int eligible = g_alias_enabled && host_visible && g_real_create_buf && g_real_buf_reqs &&
                   ti != 5 && pAllocateInfo->allocationSize <= (256u << 20) &&
                   !icd_pnext_conflicts(pAllocateInfo->pNext) &&
                   g_alias_n < ICD_ALIAS_MAX;

    if (!eligible) {
        g_alloc_plain++;
        if (diag)
            fprintf(stderr, "[VK_ICD] alloc 原样转发: type=%u size=%lluKB (host_visible=%d)\n",
                    ti, (unsigned long long)pAllocateInfo->allocationSize / 1024, host_visible);
        return g_real_alloc_mem(device, pAllocateInfo, pAllocator, pMemory);
    }

    /* 靶子 buffer: 必须声明 external handle type, 否则 HAL 在 alloc 里段错误 */
    VkExternalMemoryBufferCreateInfo embci = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &embci,
        .size = pAllocateInfo->allocationSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
               | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
               | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer target = VK_NULL_HANDLE;
    if (g_real_create_buf(device, &bci, NULL, &target) != VK_SUCCESS) {
        g_alloc_plain++;
        if (diag) fprintf(stderr, "[VK_ICD] alloc: 靶子 buffer 建不出来, 回退原样分配\n");
        return g_real_alloc_mem(device, pAllocateInfo, pAllocator, pMemory);
    }
    VkMemoryRequirements mr;
    g_real_buf_reqs(device, target, &mr);
    /* dedicated 要求 allocationSize == 资源的 memReq.size; 靶子会因对齐略微膨胀,
     * 多分一点无害, guest 只用前 allocationSize 字节 */
    if (mr.size < pAllocateInfo->allocationSize) mr.size = pAllocateInfo->allocationSize;

    VkMemoryDedicatedAllocateInfo mdai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = (void*)pAllocateInfo->pNext, .buffer = target };
    VkExportMemoryAllocateInfo emai = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &mdai,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkMemoryAllocateInfo mai = *pAllocateInfo;
    mai.pNext = &emai;
    mai.allocationSize = mr.size;

    VkResult r = g_real_alloc_mem(device, &mai, pAllocator, pMemory);
    if (r != VK_SUCCESS) {
        g_real_destroy_buf(device, target, NULL);
        g_alloc_plain++;
        if (diag)
            fprintf(stderr, "[VK_ICD] alloc: 可导出分配失败(%d), 回退原样分配 type=%u\n", r, ti);
        return g_real_alloc_mem(device, pAllocateInfo, pAllocator, pMemory);
    }

    pthread_mutex_lock(&g_alias_lock);
    if (g_alias_n < ICD_ALIAS_MAX) {
        g_alias[g_alias_n] = (IcdMemEntry){ .mem = *pMemory, .dev = device, .buf = target,
                                            .size = mr.size, .fd = -1, .lo = NULL };
        g_alias_n++;
        g_alloc_taken++;
    }
    pthread_mutex_unlock(&g_alias_lock);
    if (diag)
        fprintf(stderr, "[VK_ICD] alloc 已接管: type=%u 请求=%lluKB 实分=%lluKB mem=%p\n",
                ti, (unsigned long long)pAllocateInfo->allocationSize / 1024,
                (unsigned long long)mr.size / 1024, (void*)(uintptr_t)*pMemory);
    return VK_SUCCESS;
}

/* 导出 dmabuf 并映射到 <4GB。成功返回低位指针, 失败返回 NULL。 */
static unsigned g_alias_diag = 0;
static void* icd_map_low(IcdMemEntry* e, VkDeviceSize offset, VkDeviceSize size) {
    int diag = (g_alias_diag++ < 12);
    if (!g_real_get_mem_fd) {
        if (diag) fprintf(stderr, "[VK_ICD]   低位映射失败: 没有 vkGetMemoryFdKHR\n");
        return NULL;
    }
    if (e->fd < 0) {
        /* 每次调用都会新建一个 dmabuf fd, 只导一次并缓存, 否则 fd 会漏光 */
        VkMemoryGetFdInfoKHR gfi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = e->mem, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
        int fd = -1;
        VkResult r = g_real_get_mem_fd(e->dev, &gfi, &fd);
        if (r != VK_SUCCESS || fd < 0) {
            if (diag) fprintf(stderr, "[VK_ICD]   低位映射失败: vkGetMemoryFdKHR -> %d fd=%d\n", r, fd);
            return NULL;
        }
        e->fd = fd;
    }

    VkDeviceSize want = (size == VK_WHOLE_SIZE) ? (e->size - offset) : size;
    /* mmap 的 offset 必须页对齐, 零头加回到返回指针上 */
    VkDeviceSize base = offset & ~(VkDeviceSize)0xFFF;
    size_t delta = (size_t)(offset - base);
    size_t len = (size_t)(want + delta);
    len = (len + 0xFFF) & ~(size_t)0xFFF;
    if (base + len > e->size) len = (size_t)(e->size - base);

    void* lo = icd_reserve_low(len);
    if (!lo) {
        if (diag) fprintf(stderr, "[VK_ICD]   低位映射失败: <4GB 找不到 %.1fMB 空洞\n", len / 1048576.0);
        return NULL;
    }
    void* p = mmap(lo, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, e->fd, (off_t)base);
    if (p == MAP_FAILED) {
        if (diag) fprintf(stderr, "[VK_ICD]   低位映射失败: mmap(dmabuf fd=%d off=%llu) errno=%d (%s)\n",
                          e->fd, (unsigned long long)base, errno, strerror(errno));
        munmap(lo, len);
        return NULL;
    }
    if (((uintptr_t)p >> 32) != 0) {   /* 内核没听 hint, 白搭 */
        if (diag) fprintf(stderr, "[VK_ICD]   低位映射失败: 内核把 dmabuf 放到了高位 %p\n", p);
        munmap(p, len);
        return NULL;
    }
    e->lo = p;
    e->len = len;
    return (void*)((uintptr_t)p + delta);
}

static VkResult VKAPI_CALL shim_vkMapMemory(VkDevice device, VkDeviceMemory memory,
                                            VkDeviceSize offset, VkDeviceSize size,
                                            VkMemoryMapFlags flags, void** ppData) {
    pthread_mutex_lock(&g_alias_lock);
    IcdMemEntry* e = icd_find_entry(memory);
    void* low = NULL;
    if (e && g_alias_enabled) {
        /* 仍然对 HAL 走一次真 map: SVM 下 CPU 映射同时确立 GPU VA, 不能省。
         * 它返回的高位指针我们不用, 但 unmap 时要配对还回去。 */
        void* hi = NULL;
        VkResult rr = g_real_map_memory(device, memory, offset, size, flags, &hi);
        e->hal_mapped = (rr == VK_SUCCESS);
        low = icd_map_low(e, offset, size);
        if (!low && e->hal_mapped) {
            /* 低位没弄成, 只能把高位指针交出去 (大概率被 wow64 拒掉, 但比崩强) */
            pthread_mutex_unlock(&g_alias_lock);
            if (ppData) *ppData = hi;
            g_map_hi++; g_alias_fail++;
            return rr;
        }
    }
    pthread_mutex_unlock(&g_alias_lock);

    if (low) {
        if (ppData) *ppData = low;
        g_map_ok++; g_alias_ok++;
        if (g_alias_ok <= 16 || (g_alias_ok % 256) == 0)
            fprintf(stderr, "[VK_ICD] vkMapMemory off=%llu size=%llu -> 低位 dmabuf %p "
                            "| 累计 低位=%u 高位=%u 失败=%u (接管分配=%u 直通=%u)\n",
                    (unsigned long long)offset, (unsigned long long)size, low,
                    g_alias_ok, g_map_hi, g_map_fail, g_alloc_taken, g_alloc_plain);
        fflush(stderr);
        return VK_SUCCESS;
    }

    /* 没被接管的内存 (非 host-visible / 回退分配): 原样透传 */
    VkResult r = g_real_map_memory(device, memory, offset, size, flags, ppData);
    void* p = (r == VK_SUCCESS && ppData) ? *ppData : NULL;
    int hi = p && ((uintptr_t)p >> 32) != 0;
    if (r != VK_SUCCESS) g_map_fail++; else if (hi) g_map_hi++; else g_map_ok++;
    if ((g_map_hi + g_map_fail) <= 16 || ((g_map_hi + g_map_fail) % 256) == 0)
        fprintf(stderr, "[VK_ICD] vkMapMemory(直通) size=%llu -> r=%d ptr=%p%s | 低位=%u 高位=%u 失败=%u\n",
                (unsigned long long)size, (int)r, p,
                hi ? "  **>4GB, wow64 无法表示**" : "", g_map_ok, g_map_hi, g_map_fail);
    fflush(stderr);
    return r;
}

static PFN_vkUnmapMemory g_real_unmap_memory = NULL;
static void VKAPI_CALL shim_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    pthread_mutex_lock(&g_alias_lock);
    IcdMemEntry* e = icd_find_entry(memory);
    int hal_mapped = 1;
    if (e) {
        if (e->lo) { munmap(e->lo, e->len); e->lo = NULL; e->len = 0; }
        hal_mapped = e->hal_mapped;
        e->hal_mapped = 0;
    }
    pthread_mutex_unlock(&g_alias_lock);
    if (hal_mapped) g_real_unmap_memory(device, memory);
}

/* 释放时要一并收掉靶子 buffer、dmabuf fd 和低位映射, 否则几百帧就把 fd 用光 */
static void VKAPI_CALL shim_vkFreeMemory(VkDevice device, VkDeviceMemory memory,
                                         const VkAllocationCallbacks* pAllocator) {
    VkBuffer target = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_alias_lock);
    for (unsigned i = 0; i < g_alias_n; i++) {
        if (g_alias[i].mem != memory) continue;
        if (g_alias[i].lo) munmap(g_alias[i].lo, g_alias[i].len);
        if (g_alias[i].fd >= 0) close(g_alias[i].fd);
        target = g_alias[i].buf;
        g_alias[i] = g_alias[--g_alias_n];
        break;
    }
    pthread_mutex_unlock(&g_alias_lock);
    g_real_free_mem(device, memory, pAllocator);
    if (target && g_real_destroy_buf) g_real_destroy_buf(device, target, NULL);
}

/* 低位 dmabuf 方案要拦 alloc/map/unmap/free 四个点, 任一入口被问到时
 * 就把这一整套真实函数指针全部解析好, 避免出现"拦了 alloc 但 map 时
 * 还没有 getFd"这种半成品状态。 */
static PFN_vkCreateImage g_real_create_image = NULL;
static PFN_vkCreateImageView g_real_create_image_view = NULL;
static PFN_vkCreateRenderPass g_real_create_renderpass = NULL;
static PFN_vkCreateRenderPass2 g_real_create_renderpass2 = NULL;
static PFN_vkCreateRenderPass2KHR g_real_create_renderpass2khr = NULL;
static void icd_init_devfns(VkDevice device) {
    static int done = 0;
    if (done) return;
    done = 1;
    icd_init_alias();
#define RESOLVE(var, type, name) \
    if (!(var)) (var) = (type)g_real_get_dev_proc(device, name)
    RESOLVE(g_real_map_memory,   PFN_vkMapMemory,   "vkMapMemory");
    RESOLVE(g_real_unmap_memory, PFN_vkUnmapMemory, "vkUnmapMemory");
    RESOLVE(g_real_alloc_mem,    PFN_vkAllocateMemory, "vkAllocateMemory");
    RESOLVE(g_real_free_mem,     PFN_vkFreeMemory,  "vkFreeMemory");
    RESOLVE(g_real_create_buf,   PFN_vkCreateBuffer, "vkCreateBuffer");
    RESOLVE(g_real_destroy_buf,  PFN_vkDestroyBuffer, "vkDestroyBuffer");
    RESOLVE(g_real_create_image, PFN_vkCreateImage, "vkCreateImage");
    RESOLVE(g_real_create_image_view, PFN_vkCreateImageView, "vkCreateImageView");
    RESOLVE(g_real_create_renderpass, PFN_vkCreateRenderPass, "vkCreateRenderPass");
    RESOLVE(g_real_create_renderpass2, PFN_vkCreateRenderPass2, "vkCreateRenderPass2");
    RESOLVE(g_real_create_renderpass2khr, PFN_vkCreateRenderPass2KHR, "vkCreateRenderPass2KHR");
    RESOLVE(g_real_buf_reqs,     PFN_vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    RESOLVE(g_real_get_mem_fd,   PFN_vkGetMemoryFdKHR, "vkGetMemoryFdKHR");
#undef RESOLVE
    fprintf(stderr, "[VK_ICD] 设备函数解析: map=%p unmap=%p alloc=%p free=%p "
                    "createBuf=%p destroyBuf=%p bufReqs=%p getFd=%p\n",
            (void*)g_real_map_memory, (void*)g_real_unmap_memory, (void*)g_real_alloc_mem,
            (void*)g_real_free_mem, (void*)g_real_create_buf, (void*)g_real_destroy_buf,
            (void*)g_real_buf_reqs, (void*)g_real_get_mem_fd);
    if (!g_real_get_mem_fd || !g_real_create_buf || !g_real_buf_reqs || !g_real_alloc_mem) {
        g_alias_enabled = 0;
        fprintf(stderr, "[VK_ICD] 关键函数缺失, 低位 dmabuf 方案关闭, 走原路径\n");
    }
    fflush(stderr);
}

/* ---- D32_SFLOAT_S8_UINT -> D24_UNORM_S8_UINT 透明替换 ----
 * Adreno 540 的 HAL 在 vkGetPhysicalDeviceImageFormatProperties 里对
 * D32_SFLOAT_S8_UINT(130) 一律返回 VK_ERROR_FORMAT_NOT_SUPPORTED, 而
 * D24_UNORM_S8_UINT(129) 是真正受支持的深度格式。但 DXVK 静态把
 * DXGI D32_FLOAT_S8X24 映射到 VK D32S8 且不会自动回退, 因此我们只得在
 * 这里把 D32S8 透明替换成 D24S8: 创建图像、视图、RenderPass 三处必须一致,
 * 否则 Vulkan 会因 image/view/renderpass 格式不匹配而报错。
 * 注意: 只替换这一种格式, 其它深度格式(D32F/D24X8/D16)HAL 本来支持。
 */
static const VkFormat DEPTH_D32S8 = VK_FORMAT_D32_SFLOAT_S8_UINT;
static const VkFormat DEPTH_D24S8 = VK_FORMAT_D24_UNORM_S8_UINT;

/* ---- vkCreateImage: D32S8 -> D24S8 ---- */
static VkResult VKAPI_CALL shim_vkCreateImage(VkDevice device,
        const VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
        VkImage* pImage) {
    VkImageCreateInfo info = *pCreateInfo;
    if (!g_raw_test && info.format == DEPTH_D32S8) {
        info.format = DEPTH_D24S8;
        SHIM_DBG("[VK_ICD] D32S8->D24S8 image sub: type=%d tiling=%d usage=0x%x samples=%d\n",
                (int)info.imageType, (int)info.tiling, (unsigned)info.usage, (int)info.samples);
        fflush(stderr);
    }
    VkResult r = g_real_create_image(device, &info, pAllocator, pImage);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[VK_ICD] vkCreateImage FAILED fmt=%d type=%d tiling=%d usage=0x%x samples=%d r=%d\n",
                (int)info.format, (int)info.imageType, (int)info.tiling,
                (unsigned)info.usage, (int)info.samples, (int)r);
        fflush(stderr);
    }
    return r;
}

/* ---- vkCreateImageView: D32S8 -> D24S8 ---- */
static VkResult VKAPI_CALL shim_vkCreateImageView(VkDevice device,
        const VkImageViewCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
        VkImageView* pView) {
    VkImageViewCreateInfo info = *pCreateInfo;
    if (!g_raw_test && info.format == DEPTH_D32S8) {
        info.format = DEPTH_D24S8;
        SHIM_DBG("[VK_ICD] D32S8->D24S8 view sub\n");
        fflush(stderr);
    }
    if (!g_real_create_image_view) return VK_ERROR_INITIALIZATION_FAILED;
    return g_real_create_image_view(device, &info, pAllocator, pView);
}

/* ---- VkAttachmentDescription 中 D32S8 -> D24S8 (处理数组副本) ---- */
static void rp_fix_attachments(int count, void* pAtts, size_t stride, size_t fmt_off) {
    for (int i = 0; i < count; i++) {
        char* a = (char*)pAtts + (size_t)i * stride;
        VkFormat* f = (VkFormat*)(a + fmt_off);
        if (*f == DEPTH_D32S8) *f = DEPTH_D24S8;
    }
}

/* ---- vkCreateRenderPass: D32S8 -> D24S8 ---- */
static VkResult VKAPI_CALL shim_vkCreateRenderPass(VkDevice device,
        const VkRenderPassCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
        VkRenderPass* pRP) {
    if (!g_real_create_renderpass) return VK_ERROR_INITIALIZATION_FAILED;
    VkAttachmentDescription* atts = NULL;
    VkRenderPassCreateInfo info = *pCreateInfo;
    if (!g_raw_test && pCreateInfo->pAttachments && pCreateInfo->attachmentCount > 0) {
        atts = malloc(sizeof(VkAttachmentDescription) * pCreateInfo->attachmentCount);
        if (!atts) return VK_ERROR_OUT_OF_HOST_MEMORY;
        memcpy(atts, pCreateInfo->pAttachments,
               sizeof(VkAttachmentDescription) * pCreateInfo->attachmentCount);
        rp_fix_attachments(pCreateInfo->attachmentCount, atts,
                           sizeof(VkAttachmentDescription),
                           offsetof(VkAttachmentDescription, format));
        info.pAttachments = atts;
    }
    VkResult r = g_real_create_renderpass(device, &info, pAllocator, pRP);
    free(atts);
    return r;
}

/* ---- vkCreateRenderPass2 / 2KHR: D32S8 -> D24S8 ----
 * VkAttachmentDescription2 与 VkAttachmentDescription 的 format 字段偏移不同,
 * 用各自的 offsetof 处理。结构体整体浅拷贝即可保留 pNext。
 */
static VkResult VKAPI_CALL shim_vkCreateRenderPass2(VkDevice device,
        const VkRenderPassCreateInfo2* pCreateInfo, const VkAllocationCallbacks* pAllocator,
        VkRenderPass* pRP) {
    PFN_vkCreateRenderPass2 f = g_real_create_renderpass2 ? g_real_create_renderpass2
                                                          : (PFN_vkCreateRenderPass2)g_real_create_renderpass2khr;
    if (!f) return VK_ERROR_INITIALIZATION_FAILED;
    VkAttachmentDescription2* atts = NULL;
    VkRenderPassCreateInfo2 info = *pCreateInfo;
    if (!g_raw_test && pCreateInfo->pAttachments && pCreateInfo->attachmentCount > 0) {
        atts = malloc(sizeof(VkAttachmentDescription2) * pCreateInfo->attachmentCount);
        if (!atts) return VK_ERROR_OUT_OF_HOST_MEMORY;
        memcpy(atts, pCreateInfo->pAttachments,
               sizeof(VkAttachmentDescription2) * pCreateInfo->attachmentCount);
        rp_fix_attachments(pCreateInfo->attachmentCount, atts,
                           sizeof(VkAttachmentDescription2),
                           offsetof(VkAttachmentDescription2, format));
        info.pAttachments = atts;
    }
    VkResult r = f(device, &info, pAllocator, pRP);
    free(atts);
    return r;
}

static PFN_vkVoidFunction VKAPI_CALL shim_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!g_real_get_dev_proc) return NULL;
    if (!strcmp(pName, "vkMapMemory")) {
        icd_init_devfns(device);
        if (g_real_map_memory) return (PFN_vkVoidFunction)shim_vkMapMemory;
    }
    if (!strcmp(pName, "vkUnmapMemory")) {
        icd_init_devfns(device);
        if (g_real_unmap_memory) return (PFN_vkVoidFunction)shim_vkUnmapMemory;
    }
    if (!strcmp(pName, "vkAllocateMemory")) {
        icd_init_devfns(device);
        if (g_real_alloc_mem) return (PFN_vkVoidFunction)shim_vkAllocateMemory;
    }
    if (!strcmp(pName, "vkFreeMemory")) {
        icd_init_devfns(device);
        if (g_real_free_mem) return (PFN_vkVoidFunction)shim_vkFreeMemory;
    }
    if (!strcmp(pName, "vkCreateImage")) {
        icd_init_devfns(device);
        if (g_real_create_image) return (PFN_vkVoidFunction)shim_vkCreateImage;
    }
    if (!strcmp(pName, "vkCreateImageView")) {
        icd_init_devfns(device);
        if (g_real_create_image_view) return (PFN_vkVoidFunction)shim_vkCreateImageView;
    }
    if (!strcmp(pName, "vkCreateRenderPass")) {
        icd_init_devfns(device);
        if (g_real_create_renderpass) return (PFN_vkVoidFunction)shim_vkCreateRenderPass;
    }
    if (!strcmp(pName, "vkCreateRenderPass2") || !strcmp(pName, "vkCreateRenderPass2KHR")) {
        icd_init_devfns(device);
        if (g_real_create_renderpass2 || g_real_create_renderpass2khr)
            return (PFN_vkVoidFunction)shim_vkCreateRenderPass2;
    }
    return g_real_get_dev_proc(device, pName);
}

/* ---- namespace 链接 (同 vulkan_gpu.so) ---- */
struct android_namespace_t;
typedef struct android_namespace_t android_namespace_t;
typedef bool (*link_ns_func_t)(android_namespace_t*, android_namespace_t*, const char*);
typedef android_namespace_t* (*get_ns_func_t)(const char*);

static void link_namespaces(void) {
    get_ns_func_t get_ns = (get_ns_func_t)dlsym(RTLD_DEFAULT,
        "__loader_android_get_exported_namespace");
    link_ns_func_t link_ns = (link_ns_func_t)dlsym(RTLD_DEFAULT,
        "__loader_android_link_namespaces");
    if (get_ns && link_ns) {
        android_namespace_t* def = get_ns("default");
        android_namespace_t* sph = get_ns("sphal");
        if (def && sph) {
            link_ns(def, sph,
                "vulkan.msm8998.so:libEGL_adreno.so:libGLESv2_adreno.so:libGLESv1_CM_adreno.so:"
                "libq3dtools_adreno.so:libadreno_utils.so:libgsl.so:libllvm-glnext.so:"
                "libcutils.so:libutils.so:libhardware.so:libnativewindow.so:"
                "libvulkan.so:libvkjson.so:libsync.so");
        }
    }
}

__attribute__((constructor))
static void icd_constructor(void) {
    install_crash_handler();
    g_raw_test = getenv("VK_TEST_RAW") ? 1 : 0;
    { const char* _v = getenv("VK_ICD_VERBOSE"); if (_v) g_shim_verbose = atoi(_v); }
    link_namespaces();
    g_drv = dlopen(ADRENO_VK_DRIVER, RTLD_LAZY | RTLD_GLOBAL);
    if (!g_drv) {
        fprintf(stderr, "[VK_ICD] 警告: 无法 dlopen %s: %s\n", ADRENO_VK_DRIVER, dlerror());
        return;
    }
    g_get_proc = (PFN_adreno_get_proc)dlsym(g_drv, ADRENO_GETPROC_MANGLED);
    if (!g_get_proc) {
        fprintf(stderr, "[VK_ICD] 警告: 找不到 %s\n", ADRENO_GETPROC_MANGLED);
        return;
    }
    fprintf(stderr, "[VK_ICD] Adreno Vulkan 驱动已加载: %p\n", (void*)g_drv);

    /* 用 NULL 实例提前解析 PD 级函数 (Android HAL 大多宽容, 全局表也含 PD 级)。
     * 若成功, 运行时不再调用 HAL vkGetInstanceProcAddr, 消除多实例竞态。 */
    g_real_iffp = (PFN_vkGetPhysicalDeviceImageFormatProperties)
        g_get_proc(NULL, "vkGetPhysicalDeviceImageFormatProperties");
    g_real_fmtp = (PFN_vkGetPhysicalDeviceFormatProperties)
        g_get_proc(NULL, "vkGetPhysicalDeviceFormatProperties");
    fprintf(stderr, "[VK_ICD] 构造器 NULL 实例解析: iffp=%p fmtp=%p\n",
            (void*)g_real_iffp, (void*)g_real_fmtp);
    fflush(stderr);
}

/* ---- 标准 ICD 入口 ---- */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!g_get_proc) return NULL;
    if (!strcmp(pName, "vkCreateInstance")) return (PFN_vkVoidFunction)shim_vkCreateInstance;
    if (!strcmp(pName, "vkCreateDevice"))   return (PFN_vkVoidFunction)shim_vkCreateDevice;
    if (!strcmp(pName, "vkEnumeratePhysicalDevices")) return (PFN_vkVoidFunction)shim_vkEnumeratePhysicalDevices;
    if (!strcmp(pName, "vkGetDeviceProcAddr")) {
        if (!g_real_get_dev_proc)
            g_real_get_dev_proc = (PFN_vkGetDeviceProcAddr)g_get_proc(instance, "vkGetDeviceProcAddr");
        if (g_real_get_dev_proc) return (PFN_vkVoidFunction)shim_vkGetDeviceProcAddr;
    }
    if (!strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties")) return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceImageFormatProperties;
    if (!strcmp(pName, "vkGetPhysicalDeviceFormatProperties")) return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceFormatProperties;
    if (!strcmp(pName, "vkGetPhysicalDeviceFormatProperties2")) return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceFormatProperties2;
    return g_get_proc(instance, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkPhysicalDevice physicalDevice, const char* pName) {
    if (!g_get_proc) return NULL;
    if (!strcmp(pName, "vkCreateDevice")) return (PFN_vkVoidFunction)shim_vkCreateDevice;
    if (!strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties")) return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceImageFormatProperties;
    if (!strcmp(pName, "vkGetPhysicalDeviceFormatProperties")) return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceFormatProperties;
    if (!strcmp(pName, "vkGetPhysicalDeviceFormatProperties2")) return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceFormatProperties2;
    /* 驱动 dispatcher 按 pName 选择函数; physical device 函数经 vkGetInstanceProcAddr
     * 取得后由调用方传入正确的 PD 句柄即可。 */
    return g_get_proc((VkInstance)physicalDevice, pName);
}
