/* nd-cc-sink.c
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
#include "nd-cc-sink.h"
#include "cc/cc-client.h"
#include "wfd/wfd-media-factory.h"
#include "wfd/wfd-server.h"
#include "cc/cast_channel.pb-c.h"

#define MAX_MSG_SIZE 4096

struct _NdCCSink
{
  GObject            parent_instance;

  NdSinkState        state;

  GCancellable      *cancellable;

  GStrv              missing_video_codec;
  GStrv              missing_audio_codec;
  char              *missing_firewall_zone;

  gchar             *remote_address;
  gchar             *remote_name;

  GSocketClient     *comm_client;
  GSocketConnection *comm_client_conn; 

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

// interface related functions
static void nd_cc_sink_sink_iface_init (NdSinkIface *iface);
static NdSink * nd_cc_sink_sink_start_stream (NdSink *sink);
static void nd_cc_sink_sink_stop_stream (NdSink *sink);

static void nd_cc_sink_sink_stop_stream_int (NdCCSink *self);

G_DEFINE_TYPE_EXTENDED (NdCCSink, nd_cc_sink, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (ND_TYPE_SINK,
                                               nd_cc_sink_sink_iface_init);
                       )

static GParamSpec * props[PROP_LAST] = { NULL, };

static gchar msg_stop_projection[] = {
  0x00, 0x24, /* Length (36 bytes) */
  0x01, /* MICE Protocol Version */
  0x02, /* Command STOP_PROJECTION */

  0x00, /* Friendly Name TLV */
  0x00, 0x0A, /* Length (10 bytes) */
  /* GNOME (UTF-16-encoded) */
  0x47, 0x00, 0x4E, 0x00, 0x4F, 0x00, 0x4D, 0x00, 0x45, 0x00,

  0x03, /* Source ID TLV */
  0x00, 0x10, /* Length (16 bytes) */
  /* Source ID GnomeMICEDisplay (ascii) */
  0x47, 0x6E, 0x6F, 0x6D, 0x65, 0x4D, 0x49, 0x43, 0x45, 0x44, 0x69, 0x73, 0x70, 0x6C, 0x61, 0x79
};

