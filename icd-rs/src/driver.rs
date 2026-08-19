//! Adreno HAL 加载与函数解析。
//!
//! 与 C 原版构造器做的事一致: 链接 default<->sphal namespace, dlopen Adreno
//! Vulkan 驱动, 解析 qglinternal::vkGetInstanceProcAddr。用惰性初始化取代
//! `__attribute__((constructor))` —— Vulkan loader 本来就是在第一次调用
//! `vk_icdGetInstanceProcAddr` 时才用到本库, 惰性加载还顺带消除了构造器时序
//! 与多实例竞态 (OnceLock 保证只初始化一次)。

#![allow(dead_code)]

use crate::cffi;
use crate::ffi::*;
use core::ffi::CStr;

pub const ADRENO_VK_DRIVER: &CStr = c"/vendor/lib64/hw/vulkan.msm8998.so";
/// qglinternal::vkGetInstanceProcAddr(VkInstance_T*, const char*)
pub const ADRENO_GETPROC_MANGLED: &CStr =
    c"_ZN11qglinternal21vkGetInstanceProcAddrEP12VkInstance_TPKc";

/// PFN_vkVoidFunction: C 的 `void (*)(void)`, 用 Option 表达可空。
pub type PFN_vkVoidFunction = Option<unsafe extern "C" fn()>;
/// 驱动的 dispatcher (与 PFN_vkGetInstanceProcAddr ABI 兼容)。
pub type AdrenoGetProc = unsafe extern "C" fn(VkInstance, *const c_char) -> PFN_vkVoidFunction;

unsafe fn link_namespaces() {
    type GetNsFn = unsafe extern "C" fn(*const c_char) -> *mut c_void;
    type LinkNsFn = unsafe extern "C" fn(*mut c_void, *mut c_void, *const c_char) -> bool;

    let get_ns = cffi::dlsym(cffi::RTLD_DEFAULT, c"__loader_android_get_exported_namespace".as_ptr());
    let link_ns = cffi::dlsym(cffi::RTLD_DEFAULT, c"__loader_android_link_namespaces".as_ptr());
    if get_ns.is_null() || link_ns.is_null() {
        return;
    }
    let get_ns: GetNsFn = core::mem::transmute(get_ns);
    let link_ns: LinkNsFn = core::mem::transmute(link_ns);

    let def = get_ns(c"default".as_ptr());
    let sph = get_ns(c"sphal".as_ptr());
    if def.is_null() || sph.is_null() {
        return;
    }
    let libs = c"vulkan.msm8998.so:libEGL_adreno.so:libGLESv2_adreno.so:libGLESv1_CM_adreno.so:\
                libq3dtools_adreno.so:libadreno_utils.so:libgsl.so:libllvm-glnext.so:\
                libcutils.so:libutils.so:libhardware.so:libnativewindow.so:\
                libvulkan.so:libvkjson.so:libsync.so";
    link_ns(def, sph, libs.as_ptr());
}

/// 加载驱动并解析 dispatcher。失败返回 None (shim 全部入口会随之失效)。
pub unsafe fn init_driver() -> Option<AdrenoGetProc> {
    link_namespaces();

    let drv = cffi::dlopen(ADRENO_VK_DRIVER.as_ptr(), cffi::RTLD_LAZY | cffi::RTLD_GLOBAL);
    if drv.is_null() {
        let err = cffi::dlerror();
        if !err.is_null() {
            let s = CStr::from_ptr(err);
            crate::shim_log!(
                "警告: 无法 dlopen {}: {:?}",
                ADRENO_VK_DRIVER.to_string_lossy(),
                s
            );
        } else {
            crate::shim_log!("警告: 无法 dlopen {}", ADRENO_VK_DRIVER.to_string_lossy());
        }
        return None;
    }

    let sym = cffi::dlsym(drv, ADRENO_GETPROC_MANGLED.as_ptr());
    if sym.is_null() {
        crate::shim_log!("警告: 找不到 {}", ADRENO_GETPROC_MANGLED.to_string_lossy());
        return None;
    }
    crate::shim_log!("Adreno Vulkan 驱动已加载: drv={:p} get_proc={:p}", drv, sym);

    let get_proc: AdrenoGetProc = core::mem::transmute(sym);
    Some(get_proc)
}

/* ---- 通用解析工具 ---- */

/// 从 dispatcher 解析一个具名函数并转成目标类型 (函数指针转换)。
pub unsafe fn resolve_from_get_proc<T: Copy>(
    get_proc: AdrenoGetProc,
    inst: VkInstance,
    name: &CStr,
) -> Option<T> {
    let f: PFN_vkVoidFunction = get_proc(inst, name.as_ptr());
    let fp = f?;
    Some(core::mem::transmute_copy::<unsafe extern "C" fn(), T>(&fp))
}

/// 从设备 dispatcher (PFN_vkGetDeviceProcAddr) 解析。
pub unsafe fn resolve_from_get_dev_proc<T: Copy>(
    get_dev: unsafe extern "C" fn(VkDevice, *const c_char) -> PFN_vkVoidFunction,
    dev: VkDevice,
    name: &CStr,
) -> Option<T> {
    let f: PFN_vkVoidFunction = get_dev(dev, name.as_ptr());
    let fp = f?;
    Some(core::mem::transmute_copy::<unsafe extern "C" fn(), T>(&fp))
}

/// 把已强转为具体 fn 指针类型的函数转成 PFN_vkVoidFunction (发给 loader / 驱动)。
/// 调用方必须传入 fn 指针 (8 字节), 不能传零大小的函数项, 否则 transmute_copy 未定义。
pub fn to_void_fn<T: Copy>(f: T) -> PFN_vkVoidFunction {
    debug_assert_eq!(
        core::mem::size_of::<T>(),
        core::mem::size_of::<unsafe extern "C" fn()>()
    );
    Some(unsafe { core::mem::transmute_copy::<T, unsafe extern "C" fn()>(&f) })
}
