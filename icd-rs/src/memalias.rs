//! 低位 dmabuf 映射接管 (核心热路径)。
//!
//! 背景 (详见 C 原版注释与 docs/adreno32.md): HAL 的 vkMapMemory 返回高位指针,
//! 32 位 guest (wine wow64) 表示不了 → unmap + 合成 OOM → 白屏。方案是:
//! vkAllocateMemory 时对 HOST_VISIBLE 内存注入 export+dedicated, vkMapMemory 时
//! 导出 dmabuf 自行 mmap 到 <4GB, 把低位指针交给 guest。
//!
//! 零妥协设计:
//! - `ALIAS` 用 RwLock<HashMap>: 读锁共享, 多线程 map/unmap 不同内存互不阻塞。
//! - `MemEntry` 可变字段全部原子化 (lo/len/fd/hal_map_count), 读锁下即可
//!   CAS/增减, 唯一需要写锁的是 alloc 插入与 free 摘除。
//! - map 成功路径 = 1 把读锁 + 1 次 HAL map + 原子操作, 接近零开销。
//! - 低位映射按内存对象建一次并保持到 free (不再每次 map 重建)。
//! - HAL map 用计数器配对, 释放/销毁时归零才真正 unmap。
//! - 不用 MAP_FIXED (内核 4.4 无 MAP_FIXED_NOREPLACE), 避免覆盖并发映射。
//! - alloc 表容量先原子预留槽位, 消除"表满竞态下 destroy 靶子 buffer 但内存
//!   仍 dedicated 到它"的 UB。
//! - 所有锁获取不 panic; 所有 `from_raw_parts`/pNext 遍历有上限。

#![allow(dead_code)]

use crate::cffi;
use crate::ffi::*;
use crate::dev_fns_global;
use core::ffi::c_int;
use core::ptr::null_mut;
use std::collections::{BTreeMap, HashMap};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicPtr, AtomicU32, AtomicUsize, Ordering};
use std::sync::{LazyLock, Mutex, RwLock};

const MAX_ALIAS: usize = 4096;
const PAGE: usize = 4096;
const LOW_START: usize = 0x2000_0000; /* 避开 wine/box64 常用低端区域 */
const LOW_END: usize = 0xF000_0000;
const LOW_STEP: usize = 0x0400_0000; /* 64MB 步进 */
const MAX_ALLOC_SIZE: u64 = 256 << 20;
const MEM_TYPE_HEAP1: u32 = 5; /* 类型 5 在 heap1, 带 export 分配必失败 */
const MAX_PNEXT: u32 = 32; /* 病态 pNext 链防死循环 */

pub static ALIAS_ENABLED: AtomicBool = AtomicBool::new(true);
static ALIAS_COUNT: AtomicU32 = AtomicU32::new(0);
/// alloc 诊断计数: 只打印前 16 次接管日志 (与 C 版 g_alloc_diag 一致)。
static ALLOC_DIAG: AtomicU32 = AtomicU32::new(0);
static ALIAS: LazyLock<RwLock<HashMap<VkDeviceMemory, MemEntry>>> =
    LazyLock::new(|| RwLock::new(HashMap::new()));

/* 永不 panic 的锁获取 (panic=abort 下 poisoning 不会存活, 双保险) */
fn read_alias() -> std::sync::RwLockReadGuard<'static, HashMap<VkDeviceMemory, MemEntry>> {
    ALIAS.read().unwrap_or_else(|e| e.into_inner())
}
fn write_alias() -> std::sync::RwLockWriteGuard<'static, HashMap<VkDeviceMemory, MemEntry>> {
    ALIAS.write().unwrap_or_else(|e| e.into_inner())
}

/// 每块被接管的 VkDeviceMemory 的记账。可变字段全部原子化。
struct MemEntry {
    mem: VkDeviceMemory,
    dev: VkDevice,
    /// 为满足 DEDICATED_ONLY 而造的靶子 buffer, 随内存一起销毁。
    buf: VkBuffer,
    /// 实际分配字节数 (含靶子 buffer 对齐膨胀)。
    size: u64,
    /// vkGetMemoryFdKHR 导出的 dmabuf fd (-1 = 未导出)。
    fd: AtomicI32,
    /// 我们自己映射到 <4GB 的地址 (按整个分配映射一次, 保持到 free)。
    lo: AtomicPtr<c_void>,
    /// 低位映射长度 (与 lo 成对维护)。
    len: AtomicUsize,
    /// outstanding 的 HAL vkMapMemory 次数, 决定 unmap 时要不要配对还回去。
    hal_map_count: AtomicU32,
}

/* ---- 环境开关 ---- */

