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

// function decl
static void cc_comm_listen (CcComm *comm);
static void cc_comm_read (CcComm  *comm,
                          gsize    io_bytes,
                          gboolean read_header);


/* DEBUG HEX DUMP */

static void
cc_comm_dump_message (gchar *msg_head, guint8 *msg, gsize length)
{
#if 0
  g_autoptr(GString) line = NULL;

  g_debug ("CcComm: %s", msg_head);

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
#endif
}

static void
cc_comm_parse_received_data (CcComm *comm, uint8_t * input_buffer, gssize input_size)
{
  Cast__Channel__CastMessage *message;

  message = cast__channel__cast_message__unpack (NULL, input_size, input_buffer);
  if (message == NULL)
    {
      g_warning ("CcComm: Failed to unpack received data");
      return;
    }

  g_clear_pointer (&comm->message_buffer, g_free);

  comm->closure->message_received_cb (comm->closure, message);

  cast__channel__cast_message__free_unpacked (message, NULL);
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

// async callback for message read
static void
cc_comm_message_read_cb (GObject      *source_object,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  CcComm * comm = (CcComm *) user_data;

  g_autoptr(GError) error = NULL;
  gboolean success;
  gsize io_bytes;

  success = g_input_stream_read_all_finish (G_INPUT_STREAM (source_object), res, &io_bytes, &error);

  // If we cancelled, just return immediately.
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  g_assert (comm->con);
  g_assert (G_INPUT_STREAM (source_object) == g_io_stream_get_input_stream (G_IO_STREAM (comm->con)));

  if (!success || io_bytes == 0)
    {
      if (error)
        {
          g_error ("CcComm: Error reading message from stream: %s", error->message);
          cc_comm_listen (comm);
          return;
        }
      g_error ("CcComm: Error reading message from stream.");
      cc_comm_listen (comm);
      return;
    }

  /* dump the received message and try to parse it */
  cc_comm_dump_message ("Received message bytes:", comm->message_buffer, io_bytes);
  cc_comm_parse_received_data (comm, comm->message_buffer, io_bytes);

  cc_comm_listen (comm);
}

// async callback for header read
static void
cc_comm_header_read_cb (GObject      *source_object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  CcComm * comm = (CcComm *) user_data;

  g_autoptr(GError) error = NULL;
  gboolean success;
  gsize io_bytes;
  guint32 message_size;

  success = g_input_stream_read_all_finish (G_INPUT_STREAM (source_object), res, &io_bytes, &error);

  // If we cancelled, just return immediately.
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  // XXX: should we give up or keep on retrying if errors pop up
  /*
   * If this error is for an old connection (that should be closed already),
   * then just give up immediately with a CLOSED error.
   */
  if (comm->con &&
      g_io_stream_get_input_stream (G_IO_STREAM (comm->con)) != G_INPUT_STREAM (source_object))
    {
      g_error ("CcComm: Error on old read connection, ignoring.");
      cc_comm_listen (comm);
      return;
    }

  if (!success || io_bytes != 4)
    {
      if (error)
        {
          g_error ("CcComm: Error reading header from stream: %s", error->message);
          cc_comm_listen (comm);
          return;
        }
      g_error ("CcComm: Error reading header from stream.");
      cc_comm_listen (comm);
      return;
    }

  // if everything is well, read all `io_bytes`
  message_size = GINT32_FROM_BE (*(guint32 *) comm->header_buffer);
  g_debug ("CcComm: Message size: %d", message_size);

  g_clear_pointer (&comm->header_buffer, g_free);

  comm->message_buffer = g_malloc0 (message_size);
  cc_comm_read (comm, message_size, FALSE);
}

static void
cc_comm_read (CcComm *comm, gsize io_bytes, gboolean read_header)
{
  GInputStream *istream;

  istream = g_io_stream_get_input_stream (G_IO_STREAM (comm->con));

  if (read_header)
    {
      g_input_stream_read_all_async (istream,
                                     comm->header_buffer,
                                     io_bytes,
                                     G_PRIORITY_DEFAULT,
                                     comm->cancellable,
                                     cc_comm_header_read_cb,
                                     comm);
      return;
    }
  g_input_stream_read_all_async (istream,
                                 comm->message_buffer,
                                 io_bytes,
                                 G_PRIORITY_DEFAULT,
                                 comm->cancellable,
                                 cc_comm_message_read_cb,
                                 comm);
}

// listen to all incoming messages from Chromecast
static void
cc_comm_listen (CcComm *comm)
{
  comm->header_buffer = g_malloc0 (4);
  cc_comm_read (comm, 4, TRUE);
}

gboolean
cc_comm_make_connection (CcComm *comm, gchar *remote_address, GError **error)
{
  g_autoptr(GSocket) socket = NULL;
  GSocketType socket_type;
  GSocketFamily socket_family;
  GSocketConnectable * connectable;
  GIOStream *tls_conn;
  GSocketAddressEnumerator * enumerator;
  GSocketAddress * address = NULL;
  g_autoptr(GError) err = NULL;

  /* It is a programming error if ->con is not NULL at this point
   * i.e. when disconnecting con needs to be set to NULL!
   */
  g_assert (comm->con == NULL);

  socket_type = G_SOCKET_TYPE_STREAM;
  socket_family = G_SOCKET_FAMILY_IPV4;
  socket = g_socket_new (socket_family, socket_type, G_SOCKET_PROTOCOL_DEFAULT, error);
  if (socket == NULL)
    {
      g_warning ("CcComm: Failed to create socket: %s", (*error)->message);
      return FALSE;
    }

  // XXX
  // g_socket_set_timeout (socket, 10);

  connectable = g_network_address_parse (remote_address, 8009, error);
  if (connectable == NULL)
    {
      g_warning ("CcComm: Failed to create connectable: %s", (*error)->message);
      return FALSE;
    }

  enumerator = g_socket_connectable_enumerate (connectable);
  while (TRUE)
    {
      address = g_socket_address_enumerator_next (enumerator, comm->cancellable, error);
      if (address == NULL)
        {
          g_warning ("CcComm: Failed to create address: %s", (*error)->message);
          return FALSE;
        }

      if (g_socket_connect (socket, address, comm->cancellable, &err))
        break;

      // g_message ("Connection to %s failed: %s, trying next", socket_address_to_string (address), err->message);
      g_clear_error (&err);

      g_object_unref (address);
    }
  g_object_unref (enumerator);

  comm->con = G_IO_STREAM (g_socket_connection_factory_create_connection (socket));

  tls_conn = g_tls_client_connection_new (comm->con, connectable, error);
  if (tls_conn == NULL)
    {
      g_warning ("CcComm: Failed to create TLS connection: %s", (*error)->message);
      return FALSE;
    }

  g_signal_connect (tls_conn, "accept-certificate", G_CALLBACK (cc_comm_accept_certificate), NULL);
  g_object_unref (comm->con);

  comm->con = G_IO_STREAM (tls_conn);

  // see what should be done about cancellable
  if (!g_tls_connection_handshake (G_TLS_CONNECTION (tls_conn), comm->cancellable, error))
    {
      g_warning ("CcComm: Failed to handshake: %s", (*error)->message);
      return FALSE;
    }

  g_debug ("CcComm: Connected to %s", remote_address);

  // start listening to all incoming messages
  cc_comm_listen (comm);

  return TRUE;
}

void
cc_comm_close_connection (CcComm *comm)
{
  g_autoptr (GError) error = NULL;
  gboolean close_ok;

  if (comm->con != NULL)
    {
      close_ok = g_io_stream_close (G_IO_STREAM (comm->con), NULL, &error);
      if (!close_ok)
        {
          if (error != NULL)
            g_warning ("CcComm: Error closing communication client connection: %s", error->message);
          else
            g_warning ("CcComm: Error closing communication client connection");
        }

      g_clear_object (&comm->con);
      g_debug ("CcComm: Client connection removed");
    }
}

static gboolean
cc_comm_tls_send (CcComm  * comm,
                  uint8_t * message,
                  gssize    size,
                  GError  **error)
{
  GOutputStream *ostream;
  gssize io_bytes;

  if (!G_IS_TLS_CONNECTION (comm->con))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                           "Connection has not been established");
      return FALSE;
    }

  cc_comm_dump_message ("Sending message bytes:", message, size);

  ostream = g_io_stream_get_output_stream (G_IO_STREAM (comm->con));

  // start sending data synchronously
  while (size > 0)
    {
      io_bytes = g_output_stream_write (ostream, message, size, comm->cancellable, error);

      if (io_bytes <= 0)
        {
          g_warning ("CcComm: Failed to write: %s", (*error)->message);
          comm->closure->fatal_error_cb (comm->closure, error);
          g_clear_error (error);
          return FALSE;
        }

      size -= io_bytes;
    }

  return TRUE;
}

