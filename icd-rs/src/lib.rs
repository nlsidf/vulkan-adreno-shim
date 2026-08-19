//! vulkan_adreno_icd.so — Adreno Vulkan ICD 桥接 shim (Rust 版)。
//!
//! C 版 icd/vulkan_adreno_icd.c 的重写。解决的问题与 C 版相同:
//! 1. Adreno 540 HAL (/vendor/lib64/hw/vulkan.msm8998.so) 只导出 C++ 修饰名
//!    qglinternal::vkGetInstanceProcAddr, 没有标准 ICD 入口, 这里 dlopen 它并
//!    导出 vk_icdGetInstanceProcAddr / vk_icdGetPhysicalDeviceProcAddr 转调。
//! 2. vkMapMemory 返回的高位指针 >4GB, 32 位 guest (wine wow64) 表示不了 →
//!    拦截 alloc/map/free, 导出 dmabuf 自行映射到 <4GB (零拷贝, 见 memalias)。
//! 3. D32_SFLOAT_S8_UINT 透明替换为 D24_UNORM_S8_UINT (image/view/renderpass
//!    三处一致), 深度格式能力放宽 (见 fmtfix)。
//!
//! 相对 C 版的改进: 见 memalias.rs 头注释 (HashMap 查找、锁外 syscall、低位
//! 映射缓存、HAL map 计数器、去掉 MAP_FIXED) 与各泄漏修复 (L1/L2/L4)。

#![allow(dead_code)]
#![allow(non_camel_case_types)] /* PFN_* 类型名刻意沿用 C 命名 */
#![allow(clippy::missing_safety_doc)] /* ICD 入口由 C loader 调用, unsafe 契约即 Vulkan ABI */

mod macros;

mod alloc;
mod cffi;
mod driver;
mod ffi;
mod fmtfix;
mod intercept;
mod memalias;

use core::ffi::{c_char, c_void};
use core::ptr::null_mut;
use std::sync::{Mutex, OnceLock};

pub use intercept::{
    PFN_create_device, PFN_create_image, PFN_create_image_view, PFN_create_instance,
    PFN_create_renderpass, PFN_create_renderpass2, PFN_destroy_device, PFN_enum_pds,
    PFN_get_dev_proc, PFN_get_mem_props,
};

/* ================= 环境开关 ================= */

pub fn env_flag(name: &core::ffi::CStr) -> bool {
    unsafe { !cffi::getenv(name.as_ptr()).is_null() }
}

pub fn env_int(name: &core::ffi::CStr) -> i32 {
    unsafe {
        let p = cffi::getenv(name.as_ptr());
        if p.is_null() {
            0
        } else {
            cffi::atoi(p)
        }
    }
}

/// VK_TEST_RAW=1: 不做任何 D32S8 修复/替换, 让测试程序观察硬件真实行为。
static RAW_TEST: OnceLock<bool> = OnceLock::new();
pub fn raw_test() -> bool {
    *RAW_TEST.get_or_init(|| env_flag(c"VK_TEST_RAW"))
}

/// VK_ICD_VERBOSE>=2: 打开热路径诊断 (默认静默, 见 C 版 1f0ff6d)。
static VERBOSE: OnceLock<i32> = OnceLock::new();
pub fn verbose() -> i32 {
    *VERBOSE.get_or_init(|| env_int(c"VK_ICD_VERBOSE"))
}

/* ================= 驱动加载 ================= */

static GET_PROC: OnceLock<Option<driver::AdrenoGetProc>> = OnceLock::new();
pub fn get_proc_global() -> Option<driver::AdrenoGetProc> {
    // OnceLock 保证恰好初始化一次; 失败时缓存 None, 后续直接失效
    *GET_PROC.get_or_init(|| unsafe { driver::init_driver() })
}

/* ================= 实例 / 物理设备归属表 ================= */

struct InstEntry {
    inst: ffi::VkInstance,
    iffp: Option<fmtfix::PFN_iffp>,
    fmtp: Option<fmtfix::PFN_fmtp>,
}

