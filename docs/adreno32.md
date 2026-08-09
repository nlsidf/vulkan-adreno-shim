# Adreno 540 + box64 + Wine WOW64 32 位 Vulkan 低位地址映射修复指南（细节版）

> 适用对象：需要在 Termux (Android) + box64 + Wine(WOW64) 上跑 32 位 Windows Vulkan 游戏、并遇到「白屏有声音」问题的开发者。
>
> 只动 Windows 用户态上面的 Vulkan ICD shim，**不修改 box64 源码**。
>
> 文档版本：2026-08-08
>
> 关键文件：
> - shim 源码：`~/proton11/.build/vulkan_adreno_icd.c`
> - shim 二进制：`~/proton11/.build/vulkan_adreno_icd.so`
> - ICD 清单：`~/proton11/.build/vulkan_adreno_icd.json`
> - 启动脚本：`~/proton11/claunch-9nine-adreno.sh`
> - 探测/测试程序：`t_extmem.c`、`t_extquery.c`、`t_extfd2.c`、`t_extfd4.c`、`t_shimlow.c`

---

## 1. 背景与现象

### 1.1 完整调用链

```
Windows 32位游戏 (.exe, ILP32)
  └─ d3d9 (DXVK-Sarek 32位, 在 Wine 内运行)
       └─ dxgi / D3D9 -> Vulkan API (vkAllocateMemory / vkMapMemory / ...)
            └─ Wine 内置 winevulkan (wow64 thunk: 指针在 32 位空间)
                 └─ libvulkan (Mesa loader) -> VK_ICD_FILENAMES 指向的 shim
                      └─ vulkan_adreno_icd.so (本 shim)
                           └─ dlopen(/vendor/lib64/hw/vulkan.msm8998.so) (Adreno 540 HAL)
```

### 1.2 故障现象

- 游戏启动正常，**有 BGM / 音效**，但**画面白屏**（或黑屏/冻结在第一帧）。
- DXVK 报错（来自 `dxvk/src/dxvk/dxvk_allocator.cpp`）：

```
err:   DxvkMemoryAllocator: Mapping memory failed with VK_ERROR_OUT_OF_HOST_MEMORY
```

- 游戏随后通常卡死或崩溃退出。

### 1.3 为什么是「白屏有声音」而不是「整个游戏起不来」

- `vkCreateDevice`、**音频**走的是另一个路径（PulseAudio，与 Vulkan 无关），所以声音正常。
- 渲染所需的上传 buffer / staging buffer / 常驻纹理 都需要 `vkMapMemory` 拿到 CPU 侧指针来写入，一旦 map 失败，DXVK 的 allocator 无法初始化 → 渲染管线卡死 → 表现为白屏。

---

## 2. 根因分析

### 2.1 32 位 guest 的指针约束

- 32 位 Windows 游戏运行在 ILP32 模型下，guest 内存地址必须在 `[0, 4GB)`。
- box64 + Wine 的 **WOW64** 层（`winevulkan`）在内核态与用户态之间做 32↔64 位 thunk。当 guest 调用 `vkMapMemory` 时，wine 要在一个 32 位字段里回填映射地址。
- 如果内核返回的指针 `>= 4GB`，wine 的 thunk 无法表示，于是**主动 `vkUnmapMemory` 并把结果改写为 `VK_ERROR_OUT_OF_HOST_MEMORY`**（`dlls/winevulkan/loader_thunks.c` 里对 `ppData` 的 32 位截断处理）。

### 2.2 Adreno 540 HAL 的实际行为

- Adreno 540 的 Vulkan HAL 是 **KGSL（Kernel Graphics Support Layer）+ SVM（Shared Virtual Memory）**。
- SVM 含义：**GPU 虚拟地址 == CPU 虚拟地址**。一块 `VkDeviceMemory` 一旦被 `vkMapMemory` 映射，映射地址由 GPU VA 决定，且**与 CPU 侧映射地址相同**。
- 实测 `vkMapMemory` 返回的指针形如 `0x7fxxxxxxxx`（约 34GB，远高于 4GB）。
- 因此：HAL 内部映射必须留在高位（否则 GPU VA 布局乱掉），但 guest 要求的指针必须低位 → **矛盾**。

