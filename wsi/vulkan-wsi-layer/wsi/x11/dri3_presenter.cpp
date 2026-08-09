/*
 * Copyright (c) 2025 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file dri3_presenter.cpp
 *
 * @brief DRI3/Present-based X11 presenter — zero-copy presentation.
 *
 * Flow:
 *   1. Open render node via xcb_dri3_open (or fallback to scanning /dev/dri/)
 *   2. For each swapchain image:
 *      a. Import DMA-BUF fd into render node as GEM handle
 *      b. Create X11 pixmap via xcb_dri3_pixmap_from_buffers
 *   3. Present via xcb_present_pixmap (zero-copy to compositor)
 */

#include "dri3_presenter.hpp"
#include "surface.hpp"
#include "swapchain.hpp"
#include "util/log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include <xf86drm.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/xcb.h>

namespace wsi
{
namespace x11
{

dri3_presenter::dri3_presenter()
{
}

dri3_presenter::~dri3_presenter()
{
   if (m_render_node_fd >= 0)
   {
      close(m_render_node_fd);
   }
}

static bool query_extension_present(xcb_connection_t *connection, const char *name)
{
   /* Raw core QueryExtension request.  Unlike the generated
    * xcb_<ext>_query_version() helpers this NEVER kills the connection:
    * the generated functions resolve the major opcode via _xcb_conn_ext,
    * which shuts the whole connection down with
    * XCB_CONN_CLOSED_EXT_NOTSUPPORTED (has_error=2) when the extension is
    * absent.  We need the connection to survive a negative answer so the
    * SHM fallback can still present. */
   xcb_query_extension_cookie_t cookie =
      xcb_query_extension(connection, static_cast<uint16_t>(strlen(name)), name);
   xcb_query_extension_reply_t *reply = xcb_query_extension_reply(connection, cookie, nullptr);
   if (!reply)
   {
      return false;
   }
   bool present = reply->present != 0;
   free(reply);
   return present;
}

bool dri3_presenter::query_dri3_present(xcb_connection_t *connection)
{
   /* Presence checks MUST come first.  On a server without DRI3/Present
    * (e.g. Xvnc), calling xcb_dri3_query_version() below would kill the
    * connection — and with it every later SHM present — so bail out
    * before touching the generated functions. */
   if (!query_extension_present(connection, "DRI3"))
   {
      return false;
   }
   if (!query_extension_present(connection, "Present"))
   {
      return false;
   }

   /* Extensions are present: the generated queries are now safe. */
   auto dri3_cookie = xcb_dri3_query_version(connection, 1, 2);
   auto *dri3_reply = xcb_dri3_query_version_reply(connection, dri3_cookie, nullptr);
   if (!dri3_reply)
   {
      return false;
   }
   uint32_t dri3_major = dri3_reply->major_version;
   free(dri3_reply);

   if (dri3_major < 1)
   {
      return false;
   }

   auto present_cookie = xcb_present_query_version(connection, 1, 2);
   auto *present_reply = xcb_present_query_version_reply(connection, present_cookie, nullptr);
   if (!present_reply)
   {
      return false;
   }
   free(present_reply);

   return true;
}

int dri3_presenter::open_render_node(xcb_connection_t *connection, xcb_window_t root)
{
   /* Try xcb_dri3_open first — asks the X server for its render node */
   auto open_cookie = xcb_dri3_open(connection, root, 0 /* provider */);
   auto *open_reply = xcb_dri3_open_reply(connection, open_cookie, nullptr);
   if (open_reply)
   {
      int *fds = xcb_dri3_open_reply_fds(connection, open_reply);
      int nfd = open_reply->nfd;
      free(open_reply);

      if (nfd >= 1 && fds[0] >= 0)
      {
         /* Verify it's a render node */
         char path[256] = {};
         char proc_path[64];
         snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fds[0]);
         ssize_t len = readlink(proc_path, path, sizeof(path) - 1);
         if (len > 0)
         {
            path[len] = '\0';
            fprintf(stderr, "dri3_presenter: X server returned render node: %s\n", path);
         }
         return fds[0];
      }
   }

   /* Fallback: scan /dev/dri/ for a render node */
   fprintf(stderr, "dri3_presenter: xcb_dri3_open failed, scanning /dev/dri/ for render nodes\n");
   DIR *dir = opendir("/dev/dri");
   if (!dir)
   {
      return -1;
   }

