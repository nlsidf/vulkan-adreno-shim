//! 原生分配器注入 (VK_ICD_INJECT_ALLOC)。
//!
//! 与 C 原版相同: 分配头内联在返回指针之前, 无容量上限、无锁、
//! realloc 能拿到精确旧长度。头 8 字节 = (raw, size)。

#![allow(dead_code)]

use crate::cffi;
use crate::ffi::*;
use core::ptr::null_mut;

#[repr(C)]
#[derive(Clone, Copy)]
struct IcdHdr {
    raw: *mut c_void,
    size: usize,
}

unsafe fn icd_raw_alloc(size: usize, alignment: usize) -> *mut c_void {
    let alignment = alignment.max(core::mem::size_of::<usize>());
    if alignment & (alignment - 1) != 0 {
        return null_mut(); /* 规范要求 2 的幂 */
    }
    let total = size + alignment + core::mem::size_of::<IcdHdr>();
    let raw = cffi::malloc(total);
    if raw.is_null() {
        return null_mut();
    }
    let base = raw as usize + core::mem::size_of::<IcdHdr>();
    let aligned = (base + alignment - 1) & !(alignment - 1);
    let h = (aligned - core::mem::size_of::<IcdHdr>()) as *mut IcdHdr;
    (*h).raw = raw;
    (*h).size = size;
    aligned as *mut c_void
}

unsafe extern "C" fn icd_cb_free(_u: *mut c_void, p: *mut c_void) {
    if p.is_null() {
        return;
    }
    let h = (p as usize - core::mem::size_of::<IcdHdr>()) as *const IcdHdr;
    cffi::free((*h).raw);
}

unsafe extern "C" fn icd_cb_alloc(
    _u: *mut c_void,
    size: usize,
    alignment: usize,
    _scope: VkSystemAllocationScope,
) -> *mut c_void {
    icd_raw_alloc(size, alignment)
}

unsafe extern "C" fn icd_cb_realloc(
    _u: *mut c_void,
    orig: *mut c_void,
    size: usize,
    alignment: usize,
    _scope: VkSystemAllocationScope,
) -> *mut c_void {
    if orig.is_null() {
        return icd_raw_alloc(size, alignment);
    }
    if size == 0 {
        icd_cb_free(_u, orig);
        return null_mut();
    }
    let old = ((orig as usize - core::mem::size_of::<IcdHdr>()) as *const IcdHdr).read().size;
    let np = icd_raw_alloc(size, alignment);
    if np.is_null() {
        return null_mut(); /* 失败时 orig 必须保持有效 */
    }
    core::ptr::copy_nonoverlapping(orig, np, old.min(size));
    icd_cb_free(_u, orig);
    np
}

/// 原生 (LP64) VkAllocationCallbacks, 与 C 版 `ICD_NATIVE_ALLOC` 等价。
pub static HAL_CTX: VkAllocationCallbacks = VkAllocationCallbacks {
    p_user_data: null_mut(),
    pfn_allocation: Some(icd_cb_alloc),
    pfn_reallocation: Some(icd_cb_realloc),
    pfn_free: Some(icd_cb_free),
    pfn_internal_allocation: None,
    pfn_internal_free: None,
};

/// 若 VK_ICD_INJECT_ALLOC 已设, 用原生分配器替换调用方传来的 (可能被
/// wow64 转译坏的) 那个。
pub fn icd_pick_alloc(alloc: *const VkAllocationCallbacks) -> *const VkAllocationCallbacks {
    if crate::env_flag(c"VK_ICD_INJECT_ALLOC") {
        crate::shim_log!(
            "INJECT_ALLOC: 用原生分配器 {:p} (原 {:p})",
            &HAL_CTX as *const _,
            alloc
        );
        &HAL_CTX
    } else {
        alloc
    }
}
