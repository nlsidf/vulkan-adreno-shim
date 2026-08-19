//! 实例/设备创建拦截 + 设备函数分发 + 深度格式透明替换。

#![allow(dead_code)]

use crate::alloc;
use crate::driver::{to_void_fn, PFN_vkVoidFunction};
use crate::ffi::*;
use crate::memalias;
use crate::raw_test;
use core::ptr::null_mut;
use std::collections::HashMap;
use std::sync::{LazyLock, RwLock};

/* ---- 扩展白名单/黑名单 (同 C 版) ---- */

const WSI_KEEP_EXTS: [&str; 11] = [
    "VK_KHR_surface",
    "VK_KHR_swapchain",
    "VK_KHR_display",
    "VK_KHR_xcb_surface",
    "VK_KHR_xlib_surface",
    "VK_KHR_wayland_surface",
    "VK_KHR_get_surface_capabilities2",
    "VK_KHR_display_swapchain",
    "VK_ANDROID_native_buffer",
    "VK_ANDROID_external_memory_android_hardware_buffer",
    "VK_EXT_swapchain_maintenance1",
];

/// win32 平台扩展: Adreno HAL 枚举里偶有出现但 vkCreateDevice 会拒绝, 一律剥离。
const STRIP_BLACKLIST: [&str; 6] = [
    "VK_KHR_external_semaphore_win32",
    "VK_KHR_external_memory_win32",
    "VK_KHR_external_fence_win32",
    "VK_KHR_win32_keyed_mutex",
    "VK_KHR_win32_surface",
    "VK_EXT_external_memory_win32",
];

/// 低位 dmabuf 方案内部需要的扩展 (DXVK 不会主动开, shim 自己追加)。
/// 用 LazyLock 静态缓存, 消除每次 vkCreateDevice 的 CString 泄漏。
static NEED_EXTS_C: LazyLock<[std::ffi::CString; 4]> = LazyLock::new(|| {
    [
        std::ffi::CString::new("VK_KHR_external_memory").unwrap(),
        std::ffi::CString::new("VK_KHR_external_memory_fd").unwrap(),
        std::ffi::CString::new("VK_KHR_dedicated_allocation").unwrap(),
        std::ffi::CString::new("VK_KHR_get_memory_requirements2").unwrap(),
    ]
});

/// 应用提供的扩展/附件等数组的安全读取上限 (防恶意计数 OOM)。
const MAX_EXT_COUNT: usize = 256;
const MAX_ATTACH_COUNT: usize = 256;
const MAX_PNEXT_NODES: usize = 32;

/// pNext 精确手术: 重连链, 剔除 (1) 重复 sType 节点 (wine 注入的重复 Features2),
/// (2) pEnabledFeatures 已设时冗余的 VkPhysicalDeviceFeatures2。其余节点保留。
/// 返回新的链头。VK_ICD_STRIP_PNEXT=1 时整条剥离 (旧行为, 现场 A/B 用)。
unsafe fn fix_device_pnext(chain: *const c_void, has_enabled_features: bool) -> *const c_void {
    if crate::env_flag(c"VK_ICD_STRIP_PNEXT") {
        crate::shim_log!("VK_ICD_STRIP_PNEXT=1: 整条剥离 pNext");
        return null_mut();
    }
    if chain.is_null() {
        return null_mut();
    }
    let mut head: *mut VkBaseOutStructure = null_mut();
    let mut tail: *mut VkBaseOutStructure = null_mut();
    let mut seen: Vec<i32> = Vec::with_capacity(16);
    let mut it = chain as *mut VkBaseOutStructure;
    for _ in 0..MAX_PNEXT_NODES {
        if it.is_null() {
            break;
        }
        let node = &mut *it;
        let next = node.p_next;
        let keep = !seen.contains(&node.s_type)
            && !(node.s_type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2
                && has_enabled_features);
        if keep {
            seen.push(node.s_type);
            if tail.is_null() {
                head = it;
            } else {
                (*tail).p_next = it;
            }
            tail = it;
        }
        it = next;
    }
    if !tail.is_null() {
        (*tail).p_next = null_mut();
    }
    if head != (chain as *mut VkBaseOutStructure) {
        crate::shim_log!("pNext 手术: 剔除重复/冗余节点, 新链头 {:p}", head);
    }
    head as *const c_void
}

