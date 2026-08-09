#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <stdio.h>
#include <stdlib.h>

static void report(const char *what, xcb_connection_t *conn)
{
   printf("%-38s has_error=%d\n", what, xcb_connection_has_error(conn));
}

int main(int argc, char **argv)
{
   int screen = 0;
   xcb_connection_t *conn = xcb_connect(":1", &screen);
   printf("connect has_error=%d\n", xcb_connection_has_error(conn));

   if (argc > 1 && argv[1][0] == 'g') {
      /* get_extension_data FIRST, no prior query */
      const xcb_query_extension_reply_t *ed = xcb_get_extension_data(conn, &xcb_dri3_id);
      printf("get_extension_data DRI3: %s\n", ed ? "OK" : "NULL");
      report("after get_extension_data", conn);
   } else {
      /* generated query_version path */
      xcb_dri3_query_version_cookie_t dc = xcb_dri3_query_version(conn, 1, 2);
      report("after dri3_query_version (no reply yet)", conn);
      xcb_dri3_query_version_reply_t *dr = xcb_dri3_query_version_reply(conn, dc, NULL);
      printf("dri3_query_version_reply: %s\n", dr ? "OK" : "NULL");
      free(dr);
      report("after dri3_query_version_reply", conn);
   }

   xcb_get_input_focus_cookie_t fc = xcb_get_input_focus(conn);
   xcb_get_input_focus_reply_t *fr = xcb_get_input_focus_reply(conn, fc, NULL);
   printf("roundtrip: reply=%s\n", fr ? "OK" : "NULL");
   free(fr);
   report("final", conn);

   xcb_disconnect(conn);
   return 0;
}