static INSTS: Mutex<Vec<InstEntry>> = Mutex::new(Vec::new());
pub static PDS: Mutex<Vec<(ffi::VkPhysicalDevice, ffi::VkInstance)>> = Mutex::new(Vec::new());
static REAL_ICD_INSTANCE: Mutex<Option<ffi::VkInstance>> = Mutex::new(None);

pub fn set_real_icd_instance(inst: ffi::VkInstance) {
    *REAL_ICD_INSTANCE.lock().unwrap() = Some(inst);
}

pub fn add_inst(inst: ffi::VkInstance) {
    let mut guard = INSTS.lock().unwrap();
    if !guard.iter().any(|e| e.inst == inst) {
        guard.push(InstEntry {
            inst,
            iffp: None,
            fmtp: None,
        });
    }
}

fn inst_for_pd(pd: ffi::VkPhysicalDevice) -> ffi::VkInstance {
    let pds = PDS.lock().unwrap();
    for (p, i) in pds.iter() {
        if *p == pd {
            return *i;
        }
    }
    drop(pds);
    let insts = INSTS.lock().unwrap();
    if let Some(last) = insts.last() {
        return last.inst;
    }
    REAL_ICD_INSTANCE
        .lock()
        .unwrap()
        .unwrap_or(ffi::Handle::null())
}

/* ================= 惰性解析的真实函数 ================= */

static REAL_CREATE_INSTANCE: OnceLock<Option<intercept::PFN_create_instance>> = OnceLock::new();
pub fn real_create_instance() -> Option<intercept::PFN_create_instance> {
    let get_proc = get_proc_global()?;
    *REAL_CREATE_INSTANCE.get_or_init(|| unsafe {
        driver::resolve_from_get_proc(get_proc, ffi::Handle::null(), c"vkCreateInstance")
    })
}

static REAL_CREATE_DEVICE: OnceLock<Option<intercept::PFN_create_device>> = OnceLock::new();
pub fn real_create_device() -> Option<intercept::PFN_create_device> {
    let get_proc = get_proc_global()?;
    *REAL_CREATE_DEVICE.get_or_init(|| unsafe {
        // 与 C 版一致: 用已建成的真实实例解析 (若还没建实例则退回 NULL)
        let inst = REAL_ICD_INSTANCE.lock().unwrap().unwrap_or(ffi::Handle::null());
        driver::resolve_from_get_proc(get_proc, inst, c"vkCreateDevice")
    })
}

static REAL_ENUM_PDS: OnceLock<Option<intercept::PFN_enum_pds>> = OnceLock::new();
pub fn real_enum_pds(instance: ffi::VkInstance) -> Option<intercept::PFN_enum_pds> {
    let get_proc = get_proc_global()?;
    *REAL_ENUM_PDS.get_or_init(|| unsafe {
        // PD 级函数必须按真实实例解析 (NULL 实例在部分 HAL 上返回 NULL)
        driver::resolve_from_get_proc(get_proc, instance, c"vkEnumeratePhysicalDevices")
    })
}

/// vkGetDeviceProcAddr 由 loader 从实例入口解析, 存第一次的。
static REAL_GET_DEV_PROC: OnceLock<Option<intercept::PFN_get_dev_proc>> = OnceLock::new();
pub fn real_get_dev_proc() -> Option<intercept::PFN_get_dev_proc> {
    *REAL_GET_DEV_PROC.get().unwrap_or(&None)
}
pub fn resolve_get_dev_proc(instance: ffi::VkInstance) {
    let _ = REAL_GET_DEV_PROC.get_or_init(|| {
        let get_proc = get_proc_global()?;
        unsafe { driver::resolve_from_get_proc(get_proc, instance, c"vkGetDeviceProcAddr") }
    });
}

/* ---- iffp/fmtp: 构造期用 NULL 实例解析, 失败则按 PD 所属实例补 ---- */

static REAL_IFFP: OnceLock<Option<fmtfix::PFN_iffp>> = OnceLock::new();
static REAL_FMTP: OnceLock<Option<fmtfix::PFN_fmtp>> = OnceLock::new();
static FMTP_RESOLVE: Mutex<()> = Mutex::new(());