fn ext_in_keep(name: &str) -> bool {
    WSI_KEEP_EXTS.contains(&name)
}
fn ext_in_strip(name: &str) -> bool {
    STRIP_BLACKLIST.contains(&name)
}

/* ---- 函数指针类型 ---- */

pub type PFN_create_instance = unsafe extern "C" fn(
    *const VkInstanceCreateInfo,
    *const VkAllocationCallbacks,
    *mut VkInstance,
) -> i32;
pub type PFN_create_device = unsafe extern "C" fn(
    VkPhysicalDevice,
    *const VkDeviceCreateInfo,
    *const VkAllocationCallbacks,
    *mut VkDevice,
) -> i32;
pub type PFN_enum_pds =
    unsafe extern "C" fn(VkInstance, *mut u32, *mut VkPhysicalDevice) -> i32;
pub type PFN_get_mem_props =
    unsafe extern "C" fn(VkPhysicalDevice, *mut VkPhysicalDeviceMemoryProperties);
pub type PFN_get_dev_proc =
    unsafe extern "C" fn(VkDevice, *const core::ffi::c_char) -> PFN_vkVoidFunction;
pub type PFN_create_image = unsafe extern "C" fn(
    VkDevice,
    *const VkImageCreateInfo,
    *const VkAllocationCallbacks,
    *mut VkImage,
) -> i32;
pub type PFN_create_image_view = unsafe extern "C" fn(
    VkDevice,
    *const VkImageViewCreateInfo,
    *const VkAllocationCallbacks,
    *mut VkImageView,
) -> i32;
pub type PFN_create_renderpass = unsafe extern "C" fn(
    VkDevice,
    *const VkRenderPassCreateInfo,
    *const VkAllocationCallbacks,
    *mut VkRenderPass,
) -> i32;
pub type PFN_create_renderpass2 = unsafe extern "C" fn(
    VkDevice,
    *const VkRenderPassCreateInfo2,
    *const VkAllocationCallbacks,
    *mut VkRenderPass,
) -> i32;
pub type PFN_destroy_device =
    unsafe extern "C" fn(VkDevice, *const VkAllocationCallbacks);
pub type PFN_destroy_image =
    unsafe extern "C" fn(VkDevice, VkImage, *const VkAllocationCallbacks);
pub type PFN_cmd_clear_depth_stencil_image = unsafe extern "C" fn(
    VkCommandBuffer,
    VkImage,
    i32, /* VkImageLayout */
    *const VkClearDepthStencilValue,
    u32, /* VkImageSubresourceRange count */
    *const VkImageSubresourceRange,
);
pub type PFN_cmd_copy_image = unsafe extern "C" fn(
    VkCommandBuffer,
    VkImage,
    i32, /* VkImageLayout */
    VkImage,
    i32, /* VkImageLayout */
    u32, /* region count */
    *const VkImageCopy,
);
pub type PFN_cmd_blit_image = unsafe extern "C" fn(
    VkCommandBuffer,
    VkImage,
    i32, /* VkImageLayout */
    VkImage,
    i32, /* VkImageLayout */
    u32, /* region count */
    *const VkImageBlit,
    i32, /* VkFilter */
);
pub type PFN_cmd_resolve_image = unsafe extern "C" fn(
    VkCommandBuffer,
    VkImage,
    i32, /* VkImageLayout */
    VkImage,
    i32, /* VkImageLayout */
    u32, /* region count */
    *const VkImageResolve,
);

/* ---- vkCreateInstance ---- */

pub unsafe extern "C" fn shim_create_instance(
    p_create_info: *const VkInstanceCreateInfo,
    p_allocator: *const VkAllocationCallbacks,
    p_instance: *mut VkInstance,
) -> i32 {
    let Some(real) = crate::real_create_instance() else {
        crate::shim_log!("无法解析真实 vkCreateInstance");
        return VK_ERROR_INITIALIZATION_FAILED;
    };
    let alloc = alloc::icd_pick_alloc(p_allocator);
    let r = real(p_create_info, alloc, p_instance);
    if r == VK_SUCCESS && !p_instance.is_null() {
        let inst = *p_instance;
        crate::set_real_icd_instance(inst);
        crate::add_inst(inst);
        crate::shim_log!(
            "shim_vkCreateInstance -> 实例={:p} (pNext={:p}, 扩展数={}, alloc={:p})",
            inst,
            if p_create_info.is_null() {
                null_mut()
            } else {
                (*p_create_info).p_next
            },
            if p_create_info.is_null() {
                0
            } else {
                (*p_create_info).enabled_extension_count
            },
            p_allocator
        );
    }
    r
}