pub fn init_alias_env() {
    let mut enabled = true;
    /* 只有显式设置 VK_ICD_MAP_LOW=0 才关闭 (与 C 版一致, 未设置默认启用) */
    if crate::env_flag(c"VK_ICD_MAP_LOW") && crate::env_int(c"VK_ICD_MAP_LOW") == 0 {
        enabled = false;
        crate::shim_log!("VK_ICD_MAP_LOW=0, 关闭低位 dmabuf 映射 (仅诊断)");
    }
    if crate::raw_test() {
        enabled = false;
        crate::shim_log!("VK_TEST_RAW=1, 关闭低位 dmabuf 映射 (仅测试用)");
    }
    if enabled {
        crate::shim_log!(
            "低位 dmabuf 映射已启用: 分配时注入 export+dedicated, map 时导出 fd 自行映射到 <4GB"
        );
    }
    ALIAS_ENABLED.store(enabled, Ordering::Relaxed);
}

/* ---- pNext 冲突检查 ---- */

/// pNext 链里是否已经有会跟我们打架的结构 (应用自己搞了 dedicated/external)。
/// 链深度封顶, 防病态指针死循环。
fn pnext_conflicts(p_next: *const c_void) -> bool {
    if p_next.is_null() {
        return false;
    }
    let mut it = p_next as *const VkBaseInStructure;
    for _ in 0..MAX_PNEXT {
        let s = unsafe { &*it };
        if matches!(
            s.s_type,
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO
                | VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO
                | VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO_NV
                | VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR
        ) {
            return true;
        }
        if s.p_next.is_null() {
            return false;
        }
        it = s.p_next;
    }
    true /* 超长链视为冲突, 走直通 */
}

/* ---- 低位映射工具 ---- */

fn align_up(v: usize, align: usize) -> usize {
    (v + align - 1) & !(align - 1)
}

/* ---- 低窗地址空间空闲表 (方案一: 用户态分配器, 复用释放的洞) ---- */

/// 低窗记账。key = 洞起始地址, value = 洞长度(字节, 页对齐)。
/// 已分配的槽不入表。不变量: 一段地址恰在 {空闲表} 或 {ALIAS 的 MemEntry.lo} 之一。
/// 用 BTreeMap (std, 零依赖, 底层平衡树): 按地址有序 + 合并相邻 API 自然。
static LOW_FREE: LazyLock<Mutex<BTreeMap<usize, usize>>> =
    LazyLock::new(|| Mutex::new(BTreeMap::new()));

/// 是否启用空闲表复用机制。false => 全程降级走旧盲扫。
/// 注意: 这里**不**在启动期用 MAP_FIXED 预占整段低窗 —— MAP_FIXED 会静默解除
/// 该范围内任何既有映射, 而 box64/Wine 的 32 位 guest 内存早就映射在低地址,
/// 整窗预占会直接 clobber 它们 -> SIGSEGV (9nine/kamiyu 实测崩溃)。故空闲表初始
/// 为空, 首次分配走内核盲扫挑空闲地址, 仅对我们自己释放的槽做复用 + 重新占位。
static LOW_WIN_OK: AtomicBool = AtomicBool::new(false);

/// 首次 map 时调用: 仅启用空闲表复用机制, 不做任何 MAP_FIXED 预占 (避免覆盖
/// box64 既有低地址映射)。空闲表起初为空, 首次分配由 map_low_windowed 降级到
/// 盲扫放置, 释放后才会有可复用槽。由 LazyLock 保证只执行一次。
fn ensure_low_window() {
    if LOW_WIN_OK.load(Ordering::Relaxed) {
        return;
    }
    LOW_WIN_OK.store(true, Ordering::Relaxed);
    crate::shim_log!(
        "低位映射空闲表复用已启用 (无整窗预占, 避免覆盖 box64 既有低地址映射)"
    );
}

/// 从空闲表挑 best-fit 洞 (size>=len 且 size 最小), 划走后返回起始地址。
/// 纯内存操作, 不持锁做 syscall。
fn low_alloc(len: usize) -> Option<usize> {
    let mut g = LOW_FREE.lock().unwrap_or_else(|e| e.into_inner());
    let mut best: Option<(usize, usize)> = None;
    for (&s, &sz) in g.iter() {
        if sz >= len && (best.is_none() || sz < best.unwrap().1) {
            best = Some((s, sz));
        }
    }
    let (s, sz) = best?;
    if sz == len {
        g.remove(&s);
    } else {
        g.remove(&s);
        g.insert(s + len, sz - len);
    }
    Some(s)
}