### 2.3 为什么不能用全局开关 `BOX64_MMAP32` 解决

- `BOX64_MMAP32=1`：box64 把进程内**所有**未指定 `MAP_32BIT` 的匿名 mmap 强制塞进低 4GB（见 `box64/src/custommmap.c`）。
- 副作用：Adreno HAL 自身上万个 kgsl / 匿名映射也被压到低 4GB，导致 `vkCreateDevice` 直接返回 `-1`（`OUT_OF_HOST_MEMORY`），游戏连设备都建不出来。
- `BOX64_MMAP32=0`：HAL 映射回高位，`vkCreateDevice` 成功，但 `vkMapMemory` 回到 2.1 的矛盾。
- **结论**：`BOX64_MMAP32` 这个全进程开关无法同时满足「HAL 内部高位」和「map 返回低位」两个要求。最终 **`BOX64_MMAP32` 固定为 `0`**，低位问题另寻他法。

---

## 3. 失败探索全记录（已排除的路径）

下面每一步都是**实测**验证过不可行的，记录原因避免重复踩坑。

### 3.1 路径 A：`mremap(old_size=0, MREMAP_MAYMOVE|MREMAP_FIXED)` 建别名

- 思路：用零长 src 的 mremap 在原 VMA 上「复制」出一个低位别名。
- 验证：本机内核 `4.4.194` 对**匿名映射**和**文件共享映射**是支持该语义的（`t_mremap.c` 实测成功）。
- 失败点：KGSL 显存 VMA 由 `remap_pfn_range()` 建立，带 `VM_PFNMAP | VM_DONTEXPAND`。
- 内核侧 `vma_to_resize()` 对 `new_len > old_len` 分支有：

```c
if (vma->vm_flags & (VM_DONTEXPAND | VM_PFNMAP))
    goto Efault;
```

- 实测 errno = `EFAULT (14)`。

### 3.2 路径 B：同一 `(fd, offset)` 在低位再 `mmap` 一次

- 思路：KGSL 显存 fd 既然能在高位映射，能否在低 hint 处再映射一份？
- 失败点：KGSL 显存对象**只允许一个用户态映射**（`kgsl_mmap` 里 `if (useraddr != 0) return -EBUSY`）。
- 实测 errno = `EBUSY (16)`。

### 3.3 路径 C：先 `munmap` 高位，再在低位 `mmap` 重建

- 思路：既然只能有一个映射，那把唯一的那个「搬」到低位。
- 失败点：SVM 下 `GPU VA == CPU VA`，地址是锁死的。munmap 后原位再也映射不回去（恢复也 `MAP_FAILED`），且低位重建也失败。
- 实测 errno = `EINVAL (22)`，且不可逆。
- **结论**：KGSL/SVM 下「搬动显存映射地址」这条路整体封死。

### 3.4 路径 D：`VK_EXT_external_memory_host`

- 思路：wine 官方 WOW64 解决方案——让 wine 在**低 4GB 自己分配 host 内存**，再用 `VkImportMemoryHostPointerInfoEXT` 导入给 Vulkan。
- 验证：写 `t_extmem.c` 枚举 HAL 扩展并测试导入。
- 失败点：HAL 暴露 34 个扩展，**不包含** `VK_EXT_external_memory_host`。wine 这条路在本机走不通。

---

## 4. 突破口：external memory 探测

既然「搬动」不行，就改用「**导出**」——把 `VkDeviceMemory` 导出成 dmabuf fd（dma-buf 不受 KGSL SVM 约束，可以随便 `mmap` 到任意地址），再在低位 `mmap` 这个 fd，把低位指针交给 guest。

### 4.1 探测 1：HAL 支持哪些外部内存句柄（`t_extquery.c`）