/* ---- vkEnumeratePhysicalDevices ---- */

pub unsafe extern "C" fn shim_enum_physical_devices(
    instance: VkInstance,
    p_physical_device_count: *mut u32,
    p_physical_devices: *mut VkPhysicalDevice,
) -> i32 {
    let Some(real) = crate::real_enum_pds(instance) else {
        return VK_ERROR_INITIALIZATION_FAILED;
    };
    let r = real(instance, p_physical_device_count, p_physical_devices);
    if r == VK_SUCCESS && !p_physical_devices.is_null() {
        let n = *p_physical_device_count;
        if n > 0 {
            crate::add_inst(instance);
            let list = core::slice::from_raw_parts(p_physical_devices, n as usize);
            let mut guard = crate::PDS.lock().unwrap();
            for pd in list {
                if !guard.iter().any(|(p, _)| *p == *pd) {
                    guard.push((*pd, instance));
                }
            }
        }
    }
    r
}

/* ---- vkCreateDevice ---- */

pub unsafe extern "C" fn shim_create_device(
    physical_device: VkPhysicalDevice,
    p_create_info: *const VkDeviceCreateInfo,
    p_allocator: *const VkAllocationCallbacks,
    p_device: *mut VkDevice,
) -> i32 {
    let Some(real) = crate::real_create_device() else {
        crate::shim_log!("无法解析真实 vkCreateDevice");
        return VK_ERROR_INITIALIZATION_FAILED;
    };
    let info = unsafe { &*p_create_info };

    if crate::env_flag(c"VK_ICD_DIAG") {
        crate::shim_log!(
            "diag: pd={:p} real_create={:p} sType={} flags=0x{:x} layerCount={}",
            physical_device,
            real,
            info.s_type,
            info.flags,
            info.enabled_layer_count
        );
    }

    /* 扩展过滤: 剥离 win32 平台扩展, 保留 WSI 扩展, 追加 external 家族。
     * 计数封顶, 名字做 UTF-8 安全转换, 防病态输入。 */
    let ext_count = (info.enabled_extension_count as usize).min(MAX_EXT_COUNT);
    let mut keep: Vec<*const core::ffi::c_char> = Vec::with_capacity(ext_count + NEED_EXTS_C.len());
    let names = core::slice::from_raw_parts(info.pp_enabled_extension_names, ext_count);
    for name in names {
        let s = core::ffi::CStr::from_ptr(*name).to_bytes();
        let s = core::str::from_utf8(s).unwrap_or("");
        if ext_in_keep(s) {
            keep.push(*name);
        } else if ext_in_strip(s) {
            crate::shim_log!("剥离 win32 平台扩展: {}", s);
        } else {
            keep.push(*name);
        }
    }
    /* 追加低位 dmabuf 方案所需的扩展 (静态缓存, 无泄漏; Adreno 540 已确认支持) */
    for c in NEED_EXTS_C.iter() {
        if !keep
            .iter()
            .any(|p| core::ffi::CStr::from_ptr(*p).to_bytes() == c.as_bytes())
        {
            keep.push(c.as_ptr());
            crate::shim_log!("追加扩展: {}", c.to_string_lossy());
        }
    }

    crate::cache_mem_props(physical_device);

    let mut ci = *info;
    ci.enabled_extension_count = keep.len() as u32;
    ci.pp_enabled_extension_names = keep.as_ptr();
    /* pNext 精确手术: 不再整条剥离。只剔除 (1) 重复 sType 节点 (wine 注入的
     * 重复 Features2 病灶), (2) pEnabledFeatures 已设时冗余的 Features2。
     * 其余合法 pNext (ycbcr 等扩展特性) 保留, 不隐藏能力。
     * VK_ICD_STRIP_PNEXT=1 可回退旧的全剥离行为。 */
    ci.p_next = fix_device_pnext(ci.p_next, !ci.p_enabled_features.is_null());

    if crate::env_flag(c"VK_ICD_DIAG") && !ci.p_enabled_features.is_null() {
        let feats = core::slice::from_raw_parts(ci.p_enabled_features as *const u32, 55);
        const FEAT_NAMES: [&str; 20] = [
            "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray",
            "independentBlend", "geometryShader", "tessellationShader",
            "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect",
            "drawIndirectFirstInstance", "depthClamp", "depthBiasClamp",
            "fillModeNonSolid", "depthBounds", "wideLines", "largePoints",
            "alphaToOne", "multiViewport", "samplerAnisotropy",
        ];
        for (i, name) in FEAT_NAMES.iter().enumerate() {
            crate::shim_log!("  features: {}={}", name, feats[i]);
        }
    }

    let dev_alloc = alloc::icd_pick_alloc(p_allocator);
    let r = real(physical_device, &ci, dev_alloc, p_device);
    crate::shim_log!(
        "real vkCreateDevice -> {} ({})",
        r,
        if r == VK_SUCCESS {
            "VK_SUCCESS"
        } else if r == VK_ERROR_EXTENSION_NOT_PRESENT {
            "EXTENSION_NOT_PRESENT"
        } else {
            "other"
        }
    );
    r
}

