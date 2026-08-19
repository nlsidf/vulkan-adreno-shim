//! 手写 Vulkan C ABI 类型 (与 vulkan_core.h 布局一致)。
//!
//! 只定义 shim 真正读写/复制的结构体, 全部 `#[repr(C)]`, 并在文件末尾用
//! 编译期断言锁住关键偏移与尺寸, 防止头文件漂移。不引第三方 crate。

#![allow(dead_code)]

pub use core::ffi::{c_char, c_void};

/* ---- 不透明句柄 (VK_DEFINE_HANDLE: 指向不透明 struct 的指针) ---- */

/// 64 位下 Vulkan 句柄都是指针宽度。包装成 `#[repr(transparent)]` 新类型,
/// 使 `static Mutex<HashMap<...>>` 等容器满足 Send/Sync (句柄本身只是不透明值)。
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
#[repr(transparent)]
pub struct Handle(pub *mut c_void);
unsafe impl Send for Handle {}
unsafe impl Sync for Handle {}
impl Handle {
    pub const fn null() -> Self {
        Handle(core::ptr::null_mut())
    }
    pub const fn is_null(&self) -> bool {
        self.0.is_null()
    }
}

impl core::fmt::Pointer for Handle {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        core::fmt::Pointer::fmt(&self.0, f)
    }
}

pub type VkInstance = Handle;
pub type VkPhysicalDevice = Handle;
pub type VkDevice = Handle;
pub type VkDeviceMemory = Handle;
pub type VkBuffer = Handle;
pub type VkImage = Handle;
pub type VkImageView = Handle;
pub type VkRenderPass = Handle;

/* ---- 基础类型 ---- */
pub type VkResult = i32;
pub type VkBool32 = u32;
pub type VkStructureType = i32;
pub type VkFormat = i32;
pub type VkSystemAllocationScope = i32;
pub type VkInternalAllocationType = i32;

/* ---- 常量 ---- */
pub const VK_SUCCESS: i32 = 0;
pub const VK_ERROR_OUT_OF_HOST_MEMORY: i32 = -1;
pub const VK_ERROR_OUT_OF_DEVICE_MEMORY: i32 = -2;
pub const VK_ERROR_INITIALIZATION_FAILED: i32 = -3;
pub const VK_ERROR_EXTENSION_NOT_PRESENT: i32 = -7;
pub const VK_ERROR_FORMAT_NOT_SUPPORTED: i32 = -11;
pub const VK_WHOLE_SIZE: u64 = u64::MAX;

/* VkStructureType (仅用到的; 数值取自本机 vulkan_core.h, 与该系统 loader/HAL ABI 一致) */
pub const VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO: i32 = 1;
pub const VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO: i32 = 2;
pub const VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO: i32 = 3;
pub const VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO: i32 = 5;
pub const VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO: i32 = 12;
pub const VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO: i32 = 14;
pub const VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO: i32 = 15;
pub const VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO: i32 = 38;
pub const VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2: i32 = 1000059002;
pub const VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR: i32 = 1000360000;
pub const VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO: i32 = 1000127001;
pub const VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO: i32 = 1000072000;
pub const VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO: i32 = 1000072002;
pub const VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO_NV: i32 = 1000056001;
pub const VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR: i32 = 1000074002;
pub const VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR: i32 = 1000074000;
pub const VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2: i32 = 1000109004;
pub const VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: i32 = 1000059000;

/* VkFormat (仅用到的深度/模板格式) */
pub const VK_FORMAT_D16_UNORM: i32 = 124;
pub const VK_FORMAT_X8_D24_UNORM_PACK32: i32 = 125;
pub const VK_FORMAT_D32_SFLOAT: i32 = 126;
pub const VK_FORMAT_S8_UINT: i32 = 127;
pub const VK_FORMAT_D16_UNORM_S8_UINT: i32 = 128;
pub const VK_FORMAT_D24_UNORM_S8_UINT: i32 = 129;
pub const VK_FORMAT_D32_SFLOAT_S8_UINT: i32 = 130;