- 调用 `vkGetPhysicalDeviceExternalBufferProperties`，逐一查询 handle type。
- 结果：

```
OPAQUE_FD        : DEDICATED_ONLY 可导出 可导入
DMA_BUF          : (无)
AHARDWAREBUFFER  : 可导出 可导入
HOST_ALLOCATION  : (无)
```

- 内存类型表（6 种）：

```
[0] DEVICE_LOCAL
[1] DEVICE_LOCAL HOST_VISIBLE HOST_CACHED
[2] DEVICE_LOCAL HOST_VISIBLE HOST_COHERENT HOST_CACHED
[3] DEVICE_LOCAL
[4] DEVICE_LOCAL HOST_VISIBLE HOST_COHERENT
[5] DEVICE_LOCAL (heap1, 256MB)
```

- **关键结论**：`OPAQUE_FD` 可导出但 `DEDICATED_ONLY`；`VK_KHR_external_memory_fd` 在支持列表里。

### 4.2 探测 2：basic 导出可行性（`t_extfd2.c`）

- 造一个带 `VkExternalMemoryBufferCreateInfo` 的 buffer → 用 `VkExportMemoryAllocateInfo + VkMemoryDedicatedAllocateInfo` 分配 → `vkGetMemoryFdKHR` → 低位 `mmap(fd)`。
- 结果：**成功**。导出 fd 是 `anon_inode:dmabuf`，低位 mmap 后双向读写与 HAL 高位指针一致（零拷贝成立）。

### 4.3 探测 3：边界条件矩阵（`t_extfd4.c`）

为了确认「能否用假 buffer 让 DXVK 的任意大块分配都可导出」，写了 fork 隔离的矩阵测试（每个组合单独子进程，HAL 崩了不影响后续）：

| 测试 | 条件 | 结果 |
|---|---|---|
| `[1] external buffer + dedicated` | type 0..4 | ✅ 全通，零拷贝成立 |
| `[1] type 5` | heap1 | ❌ alloc `-3` |
| `[2] 普通 buffer + dedicated` | buffer 未声明 external | 💥 **HAL 段错误** |
| `[3] external buffer, 无 dedicated` | 仅 export | ❌ getFd `-1` |
| `[4] dedicated 内存上再绑别的 buffer` | offset 0 / 64K | ✅ 成功 |
| `[5] 16MB / 64MB 大块` | type 3/4 | ✅ 全通 |
| `[6] host-visible 类型 (1/2) rebind` | DXVK 实际挑的类型 | ✅ 成功 |

- **决定性结论**：
  1. dummy buffer **必须带** `VkExternalMemoryBufferCreateInfo`，否则 HAL 段错误。
  2. 分配 **必须带** `VkMemoryDedicatedAllocateInfo`，否则 `vkGetMemoryFdKHR` 返回 `-1`。
  3. 满足 1+2 后，memoryType 0..4 都可导出，type 5 (heap1) 不行。
  4. **dedicated 内存上仍能 `vkBindBufferMemory` 别的 buffer（offset 0 / 64K 都行）** → DXVK 的 chunk suballocation 能直接跑在上面。

---

## 5. 最终方案设计

### 5.1 四层拦截

| 拦截点 | 做了什么 | 目的 |
|---|---|---|
| `vkCreateDevice` | 自动追加 4 个扩展（external_memory, external_memory_fd, dedicated_allocation, get_memory_requirements2） | 让应用「无感知」即可导出内存 |
| `vkAllocateMemory` | 对 HOST_VISIBLE 分配：造 dummy buffer + 注入 export+dedicated | 让该内存以后能导出 dmabuf |
| `vkMapMemory` | 导出 fd → 低位 mmap → 返回低位指针（同时仍对 HAL 真 map） | 给 guest 一个 <4GB 的等价视图 |
| `vkUnmapMemory` / `vkFreeMemory` | 清低位映射 / 关 fd / 销毁 dummy buffer | 防泄漏 |

### 5.2 核心数据结构（`vulkan_adreno_icd.c:751`）