/* ---- 深度格式透明替换 D32_SFLOAT_S8_UINT -> D24_UNORM_S8_UINT ---- */

pub const DEPTH_D32S8: i32 = VK_FORMAT_D32_SFLOAT_S8_UINT;
pub const DEPTH_D24S8: i32 = VK_FORMAT_D24_UNORM_S8_UINT;

/// 命令层一致性表: 记录每个 VkImage 的 *实际* 格式 + 所属设备。
///
/// 在 shim_create_image 中, 若应用请求 D32S8, 我们透明替换为 D24S8 创建,
/// 并把 (image -> (DEPTH_D24S8, device)) 写入此表。命令层
/// (vkCmdClearDepthStencilImage 等) 据此判断图像的真实布局并解析真实函数。
/// VK_TEST_RAW 模式下不写入。
///
/// 用 RwLock: 读多写少 (命令执行是热路径, 只读), 创建/销毁时才写。
static IMAGE_FORMAT_MAP: LazyLock<RwLock<HashMap<VkImage, (VkFormat, VkDevice)>>> =
    LazyLock::new(|| RwLock::new(HashMap::new()));

#[inline]
fn image_real_entry(image: VkImage) -> Option<(VkFormat, VkDevice)> {
    IMAGE_FORMAT_MAP.read().ok()?.get(&image).copied()
}

#[inline]
fn image_is_substituted(image: VkImage) -> bool {
    image_real_entry(image).is_some_and(|(f, _)| f == DEPTH_D24S8)
}

pub unsafe extern "C" fn shim_create_image(
    device: VkDevice,
    p_create_info: *const VkImageCreateInfo,
    p_allocator: *const VkAllocationCallbacks,
    p_image: *mut VkImage,
) -> i32 {
    let Some(real) = crate::dev_fns_global(device).create_image else {
        return VK_ERROR_INITIALIZATION_FAILED;
    };
    let info = unsafe { &*p_create_info };
    let mut ci = *info;
    // 完美伪装: 应用请求 D32S8 时, 透明替换成硬件真正支持的 D24S8 创建。
    // 创建成功后把实际格式记入映射表, 供命令层 (清除/拷贝等) 判断真实布局。
    if !raw_test() && ci.format == DEPTH_D32S8 {
        ci.format = DEPTH_D24S8;
        crate::shim_dbg!(
            "D32S8->D24S8 image sub: type={} tiling={} usage=0x{:x} samples={}",
            ci.image_type,
            ci.tiling,
            ci.usage,
            ci.samples
        );
    }
    // 诊断: verbose>=2 时记录所有 image 创建格式, 看游戏真实用什么深度格式。
    if crate::verbose() >= 2 {
        crate::shim_dbg!(
            "vkCreateImage: fmt={} type={} tiling={} usage=0x{:x} samples={} (请求原 fmt={})",
            ci.format, ci.image_type, ci.tiling, ci.usage, ci.samples, info.format
        );
    }
    let r = real(device, &ci, p_allocator, p_image);
    if r == VK_SUCCESS && !p_image.is_null() {
        let image = *p_image;
        if ci.format == DEPTH_D24S8 {
            if let Ok(mut g) = IMAGE_FORMAT_MAP.write() {
                g.insert(image, (DEPTH_D24S8, device));
                crate::shim_dbg!("record image {:p} -> D24S8 (dev {:p})", image, device);
            }
        }
    }
    if r != VK_SUCCESS {
        crate::shim_log!(
            "vkCreateImage FAILED fmt={} type={} tiling={} usage=0x{:x} samples={} r={}",
            ci.format,
            ci.image_type,
            ci.tiling,
            ci.usage,
            ci.samples,
            r
        );
    }
    r
}

