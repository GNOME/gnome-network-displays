/* cc-comm.c
 *
 * Copyright 2022 Anupam Kumar <kyteinsky@gmail.com>
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

#include "cc-comm.h"
#include "cast_channel.pb-c.h"

// function decl
void cc_comm_listen (NdCCSink *sink);


static void
cc_comm_dump_message (guint8 *msg, gsize length)
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

void
cc_comm_parse_received_data(uint8_t * input_buffer, gssize input_size)
{
  Castchannel__CastMessage *message;

  message = castchannel__cast_message__unpack(NULL, input_size-4, input_buffer+4);
  if (message == NULL)
  {
    g_warning ("CCComm: Failed to unpack received data");
    return;
  }

  g_debug("CCComm: Received data: { source_id: %s, destination_id: %s, namespace_: %s, payload_type: %d, payload_utf8: %s }",
           message->source_id,
           message->destination_id,
           message->namespace_,
           message->payload_type,
           message->payload_utf8);

  castchannel__cast_message__free_unpacked(message, NULL);
}

static gboolean
cc_comm_accept_certificate (GTlsClientConnection *conn,
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

// LISTENER

// async callback for input stream read
void
cc_comm_header_read_cb (GObject *source_object,
                        GAsyncResult *res,
                        gpointer user_data) 
{
  NdCCSink * self = ND_CC_SINK (user_data);
  g_autoptr(GError) error = NULL;
  gboolean success;
  gsize io_bytes;

  success = g_input_stream_read_all_finish (G_INPUT_STREAM (source_object), res, &io_bytes, &error);

  // If we cancelled, just return immediately.
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  /*
   * If this error is for an old connection (that should be closed already),
   * then just give up immediately with a CLOSED error.
   */
  if (self->connection &&
    g_io_stream_get_input_stream (G_IO_STREAM (self->connection)) != G_INPUT_STREAM (source_object))
  {
    g_error ("CCComm: Error on old read connection, ignoring.");
    cc_comm_listen (self);
    return;
  }

  if (!success || io_bytes != 4)
  {
    if (error)
    {
      g_error ("CCComm: Error reading from stream: %s", error->message);
      cc_comm_listen (self);
      return;
    }
    g_error ("CCComm: Error reading from stream, couldn't read 4 bytes header.");
    cc_comm_listen (self);
    return;
  }

  // if everything is well, read all `io_bytes`
  cc_comm_read (self, )
}

void
cc_comm_read (NdCCSink *sink, uint8_t *buffer, gsize io_bytes)
{
  NdCCSink * self = ND_CC_SINK (sink);
  GInputStream *istream;

  istream = g_io_stream_get_input_stream (G_IO_STREAM (self->connection))

  g_input_stream_read_all_async (istream,
                                 buffer,
                                 io_bytes,
                                 G_PRIORITY_DEFAULT,
                                 NULL,
                                 (*GAsyncReadyCallback) cc_comm_header_read_cb,
                                 self);
}

// listen to all incoming messages from Chromecast
void
cc_comm_listen (NdCCSink *sink)
{
  NdCCSink * self = ND_CC_SINK (sink);
  GInputStream *istream;
  gssize io_bytes;
  g_autofree uint8_t buffer[MAX_MSG_SIZE];
  g_autofree uint8_t header_buffer[4];

  cc_comm_read (self, header_buffer, 4)




  if (io_bytes <= 0)
  {
    g_warning ("CCComm: Failed to read: %s", error->message);
    g_error_free (error);
    return FALSE;
  }

  g_debug ("CCComm: Received %" G_GSSIZE_FORMAT " bytes", io_bytes);
  g_debug ("CCComm: Received data:");
  cc_comm_dump_message (buffer, io_bytes);

  cc_comm_parse_received_data (buffer, io_bytes);

}

static gboolean
cc_comm_make_connection (NdCCSink *sink, GError **error)
{
  NdCCSink * self = ND_CC_SINK (sink);
  g_autopr(GSocket) socket = NULL;
  GSocketType socket_type;
  GSocketFamily socket_family;
  GSocketConnectable * connectable;
  GIOStream *tls_conn;
  GSocketAddressEnumerator * enumerator;
  GSocketAddress * address = NULL;
  g_autoptr(GError) err = NULL;

  socket_type = G_SOCKET_TYPE_STREAM;
  socket_family = G_SOCKET_FAMILY_IPV4;
  socket = g_socket_new (socket_family, socket_type, G_SOCKET_PROTOCOL_DEFAULT, error);
  if (socket == NULL)
  {
    g_warning ("CCComm: Failed to create socket: %s", (*error)->message);
    return FALSE;
  }

  // XXX
  // g_socket_set_timeout (socket, 10);

  connectable = g_network_address_parse (self->remote_address, 8009, error);
  if (connectable == NULL)
  {
    g_warning ("CCComm: Failed to create connectable: %s", (*error)->message);
    return FALSE;
  }

  enumerator = g_socket_connectable_enumerate (connectable);
  while (TRUE)
  {
    address = g_socket_address_enumerator_next (enumerator, NULL, error);
    if (address == NULL)
    {
      g_warning ("CCComm: Failed to create address: %s", (*error)->message);
      return FALSE;
    }

    if (g_socket_connect (socket, address, NULL, &err))
      break;

    // g_message ("Connection to %s failed: %s, trying next", socket_address_to_string (address), err->message);
    g_clear_error (&err);

    g_object_unref (address);
  }
  g_object_unref (enumerator);

  g_debug ("CCComm: Connected to %s", self->remote_address);

  self->connection = G_IO_STREAM (g_socket_connection_factory_create_connection (socket));

  tls_conn = g_tls_client_connection_new (self->connection, connectable, error);
  if (tls_conn == NULL)
  {
    g_warning ("CCComm: Failed to create TLS connection: %s", (*error)->message);
    return FALSE;
  }

  g_signal_connect (tls_conn, "accept-certificate", G_CALLBACK (cc_comm_accept_certificate), NULL);
  g_object_unref (self->connection);

  self->connection = G_IO_STREAM (tls_conn);

  // see what should be done about cancellable
  if (!g_tls_connection_handshake (G_TLS_CONNECTION (tls_conn), NULL, error))
  {
    g_warning ("CCComm: Failed to handshake: %s", (*error)->message);
    return FALSE;
  }

  g_debug ("CCComm: Connected to %s", self->remote_address);

  // start listening to all incoming messages
  cc_comm_listen_all (self);

  return TRUE;
}