```c
#define ICD_ALIAS_MAX 4096
typedef struct {
    VkDeviceMemory mem;     // 被接管的 VkDeviceMemory
    VkDevice       dev;      // 所属 device（用于 vkGetMemoryFdKHR）
    VkBuffer       buf;      // 为满足 DEDICATED_ONLY 造的靶子 buffer
    VkDeviceSize   size;     // 实际分配字节数（含对齐膨胀）
    int            fd;       // 导出的 dmabuf fd（-1 表示未导出）
    void*          lo;       // 低位 mmap 基址
    size_t         len;      // 低位映射长度
    int            hal_mapped;// 是否已对 HAL 调过 vkMapMemory
} IcdMemEntry;
static IcdMemEntry g_alias[ICD_ALIAS_MAX];
static unsigned g_alias_n = 0;
static pthread_mutex_t g_alias_lock = PTHREAD_MUTEX_INITIALIZER;
```

- 全局数组 + 互斥锁，最多 4096 块同时存活（足够：DXVK 通常几十~几百个 chunk）。

---

## 6. shim 实现详解（代码级）

### 6.1 `vkCreateDevice` 扩展注入（`vulkan_adreno_icd.c:555` 附近）

为容纳追加的扩展，把 `keep` 数组多开 8 个槽：

```c
const char** keep = calloc(n + 8, sizeof(char*));
```

在保留 WSI 扩展、剥离 win32 平台扩展后，追加本方案需要的 4 个：

```c
{
    static const char* need[] = {
        "VK_KHR_external_memory", "VK_KHR_external_memory_fd",
        "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2",
    };
    for (unsigned k = 0; k < sizeof(need)/sizeof(need[0]); k++) {
        int have = 0;
        for (uint32_t i = 0; i < keep_count; i++)
            if (!strcmp(keep[i], need[k])) { have = 1; break; }
        if (!have) { keep[keep_count++] = need[k];
                     fprintf(stderr, "[VK_ICD] 追加扩展: %s\n", need[k]); }
    }
}
icd_cache_mem_props(physicalDevice);   // 缓存内存类型表，供 alloc 时判断 HOST_VISIBLE
```

> 注意：DXVK 自己不会开这些扩展；shim 悄悄补上，对上层完全透明。

### 6.2 内存类型表缓存（`vulkan_adreno_icd.c` 约 400 行）

`vkAllocateMemory` 只有 `VkDevice` 拿不到 `VkPhysicalDevice`，所以创建设备时缓存：

```c
static VkPhysicalDeviceMemoryProperties g_mem_props;
static int g_mem_props_ok = 0;
static void icd_cache_mem_props(VkPhysicalDevice pd) {
    if (g_mem_props_ok) return;
    PFN_vkGetPhysicalDeviceMemoryProperties f =
        (PFN_vkGetPhysicalDeviceMemoryProperties)g_get_proc(inst_for_pd(pd),
                                            "vkGetPhysicalDeviceMemoryProperties");
    if (!f) return;
    f(pd, &g_mem_props);
    g_mem_props_ok = 1;
}
```

### 6.3 `vkAllocateMemory` 接管（`vulkan_adreno_icd.c:816`）

**步骤 1：判断是否适用**（`:820`）

```c
uint32_t ti = pAllocateInfo->memoryTypeIndex;
int host_visible = g_mem_props_ok && ti < g_mem_props.memoryTypeCount &&
    (g_mem_props.memoryTypes[ti].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
int eligible = g_alias_enabled && host_visible && g_real_create_buf && g_real_buf_reqs &&
               ti != 5 && pAllocateInfo->allocationSize <= (256u << 20) &&
               !icd_pnext_conflicts(pAllocateInfo->pNext) &&
               g_alias_n < ICD_ALIAS_MAX;
```

排除条件：
- 非 `HOST_VISIBLE`：image/depth 等只在 GPU 访问，不需低位映射。
- `ti == 5`（heap1）：实测带 export 分配必 `-3`。
- 超过 256MB：避免 dummy buffer 过大。
- 应用自己已带 dedicated/export/import（`icd_pnext_conflicts`）：避免冲突。