pub fn real_iffp() -> Option<fmtfix::PFN_iffp> {
    *REAL_IFFP.get().unwrap_or(&None)
}
pub fn real_fmtp() -> Option<fmtfix::PFN_fmtp> {
    *REAL_FMTP.get().unwrap_or(&None)
}

/// 多实例安全: 按 PD 所属实例解析并缓存 (OnceLock 只初始化一次)。
pub fn ensure_fmtp_resolved(pd: ffi::VkPhysicalDevice) {
    if REAL_IFFP.get().is_some() && REAL_FMTP.get().is_some() {
        return;
    }
    let _guard = FMTP_RESOLVE.lock().unwrap();
    if REAL_IFFP.get().is_some() && REAL_FMTP.get().is_some() {
        return;
    }
    let Some(get_proc) = get_proc_global() else {
        return;
    };
    let owner = inst_for_pd(pd);
    let iffp = unsafe {
        driver::resolve_from_get_proc::<fmtfix::PFN_iffp>(
            get_proc,
            owner,
            c"vkGetPhysicalDeviceImageFormatProperties",
        )
    };
    let fmtp = unsafe {
        driver::resolve_from_get_proc::<fmtfix::PFN_fmtp>(
            get_proc,
            owner,
            c"vkGetPhysicalDeviceFormatProperties",
        )
    };
    let _ = REAL_IFFP.set(iffp);
    let _ = REAL_FMTP.set(fmtp);
}

static REAL_FMTP2: OnceLock<Option<fmtfix::PFN_fmtp2>> = OnceLock::new();
pub fn real_fmtp2() -> Option<fmtfix::PFN_fmtp2> {
    *REAL_FMTP2.get().unwrap_or(&None)
}
pub fn ensure_fmtp2_resolved(pd: ffi::VkPhysicalDevice) {
    let _ = REAL_FMTP2.get_or_init(|| {
        let get_proc = get_proc_global()?;
        let owner = inst_for_pd(pd);
        unsafe {
            driver::resolve_from_get_proc::<fmtfix::PFN_fmtp2>(
                get_proc,
                owner,
                c"vkGetPhysicalDeviceFormatProperties2",
            )
        }
    });
}

/* ================= 设备函数表 ================= */

/// 需要拦截的设备的真实函数, 首次 vkGetDeviceProcAddr 时整体解析。
#[derive(Clone, Copy)]
pub struct DevFns {
    pub map_memory: Option<memalias::PFN_map>,
    pub unmap_memory: Option<memalias::PFN_unmap>,
    pub alloc_memory: Option<memalias::PFN_alloc_mem>,
    pub free_memory: Option<memalias::PFN_free>,
    pub create_buffer:
        Option<unsafe extern "C" fn(ffi::VkDevice, *const ffi::VkBufferCreateInfo, *const ffi::VkAllocationCallbacks, *mut ffi::VkBuffer) -> i32>,
    pub destroy_buffer: Option<
        unsafe extern "C" fn(ffi::VkDevice, ffi::VkBuffer, *const ffi::VkAllocationCallbacks),
    >,
    pub buffer_reqs: Option<
        unsafe extern "C" fn(ffi::VkDevice, ffi::VkBuffer, *mut ffi::VkMemoryRequirements),
    >,
    pub get_mem_fd: Option<
        unsafe extern "C" fn(ffi::VkDevice, *const ffi::VkMemoryGetFdInfoKHR, *mut i32) -> i32,
    >,
    pub create_image: Option<intercept::PFN_create_image>,
    pub create_image_view: Option<intercept::PFN_create_image_view>,
    pub create_renderpass: Option<intercept::PFN_create_renderpass>,
    pub create_renderpass2: Option<intercept::PFN_create_renderpass2>,
    pub create_renderpass2khr: Option<intercept::PFN_create_renderpass2>,
    pub destroy_device: Option<
        unsafe extern "C" fn(ffi::VkDevice, *const ffi::VkAllocationCallbacks),
    >,
}

