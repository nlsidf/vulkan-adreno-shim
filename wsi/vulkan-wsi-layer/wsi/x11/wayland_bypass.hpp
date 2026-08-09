/*
 * Copyright (c) 2025 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file wayland_bypass.hpp
 *
 * @brief Xwayland bypass presenter — zero-copy DMA-BUF presentation.
 *
 * When running under Xwayland, bypasses X11 entirely and presents
 * DMA-BUF buffers directly to the Wayland compositor via
 * zwp_linux_dmabuf_v1. Achieves zero-copy like the native Wayland path.
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>
#include <wayland-client.h>

/* Forward declarations for generated protocol types */
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct zwp_linux_dmabuf_v1;
struct zxdg_decoration_manager_v1;
struct zxdg_toplevel_decoration_v1;

namespace wsi
{
namespace x11
{

struct x11_image_data;

class wayland_bypass
{
public:
   wayland_bypass();
   ~wayland_bypass();

   /**
    * @brief Check if Xwayland bypass is available.
    *
    * Returns true if running under Xwayland (WAYLAND_DISPLAY is set)
    * and the Wayland compositor supports zwp_linux_dmabuf_v1.
    */
   bool is_available();

   /**
    * @brief Initialize the bypass presenter.
    *
    * Connects to the Wayland compositor, creates wl_surface + xdg_toplevel,
    * and negotiates DMA-BUF formats.
    */
   VkResult init(uint32_t width, uint32_t height);

   /**
    * @brief Create a wl_buffer from a DMA-BUF backed image.
    */
   VkResult create_image_resources(x11_image_data *image_data, uint32_t width, uint32_t height,
                                    uint32_t fourcc, uint64_t modifier);

   /**
    * @brief Present an image via the Wayland compositor.
    *
    * Thread-safe: protected by m_wl_mutex.
    */
   VkResult present_image(x11_image_data *image_data);

   /**
    * @brief Destroy Wayland resources for an image.
    */
   void destroy_image_resources(x11_image_data *image_data);

   /**
    * @brief Non-blocking dispatch of Wayland events and collection of released buffers.
    *
    * Reads any pending data from the compositor, dispatches events (including
    * wl_buffer.release), and returns the list of wl_buffer pointers that were
    * released since the last call.
    *
    * Thread-safe: protected by m_wl_mutex and m_release_mutex.
    *
    * @param[out] released  wl_buffer pointers released by the compositor.
    */
   void dispatch_and_get_releases(std::vector<struct wl_buffer *> &released);

   /* Wayland event handlers (called from static callbacks) */
   void handle_registry_global(uint32_t name, const char *interface, uint32_t version);
   void handle_xdg_wm_base_ping(uint32_t serial);
   void handle_xdg_surface_configure(uint32_t serial);
   void handle_xdg_toplevel_configure(int32_t width, int32_t height);
   void handle_xdg_toplevel_close();
   void handle_buffer_release(struct wl_buffer *buffer);

private:
   struct wl_display *m_wl_display = nullptr;
   struct wl_registry *m_wl_registry = nullptr;
   struct wl_compositor *m_wl_compositor = nullptr;
   struct wl_surface *m_wl_surface = nullptr;
   struct xdg_wm_base *m_xdg_wm_base = nullptr;
   struct xdg_surface *m_xdg_surface = nullptr;
   struct xdg_toplevel *m_xdg_toplevel = nullptr;
   struct zwp_linux_dmabuf_v1 *m_dmabuf = nullptr;
   struct zxdg_decoration_manager_v1 *m_decoration_manager = nullptr;
   struct zxdg_toplevel_decoration_v1 *m_toplevel_decoration = nullptr;

   bool m_configured = false;
   bool m_closed = false;
   uint32_t m_width = 0;
   uint32_t m_height = 0;

   /** Mutex protecting all Wayland display operations (commit, dispatch, flush). */
   std::mutex m_wl_mutex;

   /** Mutex protecting m_released_buffers. */
   std::mutex m_release_mutex;

   /** wl_buffer pointers released by the compositor, pending processing. */
   std::vector<struct wl_buffer *> m_released_buffers;
};

} /* namespace x11 */
} /* namespace wsi */