// builds message based on available types
static gboolean
cc_comm_build_message (Cast__Channel__CastMessage             *message,
                       gchar                                  *sender_id,
                       gchar                                  *destination_id,
                       CcMessageType                           message_type,
                       Cast__Channel__CastMessage__PayloadType payload_type,
                       ProtobufCBinaryData                    *binary_payload,
                       gchar                                  *utf8_payload)
{
  cast__channel__cast_message__init (message);

  message->protocol_version = CAST__CHANNEL__CAST_MESSAGE__PROTOCOL_VERSION__CASTV2_1_0;
  // pray we don't free these pointers before being used
  message->source_id = sender_id;
  message->destination_id = destination_id;

  switch (message_type)
    {
    case CC_MESSAGE_TYPE_AUTH:
      message->namespace_ = CC_NAMESPACE_AUTH;
      break;

    case CC_MESSAGE_TYPE_CONNECT:
    case CC_MESSAGE_TYPE_DISCONNECT:
      message->namespace_ = CC_NAMESPACE_CONNECTION;
      break;

    case CC_MESSAGE_TYPE_PING:
    case CC_MESSAGE_TYPE_PONG:
      message->namespace_ = CC_NAMESPACE_HEARTBEAT;
      break;

    case CC_MESSAGE_TYPE_RECEIVER:
      message->namespace_ = CC_NAMESPACE_RECEIVER;
      break;

    case CC_MESSAGE_TYPE_MEDIA:
      message->namespace_ = CC_NAMESPACE_MEDIA;
      break;

    case CC_MESSAGE_TYPE_WEBRTC:
      message->namespace_ = CC_NAMESPACE_WEBRTC;
      break;

    default:
      return FALSE;
    }

  message->payload_type = payload_type;
  switch (payload_type)
    {
    case CAST__CHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__BINARY:
      message->payload_binary = *binary_payload;
      message->has_payload_binary = 1;
      break;

    case CAST__CHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING:
      message->payload_utf8 = utf8_payload;
      message->has_payload_binary = 0;
      break;

    default:
      return FALSE;
    }

  return TRUE;
}

