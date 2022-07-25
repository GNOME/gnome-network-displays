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

#define MAX_MSG_SIZE 64 * 1024
// TODO: add cancellable everywhere

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
  GIOStream         *connection;

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

static void
dump_message (guint8 *msg, gsize length)
{
  g_autoptr(GString) line = NULL;

  line = g_string_new ("");
  /* Dump the buffer. */
  for (gint i = 0; i < length; i++)
    {
      g_string_append_printf (line, "%02x ", msg[i]);
      if ((i + 1) % 16 == 0)
        {
          g_debug ("%s", line->str);
          g_string_set_size (line, 0);
        }
    }

  if (line->len)
    g_debug ("%s", line->str);
}

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

  g_debug("NdCCSink: Received data:");
  g_debug("source_id: %s", message->source_id);
  g_debug("destination_id: %s", message->destination_id);
  g_debug("namespace_: %s", message->namespace_);
  g_debug("payload_type: %d", message->payload_type);
  g_debug("payload_utf8: %s", message->payload_utf8);

  castchannel__cast_message__free_unpacked(message, NULL);
}

static gboolean
accept_certificate (GTlsClientConnection *conn,
		    GTlsCertificate      *cert,
		    GTlsCertificateFlags  errors,
		    gpointer              user_data)
{
  g_print ("Certificate would have been rejected ( ");
  if (errors & G_TLS_CERTIFICATE_UNKNOWN_CA)
    g_print ("unknown-ca ");
  if (errors & G_TLS_CERTIFICATE_BAD_IDENTITY)
    g_print ("bad-identity ");
  if (errors & G_TLS_CERTIFICATE_NOT_ACTIVATED)
    g_print ("not-activated ");
  if (errors & G_TLS_CERTIFICATE_EXPIRED)
    g_print ("expired ");
  if (errors & G_TLS_CERTIFICATE_REVOKED)
    g_print ("revoked ");
  if (errors & G_TLS_CERTIFICATE_INSECURE)
    g_print ("insecure ");
  g_print (") but accepting anyway.\n");

  return TRUE;
}

static gboolean
make_connection (NdCCSink         * sink,
                 GSocket         ** socket,
                 GInputStream    ** istream,
                 GOutputStream   ** ostream,
                 GError          ** error)
{
  NdCCSink * self = ND_CC_SINK (sink);
  GSocketType socket_type;
  GSocketFamily socket_family;
  GSocketConnectable * connectable;
  GIOStream *tls_conn;
  GSocketAddressEnumerator * enumerator;
  GSocketAddress * address = NULL;
  GError * err = NULL;

  // return true if already connected
  if (*socket != NULL && G_IS_TLS_CONNECTION (self->connection))
    return TRUE;

  socket_type = G_SOCKET_TYPE_STREAM;
  socket_family = G_SOCKET_FAMILY_IPV4;
  *socket = g_socket_new (socket_family, socket_type, G_SOCKET_PROTOCOL_DEFAULT, error);
  if (*socket == NULL)
  {
    g_warning ("NdCCSink: Failed to create socket: %s", (*error)->message);
    return FALSE;
  }

  // XXX
  // g_socket_set_timeout (*socket, 10);

  connectable = g_network_address_parse (self->remote_address, 8009, error);
  if (connectable == NULL)
  {
    g_warning ("NdCCSink: Failed to create connectable: %s", (*error)->message);
    return FALSE;
  }

  enumerator = g_socket_connectable_enumerate (connectable);
  while (TRUE)
  {
    address = g_socket_address_enumerator_next (enumerator, NULL, error);
    if (address == NULL)
    {
      g_warning ("NdCCSink: Failed to create address: %s", (*error)->message);
      return FALSE;
    }

    if (g_socket_connect (*socket, address, NULL, &err))
      break;

    // g_message ("Connection to %s failed: %s, trying next", socket_address_to_string (address), err->message);
    g_clear_error (&err);

    g_object_unref (address);
  }
  g_object_unref (enumerator);

  // g_debug ("NdCCSink: Connected to %s",  (address));
  g_debug ("NdCCSink: Connected to %s", self->remote_address);

  self->connection = G_IO_STREAM (g_socket_connection_factory_create_connection (*socket));

  tls_conn = g_tls_client_connection_new (self->connection, connectable, error);
  if (tls_conn == NULL)
  {
    g_warning ("NdCCSink: Failed to create TLS connection: %s", (*error)->message);
    return FALSE;
  }

  g_signal_connect (tls_conn, "accept-certificate", G_CALLBACK (accept_certificate), NULL);
  g_object_unref (self->connection);

  self->connection = G_IO_STREAM (tls_conn);

  // see what should be done about cancellable
  if (!g_tls_connection_handshake (G_TLS_CONNECTION (tls_conn), NULL, error))
  {
    g_warning ("NdCCSink: Failed to handshake: %s", (*error)->message);
    return FALSE;
  }

  *istream = g_io_stream_get_input_stream (self->connection);
  *ostream = g_io_stream_get_output_stream (self->connection);

  g_debug ("NdCCSink: Connected to %s", self->remote_address);

  return TRUE;
}

