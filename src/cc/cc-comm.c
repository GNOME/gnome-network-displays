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

/* function decl */
static void cc_comm_close_connection (CcComm *comm,
                                      GError *error);

/* DEBUG HEX DUMP */

static void
cc_comm_dump_message (gchar *msg_head, GByteArray *arr)
{
#if 0
  g_autoptr(GString) line = NULL;

  g_debug ("CcComm: %s", msg_head);

  line = g_string_new ("");
  /* Dump the buffer. */
  for (gint i = 0; i < arr->len; i++)
    {
      g_string_append_printf (line, "%02x ", arr->data[i]);
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
cc_comm_parse_received_data (CcComm *comm, GByteArray *arr)
{
  Cast__Channel__CastMessage *message;

  message = cast__channel__cast_message__unpack (NULL, arr->len, arr->data);
  if (message == NULL)
    {
      g_warning ("CcComm: Failed to unpack received data");
      return;
    }

  comm->closure->message_received_cb (comm->closure->userdata, message);

  cast__channel__cast_message__free_unpacked (message, NULL);
}

static gboolean
cc_comm_accept_certificate (GTlsClientConnection *conn,
                            GTlsCertificate      *cert,
                            GTlsCertificateFlags  errors,
                            gpointer              user_data)
{
  /* chromecast uses a self-signed certificate */
  if (errors & G_TLS_CERTIFICATE_UNKNOWN_CA || errors & G_TLS_CERTIFICATE_BAD_IDENTITY)
    return TRUE;

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
on_read_bytes (GPollableInputStream *istream, CcComm *comm)
{
  guint io_bytes;
  guint8 tmp_buffer[4096]; /* 4 KiB at a time */
  guint32 header_buffer;

  g_autoptr(GByteArray) data = NULL;
  g_autoptr(GError) err = NULL;

  data = g_byte_array_new ();

  io_bytes = g_pollable_input_stream_read_nonblocking (istream, &header_buffer, 4, comm->cancellable, &err);

  if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return FALSE;
  else if (io_bytes == 4)
    {
      guint32 message_size = GINT_FROM_BE (header_buffer);
      g_debug ("CcComm: Received message size: %d", message_size);
      do
        {
          io_bytes = g_pollable_input_stream_read_nonblocking (istream, tmp_buffer, sizeof (tmp_buffer), comm->cancellable, &err);
          if (io_bytes > 0)
            g_byte_array_append (data, tmp_buffer, io_bytes);
        }
      while (io_bytes > 0 && (message_size -= io_bytes) > 0);
    }
  else
    return FALSE;

  if (data->len == 0 || g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return FALSE;
  else if (data->len > 0 || g_error_matches (err, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
    {
      /* no more data to read */
      cc_comm_dump_message ("Received message bytes:", data);
      cc_comm_parse_received_data (comm, data);
      return TRUE;
    }
  else
    {
      GError *error_ = g_error_new (CC_ERROR,
                                    CC_ERROR_TLS_READ_FAILED,
                                    "TLS read error: %s",
                                    err ? err->message : "none");
      g_warning ("CcComm: %s", error_->message);
      cc_comm_close_connection (comm, error_);
      return FALSE;
    }

  return FALSE;
}

static gchar *
get_local_address_from_connection (GSocket *socket, GError **error)
{
  GInetSocketAddress *inet_sock_addr = NULL;

  g_autoptr(GInetAddress) inet_addr = NULL;

  inet_sock_addr = G_INET_SOCKET_ADDRESS (g_socket_get_local_address (socket, error));
  if (!inet_sock_addr)
    return NULL;

  inet_addr = g_inet_socket_address_get_address (inet_sock_addr);

  return g_inet_address_to_string (inet_addr);
}

static void
tls_handshake_cb (GObject      *source_object,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  GInputStream *istream;
  gboolean success;
  CcComm *comm;

  GTlsConnection *tls_conn = G_TLS_CONNECTION (source_object);

  success = g_tls_connection_handshake_finish (tls_conn, res, &error);

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  comm = (CcComm *) user_data;

  if (!success)
    {
      GError *error_ = g_error_new (CC_ERROR,
                                    CC_ERROR_HANDSHAKE_FAILED,
                                    "Failed to perform TLS handshake: %s",
                                    error ? error->message : "none");
      g_warning ("CcComm: %s", error_->message);
      cc_comm_close_connection (comm, error_);
      return;
    }

  istream = g_io_stream_get_input_stream (comm->con);
  comm->input_source = g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM (istream),
                                                              comm->cancellable);
  g_source_set_callback (comm->input_source, (GSourceFunc) on_read_bytes, comm, NULL);
  g_source_attach (comm->input_source, NULL);

  comm->closure->handshake_completed (comm->closure->userdata);
}

gboolean
cc_comm_make_connection (CcComm *comm, gchar *remote_address, GError **error)
{
  g_autoptr(GSocket) socket = NULL;
  GSocketType socket_type;
  GSocketFamily socket_family;
  GSocketConnectable * connectable;
  GIOStream *tls_conn;

  g_autoptr(GSocketAddressEnumerator) enumerator = NULL;

  /* we must be disconnected */
  g_assert (comm->con == NULL);

  socket_type = G_SOCKET_TYPE_STREAM;
  socket_family = G_SOCKET_FAMILY_IPV4;
  socket = g_socket_new (socket_family, socket_type, G_SOCKET_PROTOCOL_DEFAULT, error);
  if (socket == NULL)
    {
      g_warning ("CcComm: Failed to create socket");
      return FALSE;
    }

  connectable = g_network_address_parse (remote_address, 8009, error);
  if (connectable == NULL)
    {
      g_warning ("CcComm: Failed to create connectable");
      return FALSE;
    }

  enumerator = g_socket_connectable_enumerate (connectable);
  while (TRUE)
    {
      g_autoptr(GSocketAddress) address = g_socket_address_enumerator_next (enumerator, comm->cancellable, error);
      if (address == NULL)
        {
          g_warning ("CcComm: Failed to create address");
          return FALSE;
        }

      if (g_socket_connect (socket, address, comm->cancellable, error))
        break;
    }

  comm->con = G_IO_STREAM (g_socket_connection_factory_create_connection (socket));

  tls_conn = g_tls_client_connection_new (comm->con, connectable, error);
  if (tls_conn == NULL)
    {
      g_warning ("CcComm: Failed to create TLS connection:");
      return FALSE;
    }

  g_signal_connect (tls_conn, "accept-certificate", G_CALLBACK (cc_comm_accept_certificate), NULL);
  g_object_unref (comm->con);

  comm->con = G_IO_STREAM (tls_conn);

  g_debug ("CcComm: Connecting to: %s", remote_address);

  g_tls_connection_handshake_async (G_TLS_CONNECTION (tls_conn),
                                    G_PRIORITY_DEFAULT,
                                    comm->cancellable,
                                    tls_handshake_cb,
                                    comm);

  /* Local address should be available regardless of the handshake */
  comm->local_address = get_local_address_from_connection (socket, error);
  if (!comm->local_address)
    {
      g_warning ("CcComm: Some error occurred trying to fetch the local address");
      return FALSE;
    }
  g_debug ("CcComm: Local address: %s", comm->local_address);

  /* generate a unique sender id */
  comm->sender_id = g_strdup_printf ("%s%d", CC_DEFAULT_SENDER_ID, g_random_int_range (100, 1000));

  return TRUE;
}

static gboolean
cc_comm_tls_send (CcComm  * comm,
                  uint8_t * message,
                  gssize    size)
{
  GOutputStream *ostream;
  gssize io_bytes;

  g_autoptr(GError) err = NULL;

  if (g_cancellable_is_cancelled (comm->cancellable))
    return FALSE;

  if (!G_IS_TLS_CONNECTION (comm->con))
    {
      GError *error_ = g_error_new (CC_ERROR,
                                    CC_ERROR_NO_TLS_CONN,
                                    "TLS connection not found");
      g_warning ("CcComm: %s", error_->message);
      cc_comm_close_connection (comm, error_);
      return FALSE;
    }

  cc_comm_dump_message ("Sending message bytes:", g_byte_array_new_take (message, size));

  ostream = g_io_stream_get_output_stream (G_IO_STREAM (comm->con));

  /* start sending data synchronously */
  while (size > 0)
    {
      io_bytes = g_output_stream_write (ostream, message, size, comm->cancellable, &err);

      if (io_bytes <= 0)
        {
          GError *error_ = g_error_new (CC_ERROR,
                                        CC_ERROR_TLS_WRITE_FAILED,
                                        "Failed to write bytes in the stream: %s",
                                        err ? err->message : "none");
          g_warning ("CcComm: %s", error_->message);
          return FALSE;
        }

      size -= io_bytes;
    }

  return TRUE;
}

static void
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

    default:
      g_assert_not_reached ();
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
      g_assert_not_reached ();
    }
}

gboolean
cc_comm_send_request (CcComm       *comm,
                      gchar        *destination_id,
                      CcMessageType message_type,
                      gchar        *utf8_payload)
{
  Cast__Channel__CastMessage message;
  guint32 packed_size = 0;
  guint32 packed_size_be;
  g_autofree uint8_t *sock_buffer = NULL;

  switch (message_type)
    {
    /* CAST__CHANNEL__CAST_MESSAGE__PROTOCOL_VERSION__CASTV2_1_3 allows for binary payloads over utf8 */
    case CC_MESSAGE_TYPE_AUTH:
      {
        ProtobufCBinaryData binary_payload;
        binary_payload.data = NULL;
        binary_payload.len = 0;

        cc_comm_build_message (&message,
                               comm->sender_id,
                               destination_id,
                               message_type,
                               CAST__CHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__BINARY,
                               &binary_payload,
                               NULL);
        break;
      }

    default:
      cc_comm_build_message (&message,
                             comm->sender_id,
                             destination_id,
                             message_type,
                             CAST__CHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
                             NULL,
                             utf8_payload);
    }

  packed_size = cast__channel__cast_message__get_packed_size (&message);
  sock_buffer = malloc (4 + packed_size);

  packed_size_be = GUINT32_TO_BE (packed_size);

  memcpy (sock_buffer, &packed_size_be, 4);
  cast__channel__cast_message__pack (&message, 4 + sock_buffer);

  if (message_type != CC_MESSAGE_TYPE_PING &&
      message_type != CC_MESSAGE_TYPE_PONG &&
      message_type != CC_MESSAGE_TYPE_AUTH)
    {
      g_debug ("CcComm: Sending message:");
      cc_json_helper_dump_message (&message, FALSE);
    }

  return cc_comm_tls_send (comm,
                           sock_buffer,
                           packed_size + 4);
}

static void
cc_comm_close_connection (CcComm *comm, GError *error)
{
  if (comm->closure)
    comm->closure->error_close_connection_cb (comm->closure->userdata, error);
}

void
cc_comm_finish (CcComm *comm)
{
  g_autoptr(GError) error = NULL;
  g_debug ("CcComm: Finishing");

  g_clear_pointer (&comm->sender_id, g_free);
  g_clear_pointer (&comm->local_address, g_free);
  g_source_destroy (comm->input_source);

  /* close the socket connection */
  if (comm->con != NULL)
    {
      if (!g_io_stream_close (G_IO_STREAM (comm->con), NULL, &error))
        g_warning ("CcComm: Error closing communication client connection: %s",
                   error ? error->message : "none");

      g_clear_object (&comm->con);
    }
}
