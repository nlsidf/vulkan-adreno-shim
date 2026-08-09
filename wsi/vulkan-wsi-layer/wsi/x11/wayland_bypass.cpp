/*
 * Copyright (c) 2025 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file wayland_bypass.cpp
 *
 * @brief Xwayland bypass presenter — zero-copy DMA-BUF presentation.
 *
 * When running under Xwayland, this presenter bypasses X11 entirely
 * and presents DMA-BUF buffers directly to the Wayland compositor
 * via zwp_linux_dmabuf_v1. This achieves the same zero-copy performance
 * as the native Wayland path.
 *
 * Buffer lifecycle:
 *   The compositor sends wl_buffer.release when it is done reading a
 *   buffer.  Released buffers are collected in m_released_buffers and
 *   returned to the swapchain via dispatch_and_get_releases().  The
 *   swapchain must NOT re-use a buffer until release has been received,
 *   otherwise the compositor may read an incomplete frame (flicker).
 */

#include "wayland_bypass.hpp"
#include "swapchain.hpp"
#include "util/log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

namespace wsi
{
namespace x11
{

/* --- Static Wayland callbacks --- */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
   auto *self = static_cast<wayland_bypass *>(data);
   self->handle_registry_global(name, interface, version);
   (void)registry;
}

static void registry_global_remove(void *, struct wl_registry *, uint32_t) {}

static const struct wl_registry_listener registry_listener = {
   registry_global,
   registry_global_remove,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
   auto *self = static_cast<wayland_bypass *>(data);
   self->handle_xdg_wm_base_ping(serial);
   (void)wm_base;
}

static const struct xdg_wm_base_listener wm_base_listener = {
   xdg_wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
   auto *self = static_cast<wayland_bypass *>(data);
   self->handle_xdg_surface_configure(serial);
   (void)surface;
}

static const struct xdg_surface_listener surface_listener = {
   xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                   int32_t width, int32_t height, struct wl_array *states)
{
   auto *self = static_cast<wayland_bypass *>(data);
   self->handle_xdg_toplevel_configure(width, height);
   (void)toplevel;
   (void)states;
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
   auto *self = static_cast<wayland_bypass *>(data);
   self->handle_xdg_toplevel_close();
   (void)toplevel;
}

static void xdg_toplevel_configure_bounds(void *, struct xdg_toplevel *, int32_t, int32_t) {}
static void xdg_toplevel_wm_capabilities(void *, struct xdg_toplevel *, struct wl_array *) {}

static const struct xdg_toplevel_listener toplevel_listener = {
   xdg_toplevel_configure,
   xdg_toplevel_close,
   xdg_toplevel_configure_bounds,
   xdg_toplevel_wm_capabilities,
};

static void buffer_release(void *data, struct wl_buffer *buffer)
{
   auto *self = static_cast<wayland_bypass *>(data);
   self->handle_buffer_release(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
   buffer_release,
};


/* --- wayland_bypass implementation --- */

wayland_bypass::wayland_bypass()
{
}

wayland_bypass::~wayland_bypass()
{
   if (m_toplevel_decoration)
      zxdg_toplevel_decoration_v1_destroy(m_toplevel_decoration);
   if (m_xdg_toplevel)
      xdg_toplevel_destroy(m_xdg_toplevel);
   if (m_xdg_surface)
      xdg_surface_destroy(m_xdg_surface);
   if (m_wl_surface)
      wl_surface_destroy(m_wl_surface);
   if (m_dmabuf)
      zwp_linux_dmabuf_v1_destroy(m_dmabuf);
   if (m_decoration_manager)
      zxdg_decoration_manager_v1_destroy(m_decoration_manager);
   if (m_xdg_wm_base)
      xdg_wm_base_destroy(m_xdg_wm_base);
   if (m_wl_compositor)
      wl_compositor_destroy(m_wl_compositor);
   if (m_wl_registry)
      wl_registry_destroy(m_wl_registry);
   if (m_wl_display)
   {
      wl_display_flush(m_wl_display);
      wl_display_disconnect(m_wl_display);
   }
}

bool wayland_bypass::is_available()
{
   /* Allow disabling bypass for testing/comparison */
   if (getenv("WSI_NO_WAYLAND_BYPASS"))
      return false;

   /* Try connecting to Wayland — first via WAYLAND_DISPLAY, then default socket.
    * Even if WAYLAND_DISPLAY is unset (app forced X11), we can still bypass
    * if a Wayland compositor is running. */
   struct wl_display *display = wl_display_connect(nullptr);
   if (!display)
   {
      /* Try the common default socket name */
      display = wl_display_connect("wayland-0");
   }
   if (!display)
   {
      return false;
   }
   wl_display_disconnect(display);
   return true;
}

void wayland_bypass::handle_registry_global(uint32_t name, const char *interface, uint32_t version)
{
   if (strcmp(interface, "wl_compositor") == 0)
   {
      m_wl_compositor = static_cast<struct wl_compositor *>(
         wl_registry_bind(m_wl_registry, name, &wl_compositor_interface, 4));
   }
   else if (strcmp(interface, "xdg_wm_base") == 0)
   {
      m_xdg_wm_base = static_cast<struct xdg_wm_base *>(
         wl_registry_bind(m_wl_registry, name, &xdg_wm_base_interface, 1));
      xdg_wm_base_add_listener(m_xdg_wm_base, &wm_base_listener, this);
   }
   else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0)
   {
      uint32_t bind_ver = version < 3 ? version : 3;
      m_dmabuf = static_cast<struct zwp_linux_dmabuf_v1 *>(
         wl_registry_bind(m_wl_registry, name, &zwp_linux_dmabuf_v1_interface, bind_ver));
   }
   else if (strcmp(interface, "zxdg_decoration_manager_v1") == 0)
   {
      m_decoration_manager = static_cast<struct zxdg_decoration_manager_v1 *>(
         wl_registry_bind(m_wl_registry, name, &zxdg_decoration_manager_v1_interface, 1));
   }
   (void)version;
}

void wayland_bypass::handle_xdg_wm_base_ping(uint32_t serial)
{
   xdg_wm_base_pong(m_xdg_wm_base, serial);
}

void wayland_bypass::handle_xdg_surface_configure(uint32_t serial)
{
   xdg_surface_ack_configure(m_xdg_surface, serial);
   m_configured = true;
}

void wayland_bypass::handle_xdg_toplevel_configure(int32_t width, int32_t height)
{
   if (width > 0 && height > 0)
   {
      m_width = width;
      m_height = height;
   }
}

void wayland_bypass::handle_xdg_toplevel_close()
{
   m_closed = true;
}

void wayland_bypass::handle_buffer_release(struct wl_buffer *buffer)
{
   /* Called from wl_display_dispatch_pending() context.
    * m_wl_mutex is held by the caller (dispatch_and_get_releases or init),
    * so we only need m_release_mutex here. */
   std::lock_guard<std::mutex> lock(m_release_mutex);
   m_released_buffers.push_back(buffer);
}

VkResult wayland_bypass::init(uint32_t width, uint32_t height)
{
   m_width = width;
   m_height = height;

   m_wl_display = wl_display_connect(nullptr);
   if (!m_wl_display)
   {
      m_wl_display = wl_display_connect("wayland-0");
   }
   if (!m_wl_display)
   {
      WSI_LOG_ERROR("wayland_bypass: failed to connect to Wayland compositor");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   m_wl_registry = wl_display_get_registry(m_wl_display);
   wl_registry_add_listener(m_wl_registry, &registry_listener, this);

   /* Round-trip to get globals */
   wl_display_roundtrip(m_wl_display);

   if (!m_wl_compositor)
   {
      WSI_LOG_ERROR("wayland_bypass: compositor not found");
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   if (!m_xdg_wm_base)
   {
      WSI_LOG_ERROR("wayland_bypass: xdg_wm_base not found");
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   if (!m_dmabuf)
   {
      WSI_LOG_ERROR("wayland_bypass: zwp_linux_dmabuf_v1 not found");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   /* Create surface + xdg_toplevel */
   m_wl_surface = wl_compositor_create_surface(m_wl_compositor);
   if (!m_wl_surface)
   {
      WSI_LOG_ERROR("wayland_bypass: failed to create wl_surface");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   m_xdg_surface = xdg_wm_base_get_xdg_surface(m_xdg_wm_base, m_wl_surface);
   xdg_surface_add_listener(m_xdg_surface, &surface_listener, this);

   m_xdg_toplevel = xdg_surface_get_toplevel(m_xdg_surface);
   xdg_toplevel_add_listener(m_xdg_toplevel, &toplevel_listener, this);
   xdg_toplevel_set_title(m_xdg_toplevel, "Vulkan (Xwayland bypass)");
   xdg_toplevel_set_app_id(m_xdg_toplevel, "vulkan-xwayland-bypass");

   /* Request server-side decorations (titlebar) */
   if (m_decoration_manager)
   {
      m_toplevel_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
         m_decoration_manager, m_xdg_toplevel);
      zxdg_toplevel_decoration_v1_set_mode(m_toplevel_decoration,
         ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
   }

   /* Commit to trigger configure */
   wl_surface_commit(m_wl_surface);
   wl_display_flush(m_wl_display);

   /* Wait for initial configure */
   while (!m_configured)
   {
      if (wl_display_dispatch(m_wl_display) < 0)
      {
         WSI_LOG_ERROR("wayland_bypass: display dispatch failed during configure");
         return VK_ERROR_INITIALIZATION_FAILED;
      }
   }

   /* Set the Wayland fd to non-blocking so that wl_display_read_events()
    * never blocks.  Without this, the event thread can block in read_events
    * while holding m_wl_mutex, preventing present_image from committing
    * new frames (causes slideshow-like stutter).  libwayland handles
    * EAGAIN gracefully — read_events returns 0 without reading. */
   int wl_fd = wl_display_get_fd(m_wl_display);
   int flags = fcntl(wl_fd, F_GETFL, 0);
   if (flags >= 0)
      fcntl(wl_fd, F_SETFL, flags | O_NONBLOCK);

   WSI_LOG_INFO("wayland_bypass: initialized (%ux%u), zero-copy DMA-BUF presentation",
                m_width, m_height);

   return VK_SUCCESS;
}

VkResult wayland_bypass::create_image_resources(x11_image_data *image_data, uint32_t width, uint32_t height,
                                                 uint32_t fourcc, uint64_t modifier)
{
   if (!m_dmabuf || !m_wl_display)
   {
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   int dma_buf_fd = image_data->external_mem.get_buffer_fds()[0];
   if (dma_buf_fd < 0)
   {
      WSI_LOG_ERROR("wayland_bypass: no DMA-BUF fd in image data");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   uint32_t stride = image_data->external_mem.get_strides()[0];
   uint32_t offset = image_data->external_mem.get_offsets()[0];
   uint32_t modifier_hi = modifier >> 32;
   uint32_t modifier_lo = modifier & 0xFFFFFFFF;

   /* Remap ARGB to XRGB to avoid alpha channel issues in compositing */
   if (fourcc == 0x34325241) /* DRM_FORMAT_ARGB8888 */
      fourcc = 0x34325258;   /* DRM_FORMAT_XRGB8888 */
   if (fourcc == 0x34324241) /* DRM_FORMAT_ABGR8888 */
      fourcc = 0x34324258;   /* DRM_FORMAT_XBGR8888 */

   struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(m_dmabuf);
   zwp_linux_buffer_params_v1_add(params, dma_buf_fd, 0, offset, stride, modifier_hi, modifier_lo);

   struct wl_buffer *buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, fourcc, 0);
   zwp_linux_buffer_params_v1_destroy(params);

   if (!buffer)
   {
      WSI_LOG_ERROR("wayland_bypass: failed to create wl_buffer from DMA-BUF");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   wl_buffer_add_listener(buffer, &buffer_listener, this);

   /* Store the wl_buffer pointer in the image data's cpu_buffer field (repurposed) */
   image_data->cpu_buffer = reinterpret_cast<void *>(buffer);
   image_data->width = width;
   image_data->height = height;

   WSI_LOG_INFO("wayland_bypass: created wl_buffer (%ux%u, fourcc=0x%x, mod=0x%lx)",
                width, height, fourcc, (unsigned long)modifier);

   return VK_SUCCESS;
}

VkResult wayland_bypass::present_image(x11_image_data *image_data)
{
   std::lock_guard<std::mutex> wl_lock(m_wl_mutex);

   if (!m_wl_surface || !m_wl_display)
   {
      return VK_ERROR_SURFACE_LOST_KHR;
   }

   struct wl_buffer *buffer = reinterpret_cast<struct wl_buffer *>(image_data->cpu_buffer);
   if (!buffer)
   {
      return VK_ERROR_SURFACE_LOST_KHR;
   }

   wl_surface_attach(m_wl_surface, buffer, 0, 0);
   wl_surface_damage_buffer(m_wl_surface, 0, 0, INT32_MAX, INT32_MAX);
   wl_surface_commit(m_wl_surface);

   int ret = wl_display_flush(m_wl_display);
   if (ret < 0)
   {
      WSI_LOG_ERROR("wayland_bypass: wl_display_flush failed: %s", strerror(errno));
      return VK_ERROR_SURFACE_LOST_KHR;
   }

   return VK_SUCCESS;
}

void wayland_bypass::dispatch_and_get_releases(std::vector<struct wl_buffer *> &released)
{
   {
      std::lock_guard<std::mutex> wl_lock(m_wl_mutex);
      if (m_wl_display)
      {
         /* Non-blocking read + dispatch:
          * 1. Dispatch any events already in the queue
          * 2. prepare_read acquires the internal read lock
          * 3. read_events does a non-blocking read from the socket
          * 4. Dispatch the newly read events */
         while (wl_display_prepare_read(m_wl_display) != 0)
            wl_display_dispatch_pending(m_wl_display);

         wl_display_read_events(m_wl_display);
         wl_display_dispatch_pending(m_wl_display);
      }
   }

   {
      std::lock_guard<std::mutex> rel_lock(m_release_mutex);
      released = std::move(m_released_buffers);
      m_released_buffers.clear();
   }
}

void wayland_bypass::destroy_image_resources(x11_image_data *image_data)
{
   struct wl_buffer *buffer = reinterpret_cast<struct wl_buffer *>(image_data->cpu_buffer);
   if (buffer)
   {
      std::lock_guard<std::mutex> wl_lock(m_wl_mutex);
      wl_buffer_destroy(buffer);
      image_data->cpu_buffer = nullptr;
   }
}

} /* namespace x11 */
} /* namespace wsi */