gboolean
cc_comm_send_request (CcComm       *comm,
                      gchar        *destination_id,
                      CcMessageType message_type,
                      gchar        *utf8_payload,
                      GError      **error)
{
  Cast__Channel__CastMessage message;
  guint32 packed_size = 0;
  g_autofree uint8_t *sock_buffer = NULL;

  switch (message_type)
    {
    // CAST__CHANNEL__CAST_MESSAGE__PROTOCOL_VERSION__CASTV2_1_3 allows for binary payloads over utf8
    case CC_MESSAGE_TYPE_AUTH:
      ProtobufCBinaryData binary_payload;
      binary_payload.data = NULL;
      binary_payload.len = 0;

      if (!cc_comm_build_message (&message,
                                  CC_DEFAULT_SENDER_ID,
                                  destination_id,
                                  message_type,
                                  CAST__CHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__BINARY,
                                  &binary_payload,
                                  NULL))
        {
          *error = g_error_new (1, 1, "Auth message building failed!");
          return FALSE;
        }
      break;

    default:
      if (!cc_comm_build_message (&message,
                                  CC_DEFAULT_SENDER_ID,
                                  destination_id,
                                  message_type,
                                  CAST__CHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
                                  NULL,
                                  utf8_payload))
        {
          *error = g_error_new (1, 1, "Message building failed for message type: %d", message_type);
          return FALSE;
        }
      break;
    }

  packed_size = cast__channel__cast_message__get_packed_size (&message);
  sock_buffer = malloc (4 + packed_size);

  guint32 packed_size_be = GUINT32_TO_BE (packed_size);

  memcpy (sock_buffer, &packed_size_be, 4);
  cast__channel__cast_message__pack (&message, 4 + sock_buffer);

  if (message_type != CC_MESSAGE_TYPE_PING
      && message_type != CC_MESSAGE_TYPE_PONG
      && message_type != CC_MESSAGE_TYPE_AUTH)
    {
      g_debug ("CcComm: Sending message:");
      cc_json_helper_dump_message (&message, FALSE);
    }

  return cc_comm_tls_send (comm,
                           sock_buffer,
                           packed_size + 4,
                           error);
}
