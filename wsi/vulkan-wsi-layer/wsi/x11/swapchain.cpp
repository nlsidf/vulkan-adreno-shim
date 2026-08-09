/*
 * Copyright (c) 2017-2022 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file swapchain.cpp
 *
 * @brief Contains the implementation for a x11 swapchain.
 */

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <system_error>
#include <thread>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <util/timed_semaphore.hpp>
#include <vulkan/vulkan_core.h>

#include <xcb/dri3.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include "drm_display.hpp"
#include "swapchain.hpp"
#include "util/log.hpp"
#include "util/macros.hpp"
#include "wsi/external_memory.hpp"
#include "wsi/swapchain_base.hpp"
#include "wsi/extensions/present_id.hpp"
#include "shm_presenter.hpp"
#include "dri3_presenter.hpp"
#include "wayland_bypass.hpp"

namespace wsi
{
namespace x11
{

#define X11_SWAPCHAIN_MAX_PENDING_COMPLETIONS 128

swapchain::swapchain(layer::device_private_data &dev_data, const VkAllocationCallbacks *pAllocator,
                     surface &wsi_surface)
   : swapchain_base(dev_data, pAllocator)
   , m_connection(wsi_surface.get_connection())
   , m_window(wsi_surface.get_window())
   , m_wsi_surface(&wsi_surface)
   , m_wsi_allocator(nullptr)
   , m_image_creation_parameters({}, m_allocator, {}, {})
   , m_send_sbc(0)
   , m_thread_status_lock()
   , m_thread_status_cond()
{
   m_image_create_info.format = VK_FORMAT_UNDEFINED;
}

swapchain::~swapchain()
{
   {
      auto thread_status_lock = std::unique_lock<std::mutex>(m_thread_status_lock);
      m_present_event_thread_run = false;
      m_thread_status_cond.notify_all();
   }

   /* Always join — the thread may not have set m_present_event_thread_run
    * yet when we check it (race), so join unconditionally if started. */
   if (m_present_event_thread.joinable())
   {
      m_present_event_thread.join();
   }

   /* Free all deferred images before teardown. */
   for (int i = 0; i < BYPASS_DEFER_FRAMES; i++)
   {
      if (m_bypass_deferred[i] >= 0)
      {
         unpresent_image(m_bypass_deferred[i]);
         m_bypass_deferred[i] = -1;
      }
   }

   /* Wake the page_flip_thread immediately so teardown doesn't block
    * for the full 250ms semaphore timeout.  The thread checks
    * m_page_flip_thread_run after waking and will exit cleanly. */
   m_page_flip_thread_run = false;
   m_page_flip_semaphore.post();

   /* Call the base's teardown */
   teardown();
}

VkResult swapchain::init_platform(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info,
                                  bool &use_presentation_thread)
{
   UNUSED(device);
   /* Always use the presentation thread so that buffer-release waits
    * don't block the application's rendering loop.  The page flip
    * thread can block until the compositor releases a buffer without
    * stalling the app. */
   use_presentation_thread = true;
   m_device_data.instance_data.disp.GetPhysicalDeviceMemoryProperties2KHR(m_device_data.physical_device,
                                                                          &m_memory_props);
   if (m_wsi_surface == nullptr)
   {
      WSI_LOG_ERROR("X11 swapchain init_platform: m_wsi_surface is null");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   WSIALLOC_ASSERT_VERSION();
   if (wsialloc_new(&m_wsi_allocator) != WSIALLOC_ERROR_NONE)
   {
      WSI_LOG_ERROR("Failed to create wsi allocator.");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   uint32_t w = swapchain_create_info->imageExtent.width;
   uint32_t h = swapchain_create_info->imageExtent.height;

   /* Determine the preferred presenter for this app.
    *
    * Priority: 1) config file override  2) auto-detection  3) DRI3 default
    *
    * Zink/GL apps need Wayland bypass with deferred buffer release to avoid
    * FBO flicker.  Direct Vulkan apps use DRI3 for best performance and
    * window decorations under Xwayland. */
   enum class preferred_presenter { AUTO, BYPASS, DRI3, SHM };
   preferred_presenter preferred = preferred_presenter::AUTO;

   /* 1. Check config file override (process name → presenter). */
   {
      char proc_name[256] = {};
      FILE *f = fopen("/proc/self/comm", "r");
      if (f)
      {
         if (fgets(proc_name, sizeof(proc_name), f))
         {
            /* Strip trailing newline. */
            char *nl = strchr(proc_name, '\n');
            if (nl)
               *nl = '\0';
         }
         fclose(f);
      }

      if (proc_name[0])
      {
         /* Read config file: lines of "app_name presenter" (bypass/dri3/shm).
          * Lines starting with # are comments, empty lines are skipped. */
         static const char *config_paths[] = {
            "/etc/sky1/wsi-routing.conf",
            "/usr/share/cix-gpu/wsi-routing.conf",
            nullptr
         };
         for (const char **path = config_paths; *path; path++)
         {
            FILE *cfg = fopen(*path, "r");
            if (!cfg)
               continue;
            char line[512];
            while (fgets(line, sizeof(line), cfg))
            {
               if (line[0] == '#' || line[0] == '\n')
                  continue;
               char app[256], pres[64];
               if (sscanf(line, "%255s %63s", app, pres) == 2)
               {
                  if (strcmp(app, proc_name) == 0)
                  {
                     if (strcmp(pres, "bypass") == 0)
                        preferred = preferred_presenter::BYPASS;
                     else if (strcmp(pres, "dri3") == 0)
                        preferred = preferred_presenter::DRI3;
                     else if (strcmp(pres, "shm") == 0)
                        preferred = preferred_presenter::SHM;
                     WSI_LOG_INFO("x11 swapchain: config override '%s' → %s", proc_name, pres);
                     break;
                  }
               }
            }
            fclose(cfg);
            if (preferred != preferred_presenter::AUTO)
               break;
         }
      }
   }

   /* 2. Auto-detect Zink: check /proc/self/maps for zink_dri.so loaded.
    *    Also check MESA_LOADER_DRIVER_OVERRIDE=zink env var. */
   bool is_zink = false;
   if (preferred == preferred_presenter::AUTO)
   {
      const char *driver = getenv("MESA_LOADER_DRIVER_OVERRIDE");
      if (driver && strcmp(driver, "zink") == 0)
      {
         is_zink = true;
      }
      else
      {
         FILE *maps = fopen("/proc/self/maps", "r");
         if (maps)
         {
            char line[512];
            while (fgets(line, sizeof(line), maps))
            {
               if (strstr(line, "zink_dri.so"))
               {
                  is_zink = true;
                  break;
               }
            }
            fclose(maps);
         }
      }
      if (is_zink)
      {
         preferred = preferred_presenter::BYPASS;
         WSI_LOG_INFO("x11 swapchain: auto-detected Zink → bypass");
      }
      else
      {
         preferred = preferred_presenter::DRI3;
      }
   }

   /* 3. Try the preferred presenter, with fallback chain:
    *    bypass → DRI3 → SHM
    *    DRI3 → bypass → SHM */

   /* Try bypass (for Zink/GL or explicit config) */
   if (preferred == preferred_presenter::BYPASS)
   {
      try
      {
         auto *x11_surf = static_cast<x11::surface *>(m_wsi_surface);
         auto bypass = x11_surf->get_or_create_bypass(w, h);
         if (bypass)
         {
            m_wayland_bypass = bypass;
            m_presenter = presenter_type::WAYLAND_BYPASS;
            xcb_unmap_window(m_connection, m_window);
            xcb_flush(m_connection);
            WSI_LOG_INFO("x11 swapchain: bypass (%ux%u)", w, h);
         }
      }
      catch (const std::exception &e)
      {
         WSI_LOG_WARNING("x11 swapchain: Wayland bypass exception: %s", e.what());
      }
   }

   /* Try DRI3 (preferred for direct Vulkan, or fallback from bypass) */
   if (m_presenter == presenter_type::SHM && preferred != preferred_presenter::SHM)
   {
      try
      {
         auto dri3 = std::make_unique<dri3_presenter>();
         if (dri3->is_available(m_connection))
         {
            VkResult dri3_result = dri3->init(m_connection, m_window, m_wsi_surface);
            if (dri3_result == VK_SUCCESS)
            {
               m_dri3_presenter = std::move(dri3);
               m_presenter = presenter_type::DRI3;
               WSI_LOG_INFO("x11 swapchain: using DRI3 zero-copy presenter");
            }
            else
            {
               WSI_LOG_INFO("x11 swapchain: DRI3 init failed (%d)", dri3_result);
            }
         }
         else
         {
            WSI_LOG_INFO("x11 swapchain: DRI3 not available");
         }
      }
      catch (const std::exception &e)
      {
         WSI_LOG_WARNING("x11 swapchain: DRI3 exception: %s", e.what());
      }
   }

   /* Bypass fallback (if DRI3 failed and not already tried) */
   if (m_presenter == presenter_type::SHM && preferred != preferred_presenter::BYPASS
       && preferred != preferred_presenter::SHM)
   {
      try
      {
         auto *x11_surf = static_cast<x11::surface *>(m_wsi_surface);
         auto bypass = x11_surf->get_or_create_bypass(w, h);
         if (bypass)
         {
            m_wayland_bypass = bypass;
            m_presenter = presenter_type::WAYLAND_BYPASS;
            xcb_unmap_window(m_connection, m_window);
            xcb_flush(m_connection);
            WSI_LOG_INFO("x11 swapchain: bypass fallback (%ux%u)", w, h);
         }
      }
      catch (const std::exception &e)
      {
         WSI_LOG_WARNING("x11 swapchain: Wayland bypass exception: %s", e.what());
      }
   }

   /* 4. SHM fallback (always available, but CPU copy per frame) */
   if (m_presenter == presenter_type::SHM)
   {
      try
      {
         m_shm_presenter = std::make_unique<shm_presenter>();

         if (!m_shm_presenter->is_available(m_connection, m_wsi_surface))
         {
            WSI_LOG_ERROR("SHM presenter is not available");
            return VK_ERROR_INITIALIZATION_FAILED;
         }

         VkResult init_result = m_shm_presenter->init(m_connection, m_window, m_wsi_surface);
         if (init_result != VK_SUCCESS)
         {
            WSI_LOG_ERROR("Failed to initialize SHM presenter");
            return init_result;
         }
         WSI_LOG_INFO("x11 swapchain: using SHM fallback presenter");
      }
      catch (const std::exception &e)
      {
         WSI_LOG_ERROR("Exception creating SHM presentation strategy: %s", e.what());
         return VK_ERROR_INITIALIZATION_FAILED;
      }
   }

   /* Deferred release for zero-copy presenters: keeps a 2-frame delay before
    * returning buffers.  Both bypass and DRI3 present DMA-BUFs asynchronously —
    * the compositor/X server may still be reading the buffer when the call
    * returns.  Without the delay, the app renders into a buffer the server
    * is still scanning out, causing FBO flicker. */
   m_bypass_deferred_release = (m_presenter == presenter_type::WAYLAND_BYPASS ||
                                m_presenter == presenter_type::DRI3);

   /* Resolve X11 atoms for WM_PING response (DRI3 event drain needs these
    * to reply to the window manager's liveness checks). */
   if (m_presenter == presenter_type::DRI3)
   {
      auto setup = xcb_get_setup(m_connection);
      m_root_window = xcb_setup_roots_iterator(setup).data->root;

      auto wm_cookie = xcb_intern_atom(m_connection, 0, 12, "WM_PROTOCOLS");
      auto ping_cookie = xcb_intern_atom(m_connection, 0, 12, "_NET_WM_PING");
      auto *wm_reply = xcb_intern_atom_reply(m_connection, wm_cookie, nullptr);
      auto *ping_reply = xcb_intern_atom_reply(m_connection, ping_cookie, nullptr);
      if (wm_reply)
      {
         m_wm_protocols_atom = wm_reply->atom;
         free(wm_reply);
      }
      if (ping_reply)
      {
         m_net_wm_ping_atom = ping_reply->atom;
         free(ping_reply);
      }
   }

   try
   {
      m_present_event_thread = std::thread(&swapchain::present_event_thread, this);
   }
   catch (const std::system_error &)
   {
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   catch (const std::bad_alloc &)
   {
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   return VK_SUCCESS;
}

VkResult swapchain::get_surface_compatible_formats(const VkImageCreateInfo &info,
                                                   util::vector<wsialloc_format> &importable_formats,
                                                   util::vector<uint64_t> &exportable_modifers,
                                                   util::vector<VkDrmFormatModifierPropertiesEXT> &drm_format_props)
{
   TRY_LOG(util::get_drm_format_properties(m_device_data.physical_device, info.format, drm_format_props),
           "Failed to get format properties");

   auto &display = drm_display::get_display();
   if (!display.has_value())
   {
      WSI_LOG_ERROR("DRM display not available.");
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   for (const auto &prop : drm_format_props)
   {
      drm_format_pair drm_format{ util::drm::vk_to_drm_format(info.format), prop.drmFormatModifier };

      if (!display->is_format_supported(drm_format))
      {
         continue;
      }

      VkExternalImageFormatPropertiesKHR external_props = {};
      external_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES_KHR;

      VkImageFormatProperties2KHR format_props = {};
      format_props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2_KHR;
      format_props.pNext = &external_props;

      VkResult result = VK_SUCCESS;
      {
         VkPhysicalDeviceExternalImageFormatInfoKHR external_info = {};
         external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO_KHR;
         external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

         VkPhysicalDeviceImageDrmFormatModifierInfoEXT drm_mod_info = {};
         drm_mod_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
         drm_mod_info.pNext = &external_info;
         drm_mod_info.drmFormatModifier = prop.drmFormatModifier;
         drm_mod_info.sharingMode = info.sharingMode;
         drm_mod_info.queueFamilyIndexCount = info.queueFamilyIndexCount;
         drm_mod_info.pQueueFamilyIndices = info.pQueueFamilyIndices;

         VkPhysicalDeviceImageFormatInfo2KHR image_info = {};
         image_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2_KHR;
         image_info.pNext = &drm_mod_info;
         image_info.format = info.format;
         image_info.type = info.imageType;
         image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
         image_info.usage = info.usage;
         image_info.flags = info.flags;

#if WSI_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN
         VkImageCompressionControlEXT compression_control = {};
         compression_control.sType = VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT;
         compression_control.flags = m_image_compression_control_params.flags;
         compression_control.compressionControlPlaneCount =
            m_image_compression_control_params.compression_control_plane_count;
         compression_control.pFixedRateFlags = m_image_compression_control_params.fixed_rate_flags.data();

         if (m_device_data.is_swapchain_compression_control_enabled())
         {
            compression_control.pNext = image_info.pNext;
            image_info.pNext = &compression_control;
         }
#endif
         result = m_device_data.instance_data.disp.GetPhysicalDeviceImageFormatProperties2KHR(
            m_device_data.physical_device, &image_info, &format_props);
      }
      if (result != VK_SUCCESS)
      {
         continue;
      }
      if (format_props.imageFormatProperties.maxExtent.width < info.extent.width ||
          format_props.imageFormatProperties.maxExtent.height < info.extent.height ||
          format_props.imageFormatProperties.maxExtent.depth < info.extent.depth)
      {
         continue;
      }
      if (format_props.imageFormatProperties.maxMipLevels < info.mipLevels ||
          format_props.imageFormatProperties.maxArrayLayers < info.arrayLayers)
      {
         continue;
      }
      if ((format_props.imageFormatProperties.sampleCounts & info.samples) != info.samples)
      {
         continue;
      }

      if (external_props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT_KHR)
      {
         if (!exportable_modifers.try_push_back(drm_format.modifier))
         {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
      }

      if (external_props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT_KHR)
      {
         uint64_t flags =
            (prop.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_DISJOINT_BIT) ? 0 : WSIALLOC_FORMAT_NON_DISJOINT;
         wsialloc_format import_format{ drm_format.fourcc, drm_format.modifier, flags };
         if (!importable_formats.try_push_back(import_format))
         {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
      }
   }

   return VK_SUCCESS;
}

VkResult swapchain::allocate_wsialloc(VkImageCreateInfo &image_create_info, x11_image_data *image_data,
                                      util::vector<wsialloc_format> &importable_formats,
                                      wsialloc_format *allocated_format, bool avoid_allocation)
{
   bool is_protected_memory = (image_create_info.flags & VK_IMAGE_CREATE_PROTECTED_BIT) != 0;
   uint64_t allocation_flags = is_protected_memory ? WSIALLOC_ALLOCATE_PROTECTED : 0;
   if (avoid_allocation)
   {
      allocation_flags |= WSIALLOC_ALLOCATE_NO_MEMORY;
   }

#if WSI_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN
   if (m_image_compression_control_params.flags & VK_IMAGE_COMPRESSION_FIXED_RATE_EXPLICIT_EXT)
   {
      allocation_flags |= WSIALLOC_ALLOCATE_HIGHEST_FIXED_RATE_COMPRESSION;
   }
#endif

   wsialloc_allocate_info alloc_info = { importable_formats.data(), static_cast<unsigned>(importable_formats.size()),
                                         image_create_info.extent.width, image_create_info.extent.height,
                                         allocation_flags };

   wsialloc_allocate_result alloc_result = { {}, { 0 }, { 0 }, { -1 }, false };
   /* Clear buffer_fds and average_row_strides for error purposes */
   for (int i = 0; i < WSIALLOC_MAX_PLANES; ++i)
   {
      alloc_result.buffer_fds[i] = -1;
      alloc_result.average_row_strides[i] = -1;
   }
   const auto res = wsialloc_alloc(m_wsi_allocator, &alloc_info, &alloc_result);
   if (res != WSIALLOC_ERROR_NONE)
   {
      WSI_LOG_ERROR("Failed allocation of DMA Buffer. WSI error: %d", static_cast<int>(res));
      if (res == WSIALLOC_ERROR_NOT_SUPPORTED)
      {
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      }
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   *allocated_format = alloc_result.format;
   auto &external_memory = image_data->external_mem;
   external_memory.set_strides(alloc_result.average_row_strides);
   external_memory.set_buffer_fds(alloc_result.buffer_fds);
   external_memory.set_offsets(alloc_result.offsets);

   uint32_t num_planes = util::drm::drm_fourcc_format_get_num_planes(alloc_result.format.fourcc);

   if (!avoid_allocation)
   {
      uint32_t num_memory_planes = 0;

      for (uint32_t i = 0; i < num_planes; ++i)
      {
         auto it = std::find(std::begin(alloc_result.buffer_fds) + i + 1, std::end(alloc_result.buffer_fds),
                             alloc_result.buffer_fds[i]);
         if (it == std::end(alloc_result.buffer_fds))
         {
            num_memory_planes++;
         }
      }

      assert(alloc_result.is_disjoint == (num_memory_planes > 1));
      external_memory.set_num_memories(num_memory_planes);
   }

   external_memory.set_format_info(alloc_result.is_disjoint, num_planes);
   external_memory.set_memory_handle_type(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
   return VK_SUCCESS;
}

VkResult swapchain::allocate_image(VkImageCreateInfo &image_create_info, x11_image_data *image_data)
{
   UNUSED(image_create_info);
   util::vector<wsialloc_format> importable_formats(util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));
   auto &m_allocated_format = m_image_creation_parameters.m_allocated_format;
   if (!importable_formats.try_push_back(m_allocated_format))
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   TRY_LOG_CALL(allocate_wsialloc(m_image_create_info, image_data, importable_formats, &m_allocated_format, false));

   return VK_SUCCESS;
}

VkResult swapchain::allocate_and_bind_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image)
{
   std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);
   image.status = swapchain_image::FREE;

   assert(image.data != nullptr);
   auto image_data = static_cast<x11_image_data *>(image.data);

   image_status_lock.unlock();

   uint32_t width = image_create_info.extent.width;
   uint32_t height = image_create_info.extent.height;

   int depth = 24;
   uint32_t dummy_w, dummy_h;
   if (!m_wsi_surface->get_size_and_depth(&dummy_w, &dummy_h, &depth))
   {
      WSI_LOG_WARNING("Could not get surface depth, using default: %d", depth);
   }

   auto &fmt = m_image_creation_parameters.m_allocated_format;

   if (m_presenter == presenter_type::WAYLAND_BYPASS || m_presenter == presenter_type::DRI3)
   {
      /* Zero-copy path: real DMA-BUF allocation, then create presentation
       * resources from the fds BEFORE importing into Vulkan (import closes fds). */
      TRY_LOG_CALL(allocate_image(m_image_create_info, image_data));

      if (m_presenter == presenter_type::WAYLAND_BYPASS && m_wayland_bypass)
      {
         TRY_LOG(m_wayland_bypass->create_image_resources(image_data, width, height,
                                                           fmt.fourcc, fmt.modifier),
                 "Failed to create Wayland bypass image resources");
      }
      else if (m_dri3_presenter)
      {
         uint32_t stride = image_data->external_mem.get_strides()[0];
         TRY_LOG(m_dri3_presenter->create_image_resources(image_data, width, height, depth,
                                                           stride, fmt.fourcc, fmt.modifier),
                 "Failed to create DRI3 image resources");
      }

      TRY_LOG(image_data->external_mem.import_memory_and_bind_swapchain_image(image.image),
              "Failed to import memory and bind swapchain image");
   }
   else
   {
      TRY_LOG(m_shm_presenter->create_image_resources(image_data, width, height, depth),
              "Failed to create SHM presentation image resources");
   }

   /* Initialize presentation fence.
    *
    * sync_fd_fence_sync requires VK_KHR_external_fence_fd (SYNC_FD export),
    * which Adreno 540 does not provide.  Only the zero-copy presenters
    * (DRI3 / Wayland bypass) actually need an exportable fence; the SHM
    * presenter only waits on its own payload, so a plain core fence works. */
   std::optional<fence_sync> present_fence;
   if (m_presenter == presenter_type::WAYLAND_BYPASS || m_presenter == presenter_type::DRI3)
   {
      auto fd_fence = sync_fd_fence_sync::create(m_device_data);
      if (fd_fence.has_value())
      {
         present_fence = std::move(fd_fence.value());
      }
   }
   else
   {
      present_fence = fence_sync::create(m_device_data);
   }
   if (!present_fence.has_value())
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   image_data->present_fence = std::move(present_fence.value());

   return VK_SUCCESS;
}

VkResult swapchain::create_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image)
{
   /* Create image_data */
   auto image_data = m_allocator.create<x11_image_data>(1, m_device, m_allocator);
   if (image_data == nullptr)
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   image.data = image_data;
   image_data->device = m_device;
   image_data->device_data = &m_device_data;

   if (m_presenter == presenter_type::WAYLAND_BYPASS || m_presenter == presenter_type::DRI3)
   {
      /* Zero-copy path: allocate via wsialloc (DMA-BUF heaps) */
      if (m_image_create_info.format == VK_FORMAT_UNDEFINED)
      {
         /* First image: negotiate DRM format + modifier.
          * We query the Vulkan device directly for DRM format modifier support,
          * bypassing drm_display (which requires X11 DRM connectors we don't have). */
         util::vector<VkDrmFormatModifierPropertiesEXT> drm_format_props(
            util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));

         TRY_LOG(util::get_drm_format_properties(m_device_data.physical_device,
                                                   image_create_info.format, drm_format_props),
                 "Failed to get DRM format properties");

         /* Find a usable importable format — prefer LINEAR for maximum compatibility */
         util::vector<wsialloc_format> importable_formats(
            util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));

         uint32_t vk_fourcc = util::drm::vk_to_drm_format(image_create_info.format);

         for (const auto &prop : drm_format_props)
         {
            VkExternalImageFormatPropertiesKHR external_props = {};
            external_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES_KHR;

            VkImageFormatProperties2KHR format_props = {};
            format_props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2_KHR;
            format_props.pNext = &external_props;

            VkPhysicalDeviceExternalImageFormatInfoKHR external_info = {};
            external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO_KHR;
            external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

            VkPhysicalDeviceImageDrmFormatModifierInfoEXT drm_mod_info = {};
            drm_mod_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
            drm_mod_info.pNext = &external_info;
            drm_mod_info.drmFormatModifier = prop.drmFormatModifier;
            drm_mod_info.sharingMode = image_create_info.sharingMode;

            VkPhysicalDeviceImageFormatInfo2KHR image_info = {};
            image_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2_KHR;
            image_info.pNext = &drm_mod_info;
            image_info.format = image_create_info.format;
            image_info.type = image_create_info.imageType;
            image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            image_info.usage = image_create_info.usage;
            image_info.flags = image_create_info.flags;

            VkResult result = m_device_data.instance_data.disp.GetPhysicalDeviceImageFormatProperties2KHR(
               m_device_data.physical_device, &image_info, &format_props);
            if (result != VK_SUCCESS)
               continue;

            if (external_props.externalMemoryProperties.externalMemoryFeatures &
                VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT_KHR)
            {
               uint64_t flags = (prop.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_DISJOINT_BIT)
                                   ? 0
                                   : WSIALLOC_FORMAT_NON_DISJOINT;
               wsialloc_format import_format{ vk_fourcc, prop.drmFormatModifier, flags };
               if (!importable_formats.try_push_back(import_format))
                  return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
         }

         if (importable_formats.empty())
         {
            WSI_LOG_ERROR("No importable DMA-BUF formats found for bypass/DRI3.");
            return VK_ERROR_INITIALIZATION_FAILED;
         }

         wsialloc_format allocated_format = { 0, 0, 0 };
         TRY_LOG_CALL(allocate_wsialloc(image_create_info, image_data, importable_formats,
                                         &allocated_format, true));

         for (auto &prop : drm_format_props)
         {
            if (prop.drmFormatModifier == allocated_format.modifier)
            {
               image_data->external_mem.set_num_memories(prop.drmFormatModifierPlaneCount);
            }
         }

         /* Set up image create info for DRM format modifier tiling */
         TRY_LOG_CALL(image_data->external_mem.fill_image_plane_layouts(
            m_image_creation_parameters.m_image_layout));

         if (image_data->external_mem.is_disjoint())
            image_create_info.flags |= VK_IMAGE_CREATE_DISJOINT_BIT;

         image_data->external_mem.fill_drm_mod_info(
            image_create_info.pNext, m_image_creation_parameters.m_drm_mod_info,
            m_image_creation_parameters.m_image_layout, allocated_format.modifier);
         image_data->external_mem.fill_external_info(
            m_image_creation_parameters.m_external_info,
            &m_image_creation_parameters.m_drm_mod_info);
         image_create_info.pNext = &m_image_creation_parameters.m_external_info;
         image_create_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;

         m_image_create_info = image_create_info;
         m_image_creation_parameters.m_allocated_format = allocated_format;

         WSI_LOG_INFO("x11 swapchain: DMA-BUF format: fourcc=0x%x mod=0x%lx",
                      allocated_format.fourcc, (unsigned long)allocated_format.modifier);
      }

      return m_device_data.disp.CreateImage(m_device, &m_image_create_info,
                                             get_allocation_callbacks(), &image.image);
   }
   else if (m_shm_presenter)
   {
      /* SHM path: needs HOST_VISIBLE + LINEAR for CPU readback */
      VkMemoryPropertyFlags optimal = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                      VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

      TRY_LOG_CALL(image_data->external_mem.configure_for_host_visible(image_create_info, required, optimal));

      image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
      TRY_LOG(m_device_data.disp.CreateImage(m_device, &image_create_info, get_allocation_callbacks(), &image.image),
              "Failed to create image for SHM");

      return image_data->external_mem.allocate_and_bind_image(image.image, image_create_info);
   }
   else
   {
      return allocate_image(image_create_info, image_data);
   }
}

void swapchain::present_event_thread()
{
   auto thread_status_lock = std::unique_lock<std::mutex>(m_thread_status_lock);
   m_present_event_thread_run = true;

   while (m_present_event_thread_run)
   {
      if (error_has_occured())
      {
         break;
      }

      /* Bypass mode: dispatch Wayland events (pings, configures) but
       * buffer release is handled by the 1-frame delay in present_image,
       * so no buffer tracking needed here.  Just keep the connection alive. */
      if (m_presenter == presenter_type::WAYLAND_BYPASS && m_wayland_bypass)
      {
         thread_status_lock.unlock();

         /* Non-blocking dispatch to handle pings and other housekeeping. */
         std::vector<struct wl_buffer *> unused;
         m_wayland_bypass->dispatch_and_get_releases(unused);

         thread_status_lock.lock();
         m_thread_status_cond.wait_for(thread_status_lock, std::chrono::milliseconds(16));
         continue;
      }

      /* DRI3 mode: drain XCB events to prevent socket backpressure from
       * blocking xcb_present_pixmap (apps like FurMark that don't drain
       * their own event queue would stall on the first frame otherwise).
       *
       * We must respond to _NET_WM_PING ourselves since we're consuming
       * events that the app's event loop would normally handle — without
       * the pong reply, the window manager marks the app as unresponsive. */
      if (m_presenter == presenter_type::DRI3)
      {
         thread_status_lock.unlock();

         xcb_generic_event_t *event;
         while ((event = xcb_poll_for_event(m_connection)) != nullptr)
         {
            /* Respond to WM_PING to prevent "not responding" grey-out. */
            if ((event->response_type & 0x7f) == XCB_CLIENT_MESSAGE)
            {
               auto *cm = reinterpret_cast<xcb_client_message_event_t *>(event);
               if (cm->type == m_wm_protocols_atom && cm->data.data32[0] == m_net_wm_ping_atom
                   && m_net_wm_ping_atom != XCB_ATOM_NONE)
               {
                  /* Pong: send the same event back to the root window. */
                  cm->window = m_root_window;
                  xcb_send_event(m_connection, 0, m_root_window,
                                 XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                                 reinterpret_cast<const char *>(cm));
                  xcb_flush(m_connection);
               }
            }
            free(event);
         }

         thread_status_lock.lock();
         m_thread_status_cond.wait_for(thread_status_lock, std::chrono::milliseconds(4));
         continue;
      }

      /* SHM mode: original behavior — wait for pending completions. */
      auto assume_forward_progress = false;

      for (auto &image : m_swapchain_images)
      {
         if (image.status == swapchain_image::INVALID)
            continue;

         auto data = reinterpret_cast<x11_image_data *>(image.data);
         if (data->pending_completions.size() != 0)
         {
            assume_forward_progress = true;
            break;
         }
      }

      if (!assume_forward_progress)
      {
         m_thread_status_cond.wait(thread_status_lock);
         continue;
      }

      thread_status_lock.unlock();

      thread_status_lock.lock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }

   m_present_event_thread_run = false;
   m_thread_status_cond.notify_all();
}

void swapchain::present_image(const pending_present_request &pending_present)
{
   auto image_data = reinterpret_cast<x11_image_data *>(m_swapchain_images[pending_present.image_index].data);
   auto thread_status_lock = std::unique_lock<std::mutex>(m_thread_status_lock);

   while (image_data->pending_completions.size() == X11_SWAPCHAIN_MAX_PENDING_COMPLETIONS)
   {
      if (!m_present_event_thread_run)
      {
         if (m_device_data.is_present_id_enabled())
         {
            auto *ext = get_swapchain_extension<wsi_ext_present_id>(true);
            ext->set_present_id(pending_present.present_id);
         }
         return unpresent_image(pending_present.image_index);
      }
      m_thread_status_cond.wait(thread_status_lock);
   }

   m_send_sbc++;
   uint32_t serial = (uint32_t)m_send_sbc;

   VkResult present_result;
   if (m_presenter == presenter_type::WAYLAND_BYPASS && m_wayland_bypass)
   {
      thread_status_lock.unlock();
      present_result = m_wayland_bypass->present_image(image_data);
      thread_status_lock.lock();

      if (present_result == VK_SUCCESS)
      {
         if (m_bypass_deferred_release)
         {
            /* Deferred release (Zink/GL): keep a 2-frame delay before
             * freeing.  On present N, free image N-2.  This gives the
             * compositor 2 full frames to finish reading before the
             * app can reuse the buffer (prevents FBO flicker). */
            int oldest = m_bypass_deferred[m_bypass_defer_head];
            if (oldest >= 0)
               unpresent_image(oldest);
            m_bypass_deferred[m_bypass_defer_head] = pending_present.image_index;
            m_bypass_defer_head = (m_bypass_defer_head + 1) % BYPASS_DEFER_FRAMES;
         }
         else
         {
            /* Immediate release (direct Vulkan): free right away. */
            unpresent_image(pending_present.image_index);
         }
      }
      else
      {
         WSI_LOG_ERROR("Failed to present image using bypass: %d", present_result);
         unpresent_image(pending_present.image_index);
      }
   }
   else if (m_presenter == presenter_type::DRI3 && m_dri3_presenter)
   {
      present_result = m_dri3_presenter->present_image(image_data, serial);

      if (present_result == VK_SUCCESS)
      {
         if (m_bypass_deferred_release)
         {
            int oldest = m_bypass_deferred[m_bypass_defer_head];
            if (oldest >= 0)
               unpresent_image(oldest);
            m_bypass_deferred[m_bypass_defer_head] = pending_present.image_index;
            m_bypass_defer_head = (m_bypass_defer_head + 1) % BYPASS_DEFER_FRAMES;
         }
         else
         {
            unpresent_image(pending_present.image_index);
         }
      }
      else
      {
         WSI_LOG_ERROR("Failed to present image using DRI3: %d", present_result);
         unpresent_image(pending_present.image_index);
      }
   }
   else
   {
      present_result = m_shm_presenter->present_image(image_data, serial);

      if (present_result != VK_SUCCESS)
         WSI_LOG_ERROR("Failed to present image using SHM: %d", present_result);

      unpresent_image(pending_present.image_index);
   }

   if (m_device_data.is_present_id_enabled())
   {
      auto *ext = get_swapchain_extension<wsi_ext_present_id>(true);
      ext->set_present_id(pending_present.present_id);
   }

   m_thread_status_cond.notify_all();
   thread_status_lock.unlock();
}

bool swapchain::free_image_found()
{
   for (auto &img : m_swapchain_images)
   {
      if (img.status == swapchain_image::FREE)
      {
         return true;
      }
   }
   return false;
}

VkResult swapchain::get_free_buffer(uint64_t *timeout)
{
   auto thread_status_lock = std::unique_lock<std::mutex>(m_thread_status_lock);

   if (*timeout == 0)
   {
      return free_image_found() ? VK_SUCCESS : VK_NOT_READY;
   }
   else if (*timeout == UINT64_MAX)
   {
      while (!free_image_found())
      {
         if (!m_present_event_thread_run)
            return VK_ERROR_OUT_OF_DATE_KHR;

         m_thread_status_cond.wait(thread_status_lock);
      }
   }
   else
   {
      auto time_point = std::chrono::high_resolution_clock::now() + std::chrono::nanoseconds(*timeout);

      while (!free_image_found())
      {
         if (!m_present_event_thread_run)
            return VK_ERROR_OUT_OF_DATE_KHR;

         if (m_thread_status_cond.wait_until(thread_status_lock, time_point) == std::cv_status::timeout)
         {
            return VK_TIMEOUT;
         }
      }
   }

   *timeout = 0;
   return VK_SUCCESS;
}

void swapchain::destroy_image(wsi::swapchain_image &image)
{
   std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);
   if (image.status != wsi::swapchain_image::INVALID)
   {
      if (image.image != VK_NULL_HANDLE)
      {
         m_device_data.disp.DestroyImage(m_device, image.image, get_allocation_callbacks());
         image.image = VK_NULL_HANDLE;
      }

      image.status = wsi::swapchain_image::INVALID;
   }

   image_status_lock.unlock();

   if (image.data != nullptr)
   {
      auto data = reinterpret_cast<x11_image_data *>(image.data);

      if (data != nullptr)
      {
         if (m_presenter == presenter_type::WAYLAND_BYPASS && m_wayland_bypass)
         {
            m_wayland_bypass->destroy_image_resources(data);
         }
         else if (m_presenter == presenter_type::DRI3 && m_dri3_presenter)
         {
            m_dri3_presenter->destroy_image_resources(data);
         }
         else if (m_shm_presenter)
         {
            m_shm_presenter->destroy_image_resources(data);
         }
      }

      m_allocator.destroy(1, data);
      image.data = nullptr;
   }
}

VkResult swapchain::image_set_present_payload(swapchain_image &image, VkQueue queue,
                                              const queue_submit_semaphores &semaphores, const void *submission_pnext)
{
   auto data = reinterpret_cast<x11_image_data *>(image.data);
   return data->present_fence.set_payload(queue, semaphores, submission_pnext);
}

VkResult swapchain::image_wait_present(swapchain_image &image, uint64_t timeout)
{
   auto data = reinterpret_cast<x11_image_data *>(image.data);
   return data->present_fence.wait_payload(timeout);
}

VkResult swapchain::bind_swapchain_image(VkDevice &device, const VkBindImageMemoryInfo *bind_image_mem_info,
                                         const VkBindImageMemorySwapchainInfoKHR *bind_sc_info)
{
   UNUSED(device);
   const wsi::swapchain_image &swapchain_image = m_swapchain_images[bind_sc_info->imageIndex];
   auto image_data = reinterpret_cast<x11_image_data *>(swapchain_image.data);
   return image_data->external_mem.bind_swapchain_image_memory(bind_image_mem_info->image);
}

VkResult swapchain::add_required_extensions(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info)
{
   UNUSED(device);
   UNUSED(swapchain_create_info);

   if (m_device_data.is_present_id_enabled())
   {
      if (!add_swapchain_extension(m_allocator.make_unique<wsi_ext_present_id>()))
      {
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   return VK_SUCCESS;
}

} /* namespace x11 */
} /* namespace wsi */
