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
 * @brief Implementation of a x11 WSI Surface
 */

#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/shm.h>
#include "surface.hpp"
#include "swapchain.hpp"
#include "surface_properties.hpp"
#include "wayland_bypass.hpp"

namespace wsi
{
namespace x11
{

struct surface::init_parameters
{
   const util::allocator &allocator;
   xcb_connection_t *connection;
   xcb_window_t window;
};

surface::surface(const init_parameters &params)
   : wsi::surface()
   , m_connection(params.connection)
   , m_window(params.window)
   , properties(this, params.allocator)
{
}

surface::~surface()
{
   /* If we opened our own connection, tear it down.  Do NOT touch a
    * connection passed in from winevulkan/winex11 — it is shared with
    * wine and closing it would kill the game's X client. */
   if (m_owns_connection && m_connection)
   {
      xcb_disconnect(m_connection);
      m_connection = nullptr;
   }
}

bool surface::init()
{
   auto shm_cookie = xcb_shm_query_version_unchecked(m_connection);
   auto shm_reply = xcb_shm_query_version_reply(m_connection, shm_cookie, nullptr);

   m_has_shm = shm_reply != nullptr;
   free(shm_reply);
   return true;
}

bool surface::get_size_and_depth(uint32_t *width, uint32_t *height, int *depth)
{
   auto cookie = xcb_get_geometry(m_connection, m_window);
   if (auto *geom = xcb_get_geometry_reply(m_connection, cookie, nullptr))
   {
      *width = static_cast<uint32_t>(geom->width);
      *height = static_cast<uint32_t>(geom->height);
      *depth = static_cast<int>(geom->depth);
      free(geom);
      return true;
   }
   return false;
}

wsi::surface_properties &surface::get_properties()
{
   return properties;
}

util::unique_ptr<swapchain_base> surface::allocate_swapchain(layer::device_private_data &dev_data,
                                                             const VkAllocationCallbacks *allocator)
{
   util::allocator alloc{ dev_data.get_allocator(), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, allocator };
   return util::unique_ptr<swapchain_base>(alloc.make_unique<swapchain>(dev_data, allocator, *this));
}

util::unique_ptr<surface> surface::make_surface(const util::allocator &allocator, xcb_connection_t *conn,
                                                xcb_window_t window)
{
   /*
    * The connection passed in from winevulkan/winex11 was created by the
    * x86_64 libxcb running under box64.  Operating on it from this native
    * ARM64 libxcb is unsafe: the xcb_connection_t struct layout may not
    * match, and two libxcb instances hammering one socket fd corrupt each
    * other's read buffers/sequence tracking.  In practice the shared
    * connection dies on the first xcb_shm_attach_fd (SCM_RIGHTS), which
    * takes down wine's X client with it (XIO: fatal IO error 9).
    *
    * Fix: open our own xcb connection to the same display.  X window IDs
    * are server-global, so the wine-created window stays valid here.
    */
   xcb_connection_t *own_conn = nullptr;
   const char *display = getenv("DISPLAY");
   if (display && display[0])
   {
      int screen = 0;
      own_conn = xcb_connect(display, &screen);
      if (own_conn && xcb_connection_has_error(own_conn))
      {
         WSI_LOG_WARNING("Own xcb connection to %s failed, falling back to provided connection\n", display);
         xcb_disconnect(own_conn);
         own_conn = nullptr;
      }
   }
   else
   {
      WSI_LOG_WARNING("DISPLAY not set, falling back to provided connection\n");
   }

   xcb_connection_t *use_conn = own_conn ? own_conn : conn;

   xcb_get_geometry_cookie_t test_cookie = xcb_get_geometry(use_conn, window);
   xcb_generic_error_t *test_error = nullptr;
   xcb_get_geometry_reply_t *test_geom = xcb_get_geometry_reply(use_conn, test_cookie, &test_error);
   if (test_error)
   {
      WSI_LOG_WARNING("Window 0x%x query error on own connection, falling back to provided connection\n", window);
      free(test_error);
      if (own_conn)
      {
         xcb_disconnect(own_conn);
         own_conn = nullptr;
      }
      use_conn = conn;
   }
   else if (test_geom)
   {
      free(test_geom);
   }
   else
   {
      WSI_LOG_WARNING("Window 0x%x query returned NULL during surface creation\n", window);
   }

   init_parameters params{ allocator, use_conn, window };
   auto wsi_surface = allocator.make_unique<surface>(params);
   if (wsi_surface != nullptr)
   {
      wsi_surface->m_owns_connection = (own_conn != nullptr);
      if (wsi_surface->init())
      {
         if (wsi_surface->m_owns_connection)
         {
            WSI_LOG_INFO("Surface 0x%x using its own xcb connection to %s\n", window, display);
         }
         return wsi_surface;
      }
      else
      {
         WSI_LOG_ERROR("Surface init failed for window 0x%x\n", window);
      }
   }
   else
   {
      WSI_LOG_ERROR("Failed to allocate surface for window 0x%x\n", window);
   }
   return nullptr;
}

std::shared_ptr<wayland_bypass> surface::get_or_create_bypass(uint32_t width, uint32_t height)
{
   if (m_bypass)
      return m_bypass;

   auto bypass = std::make_shared<wayland_bypass>();
   if (!bypass->is_available())
      return nullptr;

   VkResult res = bypass->init(width, height);
   if (res != VK_SUCCESS)
      return nullptr;

   m_bypass = bypass;
   return m_bypass;
}

} /* namespace x11 */
} /* namespace wsi */