   int fd = -1;
   struct dirent *entry;
   while ((entry = readdir(dir)) != nullptr)
   {
      if (strncmp(entry->d_name, "renderD", 7) != 0)
      {
         continue;
      }

      char devpath[64];
      snprintf(devpath, sizeof(devpath), "/dev/dri/%.50s", entry->d_name);

      fd = open(devpath, O_RDWR | O_CLOEXEC);
      if (fd >= 0)
      {
         fprintf(stderr, "dri3_presenter: opened render node %s\n", devpath);
         break;
      }
   }
   closedir(dir);
   return fd;
}

bool dri3_presenter::is_available(xcb_connection_t *connection)
{
   if (!query_dri3_present(connection))
   {
      return false;
   }

   /* Check that a render node exists by scanning /dev/dri/ */
   DIR *dir = opendir("/dev/dri");
   if (!dir)
   {
      return false;
   }

   bool found = false;
   struct dirent *entry;
   while ((entry = readdir(dir)) != nullptr)
   {
      if (strncmp(entry->d_name, "renderD", 7) == 0)
      {
         found = true;
         break;
      }
   }
   closedir(dir);

   return found;
}

VkResult dri3_presenter::init(xcb_connection_t *connection, xcb_window_t window, surface *wsi_surface)
{
   m_connection = connection;
   m_window = window;
   m_wsi_surface = wsi_surface;

   /* Get root window for DRI3 open */
   auto setup = xcb_get_setup(connection);
   auto screen = xcb_setup_roots_iterator(setup).data;
   xcb_window_t root = screen->root;

   m_render_node_fd = open_render_node(connection, root);
   if (m_render_node_fd < 0)
   {
      WSI_LOG_ERROR("dri3_presenter: no render node available");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   return VK_SUCCESS;
}

VkResult dri3_presenter::create_image_resources(x11_image_data *image_data, uint32_t width, uint32_t height,
                                                int depth, uint32_t stride, uint32_t fourcc, uint64_t modifier)
{
   if (m_render_node_fd < 0 || !m_connection)
   {
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   /* Get the DMA-BUF fd from the image's external memory */
   int dma_buf_fd = image_data->external_mem.get_buffer_fds()[0];
   if (dma_buf_fd < 0)
   {
      WSI_LOG_ERROR("dri3_presenter: no DMA-BUF fd in image data");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   /* Dup the fd — DRI3 takes ownership */
   int fd_for_dri3 = dup(dma_buf_fd);
   if (fd_for_dri3 < 0)
   {
      WSI_LOG_ERROR("dri3_presenter: dup() failed: %s", strerror(errno));
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   uint8_t bpp = (depth <= 24) ? 32 : 32;

   /* Create pixmap via DRI3.
    * xcb_dri3_pixmap_from_buffers creates a pixmap backed by the DMA-BUF.
    * The X server (Xwayland) imports the buffer — zero copy. */
   xcb_pixmap_t pixmap = xcb_generate_id(m_connection);

   xcb_dri3_pixmap_from_buffers(m_connection, pixmap, m_window,
                                1,          /* num_buffers */
                                width, height,
                                stride, 0,  /* stride0, offset0 */
                                0, 0,       /* stride1, offset1 */
                                0, 0,       /* stride2, offset2 */
                                0, 0,       /* stride3, offset3 */
                                depth, bpp, modifier,
                                &fd_for_dri3);

   /* Verify the pixmap was created */
   xcb_generic_error_t *geom_err = nullptr;
   auto geom_cookie = xcb_get_geometry(m_connection, pixmap);
   auto *geom_reply = xcb_get_geometry_reply(m_connection, geom_cookie, &geom_err);
   if (!geom_reply || geom_err)
   {
      uint8_t err_code = geom_err ? geom_err->error_code : 0;
      WSI_LOG_ERROR("dri3_presenter: pixmap creation failed (X11 error %d)", err_code);
      free(geom_err);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   free(geom_reply);

   image_data->pixmap = pixmap;
   image_data->width = width;
   image_data->height = height;
   image_data->depth = depth;

   fprintf(stderr, "dri3_presenter: created DRI3 pixmap %u (%ux%u, fourcc=0x%x, mod=0x%lx)\n",
           pixmap, width, height, fourcc, (unsigned long)modifier);

   return VK_SUCCESS;
}

VkResult dri3_presenter::present_image(x11_image_data *image_data, uint32_t serial)
{
   (void)serial;

   if (!m_connection || image_data->pixmap == XCB_PIXMAP_NONE)
   {
      return VK_ERROR_SURFACE_LOST_KHR;
   }

   m_present_serial++;

   /* xcb_present_pixmap with COPY option: the X server copies the pixmap
    * contents immediately, so the buffer is safe to reuse right away.
    * This prevents "jumps back" stutter from the X server re-reading
    * stale buffer contents during compositing. */
   xcb_present_pixmap(m_connection, m_window, image_data->pixmap,
                      m_present_serial,
                      XCB_NONE, /* valid_region — whole pixmap */
                      XCB_NONE, /* update_region — whole pixmap */
                      0, 0,     /* x_off, y_off */
                      XCB_NONE, /* target_crtc — let X server decide */
                      XCB_NONE, /* wait_fence */
                      XCB_NONE, /* idle_fence */
                      XCB_PRESENT_OPTION_COPY, /* copy to avoid stale reads */
                      0,        /* target_msc — immediate */
                      0, 0,     /* divisor, remainder */
                      0,        /* notifies_len */
                      nullptr); /* notifies */

   xcb_flush(m_connection);

   return VK_SUCCESS;
}

void dri3_presenter::destroy_image_resources(x11_image_data *image_data)
{
   if (m_connection && image_data->pixmap != XCB_PIXMAP_NONE)
   {
      xcb_free_pixmap(m_connection, image_data->pixmap);
      image_data->pixmap = XCB_PIXMAP_NONE;
   }
}

} /* namespace x11 */
} /* namespace wsi */