**步骤 2：造靶子 buffer**（`emaci`，`:838`）

```c
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
    return g_real_alloc_mem(device, pAllocateInfo, pAllocator, pMemory); // 回退
}
```

**步骤 3：构造 dedicated+export 的 pNext 链**（`emaci`，`:855`）

```c
VkMemoryRequirements mr;
g_real_buf_reqs(device, target, &mr);
if (mr.size < pAllocateInfo->allocationSize) mr.size = pAllocateInfo->allocationSize;

VkMemoryDedicatedAllocateInfo mdai = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
    .pNext = (void*)pAllocateInfo->pNext,   // 保留应用原有的 pNext（如 flags）
    .buffer = target };
VkExportMemoryAllocateInfo emai = {
    .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
    .pNext = &mdai,
    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
VkMemoryAllocateInfo mai = *pAllocateInfo;
mai.pNext = &emai;
mai.allocationSize = mr.size;   // dedicated 要求 == 资源的 memReq.size
```

> pNext 顺序：应用原 pNext 在**最里层**（mdai.pNext），应用不知道外部多包了两层。

**步骤 4：真实分配 + 记账**（`emaci`，`:871`）

```c
VkResult r = g_real_alloc_mem(device, &mai, pAllocator, pMemory);
if (r != VK_SUCCESS) {
    g_real_destroy_buf(device, target, NULL);
    return g_real_alloc_mem(device, pAllocateInfo, pAllocator, pMemory); // 回退原样
}
pthread_mutex_lock(&g_alias_lock);
if (g_alias_n < ICD_ALIAS_MAX) {
    g_alias[g_alias_n] = (IcdMemEntry){ .mem = *pMemory, .dev = device, .buf = target,
                                        .size = mr.size, .fd = -1, .lo = NULL };
    g_alias_n++;
}
pthread_mutex_unlock(&g_alias_lock);
return VK_SUCCESS;
```

> 任何一步失败都**静默回退**到原始 `vkAllocateMemory`，绝不影响正确性。

### 6.4 `icd_map_low`：导出 fd 并低位映射（`vulkan_adreno_icd.c:897`）

**导出 fd（每块只导一次）**：

```c
if (e->fd < 0) {
    VkMemoryGetFdInfoKHR gfi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = e->mem, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    int fd = -1;
    VkResult r = g_real_get_mem_fd(e->dev, &gfi, &fd);
    if (r != VK_SUCCESS || fd < 0) return NULL;
    e->fd = fd;   // 缓存，避免每次 map 都新建 fd
}
```

> 关键：`vkGetMemoryFdKHR` 每次调用都会**新建**一个 dmabuf fd，必须缓存，否则几百帧后 fd 用光。

**页对齐处理**（DXVK 常传非对齐 offset）：

```c
VkDeviceSize want = (size == VK_WHOLE_SIZE) ? (e->size - offset) : size;
VkDeviceSize base = offset & ~(VkDeviceSize)0xFFF;     // 下取整到页
size_t delta = (size_t)(offset - base);                // 零头
size_t len = (size_t)(want + delta);
len = (len + 0xFFF) & ~(size_t)0xFFF;                  // 上取整到页
if (base + len > e->size) len = (size_t)(e->size - base);
```

**找低位空洞 + mmap**：

```c
void* lo = icd_reserve_low(len);   // 从 0x20000000 起按 64MB 步进试 mmap(PROT_NONE)
void* p = mmap(lo, len, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_FIXED, e->fd, (off_t)base);
if (((uintptr_t)p >> 32) != 0) { munmap(p, len); return NULL; }  // 内核没听 hint
e->lo = p; e->len = len;
return (void*)((uintptr_t)p + delta);   // 把零头加回，返回逻辑地址
```

`icd_reserve_low`（`:734`）逐段探测：

