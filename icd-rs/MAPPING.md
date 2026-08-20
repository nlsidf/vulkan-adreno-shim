# 低位映射逻辑说明（memalias.rs）

## 1. 为什么需要低位映射

32 位 guest（Wine / box64 翻译执行的 x86 程序）只能理解 **< 4GB 的指针**。
而宿主是 aarch64 64 位进程，Adreno 驱动分配出的 `VkDeviceMemory` 经 `vkMapMemory`
返回的 CPU 映射地址常常落在 **> 4GB**，32 位 guest 拿到后无法表示 → 崩溃。

解决思路：拦截 `vkAllocateMemory` / `vkMapMemory` / `vkFreeMemory`，
对**可被 CPU 访问（host-visible）**的内存，构造一个 dmabuf（`vkGetMemoryFdKHR`），
由 shim 自己把它 `mmap` 到 **< 4GB 的低位地址**，把这个低位地址交给 32 位 guest。
guest 操作的始终是低位地址，shim 的低位映射与驱动的真实 dmabuf 后备存储是同一份。

> 注意：宿主 64 位进程占用的虚拟地址（包括本 shim 预占/占位的所有低位区间）
> 与 32 位 guest 看到的 4GB 虚拟空间是**两套完全不同的地址空间**，二者不相加，
> 不会因此超过 32 位 4GB 限制。

## 2. 整体结构

```
vkAllocateMemory  ──► shim_allocate_memory
                       · 注入 external + dedicated，造"靶子 buffer"承接 dmabuf
                       · 真实分配，并把 MemEntry 记入 ALIAS 表
vkMapMemory       ──► shim_map_memory
                       · 仍对 HAL 走一次真 map（配对 unmap 计数）
                       · 自己把 dmabuf fd mmap 到 <4GB 低位（见 §3）
vkUnmapMemory     ──► shim_unmap_memory   （按 hal_map_count 配对还 HAL map）
vkFreeMemory      ──► shim_free_memory    （见 §4 释放与还洞）
vkDestroyDevice   ──► cleanup_device      （设备销毁时批量清理，同 §4）
```

两套关键账本：

| 账本 | 类型 | 作用 |
|---|---|---|
| `ALIAS` (`RwLock<HashMap>`) | 每块被接管内存的生命周期：`MemEntry{ lo, len, fd, buf, dev, hal_map_count }` | 热路径只读；`lo` 原子，记录"这块内存映射到哪个低位地址" |
| `LOW_FREE` (`Mutex<BTreeMap<start,len>>`) | 低位地址空间的**空闲洞**记账 | 地址分配器；已分配的槽不在表内 |

两把锁**都不包 mmap/munmap 等 syscall**，只在选洞/还洞的纯内存操作时持锁。

## 3. 映射放置（setup_low_mapping）

每次首次 map 一块内存时：

1. `ensure_low_window()`：仅置 `LOW_WIN_OK = true`，**不做任何预占**。
   （历史教训：曾用 `MAP_FIXED` 整窗预占 `0x2000_0000..0xF000_0000`，但 `MAP_FIXED`
   会静默解除该范围内 box64 既有低地址 guest 映射 → 9nine / kamiyu 实测 SIGSEGV。
   故改为空闲表初始为空，首块由内核盲扫放置。）
2. `map_low_windowed(fd, size)`：
   - 若 `LOW_FREE` 为空 → `low_scan_reserve(len)`：用
     `mmap(NULL, PROT_NONE, MAP_ANONYMOUS)` 让**内核盲扫**挑一块空闲低地址
     （绝不 clobber box64），校验 `地址 + len <= 4GB` 后焊死进空闲表。
   - 之后统一 `low_alloc(len)` 从空闲表取 best-fit 洞。
   - 对取出的槽做 `mmap(MAP_FIXED | MAP_SHARED, fd)` 把 dmabuf 映射进去。
     `MAP_FIXED` 落点**永远是我们自己曾占位的地址**，不会覆盖他人 → 安全。
   - 失败则还回洞并降级 `map_low_dmabuf`（内核盲扫、无 MAP_FIXED）。

