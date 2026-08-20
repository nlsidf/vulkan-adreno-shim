//! 物理设备格式属性修正 (深度/模板格式放宽)。
//!
//! 与 C 原版一致:
//! - `vkGetPhysicalDeviceImageFormatProperties(/2)`: HAL 对深度格式一律报
//!   VK_ERROR_FORMAT_NOT_SUPPORTED, DXVK 的 CheckImageSupport 据此把深度格式
//!   整体判为不支持 → RenderTexture.Create 失败。这里对深度/模板格式且 usage
//!   仅为合理组合时返回宽松能力值; D32_SFLOAT_S8_UINT 直接顶替为
//!   D24_UNORM_S8_UINT 的真实能力 (HAL 真正支持的深度格式)。
//! - `vkGetPhysicalDeviceFormatProperties(/2)`: 补 DEPTH_STENCIL_ATTACHMENT +
//!   SAMPLED 位; D32_SFLOAT_S8_UINT 返回 D24_UNORM_S8_UINT 真实能力。
//! - BC 压缩格式补位 (BC1~BC7): HAL 报 0 能力位但硬件支持, DXVK/Unity 据此判
//!   压缩贴图不支持 → 3D 场景 albedo/表面贴图加载失败。
//!   该补位对 2D 游戏 星白列车(星空列车与白的旅行) 是必需的, 不可移除。
//!
//! 注: 颜色格式 COLOR_ATTACHMENT 补位经实测对标本躲猫猫无效(该游戏 3D 黑屏真因
//! 是 DXVK shader cache 损坏), 已回退, 见 adreno-depth.md。

#![allow(dead_code)]

use crate::ffi::*;
use crate::{
    bc_fix, ensure_fmtp2_resolved, ensure_fmtp_resolved, ensure_iffp2_resolved, raw_test, rtv_fix,
    real_fmtp, real_fmtp2, real_iffp, real_iffp2,
};

pub fn fmt_is_depth_stencil(fmt: VkFormat) -> bool {
    matches!(
        fmt,
        VK_FORMAT_D16_UNORM
            | VK_FORMAT_X8_D24_UNORM_PACK32
            | VK_FORMAT_D32_SFLOAT
            | VK_FORMAT_S8_UINT
            | VK_FORMAT_D16_UNORM_S8_UINT
            | VK_FORMAT_D24_UNORM_S8_UINT
            | VK_FORMAT_D32_SFLOAT_S8_UINT
    )
}

/// BC1~BC7 压缩纹理格式 (Adreno 540 HAL 报 0 能力位但硬件支持)。
pub fn fmt_is_bc(fmt: VkFormat) -> bool {
    (VK_FORMAT_BC1_RGB_UNORM_BLOCK..=VK_FORMAT_BC7_SRGB_BLOCK).contains(&fmt)
}

/// 纯函数: 给 HAL 报 0 的 BC 格式补上 SAMPLED + TRANSFER + BLIT 能力位。
/// 不触碰 Vulkan, 可单测。与 C 版 `fmtp_fix_depth` 的 BC 分支一致。
pub fn bc_fix_props(p: &mut VkFormatProperties) {
    p.optimal_tiling_features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
        | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT
        | VK_FORMAT_FEATURE_TRANSFER_DST_BIT
        | VK_FORMAT_FEATURE_BLIT_SRC_BIT
        | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    p.linear_tiling_features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
        | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT
        | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
}

/// 纯函数: 给"可采样但缺 COLOR_ATTACHMENT"的颜色格式补上 RT 颜色缓冲所需能力位。
/// 不触碰 Vulkan, 可单测。与 C 版 `fmtp_fix_depth` 的颜色补位分支一致。
/// 注: 该补位对 星白列车(星空列车与白的旅行) 等 2D 游戏是必需的, 无条件开启
/// (曾因误判"对标本无效"而移除, 导致星白回归黑屏; 现恢复为无条件, 与 C 版一致)。
pub fn color_add_rt_attachment(p: &mut VkFormatProperties) {
    p.optimal_tiling_features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
        | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT
        | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT
        | VK_FORMAT_FEATURE_TRANSFER_DST_BIT
        | VK_FORMAT_FEATURE_BLIT_SRC_BIT
        | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    p.linear_tiling_features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
        | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT
        | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
}

/* ---- vkGetPhysicalDeviceFormatProperties 修正 ---- */