```c
for (uintptr_t hint = 0x20000000UL; hint < 0xF0000000UL; hint += 0x4000000UL) {
    void* r = mmap((void*)hint, len, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (r == MAP_FAILED) continue;
    if (((uintptr_t)r >> 32) == 0 && (uintptr_t)r + len <= 0x100000000UL)
        return r;
    munmap(r, len);   // 内核把 hint 落到了高位，丢掉重试
}
return NULL;
```

### 6.5 `vkMapMemory` 接管（`vulkan_adreno_icd.c:946`）

```c
pthread_mutex_lock(&g_alias_lock);
IcdMemEntry* e = icd_find_entry(memory);
void* low = NULL;
if (e && g_alias_enabled) {
    // 仍然对 HAL 走一次真 map：SVM 下 CPU 映射同时确立 GPU VA，不能省
    void* hi = NULL;
    VkResult rr = g_real_map_memory(device, memory, offset, size, flags, &hi);
    e->hal_mapped = (rr == VK_SUCCESS);
    low = icd_map_low(e, offset, size);
    if (!low && e->hal_mapped) {
        // 低位没弄成，只能把高位指针交出去（大概率被 wow64 拒，但比崩强）
        pthread_mutex_unlock(&g_alias_lock);
        if (ppData) *ppData = hi;
        g_map_hi++; g_alias_fail++;
        return rr;
    }
}
pthread_mutex_unlock(&g_alias_lock);

if (low) {
    if (ppData) *ppData = low;   // 把低位指针交给 guest
    g_map_ok++; g_alias_ok++;
    return VK_SUCCESS;
}
// 没被接管的（非 host-visible / 回退分配）：原样透传
VkResult r = g_real_map_memory(device, memory, offset, size, flags, ppData);
```

- **关键设计**：依然调用 `g_real_map_memory`。原因——KGSL SVM 下 CPU 映射负责确立 GPU VA；若跳过，GPU 无法访问该内存。HAL 返回的高位指针被丢弃，但 `vkUnmapMemory` 时要配对还回去（见 6.6）。
- guest（wine WOW64）拿到的是低位 `low`，可以正常表示。

### 6.6 `vkUnmapMemory` / `vkFreeMemory`（`vulkan_adreno_icd.c:995`）

```c
static void VKAPI_CALL shim_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    pthread_mutex_lock(&g_alias_lock);
    IcdMemEntry* e = icd_find_entry(memory);
    int hal_mapped = 1;
    if (e) {
        if (e->lo) { munmap(e->lo, e->len); e->lo = NULL; e->len = 0; }  // 解低位映射
        hal_mapped = e->hal_mapped;
        e->hal_mapped = 0;
    }
    pthread_mutex_unlock(&g_alias_lock);
    if (hal_mapped) g_real_unmap_memory(device, memory);  // 配对 HAL 的真 unmap
}

static void VKAPI_CALL shim_vkFreeMemory(VkDevice device, VkDeviceMemory memory,
                                         const VkAllocationCallbacks* pAllocator) {
    VkBuffer target = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_alias_lock);
    for (unsigned i = 0; i < g_alias_n; i++) {
        if (g_alias[i].mem != memory) continue;
        if (g_alias[i].lo) munmap(g_alias[i].lo, g_alias[i].len);
        if (g_alias[i].fd >= 0) close(g_alias[i].fd);     // 关 dmabuf fd
        target = g_alias[i].buf;                          // 待销毁的靶子 buffer
        g_alias[i] = g_alias[--g_alias_n];                // 数组压缩
        break;
    }
    pthread_mutex_unlock(&g_alias_lock);
    g_real_free_mem(device, memory, pAllocator);
    if (target && g_real_destroy_buf) g_real_destroy_buf(device, target, NULL);  // 销毁靶子
}
```

- fd、低位映射、靶子 buffer 三件套在 free 时一并回收，避免泄漏。

### 6.7 函数指针惰性解析（`vulkan_adreno_icd.c:1029`）

在 `vkGetDeviceProcAddr` 首次命中任一拦截点时，一次性解析全部真实函数：