### best-fit 分配（low_alloc）
遍历 `BTreeMap`，挑 `size >= len` 且 **size 最小**的洞（最省碎片）；
恰好相等则整块划走，否则切出前 `len` 段、剩余留回表。

### 盲扫回退（map_low_dmabuf，降级路径）
`LOW_WIN_OK == false` 或空闲表/占位全失败时，按 hint 从 `LOW_START` 步进盲扫，
内核挑地址即接受（不强制 `MAP_FIXED`），落高位则重试下一个 hint。

## 4. 释放与还洞（shim_free_memory / cleanup_device）

释放一块低位映射时：

```
if 窗口启用:
    low_release(lo, len)          // 见下
else:
    munmap(lo, len)               // 降级盲扫路径, 仅解除
```

`low_release` 做两件事（一次 `MAP_FIXED` 原子完成）：
1. `low_reclaim(lo, len)`：用 `mmap(MAP_FIXED, PROT_NONE, MAP_ANONYMOUS)`
   **原地**把原 dmabuf 映射替换为 `PROT_NONE` 匿名占位。
   - 同时完成"解除 dmabuf 映射 + 重新占位"两个动作（无需先 munmap）。
   - 该区间从此被我们占用，外部 `mmap(NULL)` 抢不走，杜绝后续 `MAP_FIXED`
     分配到此槽时覆盖别库。
2. `low_free(lo, len)`：把这段还回 `LOW_FREE`，并**连锁合并**相邻洞。

> 若 `low_reclaim` 失败（极低概率）：`low_release` 会**显式 `munmap` 原映射**
> 再跳过还洞，避免 dmabuf 映射残留造成泄漏与地址永久占用。

### 连锁合并（low_free）
释放一段后，循环向前/向后合并所有相邻洞，直到前后都无相邻条目。
例如释放连续三个相邻空洞中的中间一段，会合并成**一条**空闲条目，
最大化后续 best-fit 可用地址，避免碎片残留（碎片只影响利用率，不造成功能错误）。

## 5. 缓存一致性（Flush / Invalidate）

32 位 guest 拿到的是 shim **自己 mmap 的低位映射**（同一 dmabuf 的 CPU 视角），
与驱动内部映射是两份独立 CPU 映射。应用调用 `vkFlush/InvalidateMappedMemoryRanges`
发往真实驱动，只作用于驱动侧映射，不会刷新我们这份低位 mmap 的缓存。

修复（`shim_flush/invalidate_mapped_memory_ranges`）：对每段 range 定位 entry 的
低位 `lo`，对其 `[lo+offset, lo+offset+size]`（页对齐）做 `msync`：
- Flush → `MS_SYNC`：脏页写回 dmabuf 后备存储
- Invalidate → `MS_SYNC | MS_INVALIDATE`：丢弃陈旧缓存行

同时把原调用转发给真实驱动，不影响驱动自身映射。

## 6. 资源释放顺序（Vulkan 规范）

`shim_free_memory` 必须按规范：先 `vkDestroyBuffer`（靶子 buffer，绑定到该内存）
再 `vkFreeMemory`（内存对象）。`munmap` / `close` / 重新占位顺序无关，保持不动。

## 7. 设计权衡与已知限制

- **占用虚拟地址、不占物理内存**：`PROT_NONE` 占位只占虚拟地址、不提交物理页。
  shim 是 64 位进程，低位区间占用无感；且仅占 we 自己管理过的区间。
- **非整窗预占**：为与 box64 共存，放弃"整窗独占"，改用"按需焊死自有槽"。
  首块之前（空闲表空时）的分配走内核盲扫，与改造前一致；只有释放过的槽才复用。
- **并发**：选洞/还洞由 `Mutex` 串行化，持锁仅亚微秒纯内存操作；`ALIAS.lo` 用原子
  CAS，并发首映射同一内存时输家复用赢家结果。
- **不引外部依赖**：`BTreeMap` 来自 std（底层即平衡树），满足项目零依赖红线。
- **降级完备**：预占/占位/映射任一失败都回退到"内核盲扫 + 高位指针"，不丢功能；
  未启用 ALIAS（`raw_test` / `VK_ICD_MAP_LOW=0`）时完全透传。