pub unsafe extern "C" fn shim_create_image_view(
    device: VkDevice,
    p_create_info: *const VkImageViewCreateInfo,
    p_allocator: *const VkAllocationCallbacks,
    p_view: *mut VkImageView,
) -> i32 {
    let Some(real) = crate::dev_fns_global(device).create_image_view else {
        return VK_ERROR_INITIALIZATION_FAILED;
    };
    let info = unsafe { &*p_create_info };
    let mut ci = *info;
    // 完美伪装: 视图格式同步替换, 与 image 保持一致。
    if !raw_test() && ci.format == DEPTH_D32S8 {
        ci.format = DEPTH_D24S8;
        crate::shim_dbg!("D32S8->D24S8 view sub");
    }
    real(device, &ci, p_allocator, p_view)
}

pub unsafe extern "C" fn shim_create_renderpass(
    device: VkDevice,
    p_create_info: *const VkRenderPassCreateInfo,
    p_allocator: *const VkAllocationCallbacks,
    p_render_pass: *mut VkRenderPass,
) -> i32 {
    let Some(real) = crate::dev_fns_global(device).create_renderpass else {
        return VK_ERROR_INITIALIZATION_FAILED;
    };
    let info = unsafe { &*p_create_info };
    let mut ci = *info;
    let atts: Vec<VkAttachmentDescription> =
        if !raw_test()
            && !info.p_attachments.is_null()
            && info.attachment_count > 0
        {
            let n = (info.attachment_count as usize).min(MAX_ATTACH_COUNT);
            let mut v = core::slice::from_raw_parts(info.p_attachments, n).to_vec();
            for a in v.iter_mut() {
                if a.format == DEPTH_D32S8 {
                    a.format = DEPTH_D24S8;
                }
            }
            ci.p_attachments = v.as_ptr();
            v
        } else {
            Vec::new()
        };
    let _ = atts;
    real(device, &ci, p_allocator, p_render_pass)
}

pub unsafe extern "C" fn shim_create_renderpass2(
    device: VkDevice,
    p_create_info: *const VkRenderPassCreateInfo2,
    p_allocator: *const VkAllocationCallbacks,
    p_render_pass: *mut VkRenderPass,
) -> i32 {
    let fns = crate::dev_fns_global(device);
    let Some(real) = fns
        .create_renderpass2
        .or(fns.create_renderpass2khr)
    else {
        return VK_ERROR_INITIALIZATION_FAILED;
    };
    let info = unsafe { &*p_create_info };
    let mut ci = *info;
    let atts: Vec<VkAttachmentDescription2> =
        if !raw_test()
            && !info.p_attachments.is_null()
            && info.attachment_count > 0
        {
            let n = (info.attachment_count as usize).min(MAX_ATTACH_COUNT);
            let mut v = core::slice::from_raw_parts(info.p_attachments, n).to_vec();
            for a in v.iter_mut() {
                if a.format == DEPTH_D32S8 {
                    a.format = DEPTH_D24S8;
                }
            }
            ci.p_attachments = v.as_ptr();
            v
        } else {
            Vec::new()
        };
    let _ = atts;
    real(device, &ci, p_allocator, p_render_pass)
}

/* ---- vkDestroyImage: 清理命令层映射表 ---- */

pub unsafe extern "C" fn shim_destroy_image(
    device: VkDevice,
    image: VkImage,
    p_allocator: *const VkAllocationCallbacks,
) {
    if !IMAGE_FORMAT_MAP.read().map_or(true, |m| m.is_empty()) {
        if let Ok(mut g) = IMAGE_FORMAT_MAP.write() {
            g.remove(&image);
        }
    }
    if let Some(real) = crate::dev_fns_global(device).destroy_image {
        real(device, image, p_allocator);
    }
}

/* ---- 命令层拦截 ---- */

/// 把 [0,1] 范围的 D32S8 浮点深度值 clamp 到合法区间。
/// 由于我们在创建层已将图像替换成 D24S8 (应用按 D32S8 语义提交), 但 Vulkan
/// 驱动按 D24S8 解析 VkClearDepthStencilValue.depth 字段时, 仍把它当 0..1 浮点
/// 处理 (D24 内部归一化), 因此只需 clamp 即可, 无需做 d32*d24max 整数换算。
#[inline]
fn clamp01(v: f32) -> f32 {
    v.clamp(0.0, 1.0)
}