gboolean
cc_comm_ensure_connection (NdCCSink * sink, GError ** error)
{
  NdCCSink * self = ND_CC_SINK (sink);
  g_autoptr(GError) err = NULL;

  if (!G_IS_TLS_CONNECTION (self->connection) && !cc_comm_make_connection (self, &err))
  {
    g_warning ("CCComm: Failed to make connection: %s", err->message);
    g_propagate_error (error, g_steal_pointer (&err));
    return FALSE;
  }

  g_assert (G_IS_TLS_CONNECTION (self->connection));

  return TRUE;
}

gboolean
cc_comm_tls_send (NdCCSink      * sink,
                  uint8_t       * message,
                  gssize          size,
                  gboolean        expect_input,
                  GError        **error)
{
  NdCCSink * self = ND_CC_SINK (sink);
  GOutputStream *ostream;
  gssize io_bytes;

  if (!G_IS_TLS_CONNECTION (self->connection))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                           "Connection has not been established");
      return FALSE:
    }

  g_debug ("Writing data:");
  cc_comm_dump_message (message, size);

  ostream = g_io_stream_get_output_stream (G_IO_STREAM (self->connection))

  // start sending data synchronously
  while (size > 0)
  {
    io_bytes = g_output_stream_write (ostream, message, size, NULL, &error);

    if (io_bytes <= 0)
    {
      g_warning ("CCComm: Failed to write: %s", error->message);
      g_error_free (error);
      return FALSE;
    }

    g_debug ("CCComm: Sent %" G_GSSIZE_FORMAT " bytes", io_bytes);

    size -= io_bytes;
  }

  return TRUE;
}

// TODO: build strong connect messaage


// builds message based on available types
Castchannel__CastMessage
cc_comm_build_message (gchar *namespace_,
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

gboolean
cc_comm_send_request (NdCCSink *sink, enum MessageType message_type, char *utf8_payload)
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
    message = cc_comm_build_message(
      "urn:x-cast:com.google.cast.tp.connection",
      CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
      NULL,
      "{\"type\":\"CONNECT\"}");
    expect_input = FALSE;
    break;

  case MESSAGE_TYPE_DISCONNECT:
    message = cc_comm_build_message(
      "urn:x-cast:com.google.cast.tp.connection",
      CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
      NULL,
      "{ \"type\": \"CLOSE\" }");
    expect_input = FALSE;
    break;

  case MESSAGE_TYPE_PING:
    message = cc_comm_build_message(
      "urn:x-cast:com.google.cast.tp.heartbeat",
      CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
      NULL,
      "{ \"type\": \"PING\" }");
    break;

  case MESSAGE_TYPE_PONG:
    message = cc_comm_build_message(
      "urn:x-cast:com.google.cast.tp.heartbeat",
      CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
      NULL,
      "{ \"type\": \"PONG\" }");
    break;

  case MESSAGE_TYPE_RECEIVER:
    message = cc_comm_build_message(
      "urn:x-cast:com.google.cast.receiver",
      CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
      NULL,
      utf8_payload);
    break;

  default:
    return FALSE;
  }

  packed_size = castchannel__cast_message__get_packed_size(&message);
  sock_buffer = malloc(4 + packed_size);

  guint32 packed_size_be = GUINT32_TO_BE(packed_size);
  memcpy(sock_buffer, &packed_size_be, 4);
  castchannel__cast_message__pack(&message, 4 + sock_buffer);

  g_debug("Sending message to %s:%s", self->remote_address, self->remote_name);
  send_ok = cc_comm_tls_send (self,
                              sock_buffer,
                              packed_size+4,
                              expect_input,
                              &error);

  if (!send_ok || error != NULL)
  {
    if (error != NULL)
      g_warning ("CCComm: Failed to connect to Chromecast: %s", error->message);
    else
      g_warning ("CCComm: Failed to connect to Chromecast");

    self->state = ND_SINK_STATE_ERROR;
    g_object_notify (G_OBJECT (self), "state");
    g_clear_object (&self->server);
    return FALSE;
  }

  return TRUE;
}

gboolean
cc_comm_send_ping (gpointer userdata)
{
  NdCCSink *self = ND_CC_SINK (userdata);

  if (!cc_comm_ensure_connection(self, &error)) return FALSE;
  cc_comm_send_request(self, MESSAGE_TYPE_PING, NULL);

  return TRUE;
}