impl DevFns {
    fn empty() -> Self {
        DevFns {
            map_memory: None,
            unmap_memory: None,
            alloc_memory: None,
            free_memory: None,
            create_buffer: None,
            destroy_buffer: None,
            buffer_reqs: None,
            get_mem_fd: None,
            create_image: None,
            create_image_view: None,
            create_renderpass: None,
            create_renderpass2: None,
            create_renderpass2khr: None,
            destroy_device: None,
        }
    }
}

static DEV_FNS: OnceLock<DevFns> = OnceLock::new();

fn init_dev_fns(device: ffi::VkDevice) -> DevFns {
    let get_dev = match REAL_GET_DEV_PROC.get().and_then(|x| *x) {
        Some(f) => f,
        None => {
            crate::shim_log!("警告: vkGetDeviceProcAddr 未解析, 设备函数表为空, 走原路径");
            return DevFns::empty();
        }
    };
    let mut fns = DevFns::empty();
    unsafe {
        fns.map_memory =
            driver::resolve_from_get_dev_proc(get_dev, device, c"vkMapMemory");
        fns.unmap_memory =
            driver::resolve_from_get_dev_proc(get_dev, device, c"vkUnmapMemory");
        fns.alloc_memory =
            driver::resolve_from_get_dev_proc(get_dev, device, c"vkAllocateMemory");
        fns.free_memory =
            driver::resolve_from_get_dev_proc(get_dev, device, c"vkFreeMemory");
        fns.create_buffer =
            driver::resolve_from_get_dev_proc(get_dev, device, c"vkCreateBuffer");
        fns.destroy_buffer =
            driver::resolve_from_get_dev_proc(get_dev, device, c"vkDestroyBuffer");
        fns.buffer_reqs = driver::resolve_from_get_dev_proc(
            get_dev,
            device,
            c"vkGetBufferMemoryRequirements",
        );
        fns.get_mem_fd =
            driver::resolve_from_get_dev_proc(get_dev, device, c"vkGetMemoryFdKHR");
        fns.create_image =
            driver::resolve_from_get_dev_proc(get_dev, device, c"vkCreateImage");
        fns.create_image_view =
            driver::resolve_from_get_dev_proc(get_dev, device, c"vkCreateImageView");
        fns.create_renderpass =
            driver::resolve_from_get_dev_proc(get_dev, device, c"vkCreateRenderPass");
        fns.create_renderpass2 =
            driver::resolve_from_get_dev_proc(get_dev, device, c"vkCreateRenderPass2");
        fns.create_renderpass2khr =
            driver::resolve_from_get_dev_proc(get_dev, device, c"vkCreateRenderPass2KHR");
        fns.destroy_device =
            driver::resolve_from_get_dev_proc(get_dev, device, c"vkDestroyDevice");
    }

    memalias::init_alias_env();
    if fns.get_mem_fd.is_none()
        || fns.create_buffer.is_none()
        || fns.buffer_reqs.is_none()
        || fns.alloc_memory.is_none()
    {
        memalias::ALIAS_ENABLED.store(false, std::sync::atomic::Ordering::Relaxed);
        crate::shim_log!("关键函数缺失, 低位 dmabuf 方案关闭, 走原路径");
    }
    crate::shim_log!(
        "设备函数解析: map=0x{:x} unmap=0x{:x} alloc=0x{:x} free=0x{:x} createBuf=0x{:x} destroyBuf=0x{:x} bufReqs=0x{:x} getFd=0x{:x}",
        fns.map_memory.map_or(0usize, |f| f as usize),
        fns.unmap_memory.map_or(0usize, |f| f as usize),
        fns.alloc_memory.map_or(0usize, |f| f as usize),
        fns.free_memory.map_or(0usize, |f| f as usize),
        fns.create_buffer.map_or(0usize, |f| f as usize),
        fns.destroy_buffer.map_or(0usize, |f| f as usize),
        fns.buffer_reqs.map_or(0usize, |f| f as usize),
        fns.get_mem_fd.map_or(0usize, |f| f as usize),
    );
    fns
}

/// 已初始化的设备函数表。未初始化返回 None (绝不 panic)。
pub fn dev_fns() -> Option<&'static DevFns> {
    DEV_FNS.get()
}