static void
nd_cc_sink_get_property (GObject *    object,
                         guint        prop_id,
                         GValue *     value,
                         GParamSpec * pspec)
{
  NdCCSink *sink = ND_CC_SINK (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      g_value_set_object (value, sink->comm_client);
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
      g_value_set_int (value, 100);
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
nd_cc_sink_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  NdCCSink *sink = ND_CC_SINK (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      /* Construct only */
      sink->comm_client = g_value_dup_object (value);
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
nd_cc_sink_finalize (GObject *object)
{
  NdCCSink *sink = ND_CC_SINK (object);

  g_debug ("NdCCSink: Finalizing");

  nd_cc_sink_sink_stop_stream_int (sink);

  g_cancellable_cancel (sink->cancellable);
  g_clear_object (&sink->cancellable);

  g_clear_pointer (&sink->missing_video_codec, g_strfreev);
  g_clear_pointer (&sink->missing_audio_codec, g_strfreev);
  g_clear_pointer (&sink->missing_firewall_zone, g_free);

  G_OBJECT_CLASS (nd_cc_sink_parent_class)->finalize (object);
}

static void
nd_cc_sink_class_init (NdCCSinkClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = nd_cc_sink_get_property;
  object_class->set_property = nd_cc_sink_set_property;
  object_class->finalize = nd_cc_sink_finalize;

  props[PROP_CLIENT] =
    g_param_spec_object ("client", "Communication Client",
                         "The GSocketClient used for Chromecast communication.",
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
nd_cc_sink_init (NdCCSink *sink)
{
  sink->state = ND_SINK_STATE_DISCONNECTED;
  sink->cancellable = g_cancellable_new ();
}

/******************************************************************
* NdSink interface implementation
******************************************************************/

static void
nd_cc_sink_sink_iface_init (NdSinkIface *iface)
{
  iface->start_stream = nd_cc_sink_sink_start_stream;
  iface->stop_stream = nd_cc_sink_sink_stop_stream;
}

static void
play_request_cb (NdCCSink *sink, GstRTSPContext *ctx, CCClient *client)
{
  g_debug ("NdCCSink: Got play request from client");

  sink->state = ND_SINK_STATE_STREAMING;
  g_object_notify (G_OBJECT (sink), "state");
}

void
parse_received_data(uint8_t * input_buffer, gssize input_size)
{
  Castchannel__CastMessage *message;

  message = castchannel__cast_message__unpack(NULL, input_size-4, input_buffer+4);
  if (message == NULL)
  {
    g_warning ("NdCCSink: Failed to unpack received data");
    return;
  }

  g_debug("NdCCSink: Received data: %s", message->payload_utf8);

  castchannel__cast_message__free_unpacked(message, NULL);
}

gboolean
comm_client_send (NdCCSink      * self,
                  GSocketClient * client,
                  gchar         * remote_address,
                  const void    * message,
                  gssize          size,
                  GCancellable  * cancellable,
                  GError        * error)
{
  GOutputStream * ostream;
  GInputStream *  istream;
  gssize input_size;
  uint8_t * input_buffer = malloc(MAX_MSG_SIZE);

  if (self->comm_client_conn == NULL)
    self->comm_client_conn = g_socket_client_connect_to_host (client,
                                                              (gchar *) remote_address,
                                                              8009,
                                                              // 8010,
                                                              NULL,
                                                              &error);

  if (!self->comm_client_conn || error != NULL)
  {
    if (error != NULL)
      g_warning ("NdCCSink: Failed to write to communication stream: %s", error->message);
    return FALSE;
  }

  g_assert (G_IO_STREAM (self->comm_client_conn));
  g_debug ("NdCCSink: Client connection established");

  ostream = g_io_stream_get_output_stream (G_IO_STREAM (self->comm_client_conn));
  if (!ostream)
  {
    g_warning ("NdCCSink: Could not signal to sink");
    return FALSE;
  }

  size = g_output_stream_write (ostream, message, size, cancellable, &error);
  if (error != NULL)
  {
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
    {
      g_warning ("NdCCSink: Communication client socket send would block");
      return FALSE;
    }
    else
    {
      g_warning ("NdCCSink: Error writing to client socket output: %s", error->message);
      return FALSE;
    }
  }

  g_debug ("NdCCSink: Sent %" G_GSSIZE_FORMAT " bytes of data", size);
  
  g_debug ("NdCCSink: Waiting for response from sink");

  // get input stream from the connection
  istream = g_io_stream_get_input_stream (G_IO_STREAM (self->comm_client_conn));
  if (!istream)
  {
    g_warning ("NdCCSink: Could not get input stream from client connection");
    return FALSE;
  }

  // read the response from the sink
  input_size = g_input_stream_read (istream, input_buffer, MAX_MSG_SIZE, NULL, &error);
  if (error != NULL)
  {
    g_warning ("NdCCSink: Error reading from client socket input: %s", error->message);
    // return FALSE;
  }

  g_debug("NdCCSink: Received %" G_GSSIZE_FORMAT " bytes of data", input_size);
  // for (int i = 0; i < input_size; i++)
  // {
  //   g_debug ("NdCCSink: Received byte %", input_buffer[i]);
  // }

  g_debug("NdCCSink: Received Message: %s", input_buffer);
  parse_received_data(input_buffer, input_size);

  free(input_buffer);

  return TRUE;
}

static void
closed_cb (NdCCSink *sink, CCClient *client)
{
  g_autoptr(GError) error = NULL;

  /* Connection was closed, do a clean shutdown*/
  gboolean comm_client_ok = comm_client_send (sink,
                                              sink->comm_client,
                                              sink->remote_address,
                                              msg_stop_projection,
                                              sizeof (msg_stop_projection),
                                              NULL,
                                              error);

  if (!comm_client_ok || error != NULL)
    {
      if (error != NULL)
        g_warning ("NdCCSink: Failed to send stop projection cmd to client: %s", error->message);
      else
        g_warning ("NdCCSink: Failed to send stop projection cmd to client");

      sink->state = ND_SINK_STATE_ERROR;
      g_object_notify (G_OBJECT (sink), "state");
      g_clear_object (&sink->server);
    }
  nd_cc_sink_sink_stop_stream (ND_SINK (sink));
}

static void
client_connected_cb (NdCCSink *sink, CCClient *client, WfdServer *server)
{
  g_debug ("NdCCSink: Got client connection");

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
server_create_source_cb (NdCCSink *sink, WfdServer *server)
{
  GstElement *res;

  g_signal_emit_by_name (sink, "create-source", &res);
  g_debug ("NdCCSink: Create source signal emitted");
  return res;
}

static GstElement *
server_create_audio_source_cb (NdCCSink *sink, WfdServer *server)
{
  GstElement *res;

  g_signal_emit_by_name (sink, "create-audio-source", &res);
  g_debug ("NdCCSink: Create audio source signal emitted");

  return res;
}

// builds message based on available types
Castchannel__CastMessage
build_message (
  gchar *namespace_,
  Castchannel__CastMessage__PayloadType payload_type,
  ProtobufCBinaryData * binary_payload,
  gchar *utf8_payload)
{
  Castchannel__CastMessage message;
  castchannel__cast_message__init(&message);

  message.protocol_version = CASTCHANNEL__CAST_MESSAGE__PROTOCOL_VERSION__CASTV2_1_0;
  message.source_id = "gnd-0";
  message.destination_id = "destination-0";
  message.namespace_ = namespace_;
  message.payload_type = payload_type;

  switch (payload_type)
  {
  case CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__BINARY:
    message.payload_binary = *binary_payload;
    message.has_payload_binary = 1;
    break;
  case CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING:
  default:
    message.payload_utf8 = utf8_payload;
    message.has_payload_binary = 0;
    break;
  }

  return message;
}

void
send_request (NdCCSink *sink, enum MessageType message_type, char * utf8_payload)
{
  NdCCSink *self = ND_CC_SINK (sink);
  g_autoptr(GError) error = NULL;
  gboolean send_ok;
  Castchannel__CastMessage message;
  // ProtobufCBinaryData binary_payload;
  // binary_payload.data = NULL;
  // binary_payload.len = 0;
  guint32 packed_size = 0;

  // TODO: how to do this again?
  // g_autoptr(uint8_t) *sock_buffer = NULL;
  uint8_t *sock_buffer = NULL;

  g_debug("Send request: %d", message_type);

  switch (message_type)
  {
  case MESSAGE_TYPE_CONNECT:
    message = build_message(
      "urn:x-cast:com.google.cast.tp.connection",
      CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
      NULL,
      "{ \"type\": \"CONNECT\" }");
    break;

  case MESSAGE_TYPE_DISCONNECT:
    message = build_message(
      "urn:x-cast:com.google.cast.tp.connection",
      CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
      NULL,
      "{ \"type\": \"CLOSE\" }");
    break;

  case MESSAGE_TYPE_PING:
    message = build_message(
      "urn:x-cast:com.google.cast.tp.heartbeat",
      CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
      NULL,
      "{ \"type\": \"PING\" }");
    break;

  case MESSAGE_TYPE_RECEIVER:
    message = build_message(
      "urn:x-cast:com.google.cast.receiver",
      CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
      NULL,
      utf8_payload);
    break;

  default:
    break;
  }

  packed_size = castchannel__cast_message__get_packed_size(&message);
  sock_buffer = malloc(4 + packed_size);

  // TODO: look for gobject way of doing this: like with g_autoptr

  guint32 packed_size_be = GUINT32_TO_BE(packed_size);
  memcpy(sock_buffer, &packed_size_be, 4);
  castchannel__cast_message__pack(&message, 4 + sock_buffer);

  g_debug("Sending message to %s:%s", self->remote_address, self->remote_name);
  send_ok = comm_client_send (self,
                              self->comm_client,
                              self->remote_address,
                              sock_buffer,
                              packed_size+4,
                              NULL,
                              error);

  // g_debug("Waiting for response");
  // g_io_add_watch(self->comm_client_conn, G_IO_IN | G_IO_HUP, msg_received_cb, self);

  if (!send_ok || error != NULL)
    {
      if (error != NULL)
        g_warning ("NdCCSink: Failed to connect to Chromecast: %s", error->message);
      else
        g_warning ("NdCCSink: Failed to connect to Chromecast");

      self->state = ND_SINK_STATE_ERROR;
      g_object_notify (G_OBJECT (self), "state");
      g_clear_object (&self->server);
    }

  free(sock_buffer);
}

static NdSink *
nd_cc_sink_sink_start_stream (NdSink *sink)
{
  NdCCSink *self = ND_CC_SINK (sink);

  g_return_val_if_fail (self->state == ND_SINK_STATE_DISCONNECTED, NULL);

  g_assert (self->server == NULL);

  // self->state = ND_SINK_STATE_ENSURE_FIREWALL;
  // g_object_notify (G_OBJECT (self), "state");

  g_debug ("NdCCSink: Attempting connection to Chromecast: %s", self->remote_name);

  // send connection request to client
  send_request(self, MESSAGE_TYPE_CONNECT, NULL);

  // send ping to client
  send_request(self, MESSAGE_TYPE_PING, NULL);

  // send req to get status
  send_request(self, MESSAGE_TYPE_RECEIVER, "{\"type\": \"GET_STATUS\"}");

  // send req to open youtube
  send_request(self, MESSAGE_TYPE_RECEIVER, "{ \"type\": \"LAUNCH\", \"appId\": \"YouTube\", \"requestId\": 1 }");

  self->server = wfd_server_new ();
  self->server_source_id = gst_rtsp_server_attach (GST_RTSP_SERVER (self->server), NULL);

  if (self->server_source_id == 0 || self->remote_address == NULL)
    {
      self->state = ND_SINK_STATE_ERROR;
      g_object_notify (G_OBJECT (self), "state");
      g_clear_object (&self->server);

      return NULL;
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

  // these were originally here
  // 1. send connect request
  // 2. send ping

  return g_object_ref (sink);
}

static void
nd_cc_sink_sink_stop_stream_int (NdCCSink *self)
{
  g_autoptr(GError) error;
  gboolean close_ok;

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  self->cancellable = g_cancellable_new ();

  /* Close the client connection */
  if (self->comm_client_conn != NULL)
    {
      close_ok = g_io_stream_close (G_IO_STREAM (self->comm_client_conn), NULL, &error);
      if (error != NULL)
        {
          g_warning ("NdCCSink: Error closing communication client connection: %s", error->message);
        }
      if (!close_ok)
        {
          g_warning ("NdCCSink: Communication client connection not closed");
        }

      g_clear_object (&self->comm_client_conn);
      g_debug ("NdCCSink: Client connection removed");
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
nd_cc_sink_sink_stop_stream (NdSink *sink)
{
  NdCCSink *self = ND_CC_SINK (sink);

  nd_cc_sink_sink_stop_stream_int (self);

  self->state = ND_SINK_STATE_DISCONNECTED;
  g_object_notify (G_OBJECT (self), "state");
}

/******************************************************************
* NdCCSink public functions
******************************************************************/

NdCCSink *
nd_cc_sink_new (GSocketClient *client,
                gchar         *name,
                gchar         *remote_address)
{
  return g_object_new (ND_TYPE_CC_SINK,
                       "client", client,
                       "name", name,
                       "address", remote_address,
                       NULL);
}

NdSinkState
nd_cc_sink_get_state (NdCCSink *sink)
{
  return sink->state;
}
