//! 物理设备格式属性修正 (深度/模板格式放宽)。
//!
//! 与 C 原版一致:
//! - `vkGetPhysicalDeviceImageFormatProperties`: HAL 对深度格式一律报
//!   VK_ERROR_FORMAT_NOT_SUPPORTED, DXVK 的 CheckImageSupport 据此把深度格式
//!   整体判为不支持 → RenderTexture.Create 失败。这里对深度/模板格式且 usage
//!   仅为合理组合时返回宽松能力值。
//! - `vkGetPhysicalDeviceFormatProperties(/2)`: 补 DEPTH_STENCIL_ATTACHMENT +
//!   SAMPLED 位; D32_SFLOAT_S8_UINT 直接顶替为 D24_UNORM_S8_UINT 真实能力。
//! - BC/颜色格式谎报已移除 (实测负优化, 见 C 版注释)。

#![allow(dead_code)]

use crate::ffi::*;
use crate::{ensure_fmtp2_resolved, ensure_fmtp_resolved, real_fmtp, real_fmtp2, real_iffp, raw_test};

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

/* ---- vkGetPhysicalDeviceFormatProperties 修正 ---- */

pub unsafe fn fmtp_fix_depth(pd: VkPhysicalDevice, format: VkFormat, p: *mut VkFormatProperties) {
    if p.is_null() || raw_test() {
        return;
    }
    if fmt_is_depth_stencil(format) {
        if format == VK_FORMAT_D32_SFLOAT_S8_UINT {
            // 顶替为 D24S8 的真实能力 (D32S8 硬件实际不能渲染)
            let mut good = VkFormatProperties {
                linear_tiling_features: 0,
                optimal_tiling_features: 0,
                buffer_features: 0,
            };
            if let Some(real) = real_fmtp() {
                real(pd, VK_FORMAT_D24_UNORM_S8_UINT, &mut good);
            }
            *p = good;
            return;
        }
        (*p).optimal_tiling_features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
            | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        (*p).linear_tiling_features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
            | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        return;
    }
    // 诊断: 真实 HAL 报告完全不支持的格式
    if (40..=220).contains(&format)
        && (*p).optimal_tiling_features == 0
        && (*p).linear_tiling_features == 0
    {
        crate::shim_dbg!("fmtp UNSUPPORTED fmt={}", format);
    }
}

/// 顺便修正 pNext 链里的 VkFormatProperties3KHR (2KHR 特性位, 64-bit)。
pub unsafe fn fmtp_fix_depth_pnext(format: VkFormat, p: *mut VkFormatProperties2) {
    if p.is_null() || !fmt_is_depth_stencil(format) {
        return;
    }
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
}

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
) -> i32 {
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
        // D32S8: 硬件无法渲染 (探针全组合 SIGSEGV, 见 tests/probe_d32s8.c), shim
        // 会透明替换成 D24S8。这里直接返回 D24S8 的真实 iffp 结果, 让能力声明
        // 与实际替换后的硬件完全一致, 不再硬编码谎报。
        if format == VK_FORMAT_D32_SFLOAT_S8_UINT {
            let mut real_props = *p_properties;
            let rr = iffp(
                physical_device,
                VK_FORMAT_D24_UNORM_S8_UINT,
                image_type,
                tiling,
                usage,
                flags,
                &mut real_props,
            );
            if rr == VK_SUCCESS {
                *p_properties = real_props;
                crate::shim_dbg!(
                    "iffp D32S8 用 D24S8 真实能力: fmt={} tiling={} usage=0x{:x} -> VK_SUCCESS",
                    format,
                    tiling,
                    usage
                );
                return VK_SUCCESS;
            }
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
}

/* ---- vkGetPhysicalDeviceFormatProperties ---- */

pub type PFN_fmtp = unsafe extern "C" fn(VkPhysicalDevice, VkFormat, *mut VkFormatProperties);

pub unsafe extern "C" fn shim_fmtp(
    physical_device: VkPhysicalDevice,
    format: VkFormat,
    p_properties: *mut VkFormatProperties,
) {
    ensure_fmtp_resolved(physical_device);
    let Some(real) = real_fmtp() else { return };
    real(physical_device, format, p_properties);
    fmtp_fix_depth(physical_device, format, p_properties);
}

/* ---- vkGetPhysicalDeviceFormatProperties2 ---- */

pub type PFN_fmtp2 =
    unsafe extern "C" fn(VkPhysicalDevice, VkFormat, *mut VkFormatProperties2);

pub unsafe extern "C" fn shim_fmtp2(
    physical_device: VkPhysicalDevice,
    format: VkFormat,
    p_properties: *mut VkFormatProperties2,
) {
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
}
