/*
 * wsialloc_stub.c — 空实现的外部 wsialloc allocator
 *
 * 目的：Sky1 vulkan-wsi-layer 的 X11 后端在 init_platform() 无条件调用
 * wsialloc_new()，失败即返回 VK_ERROR_INITIALIZATION_FAILED。
 * 本环境（Adreno 540 + 无 /dev/dma_heap、无 ion）不存在任何可用的
 * 系统 allocator，且 SHM presenter 路径从不调用 wsialloc_alloc()
 * （那是 DRI3 / Wayland-bypass 零拷贝路径才用的）。
 *
 * 因此这里提供一个最小空实现：new/delete 可行，alloc 永远
 * WSIALLOC_ERROR_NOT_SUPPORTED。只用于让 layer 编译并走 SHM 路径。
 */
#include "util/wsialloc/wsialloc.h"
#include <stdlib.h>

/* WSIALLOC_ASSERT_VERSION() 依赖此符号 */
const uint32_t WSIALLOC_IMPLEMENTATION_VERSION_SYMBOL = WSIALLOC_INTERFACE_VERSION;

wsialloc_error wsialloc_new(wsialloc_allocator **allocator)
{
   if (!allocator)
      return WSIALLOC_ERROR_INVALID;

   *allocator = (wsialloc_allocator *)malloc(1);
   if (!*allocator)
      return WSIALLOC_ERROR_NO_RESOURCE;

   return WSIALLOC_ERROR_NONE;
}

void wsialloc_delete(wsialloc_allocator *allocator)
{
   free(allocator);
}

wsialloc_error wsialloc_alloc(wsialloc_allocator *allocator,
                              const wsialloc_allocate_info *allocate_info,
                              wsialloc_allocate_result *result)
{
   /* SHM 路径不经过这里；真走到说明误用了 DRI3/Wayland 零拷贝路径 */
   (void)allocator;
   (void)allocate_info;
   (void)result;
   return WSIALLOC_ERROR_NOT_SUPPORTED;
}