/* VkImageUsageFlagBits */
pub const VK_IMAGE_USAGE_TRANSFER_SRC_BIT: u32 = 0x0000_0001;
pub const VK_IMAGE_USAGE_TRANSFER_DST_BIT: u32 = 0x0000_0002;
pub const VK_IMAGE_USAGE_SAMPLED_BIT: u32 = 0x0000_0004;
pub const VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT: u32 = 0x0000_0020;
pub const VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT: u32 = 0x0000_0080;

/* VkFormatFeatureFlagBits */
pub const VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT: u32 = 0x0000_0001;
pub const VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT: u32 = 0x0000_0200;

/* VkFormatFeatureFlagBits2 (VkFormatProperties3KHR, 64-bit) */
pub const VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT: u64 = 0x1;
pub const VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT: u64 = 0x200;

/* VkMemoryPropertyFlagBits */
pub const VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT: u32 = 0x0000_0002;

/* VkSampleCountFlagBits */
pub const VK_SAMPLE_COUNT_1_BIT: u32 = 0x1;
pub const VK_SAMPLE_COUNT_2_BIT: u32 = 0x2;
pub const VK_SAMPLE_COUNT_4_BIT: u32 = 0x4;

/* VkBufferUsageFlagBits */
pub const VK_BUFFER_USAGE_TRANSFER_SRC_BIT: u32 = 0x1;
pub const VK_BUFFER_USAGE_TRANSFER_DST_BIT: u32 = 0x2;
pub const VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT: u32 = 0x10;
pub const VK_BUFFER_USAGE_STORAGE_BUFFER_BIT: u32 = 0x20;
pub const VK_BUFFER_USAGE_INDEX_BUFFER_BIT: u32 = 0x40;
pub const VK_BUFFER_USAGE_VERTEX_BUFFER_BIT: u32 = 0x80;

/* VkSharingMode */
pub const VK_SHARING_MODE_EXCLUSIVE: i32 = 0;

/* VkExternalMemoryHandleTypeFlagBits */
pub const VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT: u32 = 0x1;