```c
static void icd_init_devfns(VkDevice device) {
    static int done = 0;
    if (done) return; done = 1;
    icd_init_alias();
    RESOLVE(g_real_map_memory,   PFN_vkMapMemory,   "vkMapMemory");
    RESOLVE(g_real_unmap_memory, PFN_vkUnmapMemory, "vkUnmapMemory");
    RESOLVE(g_real_alloc_mem,    PFN_vkAllocateMemory, "vkAllocateMemory");
    RESOLVE(g_real_free_mem,     PFN_vkFreeMemory,  "vkFreeMemory");
    RESOLVE(g_real_create_buf,   PFN_vkCreateBuffer, "vkCreateBuffer");
    RESOLVE(g_real_destroy_buf,  PFN_vkDestroyBuffer, "vkDestroyBuffer");
    RESOLVE(g_real_buf_reqs,     PFN_vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    RESOLVE(g_real_get_mem_fd,   PFN_vkGetMemoryFdKHR, "vkGetMemoryFdKHR");
    if (!g_real_get_mem_fd || !g_real_create_buf || !g_real_buf_reqs || !g_real_alloc_mem)
        g_alias_enabled = 0;   // 关键函数缺失则整体关闭，走原路径
}
```

`vkGetDeviceProcAddr` 里对 `vkMapMemory/vkUnmapMemory/vkAllocateMemory/vkFreeMemory` 先调 `icd_init_devfns` 再返回 shim 版本。

---

## 7. 编译与部署

### 7.1 重新编译 shim

```bash
cd ~/proton11/.build
gcc -shared -fPIC -O2 -Wall -o vulkan_adreno_icd.so vulkan_adreno_icd.c -ldl
```

（仅一个 `-Wunused-variable` 警告，来自历史遗留的 `g_fake_feats`，无害。）

### 7.2 启动脚本关键行（`claunch-9nine-adreno.sh`）

```bash
VK_ICD_FILENAMES=$PROJ/.build/vulkan_adreno_icd.json \
BOX64_MMAP32="${BOX64_MMAP32:-0}" \
... ./box64 .../wine "启动游戏.exe"
```

- `BOX64_MMAP32` 默认 `0`（已改）。
- `LD_PRELOAD` 含 `fake_machineid.so` + `vulkan_gpu.so`（namespace 链接，shim 自带也可）。

### 7.3 诊断开关

| 环境变量 | 行为 |
|---|---|
| `VK_ICD_MAP_LOW=0` | 关闭低位映射，仅做原路径透传（验证对比用） |
| `VK_LOADER_DEBUG=error,warn` | 看 loader 层报错 |
| `DXVK_LOG_LEVEL=info` | 看 DXVK 是否还报 Mapping memory failed |

---

## 8. 测试与验证

### 8.1 `t_extfd2.c`（basic 导出可行性）

输出节选：

```
vkCreateBuffer(external) -> 0 ✅
vkAllocateMemory(dedicated+exportable) -> 0 ✅
vkMapMemory -> 0, HAL 地址=0x7da1acf000 (高位)
vkGetMemoryFdKHR -> 0, fd=8 ✅
fd 指向: anon_inode:dmabuf
mmap(导出的 fd) -> 0x20000000 ✅低位
内容校验: 一致 ✅ 同一批物理页
低位写入后 HAL 地址读到: LOWWRITE ✅ 双向一致
```

### 8.2 `t_shimlow.c`（端到端，模拟 DXVK）

- 应用侧**一个扩展都不开**，shim 自动追加。
- 1MB/16MB/64MB × type 1/2/4 全部返回低位指针，读写一致，suballocation 成功。
- 200 轮 `alloc/map/unmap/free` 压力测试：无 fd 泄漏、无低位地址耗尽。

### 8.3 游戏实战（`claunch-9nine-adreno.sh`）

修复前日志：

```
DxvkMemoryAllocator: Mapping memory failed with VK_ERROR_OUT_OF_HOST_MEMORY
```

修复后日志：