pub unsafe fn fmtp_fix_depth(pd: VkPhysicalDevice, format: VkFormat, p: *mut VkFormatProperties) { unsafe {
    if p.is_null() || raw_test() {
        return;
    }
    if fmt_is_depth_stencil(format) {
        if format == VK_FORMAT_D32_SFLOAT_S8_UINT {
            // 完美伪装: 返回 D24S8 的真实能力值 (HAL 真正支持的深度格式),
            // 让应用以为硬件原生支持 D32S8, 从而走完整的 D32S8 路径
            // (之后由创建层/命令层统一替换成 D24S8)。
            let real = real_fmtp();
            if let Some(f) = real {
                f(pd, VK_FORMAT_D24_UNORM_S8_UINT, p);
            } else {
                (*p).linear_tiling_features = 0;
                (*p).optimal_tiling_features = 0;
                (*p).buffer_features = 0;
            }
            return;
        }
        (*p).optimal_tiling_features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
            | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        (*p).linear_tiling_features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
            | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        return;
    }
    // BC 压缩格式: HAL 报 0 能力位但硬件支持 → 补 SAMPLED+TRANSFER+BLIT。
    // 仅 VK_ICD_BC_FIX=1 时生效 (默认关), 只在 星白列车 等需要的脚本里开启。
    if bc_fix() && fmt_is_bc(format) {
        bc_fix_props(&mut *p);
        return;
    }
    // 颜色格式 COLOR_ATTACHMENT 补位 (仅 VK_ICD_RTV_FIX=1 时):
    // 可采样(SAMPLED)但缺 COLOR_ATTACHMENT → 补 COLOR_ATTACHMENT+BLEND+TRANSFER+BLIT。
    // 该补位仅 星白列车 等个别 2D 游戏需要; 对 Undertale/gal 等游戏反而会因谎报
    // 能力位导致黑屏, 故默认关闭 (门控在 rtv_fix()), 只在需要的脚本里显式开启。
    if rtv_fix()
        && (*p).optimal_tiling_features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT != 0
        && (*p).optimal_tiling_features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT == 0
    {
        crate::shim_dbg!(
            "fmtp ADD_COLOR_ATTACHMENT fmt={} opt=0x{:x}",
            format,
            (*p).optimal_tiling_features
        );
        color_add_rt_attachment(&mut *p);
        return;
    }
    // 诊断: 真实 HAL 报告完全不支持的格式
    if (40..=220).contains(&format)
        && (*p).optimal_tiling_features == 0
        && (*p).linear_tiling_features == 0
    {
        crate::shim_dbg!("fmtp UNSUPPORTED fmt={}", format);
    }
}}

/// 顺便修正 pNext 链里的 VkFormatProperties3KHR (2KHR 特性位, 64-bit)。
///
/// 深度/模板 → 补 DEPTH_STENCIL_ATTACHMENT + SAMPLED;
/// BC 压缩格式 → 补 COLOR_ATTACHMENT + SAMPLED (2KHR 位:
/// SAMPLED=0x1, COLOR_ATTACHMENT=0x80)。BC 分支同样门控在 `bc_fix()`。
pub unsafe fn fmtp_fix_depth_pnext(format: VkFormat, p: *mut VkFormatProperties2) { unsafe {
    if p.is_null() {
        return;
    }
    if fmt_is_depth_stencil(format) {
        let add = VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT
            | VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;
        let mut it = (*p).p_next as *mut VkBaseOutStructure;
        while !it.is_null() {
            if (*it).s_type == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR {
                let p3 = it as *mut VkFormatProperties3KHR;
                (*p3).optimal_tiling_features |= add;
                (*p3).linear_tiling_features |= add;
            }
            it = (*it).p_next;
        }
    } else if bc_fix() && fmt_is_bc(format) {
        // BC: 颜色附件 + 采样 (让 DXVK 认为压缩贴图可作 SRV/采样)
        let add = VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
        let mut it = (*p).p_next as *mut VkBaseOutStructure;
        while !it.is_null() {
            if (*it).s_type == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR {
                let p3 = it as *mut VkFormatProperties3KHR;
                (*p3).optimal_tiling_features |= add;
                (*p3).linear_tiling_features |= add;
            }
            it = (*it).p_next;
        }
    }
}}

/* ---- vkGetPhysicalDeviceImageFormatProperties ---- */

