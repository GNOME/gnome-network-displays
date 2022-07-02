/* nd-wfd-mice-sink.c
 *
 * Copyright 2022 Christian Glombek <lorbus@fedoraproject.org>
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

#include "gnome-network-displays-config.h"
#include "nd-wfd-mice-sink.h"
#include "wfd/wfd-client.h"
#include "wfd/wfd-media-factory.h"
#include "wfd/wfd-server.h"

struct _NdWFDMiceSink
{
  GObject            parent_instance;

  NdSinkState        state;

  GCancellable      *cancellable;

  GStrv              missing_video_codec;
  GStrv              missing_audio_codec;
  char              *missing_firewall_zone;

  gchar             *remote_address;
  gchar             *remote_name;

  GSocketClient     *signalling_client;
  GSocketConnection *signalling_client_conn;

  WfdServer         *server;
  guint              server_source_id;
};

enum {
  PROP_CLIENT = 1,
  PROP_NAME,
  PROP_ADDRESS,

  PROP_DISPLAY_NAME,
  PROP_MATCHES,
  PROP_PRIORITY,
  PROP_STATE,
  PROP_MISSING_VIDEO_CODEC,
  PROP_MISSING_AUDIO_CODEC,
  PROP_MISSING_FIREWALL_ZONE,

  PROP_LAST = PROP_DISPLAY_NAME,
};

static void nd_wfd_mice_sink_sink_iface_init (NdSinkIface *iface);
static NdSink * nd_wfd_mice_sink_sink_start_stream (NdSink *sink);
static void nd_wfd_mice_sink_sink_stop_stream (NdSink *sink);

static void nd_wfd_mice_sink_sink_stop_stream_int (NdWFDMiceSink *self);

G_DEFINE_TYPE_EXTENDED (NdWFDMiceSink, nd_wfd_mice_sink, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (ND_TYPE_SINK,
                                               nd_wfd_mice_sink_sink_iface_init);
                       )

static GParamSpec * props[PROP_LAST] = { NULL, };

static gchar msg_source_ready[] = {
  0x00, 0x29, /* Length (41 bytes) */
  0x01, /* MICE Protocol Version */
  0x01, /* Command SOURCE_READY */

  0x00, /* Friendly Name TLV */
  0x00, 0x0A, /* Length (10 bytes) */
  /* GNOME (UTF-16-encoded) */
  0x47, 0x00, 0x4E, 0x00, 0x4F, 0x00, 0x4D, 0x00, 0x45, 0x00,

  0x02, /* RTSP Port TLV */
  0x00, 0x02, /* Length (2 bytes) */
  0x1C, 0x44, /* Port 7236 */

  0x03, /* Source ID TLV */
  0x00, 0x10, /* Length (16 bytes) */
  /* Source ID GnomeMICEDisplay (ascii) */
  0x47, 0x6E, 0x6F, 0x6D, 0x65, 0x4D, 0x49, 0x43, 0x45, 0x44, 0x69, 0x73, 0x70, 0x6C, 0x61, 0x79
};

