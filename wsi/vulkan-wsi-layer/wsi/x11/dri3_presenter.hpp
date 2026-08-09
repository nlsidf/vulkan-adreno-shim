/*
 * Copyright (c) 2025 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file dri3_presenter.hpp
 *
 * @brief DRI3/Present-based X11 presenter implementation.
 *
 * Zero-copy presentation via DRI3 pixmap_from_buffers + Present extension.
 * Requires a DRM render node (e.g. vgem) for GEM handle ↔ DMA-BUF conversion.
 */

#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>

namespace wsi
{
namespace x11
{

class surface;
struct x11_image_data;

class dri3_presenter
{
public:
   dri3_presenter();
   ~dri3_presenter();

   /**
    * @brief Check if DRI3 presentation is available.
    *
    * Checks for DRI3 + Present extensions and a usable render node.
    */
   bool is_available(xcb_connection_t *connection);

   /**
    * @brief Initialize the DRI3 presenter.
    *
    * Opens the render node via DRI3, queries Present extension.
    */
   VkResult init(xcb_connection_t *connection, xcb_window_t window, surface *wsi_surface);

   /**
    * @brief Create a DRI3 pixmap from DMA-BUF fds for an image.
    *
    * Imports the DMA-BUF fd into the render node as a GEM handle,
    * then creates an X11 pixmap via xcb_dri3_pixmap_from_buffers.
    *
    * @param image_data  Image data with external_mem containing DMA-BUF fds
    * @param width       Image width
    * @param height      Image height
    * @param depth       X11 visual depth (typically 24)
    * @param stride      Row stride in bytes
    * @param fourcc      DRM fourcc format code
    * @param modifier    DRM format modifier
    */
   VkResult create_image_resources(x11_image_data *image_data, uint32_t width, uint32_t height,
                                   int depth, uint32_t stride, uint32_t fourcc, uint64_t modifier);

   /**
    * @brief Present an image via the Present extension.
    *
    * Zero-copy: sends the pre-created pixmap to the X server.
    */
   VkResult present_image(x11_image_data *image_data, uint32_t serial);

   /**
    * @brief Destroy DRI3 resources for an image.
    */
   void destroy_image_resources(x11_image_data *image_data);

   /**
    * @brief Get the render node fd (for DRM format queries).
    */
   int get_render_node_fd() const { return m_render_node_fd; }

private:
   xcb_connection_t *m_connection = nullptr;
   xcb_window_t m_window = 0;
   surface *m_wsi_surface = nullptr;

   int m_render_node_fd = -1;
   uint32_t m_present_serial = 0;

   bool query_dri3_present(xcb_connection_t *connection);
   int open_render_node(xcb_connection_t *connection, xcb_window_t root);
};

} /* namespace x11 */
} /* namespace wsi */