/// 保证设备函数表已初始化 (首次调用按 device 惰性解析)。
pub fn dev_fns_global(device: ffi::VkDevice) -> &'static DevFns {
    DEV_FNS.get_or_init(|| init_dev_fns(device))
}

/* ================= 物理设备内存类型缓存 ================= */

static MEM_PROPS: OnceLock<ffi::VkPhysicalDeviceMemoryProperties> = OnceLock::new();

pub fn cache_mem_props(pd: ffi::VkPhysicalDevice) {
    let _ = MEM_PROPS.get_or_init(|| {
        let mut props = ffi::VkPhysicalDeviceMemoryProperties {
            memory_type_count: 0,
            memory_types: [ffi::VkMemoryType {
                property_flags: 0,
                heap_index: 0,
            }; ffi::VK_MAX_MEMORY_TYPES],
            memory_heap_count: 0,
            memory_heaps: [ffi::VkMemoryHeap { size: 0, flags: 0 }; ffi::VK_MAX_MEMORY_HEAPS],
        };
        let get_proc = get_proc_global();
        if let Some(get_proc) = get_proc {
            let owner = inst_for_pd(pd);
            if let Some(f) = unsafe {
                driver::resolve_from_get_proc::<intercept::PFN_get_mem_props>(
                    get_proc,
                    owner,
                    c"vkGetPhysicalDeviceMemoryProperties",
                )
            } {
                unsafe { f(pd, &mut props) };
            }
        }
        props
    });
}

pub fn mem_props_static() -> &'static ffi::VkPhysicalDeviceMemoryProperties {
    MEM_PROPS.get().unwrap_or(&EMPTY_MEM_PROPS)
}

static EMPTY_MEM_PROPS: ffi::VkPhysicalDeviceMemoryProperties = ffi::VkPhysicalDeviceMemoryProperties {
    memory_type_count: 0,
    memory_types: [ffi::VkMemoryType {
        property_flags: 0,
        heap_index: 0,
    }; ffi::VK_MAX_MEMORY_TYPES],
    memory_heap_count: 0,
    memory_heaps: [ffi::VkMemoryHeap { size: 0, flags: 0 }; ffi::VK_MAX_MEMORY_HEAPS],
};

/* ================= 崩溃回溯 (VK_ICD_BT=1) ================= */

const CRASH_MSG: &[u8] = b"\n[VK_ICD] SIG ";
const CRASH_MSG2: &[u8] = b" addr 0x";
const CRASH_MSG3: &[u8] = b"\n";

unsafe extern "C" fn crash_handler(sig: i32, si: *mut cffi::Siginfo, _uc: *mut c_void) {
    let mut buf = [0u8; 64];
    let mut n = 0;
    buf[..CRASH_MSG.len()].copy_from_slice(CRASH_MSG);
    n += CRASH_MSG.len();
    n += format_int(&mut buf[n..], sig as u32);
    buf[n..n + CRASH_MSG2.len()].copy_from_slice(CRASH_MSG2);
    n += CRASH_MSG2.len();
    let addr = if si.is_null() {
        0usize
    } else {
        (*si).si_addr as usize
    };
    n += format_hex(&mut buf[n..], addr);
    buf[n..n + CRASH_MSG3.len()].copy_from_slice(CRASH_MSG3);
    n += CRASH_MSG3.len();
    cffi::write(2, buf.as_ptr() as *const c_void, n);
    cffi::_exit(139);
}

fn format_int(buf: &mut [u8], mut v: u32) -> usize {
    let mut tmp = [0u8; 10];
    let mut i = 0;
    if v == 0 {
        tmp[0] = b'0';
        i = 1;
    } else {
        while v > 0 {
            tmp[i] = b'0' + (v % 10) as u8;
            v /= 10;
            i += 1;
        }
    }
    let mut n = 0;
    while i > 0 {
        i -= 1;
        buf[n] = tmp[i];
        n += 1;
    }
    n
}

