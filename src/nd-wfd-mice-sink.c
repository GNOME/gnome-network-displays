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
#include "nd-enum-types.h"
#include "nd-uri-helpers.h"
#include "nd-wfd-mice-sink.h"
#include "wfd/wfd-client.h"
#include "wfd/wfd-server.h"

struct _NdWFDMiceSink
{
  GObject            parent_instance;

  NdSinkState        state;

  GCancellable      *cancellable;

  GtkStringList     *missing_video_codec;
  GtkStringList     *missing_audio_codec;
  char              *missing_firewall_zone;

  gchar             *uuid;
  gchar             *name;
  gchar             *ip;
  gchar             *p2p_mac;
  gint               interface;

  GSocketClient     *signalling_client;
  GSocketConnection *signalling_client_conn;

  WfdServer         *server;
  guint              server_source_id;
};

enum {
  PROP_CLIENT = 1,
  PROP_NAME,
  PROP_IP,
  PROP_INTERFACE,
  PROP_P2P_MAC,
  PROP_UUID,
  PROP_DISPLAY_NAME,
  PROP_MATCHES,
  PROP_PRIORITY,
  PROP_STATE,
  PROP_PROTOCOL,
  PROP_MISSING_VIDEO_CODEC,
  PROP_MISSING_AUDIO_CODEC,
  PROP_MISSING_FIREWALL_ZONE,

  PROP_LAST = PROP_UUID,
};

const static NdSinkProtocol protocol = ND_SINK_PROTOCOL_WFD_MICE;

static void nd_wfd_mice_sink_sink_iface_init (NdSinkIface *iface);
static NdSink * nd_wfd_mice_sink_sink_start_stream (NdSink *sink);
static void nd_wfd_mice_sink_sink_stop_stream (NdSink *sink);
static gchar * nd_wfd_mice_sink_sink_to_uri (NdSink *sink);

static void nd_wfd_mice_sink_sink_stop_stream_int (NdWFDMiceSink *self);

G_DEFINE_TYPE_EXTENDED (NdWFDMiceSink, nd_wfd_mice_sink, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (ND_TYPE_SINK,
                                               nd_wfd_mice_sink_sink_iface_init);
                       )

static GParamSpec * props[PROP_LAST] = { NULL, };

static uint MICE_HOSTNAME_BUFFER_OFFSET = 7;
static uint MICE_HOSTNAME_LEN_OFFSET = 5;
/* Maximum friendly name size 520 bytes, divide to avoid any possible utf-16 overflows */
#define MICE_HOSTNAME_MAX_UTF8_LEN ((520 / 4) - 1)