gboolean
tls_send (NdCCSink      * sink,
          uint8_t       * message,
          gssize          size,
          gboolean        expect_input,
          GError        * error)
{
  NdCCSink * self = ND_CC_SINK (sink);
  GSocket * socket;
  GInputStream * istream;
  GOutputStream * ostream;
  gssize io_bytes;
  uint8_t buffer[MAX_MSG_SIZE];

  if (!make_connection (self, &socket, &istream, &ostream, &error))
  {
    g_warning ("NdCCSink: Failed to make connection: %s", error->message);
    g_error_free (error);
    return FALSE;
  }

  g_assert (G_IS_TLS_CONNECTION (self->connection));

  // start sending data
  g_debug ("Writing data:");
  dump_message (message, size);

  while (size > 0)
  {
    g_socket_condition_check (socket, G_IO_OUT);
    io_bytes = g_output_stream_write (ostream, message, size, NULL, &error);

    if (io_bytes <= 0)
    {
      g_warning ("NdCCSink: Failed to write: %s", error->message);
      g_error_free (error);
      return FALSE;
    }

    g_debug ("NdCCSink: Sent %" G_GSSIZE_FORMAT " bytes", io_bytes);

    size -= io_bytes;
  }

  if (!expect_input) return TRUE;

  g_socket_condition_check (socket, G_IO_IN);
  io_bytes = g_input_stream_read (istream, buffer, MAX_MSG_SIZE, NULL, &error);

  if (io_bytes <= 0)
  {
    g_warning ("NdCCSink: Failed to read: %s", error->message);
    g_error_free (error);
    return FALSE;
  }

  g_debug ("NdCCSink: Received %" G_GSSIZE_FORMAT " bytes", io_bytes);
  g_debug ("Received data:");
  dump_message (buffer, io_bytes);

  parse_received_data (buffer, io_bytes);

  return TRUE;
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
  message.source_id = "sender-0";
  message.destination_id = "receiver-0";
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
  guint32 packed_size = 0;
  gboolean expect_input = TRUE;
  g_autofree uint8_t *sock_buffer = NULL;

  g_debug("Send request: %d", message_type);

  switch (message_type)
  {
  case MESSAGE_TYPE_CONNECT:
    message = build_message(
      "urn:x-cast:com.google.cast.tp.connection",
      CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
      NULL,
      "{\"type\":\"CONNECT\"}");
    expect_input = FALSE;
    break;

  case MESSAGE_TYPE_DISCONNECT:
    message = build_message(
      "urn:x-cast:com.google.cast.tp.connection",
      CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
      NULL,
      "{ \"type\": \"CLOSE\" }");
    expect_input = FALSE;
    break;

  case MESSAGE_TYPE_PING:
    message = build_message(
      "urn:x-cast:com.google.cast.tp.heartbeat",
      CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
      NULL,
      "{ \"type\": \"PING\" }");
    break;

  case MESSAGE_TYPE_PONG:
    message = build_message(
      "urn:x-cast:com.google.cast.tp.heartbeat",
      CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
      NULL,
      "{ \"type\": \"PONG\" }");
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

  guint32 packed_size_be = GUINT32_TO_BE(packed_size);
  memcpy(sock_buffer, &packed_size_be, 4);
  castchannel__cast_message__pack(&message, 4 + sock_buffer);

  g_debug("Sending message to %s:%s", self->remote_address, self->remote_name);
  send_ok = tls_send (self,
                      sock_buffer,
                      packed_size+4,
                      expect_input,
                      error);

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
}

static void
closed_cb (NdCCSink *sink, CCClient *client)
{
  /* Connection was closed, do a clean shutdown */
  send_request(ND_CC_SINK (sink), MESSAGE_TYPE_DISCONNECT, NULL);

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

static NdSink *
nd_cc_sink_sink_start_stream (NdSink *sink)
{
  NdCCSink *self = ND_CC_SINK (sink);

  g_return_val_if_fail (self->state == ND_SINK_STATE_DISCONNECTED, NULL);

  g_assert (self->server == NULL);

  // self->state = ND_SINK_STATE_ENSURE_FIREWALL;
  // g_object_notify (G_OBJECT (self), "state");

  self->state = ND_SINK_STATE_WAIT_SOCKET;
  g_object_notify (G_OBJECT (self), "state");

  g_debug ("NdCCSink: Attempting connection to Chromecast: %s", self->remote_name);

  // send connection request to client
  // send_request(self, MESSAGE_TYPE_DISCONNECT, NULL);

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
  if (self->connection != NULL)
    {
      close_ok = g_io_stream_close (G_IO_STREAM (self->connection), NULL, &error);
      if (error != NULL)
        {
          g_warning ("NdCCSink: Error closing communication client connection: %s", error->message);
        }
      if (!close_ok)
        {
          g_warning ("NdCCSink: Communication client connection not closed");
        }

      g_clear_object (&self->connection);
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

// XXX: no use for client
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