static void
nd_wfd_mice_sink_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  NdWFDMiceSink *sink = ND_WFD_MICE_SINK (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      g_value_set_object (value, sink->signalling_client);
      break;

    case PROP_NAME:
      g_value_set_string (value, sink->remote_name);
      break;

    case PROP_ADDRESS:
      g_value_set_string (value, sink->remote_address);
      break;

    case PROP_DISPLAY_NAME:
      g_object_get_property (G_OBJECT (sink), "name", value);
      break;

    case PROP_MATCHES:
      {
        g_autoptr(GPtrArray) res = NULL;
        res = g_ptr_array_new_with_free_func (g_free);

        if (sink->remote_name)
          g_ptr_array_add (res, g_strdup (sink->remote_name));

        g_value_take_boxed (value, g_steal_pointer (&res));
        break;
      }

    case PROP_PRIORITY:
      g_value_set_int (value, 200);
      break;

    case PROP_STATE:
      g_value_set_enum (value, sink->state);
      break;

    case PROP_MISSING_VIDEO_CODEC:
      g_value_set_boxed (value, sink->missing_video_codec);
      break;

    case PROP_MISSING_AUDIO_CODEC:
      g_value_set_boxed (value, sink->missing_audio_codec);
      break;

    case PROP_MISSING_FIREWALL_ZONE:
      g_value_set_string (value, sink->missing_firewall_zone);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_wfd_mice_sink_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  NdWFDMiceSink *sink = ND_WFD_MICE_SINK (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      /* Construct only */
      sink->signalling_client = g_value_dup_object (value);
      break;

    case PROP_NAME:
      sink->remote_name = g_value_dup_string (value);
      g_object_notify (G_OBJECT (sink), "display-name");
      break;

    case PROP_ADDRESS:
      g_assert (sink->remote_address == NULL);
      sink->remote_address = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

void
nd_wfd_mice_sink_finalize (GObject *object)
{
  NdWFDMiceSink *sink = ND_WFD_MICE_SINK (object);

  g_debug ("NdWFDMiceSink: Finalizing");

  nd_wfd_mice_sink_sink_stop_stream_int (sink);

  g_cancellable_cancel (sink->cancellable);
  g_clear_object (&sink->cancellable);
  g_clear_object (&sink->signalling_client);

  g_clear_pointer (&sink->missing_video_codec, g_strfreev);
  g_clear_pointer (&sink->missing_audio_codec, g_strfreev);
  g_clear_pointer (&sink->missing_firewall_zone, g_free);

  G_OBJECT_CLASS (nd_wfd_mice_sink_parent_class)->finalize (object);
}

static void
nd_wfd_mice_sink_class_init (NdWFDMiceSinkClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = nd_wfd_mice_sink_get_property;
  object_class->set_property = nd_wfd_mice_sink_set_property;
  object_class->finalize = nd_wfd_mice_sink_finalize;

  props[PROP_CLIENT] =
    g_param_spec_object ("client", "Signalling Client",
                         "The GSocketClient used for MICE signalling.",
                         G_TYPE_SOCKET_CLIENT,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  props[PROP_NAME] =
    g_param_spec_string ("name", "Sink Name",
                         "The sink name found by the Avahi Client.",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  props[PROP_ADDRESS] =
    g_param_spec_string ("address", "Sink Address",
                         "The address the sink was found on.",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, props);

  g_object_class_override_property (object_class, PROP_DISPLAY_NAME, "display-name");
  g_object_class_override_property (object_class, PROP_MATCHES, "matches");
  g_object_class_override_property (object_class, PROP_PRIORITY, "priority");
  g_object_class_override_property (object_class, PROP_STATE, "state");
  g_object_class_override_property (object_class, PROP_MISSING_VIDEO_CODEC, "missing-video-codec");
  g_object_class_override_property (object_class, PROP_MISSING_AUDIO_CODEC, "missing-audio-codec");
  g_object_class_override_property (object_class, PROP_MISSING_FIREWALL_ZONE, "missing-firewall-zone");
}

static void
nd_wfd_mice_sink_init (NdWFDMiceSink *sink)
{
  sink->state = ND_SINK_STATE_DISCONNECTED;
  sink->cancellable = g_cancellable_new ();
  sink->signalling_client = g_socket_client_new ();
}

/******************************************************************
* NdSink interface implementation
******************************************************************/

static void
nd_wfd_mice_sink_sink_iface_init (NdSinkIface *iface)
{
  iface->start_stream = nd_wfd_mice_sink_sink_start_stream;
  iface->stop_stream = nd_wfd_mice_sink_sink_stop_stream;
}

static void
play_request_cb (NdWFDMiceSink *sink, GstRTSPContext *ctx, WfdClient *client)
{
  g_debug ("NdWFDMiceSink: Got play request from client");

  sink->state = ND_SINK_STATE_STREAMING;
  g_object_notify (G_OBJECT (sink), "state");
}

gboolean
signalling_client_send (NdWFDMiceSink * self,
                        const void    * message,
                        gssize          size,
                        GCancellable  * cancellable,
                        GError        * error)
{
  GOutputStream * ostream;

  if (self->signalling_client == NULL)
    self->signalling_client = g_socket_client_new ();

  if (self->signalling_client_conn == NULL)
    {
      self->signalling_client_conn = g_socket_client_connect_to_host (self->signalling_client,
                                                                      (gchar *) self->remote_address,
                                                                      7250,
                                                                      NULL,
                                                                      &error);
    }

  if (!self->signalling_client_conn || error != NULL)
    {
      if (error != NULL)
        g_warning ("NdWFDMiceSink: Failed to write to signalling stream: %s", error->message);

      return FALSE;

    }

  g_assert (G_IO_STREAM (self->signalling_client_conn));

  g_debug ("NdWFDMiceSink: Client connection established");

  ostream = g_io_stream_get_output_stream (G_IO_STREAM (self->signalling_client_conn));
  if (!ostream)
    {
      g_warning ("NdWFDMiceSink: Could not get output stream");

      return FALSE;
    }

  size = g_output_stream_write (ostream, message, size, cancellable, &error);
  if (error != NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
        {
          g_warning ("NdWFDMiceSink: Signalling client socket send would block");
          return FALSE;
        }
      else
        {
          g_warning ("NdWFDMiceSink: Error writing to client socket output stream: %s", error->message);
          return FALSE;
        }
    }

  g_debug ("NdWFDMiceSink: Sent %" G_GSSIZE_FORMAT " bytes of data", size);

  return TRUE;
}

static void
closed_cb (NdWFDMiceSink *sink, WfdClient *client)
{
  g_autoptr(GError) error = NULL;

  /* Connection was closed, do a clean shutdown*/
  nd_wfd_mice_sink_sink_stop_stream (ND_SINK (sink));
}

static void
client_connected_cb (NdWFDMiceSink *sink, WfdClient *client, WfdServer *server)
{
  g_debug ("NdWFDMiceSink: Got client connection");

  g_signal_handlers_disconnect_by_func (sink->server, client_connected_cb, sink);
  sink->state = ND_SINK_STATE_WAIT_STREAMING;
  g_object_notify (G_OBJECT (sink), "state");

  /* XXX: connect to further events. */
  g_signal_connect_object (client,
                           "play-request",
                           (GCallback) play_request_cb,
                           sink,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (client,
                           "closed",
                           (GCallback) closed_cb,
                           sink,
                           G_CONNECT_SWAPPED);
}

static GstElement *
server_create_source_cb (NdWFDMiceSink *sink, WfdServer *server)
{
  GstElement *res;

  g_signal_emit_by_name (sink, "create-source", &res);
  g_debug ("NdWFDMiceSink: Create source signal emitted");
  return res;
}

static GstElement *
server_create_audio_source_cb (NdWFDMiceSink *sink, WfdServer *server)
{
  GstElement *res;

  g_signal_emit_by_name (sink, "create-audio-source", &res);
  g_debug ("NdWFDMiceSink: Create audio source signal emitted");

  return res;
}

static NdSink *
nd_wfd_mice_sink_sink_start_stream (NdSink *sink)
{
  g_autoptr(GError) error = NULL;
  NdWFDMiceSink *self = ND_WFD_MICE_SINK (sink);
  gboolean have_basic_codecs, send_ok;
  GStrv missing_video, missing_audio;

  g_return_val_if_fail (self->state == ND_SINK_STATE_DISCONNECTED, NULL);

  g_assert (self->server == NULL);

  have_basic_codecs = wfd_get_missing_codecs (&missing_video, &missing_audio);

  g_clear_pointer (&self->missing_video_codec, g_strfreev);
  g_clear_pointer (&self->missing_audio_codec, g_strfreev);

  self->missing_video_codec = g_strdupv (missing_video);
  self->missing_audio_codec = g_strdupv (missing_audio);

  g_object_notify (G_OBJECT (self), "missing-video-codec");
  g_object_notify (G_OBJECT (self), "missing-audio-codec");

  if (!have_basic_codecs)
    {
      g_warning ("Essential codecs are missing!");
      self->state = ND_SINK_STATE_ERROR;
      g_object_notify (G_OBJECT (self), "state");

      return g_object_ref (sink);
    }

  g_assert (self->server == NULL);
  self->server = wfd_server_new ();
  self->server_source_id = gst_rtsp_server_attach (GST_RTSP_SERVER (self->server), NULL);

  if (self->server_source_id == 0 || self->remote_address == NULL)
    {
      self->state = ND_SINK_STATE_ERROR;
      g_object_notify (G_OBJECT (self), "state");
      g_clear_object (&self->server);

      return g_object_ref (sink);
    }

  g_signal_connect_object (self->server,
                           "client-connected",
                           (GCallback) client_connected_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->server,
                           "create-source",
                           (GCallback) server_create_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->server,
                           "create-audio-source",
                           (GCallback) server_create_audio_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  self->state = ND_SINK_STATE_WAIT_SOCKET;
  g_object_notify (G_OBJECT (self), "state");

  send_ok = signalling_client_send (self,
                                    msg_source_ready,
                                    sizeof (msg_source_ready),
                                    NULL,
                                    error);
  if (!send_ok || error != NULL)
    {
      if (error != NULL)
        g_warning ("NdWFDMiceSink: Failed to create MICE client: %s", error->message);
      else
        g_warning ("NdWFDMiceSink: Failed to create MICE client");

      self->state = ND_SINK_STATE_ERROR;
      g_object_notify (G_OBJECT (self), "state");
      g_clear_object (&self->server);
    }

  return g_object_ref (sink);
}

static void
nd_wfd_mice_sink_sink_stop_stream_int (NdWFDMiceSink *self)
{
  GError *error = NULL;
  gboolean close_ok;

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  self->cancellable = g_cancellable_new ();

  /* Close the client connection */
  if (self->signalling_client_conn != NULL)
    {
      close_ok = g_io_stream_close (G_IO_STREAM (self->signalling_client_conn), NULL, &error);
      if (error != NULL)
        g_warning ("NdWFDMiceSink: Error closing signalling client connection: %s", error->message);
      if (!close_ok)
        g_warning ("NdWFDMiceSink: Signalling client connection not closed");

      g_clear_object (&self->signalling_client_conn);
      g_debug ("NdWFDMiceSink: Client connection removed");
    }

  /* Destroy the server that is streaming. */
  if (self->server_source_id)
    {
      g_source_remove (self->server_source_id);
      self->server_source_id = 0;
    }

  /* Needs to protect against recursion. */
  if (self->server)
    {
      g_autoptr(WfdServer) server = NULL;

      server = g_steal_pointer (&self->server);
      g_signal_handlers_disconnect_by_data (server, self);
      wfd_server_purge (server);
    }
}

static void
nd_wfd_mice_sink_sink_stop_stream (NdSink *sink)
{
  NdWFDMiceSink *self = ND_WFD_MICE_SINK (sink);

  nd_wfd_mice_sink_sink_stop_stream_int (self);

  self->state = ND_SINK_STATE_DISCONNECTED;
  g_object_notify (G_OBJECT (self), "state");
}

/******************************************************************
* NdWFDMiceSink public functions
******************************************************************/

NdWFDMiceSink *
nd_wfd_mice_sink_new (gchar         *name,
                      gchar         *remote_address)
{
  return g_object_new (ND_TYPE_WFD_MICE_SINK,
                       "name", name,
                       "address", remote_address,
                       NULL);
}

NdSinkState
nd_wfd_mice_sink_get_state (NdWFDMiceSink *sink)
{
  return sink->state;
}

GSocketClient *
nd_wfd_mice_sink_get_signalling_client (NdWFDMiceSink *sink)
{
  return sink->signalling_client;
}