static gchar msg_source_ready[] = {
  0x00, 0x00, /* Length of the message. Will be populated at runtime */
  0x01, /* MICE Protocol Version */
  0x01, /* Command SOURCE_READY */

  0x00, /* Friendly Name TLV */
  0x00, 0x00, /* Hostname length (number of bytes, UTF-16 encoded) */

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
      g_value_set_string (value, sink->name);
      break;

    case PROP_IP:
      g_value_set_string (value, sink->ip);
      break;

    case PROP_P2P_MAC:
      g_value_set_string (value, sink->p2p_mac);
      break;

    case PROP_UUID:
      g_value_set_string (value, sink->uuid);
      break;

    case PROP_DISPLAY_NAME:
      g_object_get_property (G_OBJECT (sink), "name", value);
      break;

    case PROP_INTERFACE:
      g_value_set_int (value, sink->interface);
      break;

    case PROP_MATCHES:
      {
        g_autoptr(GPtrArray) res = NULL;
        res = g_ptr_array_new_with_free_func (g_free);

        if (sink->ip)
          {
            g_debug ("NdWFDMiceSink: Adding IP %s to match list", sink->ip);
            g_ptr_array_add (res, g_strdup (sink->ip));
          }
        if (sink->p2p_mac)
          {
            gchar *p2p_mac = g_utf8_strup (sink->p2p_mac, -1);
            g_debug ("NdWFDMiceSink: Adding P2P MAC %s to match list", p2p_mac);
            g_ptr_array_add (res, p2p_mac);
          }
        g_value_take_boxed (value, g_steal_pointer (&res));
        break;
      }

    case PROP_PRIORITY:
      g_value_set_int (value, 200);
      break;

    case PROP_STATE:
      g_value_set_enum (value, sink->state);
      break;

    case PROP_PROTOCOL:
      g_value_set_enum (value, protocol);
      break;

    case PROP_MISSING_VIDEO_CODEC:
      g_value_set_object (value, sink->missing_video_codec);
      break;

    case PROP_MISSING_AUDIO_CODEC:
      g_value_set_object (value, sink->missing_audio_codec);
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
      sink->name = g_value_dup_string (value);
      g_object_notify (G_OBJECT (sink), "display-name");
      break;

    case PROP_IP:
      g_assert (sink->ip == NULL);
      sink->ip = g_value_dup_string (value);
      break;

    case PROP_P2P_MAC:
      g_assert (sink->p2p_mac == NULL);
      sink->p2p_mac = g_value_dup_string (value);
      break;

    case PROP_INTERFACE:
      sink->interface = g_value_get_int (value);
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

  g_clear_object (&sink->missing_video_codec);
  g_clear_object (&sink->missing_audio_codec);
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

  props[PROP_IP] =
    g_param_spec_string ("ip", "Sink IP Address",
                         "The IP address the sink was found on.",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  props[PROP_P2P_MAC] =
    g_param_spec_string ("p2p-mac", "Sink P2P MAC Address",
                         "The P2P MAC address the sink was found on.",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  props[PROP_INTERFACE] =
    g_param_spec_int ("interface", "Network Interface",
                      "The network interface Avahi discovered this entry on",
                      0, G_MAXINT, 0,
                      G_PARAM_READWRITE);
  g_object_class_install_properties (object_class, PROP_LAST, props);

  g_object_class_override_property (object_class, PROP_UUID, "uuid");
  g_object_class_override_property (object_class, PROP_DISPLAY_NAME, "display-name");
  g_object_class_override_property (object_class, PROP_MATCHES, "matches");
  g_object_class_override_property (object_class, PROP_PRIORITY, "priority");
  g_object_class_override_property (object_class, PROP_STATE, "state");
  g_object_class_override_property (object_class, PROP_PROTOCOL, "protocol");
  g_object_class_override_property (object_class, PROP_MISSING_VIDEO_CODEC, "missing-video-codec");
  g_object_class_override_property (object_class, PROP_MISSING_AUDIO_CODEC, "missing-audio-codec");
  g_object_class_override_property (object_class, PROP_MISSING_FIREWALL_ZONE, "missing-firewall-zone");
}

static void
nd_wfd_mice_sink_init (NdWFDMiceSink *sink)
{
  sink->uuid = g_uuid_string_random ();
  sink->state = ND_SINK_STATE_DISCONNECTED;
  sink->cancellable = g_cancellable_new ();
  sink->signalling_client = g_socket_client_new ();
  sink->interface = 0;
}

static gchar *
nd_wfd_mice_sink_sink_to_uri (NdSink *sink)
{
  NdWFDMiceSink *self = ND_WFD_MICE_SINK (sink);
  GHashTable *params = g_hash_table_new (g_str_hash, g_str_equal);

  /* protocol */
  g_hash_table_insert (params, "protocol", (gpointer *) g_strdup_printf ("%d", protocol));

  /* remote name */
  g_hash_table_insert (params, "name", (gpointer *) g_strdup (self->name));

  /* remote ip */
  g_hash_table_insert (params, "ip", (gpointer *) g_strdup (self->ip));

  return nd_uri_helpers_generate_uri (params);
}


/******************************************************************
* NdSink interface implementation
******************************************************************/

static void
nd_wfd_mice_sink_sink_iface_init (NdSinkIface *iface)
{
  iface->start_stream = nd_wfd_mice_sink_sink_start_stream;
  iface->stop_stream = nd_wfd_mice_sink_sink_stop_stream;
  iface->to_uri = nd_wfd_mice_sink_sink_to_uri;
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
                                                                      (gchar *) self->ip,
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
  const gchar *hostname = NULL;
  g_autofree gunichar2 *hostname_utf16 = NULL;
  gchar hostname_truncated[3 + 4 * MICE_HOSTNAME_MAX_UTF8_LEN + 1] = { 0xEF, 0xBB, 0xBF };
  glong hostname_items_written = 0;
  g_autofree gchar *rendered_msg_source_ready = NULL;
  uint rendered_msg_source_ready_len = sizeof (msg_source_ready);
  uint hostname_bytelen = 0;
  NdWFDMiceSink *self = ND_WFD_MICE_SINK (sink);
  gboolean have_wfd_codecs, send_ok;
  GStrv missing_video = NULL, missing_audio = NULL;

  g_debug ("NdWFDMiceSink: Start Stream");

  g_return_val_if_fail (self->state == ND_SINK_STATE_DISCONNECTED, NULL);

  if (self->ip == NULL)
    {
      g_warning ("NdWFDMiceSink: Cannot start without IP");
      goto error;
    }

  g_assert (self->server == NULL);
  self->server = wfd_server_new ();

  have_wfd_codecs = wfd_server_lookup_encoders (self->server,
                                                &missing_video,
                                                &missing_audio);

  g_clear_object (&self->missing_video_codec);
  g_clear_object (&self->missing_audio_codec);

  self->missing_video_codec = gtk_string_list_new ((const char *const *) missing_video);
  self->missing_audio_codec = gtk_string_list_new ((const char *const *) missing_audio);

  g_object_notify (G_OBJECT (self), "missing-video-codec");
  g_object_notify (G_OBJECT (self), "missing-audio-codec");

  if (!have_wfd_codecs)
    {
      g_warning ("NdWFDMiceSink: Essential codecs are missing!");
      goto error;
    }

  self->server_source_id = gst_rtsp_server_attach (GST_RTSP_SERVER (self->server), NULL);
  if (self->server_source_id == 0)
    {
      g_warning ("NdWFDMiceSink: Couldn't attach RTSP server!");
      goto error;
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

  hostname = g_get_host_name ();

  if (hostname == NULL)
    {
      g_warning ("Couldn't resolve the hostname. Defaulting to legacy device name \"GNOME\"");
      hostname = "GNOME";
    }

  g_debug ("NdWFDMiceSink: device name is %s", hostname);

  g_utf8_strncpy (hostname_truncated + 3, hostname, MICE_HOSTNAME_MAX_UTF8_LEN);
  hostname_utf16 = g_utf8_to_utf16 (hostname_truncated, -1, NULL, &hostname_items_written, &error);

  hostname_bytelen = hostname_items_written * 2;

  if (hostname_bytelen > MICE_HOSTNAME_MAX_UTF8_LEN)
    g_debug ("Device name is too long. Using the first %d bytes", MICE_HOSTNAME_MAX_UTF8_LEN);

  rendered_msg_source_ready_len += hostname_bytelen;

  rendered_msg_source_ready = g_malloc (rendered_msg_source_ready_len);

  // Copy the header
  memcpy (rendered_msg_source_ready, msg_source_ready, MICE_HOSTNAME_BUFFER_OFFSET);
  // Copy the footer
  memcpy (rendered_msg_source_ready + MICE_HOSTNAME_BUFFER_OFFSET + hostname_bytelen,
          msg_source_ready + MICE_HOSTNAME_BUFFER_OFFSET,
          sizeof (msg_source_ready) - MICE_HOSTNAME_BUFFER_OFFSET);


  if (error != NULL || hostname_utf16 == NULL)
    {
      g_warning ("NdWFDMiceSink: Unable to convert the device name '%s' to UTF-16: %s", hostname_truncated, error->message);
      goto error;
    }

  // Copy the friendly device name, its size and update the total message length
  memcpy (rendered_msg_source_ready + MICE_HOSTNAME_BUFFER_OFFSET, hostname_utf16, hostname_bytelen);
  rendered_msg_source_ready[MICE_HOSTNAME_LEN_OFFSET] = hostname_bytelen >> 8;
  rendered_msg_source_ready[MICE_HOSTNAME_LEN_OFFSET + 1] = hostname_bytelen & 0xff;
  rendered_msg_source_ready[0] = rendered_msg_source_ready_len >> 8;
  rendered_msg_source_ready[1] = rendered_msg_source_ready_len & 0xff;

  send_ok = signalling_client_send (self,
                                    rendered_msg_source_ready,
                                    rendered_msg_source_ready_len,
                                    NULL,
                                    error);
  if (!send_ok || error != NULL)
    {
      if (error != NULL)
        g_warning ("NdWFDMiceSink: Failed to create MICE client: %s", error->message);
      else
        g_warning ("NdWFDMiceSink: Failed to create MICE client");

      goto error;
    }

  return g_object_ref (sink);

error:
  self->state = ND_SINK_STATE_ERROR;
  g_object_notify (G_OBJECT (self), "state");
  g_clear_object (&self->server);

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
nd_wfd_mice_sink_new (gchar *name,
                      gchar *ip,
                      gchar *p2p_mac,
                      gint   interface)
{
  return g_object_new (ND_TYPE_WFD_MICE_SINK,
                       "name", name,
                       "ip", ip,
                       "p2p-mac", p2p_mac,
                       "interface", interface,
                       NULL);
}

/**
 * nd_wfd_mice_sink_from_uri
 * @uri: a URI string
 *
 * Construct a #NdWFDMiceSink using the information encoded in the URI string
 *
 * Returns: The newly constructed #NdWFDMiceSink
 */
NdWFDMiceSink *
nd_wfd_mice_sink_from_uri (gchar *uri)
{
  GHashTable *params = nd_uri_helpers_parse_uri (uri);

  /* protocol */
  const gchar *protocol_in_uri_str = g_hash_table_lookup (params, "protocol");

  ;
  NdSinkProtocol protocol_in_uri = g_ascii_strtoll (protocol_in_uri_str, NULL, 10);
  if (protocol != protocol_in_uri)
    {
      g_warning ("NdWFDMiceSink: Attempted to create sink whose protocol (%s) doesn't match the URI (%s)",
                 g_enum_to_string (ND_TYPE_SINK_PROTOCOL, protocol),
                 g_enum_to_string (ND_TYPE_SINK_PROTOCOL, protocol_in_uri));
      return NULL;
    }

  /* remote name */
  gchar *name = g_hash_table_lookup (params, "name");
  if (!name)
    {
      g_warning ("NdWFDMiceSink: Failed to find remote name in the URI %s", uri);
      return NULL;
    }

  /* remote ip */
  gchar *ip = g_hash_table_lookup (params, "ip");
  if (!ip)
    {
      g_warning ("NdWFDMiceSink: Failed to find remote IP address in the URI %s", uri);
      return NULL;
    }

  /* optional remote p2p mac */
  gchar *p2p_mac = NULL;
  p2p_mac = g_hash_table_lookup (params, "p2p-mac");

  return nd_wfd_mice_sink_new (name, ip, p2p_mac, 0);
}

GSocketClient *
nd_wfd_mice_sink_get_signalling_client (NdWFDMiceSink *sink)
{
  return sink->signalling_client;
}
