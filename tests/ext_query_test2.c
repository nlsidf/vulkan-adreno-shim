#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void report(const char *what, xcb_connection_t *conn)
{
   printf("%-36s has_error=%d\n", what, xcb_connection_has_error(conn));
}

int main(void)
{
   int screen = 0;
   xcb_connection_t *conn = xcb_connect(":1", &screen);
   printf("connect has_error=%d\n", xcb_connection_has_error(conn));

   /* A: raw xcb_query_extension for DRI3 (always valid core request) */
   xcb_query_extension_cookie_t c1 = xcb_query_extension(conn, 4, "DRI3");
   xcb_query_extension_reply_t *r1 = xcb_query_extension_reply(conn, c1, NULL);
   printf("raw query DRI3: reply=%s present=%d\n", r1 ? "OK" : "NULL", r1 ? r1->present : -1);
   free(r1);
   report("after RAW query_extension(DRI3)", conn);

   /* B: xcb_get_extension_data for DRI3 */
   const xcb_query_extension_reply_t *ed = xcb_get_extension_data(conn, &xcb_dri3_id);
   printf("get_extension_data DRI3: %s\n", ed ? "OK" : "NULL");
   report("after get_extension_data(DRI3)", conn);

   /* C: roundtrip still alive? */
   xcb_get_input_focus_cookie_t fc = xcb_get_input_focus(conn);
   xcb_get_input_focus_reply_t *fr = xcb_get_input_focus_reply(conn, fc, NULL);
   printf("roundtrip: reply=%s\n", fr ? "OK" : "NULL");
   free(fr);
   report("final", conn);

   xcb_disconnect(conn);
   return 0;
}
