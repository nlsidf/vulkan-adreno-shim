#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/dri3.h>
#include <xcb/sync.h>
#include <stdio.h>
#include <stdlib.h>

static void report(const char *what, xcb_connection_t *conn)
{
   printf("%-28s has_error=%d\n", what, xcb_connection_has_error(conn));
}

int main(void)
{
   int screen = 0;
   xcb_connection_t *conn = xcb_connect(":1", &screen);
   printf("connect has_error=%d\n", xcb_connection_has_error(conn));

   /* 1. SHM (present on Xvnc) */
   auto qc = xcb_shm_query_version(conn);
   auto *qr = xcb_shm_query_version_reply(conn, qc, NULL);
   printf("SHM query_version: reply=%s\n", qr ? "OK" : "NULL");
   free(qr);
   report("after SHM", conn);

   /* 2. DRI3 query_version (NOT present on Xvnc) — replicate dri3_presenter */
   auto dc = xcb_dri3_query_version(conn, 1, 2);
   auto *dr = xcb_dri3_query_version_reply(conn, dc, NULL);
   printf("DRI3 query_version: reply=%s\n", dr ? "OK" : "NULL");
   free(dr);
   report("after DRI3", conn);

   /* 3. xcb_get_extension_data for sync (NOT present on Xvnc) */
   const xcb_query_extension_reply_t *se = xcb_get_extension_data(conn, &xcb_sync_id);
   printf("XSync get_extension_data: %s (present=%d)\n", se ? "OK" : "NULL", se ? se->present : -1);
   report("after XSync ext", conn);

   /* 4. Roundtrip to confirm connection still alive */
   auto fc = xcb_get_input_focus(conn);
   auto *fr = xcb_get_input_focus_reply(conn, fc, NULL);
   printf("roundtrip after ext queries: reply=%s\n", fr ? "OK" : "NULL");
   free(fr);
   report("final", conn);

   xcb_disconnect(conn);
   return 0;
}
