//! 日志宏。默认只打初始化/错误; 热路径诊断由 `verbose() >= 2` 门控
//! (VK_ICD_VERBOSE=2), 与原 C 的 SHIM_DBG 行为一致。

/// 一次性/错误日志, 始终输出到 stderr。
#[macro_export]
macro_rules! shim_log {
    ($($arg:tt)*) => {
        eprintln!("[VK_ICD] {}", format_args!($($arg)*))
    };
}

/// 热路径诊断, 仅 `VK_ICD_VERBOSE>=2` 时输出。
#[macro_export]
macro_rules! shim_dbg {
    ($($arg:tt)*) => {
        if $crate::verbose() >= 2 {
            eprintln!("[VK_ICD] {}", format_args!($($arg)*))
        }
    };
}