pub type PFN_iffp = unsafe extern "C" fn(
    VkPhysicalDevice,
    VkFormat,
    i32, /* VkImageType */
    i32, /* VkImageTiling */
    u32, /* VkImageUsageFlags */
    u32, /* VkImageCreateFlags */
    *mut VkImageFormatProperties,
) -> i32;

pub unsafe extern "C" fn shim_iffp(
    physical_device: VkPhysicalDevice,
    format: VkFormat,
    image_type: i32,
    tiling: i32,
    usage: u32,
    flags: u32,
    p_properties: *mut VkImageFormatProperties,
) -> i32 { unsafe {
    ensure_fmtp_resolved(physical_device);
    let Some(iffp) = real_iffp() else {
        return VK_ERROR_INITIALIZATION_FAILED;
    };
    let r = iffp(physical_device, format, image_type, tiling, usage, flags, p_properties);
    if r == VK_SUCCESS || raw_test() {
        return r;
    }

    // 深度/模板格式放宽。DXVK 会分别用 DEPTH_STENCIL_ATTACHMENT 与 SAMPLED
    // 各探测一次, 任一失败就把该 DXGI 格式整体标记为不支持并缓存 → 之后
    // RenderTexture.Create 全部失败。故 SAMPLED/TRANSFER 也一起放宽, 只挡
    // 真正没意义的 usage (STORAGE/COLOR_ATTACHMENT 等)。
    let ds_ok = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
        | VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if fmt_is_depth_stencil(format) && (usage & ds_ok) != 0 && (usage & !ds_ok) == 0 {
        // D32S8: 完美伪装, 返回 D24S8 的真实图像格式能力 (HAL 真正支持),
        // 让应用以为 D32S8 原生可用, 走完整 D32S8 路径后由创建/命令层替换。
        if format == VK_FORMAT_D32_SFLOAT_S8_UINT {
            let mut tmp: VkImageFormatProperties = core::mem::zeroed();
            if let Some(iffp) = real_iffp() {
                let rr = iffp(
                    physical_device,
                    VK_FORMAT_D24_UNORM_S8_UINT,
                    image_type,
                    tiling,
                    usage,
                    flags,
                    &mut tmp,
                );
                if rr == VK_SUCCESS {
                    if !p_properties.is_null() {
                        *p_properties = tmp;
                    }
                    crate::shim_dbg!(
                        "iffp D32S8 伪装为 D24S8 能力: tiling={} usage=0x{:x} -> VK_SUCCESS",
                        tiling,
                        usage
                    );
                    return VK_SUCCESS;
                }
            }
            // 兜底层: 若 D24S8 也查不到, 退回保守放宽 (与 C 版一致)。
        }
        // 保守回退 (仅当 D24S8 查询也失败时)
        let p = &mut *p_properties;
        p.max_extent = VkExtent3D {
            width: 8192,
            height: 8192,
            depth: 1,
        };
        p.max_mip_levels = 15;
        p.max_array_layers = 256;
        p.sample_counts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;
        p.max_resource_size = 0x8000_0000;
        crate::shim_dbg!(
            "iffp 深度格式保守放宽: fmt={} tiling={} usage=0x{:x} -> VK_SUCCESS",
            format,
            tiling,
            usage
        );
        VK_SUCCESS
    } else {
        r
    }
}}

/* ---- vkGetPhysicalDeviceFormatProperties ---- */

pub type PFN_fmtp = unsafe extern "C" fn(VkPhysicalDevice, VkFormat, *mut VkFormatProperties);

pub unsafe extern "C" fn shim_fmtp(
    physical_device: VkPhysicalDevice,
    format: VkFormat,
    p_properties: *mut VkFormatProperties,
) { unsafe {
    ensure_fmtp_resolved(physical_device);
    let Some(real) = real_fmtp() else { return };
    real(physical_device, format, p_properties);
    fmtp_fix_depth(physical_device, format, p_properties);
}}

/* ---- vkGetPhysicalDeviceImageFormatProperties2 (DXVK 实际探测路径) ---- */

pub type PFN_iffp2 = unsafe extern "C" fn(
    VkPhysicalDevice,
    *const VkPhysicalDeviceImageFormatInfo2,
    *mut VkImageFormatProperties2,
) -> i32;