/* ---- pNext 基础结构 ---- */
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkBaseInStructure {
    pub s_type: VkStructureType,
    pub p_next: *const VkBaseInStructure,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkBaseOutStructure {
    pub s_type: VkStructureType,
    pub p_next: *mut VkBaseOutStructure,
}

/* ---- 实例 ---- */
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkApplicationInfo {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub p_application_name: *const c_char,
    pub application_version: u32,
    pub p_engine_name: *const c_char,
    pub engine_version: u32,
    pub api_version: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkInstanceCreateInfo {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub flags: u32,
    pub p_application_info: *const VkApplicationInfo,
    pub enabled_layer_count: u32,
    pub pp_enabled_layer_names: *const *const c_char,
    pub enabled_extension_count: u32,
    pub pp_enabled_extension_names: *const *const c_char,
}

/* ---- 分配回调 ---- */
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkAllocationCallbacks {
    pub p_user_data: *mut c_void,
    pub pfn_allocation:
        Option<unsafe extern "C" fn(*mut c_void, usize, usize, VkSystemAllocationScope) -> *mut c_void>,
    pub pfn_reallocation: Option<
        unsafe extern "C" fn(*mut c_void, *mut c_void, usize, usize, VkSystemAllocationScope)
            -> *mut c_void,
    >,
    pub pfn_free: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    pub pfn_internal_allocation: Option<
        unsafe extern "C" fn(*mut c_void, usize, VkInternalAllocationType, VkSystemAllocationScope),
    >,
    pub pfn_internal_free: Option<
        unsafe extern "C" fn(*mut c_void, usize, VkInternalAllocationType, VkSystemAllocationScope),
    >,
}
// 该结构只作值传递/只读转发; p_user_data 在本 shim 中恒为 NULL, 回调线程无关。
unsafe impl Send for VkAllocationCallbacks {}
unsafe impl Sync for VkAllocationCallbacks {}

/* ---- 设备 ---- */
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkDeviceQueueCreateInfo {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub flags: u32,
    pub queue_family_index: u32,
    pub queue_count: u32,
    pub p_queue_priorities: *const f32,
}

/// VkPhysicalDeviceFeatures — 56 个 VkBool32, 全字段按头文件顺序。
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkPhysicalDeviceFeatures {
    pub robust_buffer_access: VkBool32,
    pub full_draw_index_uint32: VkBool32,
    pub image_cube_array: VkBool32,
    pub independent_blend: VkBool32,
    pub geometry_shader: VkBool32,
    pub tessellation_shader: VkBool32,
    pub sample_rate_shading: VkBool32,
    pub dual_src_blend: VkBool32,
    pub logic_op: VkBool32,
    pub multi_draw_indirect: VkBool32,
    pub draw_indirect_first_instance: VkBool32,
    pub depth_clamp: VkBool32,
    pub depth_bias_clamp: VkBool32,
    pub fill_mode_non_solid: VkBool32,
    pub depth_bounds: VkBool32,
    pub wide_lines: VkBool32,
    pub large_points: VkBool32,
    pub alpha_to_one: VkBool32,
    pub multi_viewport: VkBool32,
    pub sampler_anisotropy: VkBool32,
    pub texture_compression_etc2: VkBool32,
    pub texture_compression_astc_ldr: VkBool32,
    pub texture_compression_bc: VkBool32,
    pub occlusion_query_precise: VkBool32,
    pub pipeline_statistics_query: VkBool32,
    pub vertex_pipeline_stores_and_atomics: VkBool32,
    pub fragment_stores_and_atomics: VkBool32,
    pub shader_tessellation_and_geometry_point_size: VkBool32,
    pub shader_image_gather_extended: VkBool32,
    pub shader_storage_image_extended_formats: VkBool32,
    pub shader_storage_image_multisample: VkBool32,
    pub shader_storage_image_read_without_format: VkBool32,
    pub shader_storage_image_write_without_format: VkBool32,
    pub shader_uniform_buffer_array_dynamic_indexing: VkBool32,
    pub shader_sampled_image_array_dynamic_indexing: VkBool32,
    pub shader_storage_buffer_array_dynamic_indexing: VkBool32,
    pub shader_storage_image_array_dynamic_indexing: VkBool32,
    pub shader_clip_distance: VkBool32,
    pub shader_cull_distance: VkBool32,
    pub shader_float64: VkBool32,
    pub shader_int64: VkBool32,
    pub shader_int16: VkBool32,
    pub shader_residency: VkBool32,
    pub shader_resource_min_lod: VkBool32,
    pub sparse_binding: VkBool32,
    pub sparse_residency_buffer: VkBool32,
    pub sparse_residency_image_2d: VkBool32,
    pub sparse_residency_image_3d: VkBool32,
    pub sparse_residency_2_samples: VkBool32,
    pub sparse_residency_4_samples: VkBool32,
    pub sparse_residency_8_samples: VkBool32,
    pub sparse_residency_16_samples: VkBool32,
    pub sparse_residency_aliased: VkBool32,
    pub variable_multisample_rate: VkBool32,
    pub inherited_queries: VkBool32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkDeviceCreateInfo {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub flags: u32,
    pub queue_create_info_count: u32,
    pub p_queue_create_infos: *const VkDeviceQueueCreateInfo,
    pub enabled_layer_count: u32,
    pub pp_enabled_layer_names: *const *const c_char,
    pub enabled_extension_count: u32,
    pub pp_enabled_extension_names: *const *const c_char,
    pub p_enabled_features: *const VkPhysicalDeviceFeatures,
}

/* ---- 物理设备属性 ---- */
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkExtent3D {
    pub width: u32,
    pub height: u32,
    pub depth: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkImageFormatProperties {
    pub max_extent: VkExtent3D,
    pub max_mip_levels: u32,
    pub max_array_layers: u32,
    pub sample_counts: u32,
    pub max_resource_size: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkFormatProperties {
    pub linear_tiling_features: u32,
    pub optimal_tiling_features: u32,
    pub buffer_features: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkFormatProperties2 {
    pub s_type: VkStructureType,
    pub p_next: *mut c_void,
    pub format_properties: VkFormatProperties,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkFormatProperties3KHR {
    pub s_type: VkStructureType,
    pub p_next: *mut c_void,
    pub linear_tiling_features: u64,
    pub optimal_tiling_features: u64,
    pub buffer_features: u64,
}

pub const VK_MAX_MEMORY_TYPES: usize = 32;
pub const VK_MAX_MEMORY_HEAPS: usize = 16;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkMemoryType {
    pub property_flags: u32,
    pub heap_index: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkMemoryHeap {
    pub size: u64,
    pub flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkPhysicalDeviceMemoryProperties {
    pub memory_type_count: u32,
    pub memory_types: [VkMemoryType; VK_MAX_MEMORY_TYPES],
    pub memory_heap_count: u32,
    pub memory_heaps: [VkMemoryHeap; VK_MAX_MEMORY_HEAPS],
}

/* ---- 内存 ---- */
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkMemoryAllocateInfo {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub allocation_size: u64,
    pub memory_type_index: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkMemoryRequirements {
    pub size: u64,
    pub alignment: u64,
    pub memory_type_bits: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkMemoryDedicatedAllocateInfo {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub image: VkImage,
    pub buffer: VkBuffer,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkExportMemoryAllocateInfo {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub handle_types: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkMemoryGetFdInfoKHR {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub memory: VkDeviceMemory,
    pub handle_type: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkExternalMemoryBufferCreateInfo {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub handle_types: u32,
}

/* ---- buffer / image ---- */
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkBufferCreateInfo {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub flags: u32,
    pub size: u64,
    pub usage: u32,
    pub sharing_mode: i32,
    pub queue_family_index_count: u32,
    pub p_queue_family_indices: *const u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkImageCreateInfo {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub flags: u32,
    pub image_type: i32,
    pub format: VkFormat,
    pub extent: VkExtent3D,
    pub mip_levels: u32,
    pub array_layers: u32,
    pub samples: u32,
    pub tiling: i32,
    pub usage: u32,
    pub sharing_mode: i32,
    pub queue_family_index_count: u32,
    pub p_queue_family_indices: *const u32,
    pub initial_layout: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkComponentMapping {
    pub r: i32,
    pub g: i32,
    pub b: i32,
    pub a: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkImageSubresourceRange {
    pub aspect_mask: u32,
    pub base_mip_level: u32,
    pub level_count: u32,
    pub base_array_layer: u32,
    pub layer_count: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkImageViewCreateInfo {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub flags: u32,
    pub image: VkImage,
    pub view_type: i32,
    pub format: VkFormat,
    pub components: VkComponentMapping,
    pub subresource_range: VkImageSubresourceRange,
}

/* ---- render pass ---- */
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkAttachmentDescription {
    pub flags: u32,
    pub format: VkFormat,
    pub samples: u32,
    pub load_op: i32,
    pub store_op: i32,
    pub stencil_load_op: i32,
    pub stencil_store_op: i32,
    pub initial_layout: i32,
    pub final_layout: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkAttachmentReference {
    pub attachment: u32,
    pub layout: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkSubpassDescription {
    pub flags: u32,
    pub pipeline_bind_point: i32,
    pub input_attachment_count: u32,
    pub p_input_attachments: *const VkAttachmentReference,
    pub color_attachment_count: u32,
    pub p_color_attachments: *const VkAttachmentReference,
    pub p_resolve_attachments: *const VkAttachmentReference,
    pub p_depth_stencil_attachment: *const VkAttachmentReference,
    pub preserve_attachment_count: u32,
    pub p_preserve_attachments: *const u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkSubpassDependency {
    pub src_subpass: u32,
    pub dst_subpass: u32,
    pub src_stage_mask: u32,
    pub dst_stage_mask: u32,
    pub src_access_mask: u32,
    pub dst_access_mask: u32,
    pub dependency_flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkRenderPassCreateInfo {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub flags: u32,
    pub attachment_count: u32,
    pub p_attachments: *const VkAttachmentDescription,
    pub subpass_count: u32,
    pub p_subpasses: *const VkSubpassDescription,
    pub dependency_count: u32,
    pub p_dependencies: *const VkSubpassDependency,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkAttachmentDescription2 {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub flags: u32,
    pub format: VkFormat,
    pub samples: u32,
    pub load_op: i32,
    pub store_op: i32,
    pub stencil_load_op: i32,
    pub stencil_store_op: i32,
    pub initial_layout: i32,
    pub final_layout: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkAttachmentReference2 {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub attachment: u32,
    pub layout: i32,
    pub aspect_mask: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkSubpassDescription2 {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub flags: u32,
    pub pipeline_bind_point: i32,
    pub view_mask: u32,
    pub input_attachment_count: u32,
    pub p_input_attachments: *const VkAttachmentReference2,
    pub color_attachment_count: u32,
    pub p_color_attachments: *const VkAttachmentReference2,
    pub p_resolve_attachments: *const VkAttachmentReference2,
    pub p_depth_stencil_attachment: *const VkAttachmentReference2,
    pub preserve_attachment_count: u32,
    pub p_preserve_attachments: *const u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkSubpassDependency2 {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub src_subpass: u32,
    pub dst_subpass: u32,
    pub src_stage_mask: u32,
    pub dst_stage_mask: u32,
    pub src_access_mask: u32,
    pub dst_access_mask: u32,
    pub dependency_flags: u32,
    pub view_offset: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkRenderPassCreateInfo2 {
    pub s_type: VkStructureType,
    pub p_next: *const c_void,
    pub flags: u32,
    pub attachment_count: u32,
    pub p_attachments: *const VkAttachmentDescription2,
    pub subpass_count: u32,
    pub p_subpasses: *const VkSubpassDescription2,
    pub dependency_count: u32,
    pub p_dependencies: *const VkSubpassDependency2,
    pub correlated_view_mask_count: u32,
    pub p_correlated_view_masks: *const u32,
}

/* ---- 编译期布局断言 (与 vulkan_core.h 对齐) ---- */
const _: () = {
    use core::mem::{offset_of, size_of};

    assert!(size_of::<VkAllocationCallbacks>() == 48);
    assert!(offset_of!(VkAllocationCallbacks, pfn_allocation) == 8);
    assert!(offset_of!(VkAllocationCallbacks, pfn_free) == 24);

    assert!(size_of::<VkPhysicalDeviceFeatures>() == 55 * 4);

    assert!(size_of::<VkImageFormatProperties>() == 32);
    assert!(size_of::<VkFormatProperties>() == 12);
    assert!(size_of::<VkFormatProperties2>() == 32);
    assert!(size_of::<VkFormatProperties3KHR>() == 40);

    assert!(size_of::<VkPhysicalDeviceMemoryProperties>() == 520);
    assert!(offset_of!(VkPhysicalDeviceMemoryProperties, memory_types) == 4);
    assert!(offset_of!(VkPhysicalDeviceMemoryProperties, memory_heaps) == 264);

    assert!(size_of::<VkMemoryAllocateInfo>() == 32);
    assert!(size_of::<VkBufferCreateInfo>() == 56);
    assert!(size_of::<VkImageCreateInfo>() == 88);
    assert!(size_of::<VkImageViewCreateInfo>() == 80);
    assert!(size_of::<VkDeviceCreateInfo>() == 72);
    assert!(size_of::<VkMemoryRequirements>() == 24);

    // 深度格式替换依赖这两个 format 字段的偏移 (rp_fix_attachments 用 offsetof)
    assert!(offset_of!(VkAttachmentDescription, format) == 4);
    assert!(offset_of!(VkAttachmentDescription2, format) == 20);
    assert!(size_of::<VkAttachmentDescription>() == 36);
    assert!(size_of::<VkAttachmentDescription2>() == 56);
    assert!(size_of::<VkRenderPassCreateInfo>() == 64);
    assert!(size_of::<VkRenderPassCreateInfo2>() == 80);
};