pub unsafe extern "C" fn shim_cmd_clear_depth_stencil_image(
    command_buffer: VkCommandBuffer,
    image: VkImage,
    image_layout: i32,
    p_clear_value: *const VkClearDepthStencilValue,
    range_count: u32,
    p_ranges: *const VkImageSubresourceRange,
) {
    let Some(entry) = image_real_entry(image) else {
        return;
    };
    let (_, device) = entry;
    let Some(real) = crate::dev_fns_global(device).cmd_clear_depth_stencil_image else {
        return;
    };
    // 仅当图像被我们替换成 D24S8 时才需处理。
    if !raw_test() && image_is_substituted(image) && !p_clear_value.is_null() {
        let cv = &*p_clear_value;
        crate::shim_dbg!(
            "CmdClearDepthStencilImage: 图像={:p} 原 D32S8 clear depth={} stencil={}",
            image,
            cv.depth,
            cv.stencil
        );
        // depth 按 0..1 浮点 clamp 后透传 (驱动按 D24S8 解析即正确);
        // stencil 8-bit 直接透传。这里就地构造一份 clamp 后的副本。
        let fixed = VkClearDepthStencilValue {
            depth: clamp01(cv.depth),
            stencil: cv.stencil,
        };
        real(
            command_buffer,
            image,
            image_layout,
            &fixed,
            range_count,
            p_ranges,
        );
        return;
    }
    real(
        command_buffer,
        image,
        image_layout,
        p_clear_value,
        range_count,
        p_ranges,
    );
}

pub unsafe extern "C" fn shim_cmd_copy_image(
    command_buffer: VkCommandBuffer,
    src_image: VkImage,
    src_layout: i32,
    dst_image: VkImage,
    dst_layout: i32,
    region_count: u32,
    p_regions: *const VkImageCopy,
) {
    let Some(device) = image_real_entry(src_image).map(|(_, d)| d).or_else(|| {
        image_real_entry(dst_image).map(|(_, d)| d)
    }) else {
        return;
    };
    let Some(real) = crate::dev_fns_global(device).cmd_copy_image else {
        return;
    };
    // 创建层已统一替换: 所有图像的实际格式一致 (D24S8), 数据布局匹配,
    // 直接透传。仅防御性诊断记录。
    if !raw_test() && (image_is_substituted(src_image) || image_is_substituted(dst_image)) {
        crate::shim_dbg!(
            "CmdCopyImage: src={:p} dst={:p} (D24S8 同布局, 透传)",
            src_image,
            dst_image
        );
    }
    real(
        command_buffer,
        src_image,
        src_layout,
        dst_image,
        dst_layout,
        region_count,
        p_regions,
    );
}

pub unsafe extern "C" fn shim_cmd_blit_image(
    command_buffer: VkCommandBuffer,
    src_image: VkImage,
    src_layout: i32,
    dst_image: VkImage,
    dst_layout: i32,
    region_count: u32,
    p_regions: *const VkImageBlit,
    filter: i32,
) {
    let Some(device) = image_real_entry(src_image).map(|(_, d)| d).or_else(|| {
        image_real_entry(dst_image).map(|(_, d)| d)
    }) else {
        return;
    };
    let Some(real) = crate::dev_fns_global(device).cmd_blit_image else {
        return;
    };
    if !raw_test() && (image_is_substituted(src_image) || image_is_substituted(dst_image)) {
        crate::shim_dbg!(
            "CmdBlitImage: src={:p} dst={:p} (D24S8 同布局, 透传)",
            src_image,
            dst_image
        );
    }
    real(
        command_buffer,
        src_image,
        src_layout,
        dst_image,
        dst_layout,
        region_count,
        p_regions,
        filter,
    );
}

pub unsafe extern "C" fn shim_cmd_resolve_image(
    command_buffer: VkCommandBuffer,
    src_image: VkImage,
    src_layout: i32,
    dst_image: VkImage,
    dst_layout: i32,
    region_count: u32,
    p_regions: *const VkImageResolve,
) {
    let Some(device) = image_real_entry(src_image).map(|(_, d)| d).or_else(|| {
        image_real_entry(dst_image).map(|(_, d)| d)
    }) else {
        return;
    };
    let Some(real) = crate::dev_fns_global(device).cmd_resolve_image else {
        return;
    };
    if !raw_test() && (image_is_substituted(src_image) || image_is_substituted(dst_image)) {
        crate::shim_dbg!(
            "CmdResolveImage: src={:p} dst={:p} (D24S8 同布局, 透传)",
            src_image,
            dst_image
        );
    }
    real(
        command_buffer,
        src_image,
        src_layout,
        dst_image,
        dst_layout,
        region_count,
        p_regions,
    );
}

