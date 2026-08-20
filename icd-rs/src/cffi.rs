//! 零依赖 libc 绑定 (bionic/Android aarch64)。
//!
//! 只声明 shim 用到的符号与常量, 不引 `libc` crate, 与 C 原版零依赖风格一致。
//! 常量值取自 bionic 头文件 (Termux = Android bionic)。

#![allow(dead_code)]
#![allow(non_camel_case_types)]

use core::ffi::{c_char, c_int, c_void};

/* ---- dlopen/dlsym ---- */
pub const RTLD_LAZY: c_int = 0x0001;
pub const RTLD_NOW: c_int = 0x0002;
pub const RTLD_LOCAL: c_int = 0;
pub const RTLD_GLOBAL: c_int = 0x0100;
pub const RTLD_DEFAULT: *mut c_void = core::ptr::null_mut();

/* ---- mmap ---- */
pub const PROT_NONE: c_int = 0x0;
pub const PROT_READ: c_int = 0x1;
pub const PROT_WRITE: c_int = 0x2;
pub const MAP_SHARED: c_int = 0x01;
pub const MAP_PRIVATE: c_int = 0x02;
pub const MAP_FIXED: c_int = 0x10;
pub const MAP_ANONYMOUS: c_int = 0x20;
pub const MAP_NORESERVE: c_int = 0x40;
pub const MAP_FAILED: *mut c_void = usize::MAX as *mut c_void;

/* ---- signal ---- */
pub const SIGSEGV: c_int = 11;
pub const SIGBUS: c_int = 7;
pub const SIGABRT: c_int = 6;
pub const SA_SIGINFO: c_int = 0x0000_0004;

/* ---- sysconf ---- */
pub const _SC_PAGESIZE: c_int = 30;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct DlInfo {
    pub dli_fname: *const c_char,
    pub dli_fbase: *mut c_void,
    pub dli_sname: *const c_char,
    pub dli_saddr: *mut c_void,
}

/// siginfo_t 的最小可见前缀 (SIGSEGV/BUS 时内核填充 _sigfault 联合)。
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Siginfo {
    pub si_signo: c_int,
    pub si_errno: c_int,
    pub si_code: c_int,
    _pad: c_int,
    pub si_addr: *mut c_void,
}

/// struct sigaction (Linux aarch64: union + sigset_t(8B) + flags + restorer)
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SigAction {
    pub sa_handler: usize,
    pub sa_mask: u64,
    pub sa_flags: c_int,
    pub sa_restorer: usize,
}

unsafe extern "C" {
    pub fn malloc(size: usize) -> *mut c_void;
    pub fn free(ptr: *mut c_void);
    pub fn realloc(ptr: *mut c_void, size: usize) -> *mut c_void;

    pub fn dlopen(filename: *const c_char, flag: c_int) -> *mut c_void;
    pub fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
    pub fn dladdr(addr: *const c_void, info: *mut DlInfo) -> c_int;
    pub fn dlerror() -> *mut c_char;

    pub fn mmap(
        addr: *mut c_void,
        length: usize,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    pub fn munmap(addr: *mut c_void, length: usize) -> c_int;
    pub fn close(fd: c_int) -> c_int;
    pub fn sysconf(name: c_int) -> i64;

    pub fn getenv(name: *const c_char) -> *mut c_char;
    pub fn atoi(nptr: *const c_char) -> c_int;
    pub fn sigaction(signum: c_int, act: *const SigAction, oldact: *mut SigAction) -> c_int;
    pub fn _exit(status: c_int) -> !;
    pub fn write(fd: c_int, buf: *const c_void, count: usize) -> isize;
}