/// 把 [lo, lo+len) 还回空闲表并与相邻洞合并。纯内存操作。
fn low_free(lo: usize, len: usize) {
    let mut g = LOW_FREE.lock().unwrap_or_else(|e| e.into_inner());
    let mut start = lo;
    let mut total = len;
    /* 连锁合并: 单次前邻/后邻合并后, 合并出的新洞可能又与更远的洞相接,
     * 故用循环直到前后都无相邻洞。避免连续多个相邻空洞残留成多条条目
     * (影响地址利用率, 虽不造成功能错误)。 */
    loop {
        /* 合并前邻: 最大 start<start 且相接 */
        let front = g
            .range(..start)
            .next_back()
            .filter(|&(&ps, &psz)| ps + psz == start)
            .map(|(&ps, &psz)| (ps, psz));
        if let Some((ps, psz)) = front {
            start = ps;
            total += psz;
            g.remove(&ps);
        } else {
            break;
        }
    }
    loop {
        /* 合并后邻: 紧接 end 的洞 (end 可能因前邻合并改变) */
        let end = start + total;
        if let Some(&nsz) = g.get(&end) {
            total += nsz;
            g.remove(&end);
        } else {
            break;
        }
    }
    g.insert(start, total);
}

/// 用 MAP_FIXED 把原 dmabuf 映射原地替换为 PROT_NONE 匿名占位: 一次系统调用
/// 同时完成"解除 dmabuf 映射 + 重新占位"两个动作 (MAP_FIXED 覆盖既有映射是原子的,
/// 无需先 munmap)。这样该区间内核侧始终被我们占用, 外部 mmap(NULL) 抢不走;
/// 否则 LOW_FREE 仍记为可用, 后续 low_alloc 把它分给 MAP_FIXED 会静默覆盖别库 -> 崩溃。
/// 占位失败(极低概率)返回 false, 调用方应跳过 low_free, 避免空闲表记下错误的
/// "可用" 地址。只占虚拟地址不占物理页, 与窗口预占同机制。
fn low_reclaim(lo: usize, len: usize) -> bool {
    let p = unsafe {
        cffi::mmap(
            lo as *mut c_void,
            len,
            cffi::PROT_NONE,
            cffi::MAP_FIXED | cffi::MAP_ANONYMOUS | cffi::MAP_PRIVATE,
            -1,
            0,
        )
    };
    if p == cffi::MAP_FAILED {
        crate::shim_log!("低位洞重新占位失败: lo=0x{:x} len={} (跳过还洞)", lo, len);
        return false;
    }
    true
}

/// 释放一块低位映射: 用 MAP_FIXED 原子替换为 PROT_NONE 占位并还洞到空闲表。
/// 若重新占位失败(极低概率), 必须显式 munmap 解除原 dmabuf 映射, 否则映射残留
/// 造成内存泄漏且地址永久被占; 此时不写空闲表 (地址不确定可用)。供 shim_free_memory
/// 与 cleanup_device 共用, 保证两处行为一致。
fn low_release(lo: usize, len: usize) {
    let rlen = align_up(len, PAGE);
    if low_reclaim(lo, rlen) {
        low_free(lo, rlen);
    } else {
        crate::shim_log!("低位洞释放: 重新占位失败, 强制 munmap(0x{:x}, {})", lo, rlen);
        unsafe {
            cffi::munmap(lo as *mut c_void, rlen);
        }
    }
}

/// 空闲表是否为空 (纯内存操作)。
fn low_is_empty() -> bool {
    LOW_FREE
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .is_empty()
}

/// 空闲表为空时, 让内核盲扫挑一块空闲低地址 (PROT_NONE 匿名, 绝不 clobber box64
/// 既有映射), 立即焊死进空闲表, 使后续统一走 low_alloc 复用路径。返回起始地址。
/// 这样从第一块起所有槽都经自有占位, 释放时由 low_reclaim 重新占位, 全程对外是
/// 铁板一块, 且 MAP_FIXED 落点永远是我们自己占位的地址 -> 安全。
fn low_scan_reserve(len: usize) -> Option<usize> {
    let probe = unsafe {
        cffi::mmap(
            null_mut(),
            len,
            cffi::PROT_NONE,
            cffi::MAP_ANONYMOUS | cffi::MAP_PRIVATE,
            -1,
            0,
        )
    };
    if probe == cffi::MAP_FAILED {
        return None;
    }
    let a = probe as usize;
    /* 内核可能返回 >4GB 地址; 该地址无法给 32 位 guest 用, 且后续 MAP_FIXED
     * 映射 dmabuf 会失败。必须丢弃 (munmap) 并返回 None, 降级盲扫放置。 */
    if a + len > 0x1_0000_0000 {
        unsafe {
            cffi::munmap(probe, len);
        }
        return None;
    }
    low_free(a, len); // 入空闲表 (单块时即自身)
    Some(a)
}

