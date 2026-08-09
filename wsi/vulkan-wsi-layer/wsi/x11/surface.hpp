/*
 * Copyright (c) 2021 Arm Limited.
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

/** @file
 * @brief Definitions for a x11 WSI Surface
 */

#pragma once
#include <memory>
#include <vulkan/vk_icd.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/shm.h>
#include "wsi/surface.hpp"
#include "surface_properties.hpp"

namespace wsi
{
namespace x11
{

class surface : public wsi::surface
{
public:
   /**
    * @brief Initialize the WSI surface.
    *
    * @return true on success, false otherwise.
    */
   bool init();

   surface() = delete;
   struct init_parameters;

   surface(const init_parameters &);
   ~surface();

   wsi::surface_properties &get_properties() override;
   util::unique_ptr<swapchain_base> allocate_swapchain(layer::device_private_data &dev_data,
                                                       const VkAllocationCallbacks *allocator) override;
   static util::unique_ptr<surface> make_surface(const util::allocator &allocator, xcb_connection_t *conn,
                                                 xcb_window_t window);

   bool get_size_and_depth(uint32_t *width, uint32_t *height, int *depth);

   xcb_connection_t *get_connection()
   {
      return m_connection;
   }

   xcb_window_t get_window()
   {
      return m_window;
   };

   bool has_shm() const
   {
      return m_has_shm;
   }

   /** Get or create the shared Wayland bypass presenter.
    * The bypass persists across swapchain recreations to avoid
    * creating new Wayland surfaces (which causes window jumping). */
   std::shared_ptr<class wayland_bypass> get_or_create_bypass(uint32_t width, uint32_t height);

private:
   xcb_connection_t *m_connection;
   xcb_window_t m_window;
   /** True if m_connection was opened by this surface (and must be
    * disconnected on destruction).  When false, the connection was passed
    * in from winevulkan/winex11 and must NOT be closed here. */
   bool m_owns_connection = false;
   /** Surface properties specific to the X11 surface. */
   surface_properties properties;

   /** X11 extension capabilities */
   bool m_has_shm = false;

   /** Shared bypass presenter — persists across swapchain recreations. */
   std::shared_ptr<class wayland_bypass> m_bypass;
};

} /* namespace x11 */
} /* namespace wsi */