pub unsafe extern "C" fn shim_iffp2(
    physical_device: VkPhysicalDevice,
    p_image_format_info: *const VkPhysicalDeviceImageFormatInfo2,
    p_image_format_properties: *mut VkImageFormatProperties2,
) -> i32 { unsafe {
    ensure_iffp2_resolved(physical_device);
    let Some(iffp2) = real_iffp2() else {
        return VK_ERROR_INITIALIZATION_FAILED;
    };
    // 先取输入信息 (复制一份), 再调真实驱动。
    let info = if p_image_format_info.is_null() {
        return VK_ERROR_INITIALIZATION_FAILED;
    } else {
        *p_image_format_info
    };
    let r = iffp2(physical_device, &info, p_image_format_properties);
    if r == VK_SUCCESS || raw_test() {
        return r;
    }

    // 深度/模板格式放宽 (与 v1 iffy 同策略): DXVK 的 CheckImageSupport 会分别用
    // DEPTH_STENCIL_ATTACHMENT 与 SAMPLED 各探测一次, 任一失败就把该 DXGI 格式
    // 整体标记不支持并缓存。故 SAMPLED/TRANSFER 也一起放宽。
    let ds_ok = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
        | VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if fmt_is_depth_stencil(info.format)
        && (info.usage & ds_ok) != 0
        && (info.usage & !ds_ok) == 0
    {
        // D32S8: 完美伪装, 返回 D24S8 的真实图像格式能力 (HAL 真正支持)。
        if info.format == VK_FORMAT_D32_SFLOAT_S8_UINT {
            let mut tmp: VkImageFormatProperties2 = core::mem::zeroed();
            if let Some(iffp2) = real_iffp2() {
                let rr = iffp2(
                    physical_device,
                    &VkPhysicalDeviceImageFormatInfo2 {
                        s_type: info.s_type,
                        p_next: info.p_next,
                        format: VK_FORMAT_D24_UNORM_S8_UINT,
                        image_type: info.image_type,
                        tiling: info.tiling,
                        usage: info.usage,
                        flags: info.flags,
                    },
                    &mut tmp,
                );
                if rr == VK_SUCCESS {
                    if !p_image_format_properties.is_null() {
                        // 仅覆盖 image_format_properties 字段, 保留应用挂载在
                        // 输出结构上的 s_type 与 p_next 扩展链 (整结构体赋会
                        // 把 s_type/p_next 也覆盖掉, 破坏扩展输出)。
                        (*p_image_format_properties).image_format_properties =
                            tmp.image_format_properties;
                    }
                    crate::shim_dbg!(
                        "iffp2 D32S8 伪装为 D24S8 能力: tiling={} usage=0x{:x} -> VK_SUCCESS",
                        info.tiling,
                        info.usage
                    );
                    return VK_SUCCESS;
                }
            }
            // 兜底层: 若 D24S8 也查不到, 退回保守放宽。
        }
        if !p_image_format_properties.is_null() {
            let p = &mut (*p_image_format_properties).image_format_properties;
            p.max_extent = VkExtent3D {
                width: 8192,
                height: 8192,
                depth: 1,
            };
            p.max_mip_levels = 15;
            p.max_array_layers = 256;
            p.sample_counts = VK_SAMPLE_COUNT_1_BIT
                | VK_SAMPLE_COUNT_2_BIT
                | VK_SAMPLE_COUNT_4_BIT;
            p.max_resource_size = 0x8000_0000;
            crate::shim_dbg!(
                "iffp2 深度格式保守放宽: fmt={} tiling={} usage=0x{:x} -> VK_SUCCESS",
                info.format,
                info.tiling,
                info.usage
            );
            return VK_SUCCESS;
        }
    }
    r
}}

/* ---- vkGetPhysicalDeviceFormatProperties2 ---- */

pub type PFN_fmtp2 =
    unsafe extern "C" fn(VkPhysicalDevice, VkFormat, *mut VkFormatProperties2);

pub unsafe extern "C" fn shim_fmtp2(
    physical_device: VkPhysicalDevice,
    format: VkFormat,
    p_properties: *mut VkFormatProperties2,
) { unsafe {
    if p_properties.is_null() {
        return;
    }
    ensure_fmtp2_resolved(physical_device);
    let Some(real) = real_fmtp2() else {
        // 退化到 v1
        shim_fmtp(physical_device, format, &mut (*p_properties).format_properties);
        return;
    };
    real(physical_device, format, p_properties);
    fmtp_fix_depth(physical_device, format, &mut (*p_properties).format_properties);
    fmtp_fix_depth_pnext(format, p_properties);
}}