/// 把 dmabuf fd 映射到 <4GB 的某段低窗槽, 复用空闲表里释放的洞以减少碎片。
/// 空闲表为空 (首轮分配) 时先盲扫保留一块进表, 之后统一走 low_alloc 复用路径;
/// 首块起所有槽都是我们自有占位, MAP_FIXED 绝不会 clobber 他人。LOW_WIN_OK=false
/// 或盲扫也失败则降级 map_low_dmabuf。
unsafe fn map_low_windowed(fd: i32, size: u64) -> *mut c_void { unsafe {
    if !LOW_WIN_OK.load(Ordering::Relaxed) {
        return map_low_dmabuf(fd, size);
    }
    let len = align_up(size as usize, PAGE);
    /* 空闲表空: 盲扫保留一块进表, 使首块也走复用路径 (不 clobber box64) */
    if low_is_empty() {
        low_scan_reserve(len);
    }
    let Some(slot) = low_alloc(len) else {
        return map_low_dmabuf(fd, size);
    };
    let p = cffi::mmap(
        slot as *mut c_void,
        len,
        cffi::PROT_READ | cffi::PROT_WRITE,
        cffi::MAP_FIXED | cffi::MAP_SHARED,
        fd,
        0,
    );
    if p == cffi::MAP_FAILED {
        /* 落点为我们自有占位, 失败属异常 -> 还回槽, 降级盲扫 */
        low_free(slot, len);
        return map_low_dmabuf(fd, size);
    }
    p
}}

/// 降级路径: 用 hint 把 dmabuf 映射到低位 (<4GB)。不用 MAP_FIXED: 高位就 munmap
/// 换下一个 hint 重试, 避免覆盖并发映射 (内核 4.4 无 MAP_FIXED_NOREPLACE)。
/// 仅在 LOW_WIN_OK=false 时由 map_low_windowed 调用。
unsafe fn map_low_dmabuf(fd: i32, size: u64) -> *mut c_void { unsafe {
    let len = align_up(size as usize, PAGE);
    let mut hint = LOW_START;
    while hint < LOW_END {
        let p = cffi::mmap(
            hint as *mut c_void,
            len,
            cffi::PROT_READ | cffi::PROT_WRITE,
            cffi::MAP_SHARED,
            fd,
            0,
        );
        if p == cffi::MAP_FAILED {
            hint += LOW_STEP;
            continue;
        }
        let a = p as usize;
        if (a >> 32) == 0 && a + len <= 0x1_0000_0000 {
            return p;
        }
        cffi::munmap(p, len); /* 内核没听 hint, 落到高位, 丢掉重试 */
        hint += LOW_STEP;
    }
    null_mut()
}}

/// 导出一次 dmabuf fd (每次 vkGetMemoryFdKHR 都会新建 fd, 必须缓存否则漏光)。
unsafe fn export_fd(dev: VkDevice, mem: VkDeviceMemory) -> i32 { unsafe {
    let Some(get_mem_fd) = dev_fns_global(dev).get_mem_fd else {
        return -1;
    };
    let gfi = VkMemoryGetFdInfoKHR {
        s_type: VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        p_next: null_mut(),
        memory: mem,
        handle_type: VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    let mut fd: i32 = -1;
    let r = get_mem_fd(dev, &gfi, &mut fd);
    if r != VK_SUCCESS {
        crate::shim_log!("vkGetMemoryFdKHR -> {} fd={} (mem={:p})", r, fd, mem);
        -1
    } else if fd < 0 {
        crate::shim_log!("vkGetMemoryFdKHR -> VK_SUCCESS 但 fd={}", fd);
        -1
    } else {
        fd
    }
}}

fn mem_props() -> &'static VkPhysicalDeviceMemoryProperties {
    crate::mem_props_static()
}

/* ---- vkAllocateMemory ---- */

pub type PFN_alloc_mem = unsafe extern "C" fn(
    VkDevice,
    *const VkMemoryAllocateInfo,
    *const VkAllocationCallbacks,
    *mut VkDeviceMemory,
) -> i32;

