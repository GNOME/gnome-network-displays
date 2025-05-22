/* main.c
 *
 * Copyright 2024 Pedro Sader Azevedo <pedro.saderazevedo@proton.me>
 * Copyright 2024 Christian Glombek <lorbus@fedoraproject.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <glib/gi18n.h>
#include <gst/gst.h>
#include "nd-stream.h"
#include "nd-sink.h"
#include "nd-uri-helpers.h"

gint
on_open (GApplication *self,
         GFile       **files,
         gint          n_files,
         const gchar  *hint,
         gpointer      user_data)
{
  NdSink *sink = NULL;
  gchar *uri;

  if (n_files != 1)
    {
      g_warning ("NdStream: Unexpected amount of command line arguments");
      return 1;
    }

  uri = g_file_get_uri (files[0]);

  if (!uri)
    {
      g_warning ("NdStream: Unable to get URI from command line argument");
      return 1;
    }

  sink = nd_uri_helpers_uri_to_sink (uri);

  if (!sink)
    {
      g_warning ("NdStream: Got NULL sink from command line argument");
      return 1;
    }

  g_object_set (self,
                "sink", sink,
                NULL);

  return 0;
}

int
main (int   argc,
      char *argv[])
{
  NdStream *stream = NULL;
  int ret;

  gst_init (&argc, &argv);

  stream = nd_stream_new ();

  /*
   * Connect "open" signal to handle the URI command line argument
   */
  g_signal_connect (stream,
                    "open",
                    G_CALLBACK (on_open),
                    NULL);

  ret = g_application_run (G_APPLICATION (stream), argc, argv);
  return ret;
}