fn format_hex(buf: &mut [u8], mut v: usize) -> usize {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut tmp = [0u8; 16];
    let mut i = 0;
    if v == 0 {
        tmp[0] = b'0';
        i = 1;
    } else {
        while v > 0 {
            tmp[i] = HEX[v & 0xf];
            v >>= 4;
            i += 1;
        }
    }
    let mut n = 0;
    while i > 0 {
        i -= 1;
        buf[n] = tmp[i];
        n += 1;
    }
    n
}

fn install_crash_handler() {
    if !env_flag(c"VK_ICD_BT") {
        return;
    }
    let mut sa: cffi::SigAction = unsafe { core::mem::zeroed() };
    sa.sa_handler = crash_handler
        as unsafe extern "C" fn(i32, *mut cffi::Siginfo, *mut c_void)
        as usize;
    sa.sa_flags = cffi::SA_SIGINFO;
    unsafe {
        cffi::sigaction(cffi::SIGSEGV, &sa, null_mut());
        cffi::sigaction(cffi::SIGBUS, &sa, null_mut());
        cffi::sigaction(cffi::SIGABRT, &sa, null_mut());
    }
}

/* ================= 标准 ICD 入口 ================= */

#[no_mangle]
pub unsafe extern "C" fn vk_icdGetInstanceProcAddr(
    instance: ffi::VkInstance,
    p_name: *const c_char,
) -> driver::PFN_vkVoidFunction {
    if p_name.is_null() {
        return None;
    }
    let get_proc = get_proc_global()?;
    let name = core::ffi::CStr::from_ptr(p_name).to_bytes();

    if name == b"vkCreateInstance" {
        return driver::to_void_fn(intercept::shim_create_instance as intercept::PFN_create_instance);
    }
    if name == b"vkCreateDevice" {
        return driver::to_void_fn(intercept::shim_create_device as intercept::PFN_create_device);
    }
    if name == b"vkEnumeratePhysicalDevices" {
        return driver::to_void_fn(intercept::shim_enum_physical_devices as intercept::PFN_enum_pds);
    }
    if name == b"vkGetDeviceProcAddr" {
        resolve_get_dev_proc(instance);
        if REAL_GET_DEV_PROC.get().is_some_and(|x| x.is_some()) {
            return driver::to_void_fn(intercept::shim_get_device_proc_addr as intercept::PFN_get_dev_proc);
        }
    }
    if name == b"vkGetPhysicalDeviceImageFormatProperties" {
        return driver::to_void_fn(fmtfix::shim_iffp as fmtfix::PFN_iffp);
    }
    if name == b"vkGetPhysicalDeviceFormatProperties" {
        return driver::to_void_fn(fmtfix::shim_fmtp as fmtfix::PFN_fmtp);
    }
    if name == b"vkGetPhysicalDeviceFormatProperties2" {
        return driver::to_void_fn(fmtfix::shim_fmtp2 as fmtfix::PFN_fmtp2);
    }
    get_proc(instance, p_name)
}

#[no_mangle]
pub unsafe extern "C" fn vk_icdGetPhysicalDeviceProcAddr(
    physical_device: ffi::VkPhysicalDevice,
    p_name: *const c_char,
) -> driver::PFN_vkVoidFunction {
    if p_name.is_null() {
        return None;
    }
    let get_proc = get_proc_global()?;
    let name = core::ffi::CStr::from_ptr(p_name).to_bytes();

    if name == b"vkCreateDevice" {
        return driver::to_void_fn(intercept::shim_create_device as intercept::PFN_create_device);
    }
    if name == b"vkGetPhysicalDeviceImageFormatProperties" {
        return driver::to_void_fn(fmtfix::shim_iffp as fmtfix::PFN_iffp);
    }
    if name == b"vkGetPhysicalDeviceFormatProperties" {
        return driver::to_void_fn(fmtfix::shim_fmtp as fmtfix::PFN_fmtp);
    }
    if name == b"vkGetPhysicalDeviceFormatProperties2" {
        return driver::to_void_fn(fmtfix::shim_fmtp2 as fmtfix::PFN_fmtp2);
    }
    /* 其余 PD 级函数按 pName 交给驱动 dispatcher */
    get_proc(physical_device as ffi::VkInstance, p_name)
}