pub unsafe extern "C" fn shim_allocate_memory(
    device: VkDevice,
    p_allocate_info: *const VkMemoryAllocateInfo,
    p_allocator: *const VkAllocationCallbacks,
    p_memory: *mut VkDeviceMemory,
) -> i32 { unsafe {
    let fns = crate::dev_fns_global(device);
    let Some(alloc_mem) = fns.alloc_memory else {
        return VK_ERROR_INITIALIZATION_FAILED;
    };
    let info = &*p_allocate_info;

    let ti = info.memory_type_index;
    let props = mem_props();
    let host_visible = (ti as usize) < props.memory_type_count as usize
        && (props.memory_types[ti as usize].property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            != 0;
    let eligible = ALIAS_ENABLED.load(Ordering::Relaxed)
        && host_visible
        && fns.create_buffer.is_some()
        && fns.buffer_reqs.is_some()
        && ti != MEM_TYPE_HEAP1
        && info.allocation_size <= MAX_ALLOC_SIZE
        && !pnext_conflicts(info.p_next);

    if !eligible {
        return alloc_mem(device, info, p_allocator, p_memory);
    }

    /* 原子预留槽位: 后续插入必然成功, 消除表满竞态 UB */
    if ALIAS_COUNT
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| {
            (n < MAX_ALIAS as u32).then_some(n + 1)
        })
        .is_err()
    {
        return alloc_mem(device, info, p_allocator, p_memory);
    }
    /* 失败路径必须回滚槽位 */
    let mut reserved = true;
    let mut rollback = || {
        if reserved {
            ALIAS_COUNT.fetch_sub(1, Ordering::Relaxed);
            reserved = false;
        }
    };

    let create_buffer = fns.create_buffer.unwrap();
    let buffer_reqs = fns.buffer_reqs.unwrap();

    /* 靶子 buffer: 必须声明 external handle type, 否则 HAL 在 alloc 里段错误 */
    let embci = VkExternalMemoryBufferCreateInfo {
        s_type: VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        p_next: null_mut(),
        handle_types: VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    let bci = VkBufferCreateInfo {
        s_type: VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        p_next: &embci as *const _ as *const c_void,
        flags: 0,
        size: info.allocation_size,
        usage: VK_BUFFER_USAGE_TRANSFER_SRC_BIT
            | VK_BUFFER_USAGE_TRANSFER_DST_BIT
            | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
            | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
            | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
            | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        sharing_mode: VK_SHARING_MODE_EXCLUSIVE,
        queue_family_index_count: 0,
        p_queue_family_indices: null_mut(),
    };
    let mut target: VkBuffer = Handle::null();
    if create_buffer(device, &bci, null_mut(), &mut target) != VK_SUCCESS {
        rollback();
        /* 靶子 buffer 建不出来, 回退原样分配 */
        return alloc_mem(device, info, p_allocator, p_memory);
    }

    let mut mr = VkMemoryRequirements {
        size: 0,
        alignment: 0,
        memory_type_bits: 0,
    };
    buffer_reqs(device, target, &mut mr);
    /* dedicated 要求 allocationSize == 资源的 memReq.size; 靶子会因对齐略微膨胀,
     * 多分一点无害, guest 只用前 allocationSize 字节 */
    if mr.size < info.allocation_size {
        mr.size = info.allocation_size;
    }

    let mdai = VkMemoryDedicatedAllocateInfo {
        s_type: VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        p_next: info.p_next,
        image: Handle::null(),
        buffer: target,
    };
    let emai = VkExportMemoryAllocateInfo {
        s_type: VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        p_next: &mdai as *const _ as *const c_void,
        handle_types: VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    let mai = VkMemoryAllocateInfo {
        s_type: info.s_type,
        p_next: &emai as *const _ as *const c_void,
        allocation_size: mr.size,
        memory_type_index: info.memory_type_index,
    };

    let mut mem: VkDeviceMemory = Handle::null();
    let r = alloc_mem(device, &mai, p_allocator, &mut mem);
    if r != VK_SUCCESS {
        if let Some(destroy) = fns.destroy_buffer {
            destroy(device, target, null_mut());
        }
        rollback();
        /* 可导出分配失败, 回退原样分配 */
        return alloc_mem(device, info, p_allocator, p_memory);
    }
    /* 必须把句柄回写给调用方 (HAL 只写进了我们的局部变量) */
    *p_memory = mem;

    /* 插入记账表 (槽位已预留, 必然成功; 写锁只护表本身) */
    write_alias().insert(
        mem,
        MemEntry {
            mem,
            dev: device,
            buf: target,
            size: mr.size,
            fd: AtomicI32::new(-1),
            lo: AtomicPtr::new(null_mut()),
            len: AtomicUsize::new(0),
            hal_map_count: AtomicU32::new(0),
        },
    );

    let diag = ALLOC_DIAG.fetch_add(1, Ordering::Relaxed) < 16;
    if diag {
        crate::shim_log!(
            "alloc 已接管: type={} 请求={}KB 实分={}KB mem={:p}",
            ti,
            info.allocation_size / 1024,
            mr.size / 1024,
            mem
        );
    }
    VK_SUCCESS
}}

/* ---- vkMapMemory ---- */

pub type PFN_map = unsafe extern "C" fn(
    VkDevice,
    VkDeviceMemory,
    u64,
    u64,
    u32,
    *mut *mut c_void,
) -> i32;

pub unsafe extern "C" fn shim_map_memory(
    device: VkDevice,
    memory: VkDeviceMemory,
    offset: u64,
    size: u64,
    flags: u32,
    pp_data: *mut *mut c_void,
) -> i32 { unsafe {
    let fns = crate::dev_fns_global(device);
    let Some(real_map) = fns.map_memory else {
        return VK_ERROR_INITIALIZATION_FAILED;
    };
    if !ALIAS_ENABLED.load(Ordering::Relaxed) {
        return real_map(device, memory, offset, size, flags, pp_data);
    }

    /* 成功路径: 1 把读锁 (共享, 不阻塞其他线程) + 原子计数/取 lo */
    let lo = {
        let g = read_alias();
        let Some(e) = g.get(&memory) else {
            /* 没被接管的内存: 原样透传 */
            return real_map(device, memory, offset, size, flags, pp_data);
        };
        e.hal_map_count.fetch_add(1, Ordering::Relaxed);
        e.lo.load(Ordering::Acquire)
    };

    /* 仍然对 HAL 走一次真 map: SVM 下 CPU 映射同时确立 GPU VA, 不能省。
     * 它返回的高位指针我们不用, 但 unmap 时要配对还回去 (计数器)。 */
    let mut hi: *mut c_void = null_mut();
    let rr = real_map(device, memory, offset, size, flags, &mut hi);
    if rr != VK_SUCCESS {
        /* 冷路径: 回滚计数 */
        let g = read_alias();
        if let Some(e) = g.get(&memory) {
            e.hal_map_count.fetch_sub(1, Ordering::Relaxed);
        }
        return rr;
    }

    if lo.is_null() {
        /* 冷路径: 首次建低位映射 (fd/lo 都用 CAS, 无需写锁) */
        let built = setup_low_mapping(device, memory);
        if !built.is_null() {
            if !pp_data.is_null() {
                *pp_data = built.add(offset as usize);
            }
            return VK_SUCCESS;
        }
        /* setup 失败有两种成因:
         * (a) 真失败 (导不出 fd / 低 4GB 无空洞);
         * (b) 并发线程抢先 CAS 成功建立 lo, 本线程返回 null。
         * 对 (b) 必须重新读取 entry 的 lo 复用, 否则会把高位指针交给 32 位
         * guest (表示不了 -> 崩溃)。仅当 lo 仍为空才退回到 HAL 高位指针。 */
        let recheck = {
            let g = read_alias();
            g.get(&memory)
                .map(|e| e.lo.load(Ordering::Acquire))
                .unwrap_or(null_mut())
        };
        if !recheck.is_null() {
            if !pp_data.is_null() {
                *pp_data = recheck.add(offset as usize);
            }
            return VK_SUCCESS;
        }
        /* 确实建不出低位映射: 把 HAL 高位指针交出去 (比崩强);
         * 计数保留, 后续 unmap 会配对还回去 */
        if !pp_data.is_null() {
            *pp_data = hi;
        }
        return rr;
    }

    if !pp_data.is_null() {
        *pp_data = lo.add(offset as usize);
    }
    VK_SUCCESS
}}

/// 首次 map 时建立整块低位映射并挂到 entry (全程原子, 无写锁)。
/// 返回低位基址; 失败返回 NULL (调用方回退到 HAL 高位指针)。
unsafe fn setup_low_mapping(device: VkDevice, memory: VkDeviceMemory) -> *mut c_void { unsafe {
    /* 1) 取/导出 fd */
    let fd = {
        let g = read_alias();
        let Some(e) = g.get(&memory) else { return null_mut(); };
        let cur = e.fd.load(Ordering::Relaxed);
        if cur >= 0 {
            cur
        } else {
            drop(g);
            let f = export_fd(device, memory);
            if f < 0 {
                return null_mut();
            }
            let g = read_alias();
            match g.get(&memory) {
                Some(e) => match e.fd.compare_exchange(-1, f, Ordering::Relaxed, Ordering::Relaxed)
                {
                    Ok(_) => f,
                    Err(_) => {
                        cffi::close(f); /* 并发线程已导出 */
                        e.fd.load(Ordering::Relaxed)
                    }
                },
                None => {
                    cffi::close(f);
                    return null_mut();
                }
            }
        }
    };
    if fd < 0 {
        return null_mut();
    }

    /* 2) 整块映射到低位 (窗口化空闲表, 复用释放的洞) */
    ensure_low_window();
    let total = {
        let g = read_alias();
        g.get(&memory).map(|e| e.size).unwrap_or(0)
    };
    if total == 0 {
        return null_mut();
    }
    let p = map_low_windowed(fd, total);
    if p.is_null() {
        crate::shim_log!(
            "低位映射失败: <4GB 找不到 {}MB 空洞 (fd={})",
            total / (1024 * 1024),
            fd
        );
        return null_mut();
    }
    let len = align_up(total as usize, PAGE);

    /* 3) 挂到 entry (CAS, 并发线程先建好则丢弃我们这份) */
    let g = read_alias();
    match g.get(&memory) {
        Some(e) => match e
            .lo
            .compare_exchange(null_mut(), p, Ordering::AcqRel, Ordering::Acquire)
        {
            Ok(_) => {
                e.len.store(len, Ordering::Release);
                p
            }
            Err(_) => {
                cffi::munmap(p, len);
                null_mut()
            }
        },
        None => {
            cffi::munmap(p, len);
            null_mut()
        }
    }
}}

/* ---- vkUnmapMemory ---- */

pub type PFN_unmap = unsafe extern "C" fn(VkDevice, VkDeviceMemory);

pub unsafe extern "C" fn shim_unmap_memory(device: VkDevice, memory: VkDeviceMemory) { unsafe {
    let need_real = {
        let g = read_alias();
        match g.get(&memory) {
            /* 计数器归零前每次 unmap 都还一次 HAL map */
            Some(e) => e
                .hal_map_count
                .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |c| c.checked_sub(1))
                .is_ok(),
            /* 未被接管的内存: 原样透传 */
            None => true,
        }
    };
    if need_real
        && let Some(unmap) = crate::dev_fns_global(device).unmap_memory
    {
        unmap(device, memory);
    }
}}

/* ---- vkFlushMappedMemoryRanges / vkInvalidateMappedMemoryRanges ---- */

/// 32 位 guest 拿到的是 shim 自己 mmap 的低位映射 (同一 dmabuf fd 的 CPU 视角),
/// 与驱动内部映射是两份独立 CPU 映射。应用调用 Flush/Invalidate 时, 这些调用
/// 发往真实驱动, 只作用于驱动侧映射, 不会刷新我们那份低位 mmap 的 CPU 缓存,
/// 从而在 host-visible (非 coherent) 内存上产生缓存一致性盲区 -> 数据陈旧/错乱。
/// 解决: 对每段 range, 定位 entry 的低位 lo, 对其 [lo+offset, lo+offset+size]
/// 区间做 msync, 使我们的 CPU 映射与 dmabuf 后备存储一致。Flush=MS_SYNC 把脏页
/// 写回后备存储; Invalidate=MS_INVALIDATE 丢弃陈旧缓存行重新从后备存储读。
/// 同时把原调用转发给真实驱动, 不影响驱动自己那份映射。
pub type PFN_flush = unsafe extern "C" fn(
    VkDevice,
    u32,
    *const VkMappedMemoryRange,
) -> i32;

unsafe fn sync_mapped_ranges(
    device: VkDevice,
    range_count: u32,
    p_ranges: *const VkMappedMemoryRange,
    flags: c_int,
) -> i32 { unsafe {
    if ALIAS_ENABLED.load(Ordering::Relaxed) {
        for i in 0..range_count as usize {
            let r = &*p_ranges.add(i);
            let g = read_alias();
            if let Some(e) = g.get(&r.memory) {
                let lo = e.lo.load(Ordering::Acquire);
                if !lo.is_null() {
                    /* 区间按 mmap 页对齐 (msync 要求) */
                    let base = lo as usize;
                    let off = r.offset as usize;
                    let len_u = if r.size == VK_WHOLE_SIZE {
                        e.size.saturating_sub(r.offset) as usize
                    } else {
                        r.size as usize
                    };
                    let start = align_down(base + off, PAGE);
                    let end = base + off + len_u;
                    let len = end.saturating_sub(start);
                    if len > 0 {
                        cffi::msync(start as *mut c_void, len, flags);
                    }
                }
            }
        }
    }
    /* 转发给真实驱动, 保证其自身映射一致性不变 */
    let fns = dev_fns_global(device);
    let real = if flags & cffi::MS_INVALIDATE != 0 {
        fns.invalidate_ranges
    } else {
        fns.flush_ranges
    };
    match real {
        Some(f) => f(device, range_count, p_ranges),
        None => VK_SUCCESS,
    }
}}

fn align_down(v: usize, align: usize) -> usize {
    v & !(align - 1)
}

pub unsafe extern "C" fn shim_flush_mapped_memory_ranges(
    device: VkDevice,
    range_count: u32,
    p_ranges: *const VkMappedMemoryRange,
) -> i32 { unsafe {
    sync_mapped_ranges(device, range_count, p_ranges, cffi::MS_SYNC)
}}

pub unsafe extern "C" fn shim_invalidate_mapped_memory_ranges(
    device: VkDevice,
    range_count: u32,
    p_ranges: *const VkMappedMemoryRange,
) -> i32 { unsafe {
    sync_mapped_ranges(device, range_count, p_ranges, cffi::MS_SYNC | cffi::MS_INVALIDATE)
}}

/* ---- vkFreeMemory ---- */

pub type PFN_free = unsafe extern "C" fn(VkDevice, VkDeviceMemory, *const VkAllocationCallbacks);

pub unsafe extern "C" fn shim_free_memory(
    device: VkDevice,
    memory: VkDeviceMemory,
    p_allocator: *const VkAllocationCallbacks,
) { unsafe {
    /* 写锁摘 entry, 把要释放的资源拷贝出来, syscall 全部在锁外 */
    let (target, lo, len, fd, map_count) = {
        let mut g = write_alias();
        let Some(e) = g.remove(&memory) else {
            /* 未被接管的内存: 原样透传 */
            if let Some(free) = crate::dev_fns_global(device).free_memory {
                free(device, memory, p_allocator);
            }
            return;
        };
        ALIAS_COUNT.fetch_sub(1, Ordering::Relaxed);
        (
            e.buf,
            e.lo.load(Ordering::Acquire),
            e.len.load(Ordering::Relaxed),
            e.fd.load(Ordering::Relaxed),
            e.hal_map_count.load(Ordering::Relaxed),
        )
    };

    /* 防御: 应用未 unmap 就 free (DXVK 有此模式)。HAL 的每次 map 都要配对还回去:
     * map_count 可能是多次 vkMapMemory 的累积, 只还一次会漏掉其余 HAL 映射
     * (显存泄漏), 故循环到归零。 */
    if map_count > 0
        && let Some(unmap) = crate::dev_fns_global(device).unmap_memory
    {
        for _ in 0..map_count {
            unmap(device, memory);
        }
    }
    if !lo.is_null() {
        /* 窗口启用: MAP_FIXED 原子替换为 PROT_NONE 占位 + 还洞 (失败则强制 munmap)。
         * 未启用窗口(降级盲扫): 无空闲表可还, 仅解除原映射。 */
        if LOW_WIN_OK.load(Ordering::Relaxed) {
            low_release(lo as usize, len);
        } else {
            cffi::munmap(lo, len);
        }
    }
    if fd >= 0 {
        cffi::close(fd);
    }
    /* Vulkan 规范: 绑定到内存对象的资源 (靶子 buffer) 必须先于内存对象销毁。
     * 故 destroy_buffer 必须在 free_memory 之前, 否则驱动可能崩溃或 UB。 */
    if !target.is_null()
        && let Some(destroy) = crate::dev_fns_global(device).destroy_buffer
    {
        destroy(device, target, null_mut());
    }
    if let Some(free) = crate::dev_fns_global(device).free_memory {
        free(device, memory, p_allocator);
    }
}}

/* ---- vkDestroyDevice 清理 (设备销毁时释放 shim 自有资源) ---- */

pub unsafe fn cleanup_device(device: VkDevice) { unsafe {
    let fns = crate::dev_fns_global(device);
    // 写锁内一次性 drain 掉属于该 device 的全部 entry, 把待释放资源拷贝出来;
    // syscall (unmap/munmap/close/destroy) 全部在锁外执行。
    // 这样既不会漏掉"收集期间新插入"的表项 (drain 在持锁内完成),
    // 也无需先收集后删除两趟遍历。
    let mut victims: Vec<(VkDeviceMemory, VkBuffer, *mut c_void, usize, i32, u32)> = Vec::new();
    {
        let mut g = write_alias();
        let drained: Vec<VkDeviceMemory> = g
            .iter()
            .filter(|(_, e)| e.dev == device)
            .map(|(mem, _)| *mem)
            .collect();
        for mem in drained {
            if let Some(e) = g.remove(&mem) {
                ALIAS_COUNT.fetch_sub(1, Ordering::Relaxed);
                victims.push((
                    mem,
                    e.buf,
                    e.lo.load(Ordering::Acquire),
                    e.len.load(Ordering::Relaxed),
                    e.fd.load(Ordering::Relaxed),
                    e.hal_map_count.load(Ordering::Relaxed),
                ));
            }
        }
    }
    let n = victims.len();
    for (mem, buf, lo, len, fd, map_count) in victims {
        /* 同样把 outstanding 的 HAL map 全部配对还掉, 再清理 shim 自有资源 */
        if map_count > 0
            && let Some(unmap) = fns.unmap_memory
        {
            for _ in 0..map_count {
                unmap(device, mem);
            }
        }
        if !lo.is_null() {
            /* 与 shim_free_memory 一致: 窗口启用走 low_release (占位+还洞,
             * 失败强制 munmap), 降级盲扫则仅解除原映射。 */
            if LOW_WIN_OK.load(Ordering::Relaxed) {
                low_release(lo as usize, len);
            } else {
                cffi::munmap(lo, len);
            }
        }
        if fd >= 0 {
            cffi::close(fd);
        }
        if !buf.is_null()
            && let Some(destroy) = fns.destroy_buffer
        {
            destroy(device, buf, null_mut());
        }
    }
    crate::shim_log!("vkDestroyDevice: 已清理 {} 块接管内存", n);
}}

/* ---- 供测试/诊断查询的统计 ---- */

pub fn alias_count() -> u32 {
    ALIAS_COUNT.load(Ordering::Relaxed)
}