```
[VK_ICD] 追加扩展: VK_KHR_external_memory
[VK_ICD] 追加扩展: VK_KHR_external_memory_fd
[VK_ICD] 追加扩展: VK_KHR_dedicated_allocation
[VK_ICD] 追加扩展: VK_KHR_get_memory_requirements2
[VK_ICD] 低位 dmabuf 映射已启用
[VK_ICD] alloc 已接管: type=2 请求=16384KB 实分=16389KB mem=0x...
[VK_ICD] vkMapMemory off=0 size=18446744073709551615 -> 低位 dmabuf 0x34000000
...
info:  D3D9DeviceEx::ResetSwapChain:
info:    Requested Presentation Parameters
info:      - Width: 640  Height: 480  Format: X8R8G8B8  Windowed: true
info:  Presenter: Actual swap chain properties:
info:    Format: VK_FORMAT_B8G8R8A8_UNORM
info:    Buffer size: 960x540  Image count: 4
```

- `Mapping memory failed` **彻底消失**。
- swapchain 成功建立 960x540。
- VNC 截屏：9-nine Episode 1 主菜单正常渲染（背景、飘落花瓣、Logo、菜单按钮）。

---

## 9. 限制与注意事项

1. **仅对 HOST_VISIBLE 内存启用**。DEVICE_LOCAL image/depth 不 map，不接管。
2. **type 5（heap1, 256MB）不接管**：带 export 分配会 `-3`。
3. **分配上限 256MB**：超了走原路径（dummy buffer 可能无法创建）。
4. **dummy buffer 对齐膨胀**：`memReq.size` 比请求略大（约 0.5%），guest 只用前 `allocationSize` 字节。
5. **HAL 内部映射仍保留**：shim 仍调用真实 `vkMapMemory`，GPU VA 正确建立。
6. **fd 缓存**：每块内存只导一次 fd，`vkFreeMemory` 时关闭。
7. **并发**：`g_alias_lock` 保护全局表；`icd_reserve_low` 的 mmap 循环持锁，DXVK 单线程内存管理无瓶颈。
8. **堆占用**：`ICD_ALIAS_MAX=4096`，每块 entry 约 56 字节，数组固定约 224KB。

---

## 10. 故障排查

| 症状 | 日志关键字 | 排查 |
|---|---|---|
| 仍然白屏 | `Mapping memory failed` | 确认 `BOX64_MMAP32=0`；确认 `VK_ICD_FILENAMES` 指向新 shim |
| shim 未启用 | 看不到 `低位 dmabuf 映射已启用` | 检查 shim 是否被正确加载（json 路径） |
| 接管为 0 | `接管分配=0` | 看是否所有分配都走 `alloc 原样转发`（可能 memoryType 不匹配） |
| fd 泄漏 | 运行久后 `mmap(dmabuf ...) errno=24 (EMFILE)` | 确认 `vkFreeMemory` 路径被 shim 接管（应用是否自己调了 import） |
| HAL 段错误 | `SIGSEGV` 在 alloc 内 | dummy buffer 漏了 `VkExternalMemoryBufferCreateInfo` |
| getFd=-1 | `vkGetMemoryFdKHR -> -1` | 分配漏了 `VkMemoryDedicatedAllocateInfo` |

---

## 11. 后续可优化点

1. **32 位镜像**：当前 shim 是 64 位，给 64 位 Wine 用；32 位游戏（如 9-nine 32 位 exe）走的是 wine wow64 的 64 位 host 进程，所以 64 位 shim 正确。若遇到纯 32 位 Wine 前缀，需另编 32 位 shim。
2. **更省 dummy buffer**：用 `VkBuffer` 的最小可用 usage 组合，减小对齐膨胀。
3. **低位地址池**：`icd_reserve_low` 目前每次新探；可维护一个空闲低位区间列表减少 mmap 系统调用。
4. **`VK_ICD_MAP_LOW` 细粒度**：可按 memoryType / 大小开关，便于灰度。

---

*文档版本：2026-08-08（细节版）*