/* ---- vkGetDeviceProcAddr ---- */

pub unsafe extern "C" fn shim_get_device_proc_addr(
    device: VkDevice,
    p_name: *const core::ffi::c_char,
) -> PFN_vkVoidFunction {
    if p_name.is_null() {
        return None;
    }
    let get_dev = crate::real_get_dev_proc()?;
    let fns = crate::dev_fns_global(device);
    let name = core::ffi::CStr::from_ptr(p_name).to_bytes();

    if name == b"vkMapMemory" && fns.map_memory.is_some() {
        return to_void_fn(memalias_shim_map_memory as memalias::PFN_map);
    }
    if name == b"vkUnmapMemory" && fns.unmap_memory.is_some() {
        return to_void_fn(memalias_shim_unmap_memory as memalias::PFN_unmap);
    }
    if name == b"vkAllocateMemory" && fns.alloc_memory.is_some() {
        return to_void_fn(memalias_shim_allocate_memory as memalias::PFN_alloc_mem);
    }
    if name == b"vkFreeMemory" && fns.free_memory.is_some() {
        return to_void_fn(memalias_shim_free_memory as memalias::PFN_free);
    }
    if name == b"vkCreateImage" && fns.create_image.is_some() {
        return to_void_fn(shim_create_image as PFN_create_image);
    }
    if name == b"vkCreateImageView" && fns.create_image_view.is_some() {
        return to_void_fn(shim_create_image_view as PFN_create_image_view);
    }
    if name == b"vkCreateRenderPass" && fns.create_renderpass.is_some() {
        return to_void_fn(shim_create_renderpass as PFN_create_renderpass);
    }
    if (name == b"vkCreateRenderPass2" || name == b"vkCreateRenderPass2KHR")
        && (fns.create_renderpass2.is_some() || fns.create_renderpass2khr.is_some())
    {
        return to_void_fn(shim_create_renderpass2 as PFN_create_renderpass2);
    }
    if name == b"vkDestroyImage" && fns.destroy_image.is_some() {
        return to_void_fn(shim_destroy_image as PFN_destroy_image);
    }
    if name == b"vkCmdClearDepthStencilImage" && fns.cmd_clear_depth_stencil_image.is_some() {
        return to_void_fn(
            shim_cmd_clear_depth_stencil_image as PFN_cmd_clear_depth_stencil_image,
        );
    }
    if name == b"vkCmdCopyImage" && fns.cmd_copy_image.is_some() {
        return to_void_fn(shim_cmd_copy_image as PFN_cmd_copy_image);
    }
    if name == b"vkCmdBlitImage" && fns.cmd_blit_image.is_some() {
        return to_void_fn(shim_cmd_blit_image as PFN_cmd_blit_image);
    }
    if name == b"vkCmdResolveImage" && fns.cmd_resolve_image.is_some() {
        return to_void_fn(shim_cmd_resolve_image as PFN_cmd_resolve_image);
    }
    if name == b"vkDestroyDevice" && fns.destroy_device.is_some() {
        return to_void_fn(shim_destroy_device as PFN_destroy_device);
    }
    get_dev(device, p_name)
}

/* ---- vkDestroyDevice (清理 shim 自有的 fd/低位映射/靶子 buffer) ---- */

pub unsafe extern "C" fn shim_destroy_device(
    device: VkDevice,
    p_allocator: *const VkAllocationCallbacks,
) {
    /* 先释放 shim 记账的资源, 再销毁设备 (HAL 内存对象归应用管) */
    crate::memalias::cleanup_device(device);
    if let Some(real) = crate::dev_fns_global(device).destroy_device {
        real(device, p_allocator);
    }
}

use crate::memalias::{
    shim_allocate_memory as memalias_shim_allocate_memory,
    shim_free_memory as memalias_shim_free_memory,
    shim_map_memory as memalias_shim_map_memory,
    shim_unmap_memory as memalias_shim_unmap_memory,
};
