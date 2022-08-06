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

#include <math.h>

#include "cc-comm.h"
#include "cast_channel.pb-c.h"

// function decl
static void cc_comm_listen (CcComm *comm);
static void cc_comm_read (CcComm  *comm,
                          gsize    io_bytes,
                          gboolean read_header);


static gboolean
cc_comm_load_media_cb (CcComm *comm)
{
  if (!cc_comm_send_request (comm, MESSAGE_TYPE_MEDIA, "{ \"type\": \"LOAD\", \"media\": { \"contentId\": \"https://commondatastorage.googleapis.com/gtv-videos-bucket/CastVideos/mp4/BigBuckBunny.mp4\", \"streamType\": \"BUFFERED\", \"contentType\": \"video/mp4\" }, \"requestId\": 4 }", NULL))
    g_warning ("NdCCSink: something went wrong with load media");

  return FALSE;
}

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

static void
cc_comm_dump_json_message (Castchannel__CastMessage *message)
{
  g_debug ("{ source_id: %s, destination_id: %s, namespace_: %s, payload_type: %d, payload_utf8: %s }",
           message->source_id,
           message->destination_id,
           message->namespace_,
           message->payload_type,
           message->payload_utf8);
}

// returns FALSE if message is PONG
// returns TRUE if the message is to be logged
static gboolean
cc_comm_parse_json_data (CcComm *comm, char *payload)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(JsonReader) reader = NULL;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, payload, -1, &error))
    {
      g_warning ("NdCCSink: Error parsing received messaage JSON: %s", error->message);
      return TRUE;
    }

  reader = json_reader_new (json_parser_get_root (parser));

  json_reader_read_member (reader, "type");
  const char *message_type = json_reader_get_string_value (reader);
  json_reader_end_member (reader);

  if (g_strcmp0 (message_type, "PONG") == 0)
    return FALSE;

  if (g_strcmp0 (message_type, "RECEIVER_STATUS") == 0)
    {
      if (json_reader_read_member (reader, "status"))
        {
          if (json_reader_read_member (reader, "applications"))
            {
              if (json_reader_read_element (reader, 0))
                {
                  if (json_reader_read_member (reader, "appId"))
                    {
                      const char *app_id = json_reader_get_string_value (reader);
                      if (g_strcmp0 (app_id, "CC1AD845") == 0)
                        {
                          json_reader_end_member (reader);
                          json_reader_read_member (reader, "transportId");
                          const char *transport_id = json_reader_get_string_value (reader);
                          g_debug ("CcComm: Transport Id: %s!", transport_id);

                          // start a new virtual connection
                          comm->destination_id = g_strdup (transport_id);
                          g_debug ("CcComm: Sending second connect request");
                          if (!cc_comm_send_request (comm, MESSAGE_TYPE_CONNECT, NULL, NULL))
                            {
                              g_warning ("CcComm: Something went wrong with VC request for media");
                              return TRUE;
                            }
                          // call the LOAD media request after 2 seconds
                          g_timeout_add_seconds (2, G_SOURCE_FUNC (cc_comm_load_media_cb), comm);
                        }
                      json_reader_end_member (reader);
                    }
                  json_reader_end_element (reader);
                }
              json_reader_end_member (reader);
            }
          json_reader_end_member (reader);
        }
    }

  return TRUE;
}

static void
cc_comm_parse_received_data (CcComm *comm, uint8_t * input_buffer, gssize input_size)
{
  Castchannel__CastMessage *message;

  message = castchannel__cast_message__unpack (NULL, input_size, input_buffer);
  if (message == NULL)
    {
      g_warning ("CcComm: Failed to unpack received data");
      return;
    }

  if (cc_comm_parse_json_data (comm, message->payload_utf8))
    {
      g_debug ("CcComm: Received message:");
      cc_comm_dump_json_message (message);
    }

  castchannel__cast_message__free_unpacked (message, NULL);
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

static guint32
cc_comm_to_message_size (CcComm *comm)
{
  return GINT32_FROM_BE (*(guint32 *) comm->header_buffer);
}

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

  // XXX: should we give up or keep on retrying if errors pop up
  /*
   * If this error is for an old connection (that should be closed already),
   * then just give up immediately with a CLOSED error.
   */
  if (comm->con &&
      g_io_stream_get_input_stream (G_IO_STREAM (comm->con)) != G_INPUT_STREAM (source_object))
    {
      g_error ("CcComm: Error on old read connection, ignoring.");
      // cc_comm_listen (comm);
      return;
    }

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

  // dump the received message and try to parse it
  // cc_comm_dump_message (comm->message_buffer, io_bytes);
  cc_comm_parse_received_data (comm, comm->message_buffer, io_bytes);

  g_clear_pointer (&comm->message_buffer, g_free);

  // go for another round
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
  g_debug ("CcComm: Raw header dump:");
  cc_comm_dump_message (comm->header_buffer, 4);

  message_size = cc_comm_to_message_size (comm);
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

  // g_debug ("Writing data to Chromecast command channel:");
  // cc_comm_dump_message (message, size);

  ostream = g_io_stream_get_output_stream (G_IO_STREAM (comm->con));

  // start sending data synchronously
  while (size > 0)
    {
      io_bytes = g_output_stream_write (ostream, message, size, comm->cancellable, error);

      if (io_bytes <= 0)
        {
          g_warning ("CcComm: Failed to write: %s", (*error)->message);
          g_clear_error (error);
          return FALSE;
        }

      // g_debug ("CcComm: Sent %" G_GSSIZE_FORMAT " bytes", io_bytes);

      size -= io_bytes;
    }

  return TRUE;
}

// builds message based on available types
static Castchannel__CastMessage
cc_comm_build_message (gchar                                *namespace_,
                       Castchannel__CastMessage__PayloadType payload_type,
                       ProtobufCBinaryData                 * binary_payload,
                       gchar                                *utf8_payload)
{
  Castchannel__CastMessage message;

  castchannel__cast_message__init (&message);

  message.protocol_version = CASTCHANNEL__CAST_MESSAGE__PROTOCOL_VERSION__CASTV2_1_0;
  message.source_id = "sender-gnd";
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
cc_comm_send_request (CcComm * comm, enum MessageType message_type, char *utf8_payload, GError **error)
{
  Castchannel__CastMessage message;
  guint32 packed_size = 0;
  g_autofree uint8_t *sock_buffer = NULL;

  switch (message_type)
    {
    case MESSAGE_TYPE_AUTH:
      ProtobufCBinaryData binary_payload;
      binary_payload.data = NULL;
      binary_payload.len = 0;

      message = cc_comm_build_message (
        "urn:x-cast:com.google.cast.tp.deviceauth",
        CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__BINARY,
        &binary_payload,
        NULL);
      break;

    case MESSAGE_TYPE_CONNECT:
      message = cc_comm_build_message (
        "urn:x-cast:com.google.cast.tp.connection",
        CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
        NULL,
        // "{ \"type\": \"CONNECT\" }");
        "{ \"type\": \"CONNECT\", \"userAgent\": \"GND/0.90.5  (X11; Linux x86_64)\", \"connType\": 0, \"origin\": {}, \"senderInfo\": { \"sdkType\": 2, \"version\": \"X11; Linux x86_64\", \"browserVersion\": \"X11; Linux x86_64\", \"platform\": 6, \"connectionType\": 1 } }");
      message.destination_id = comm->destination_id;
      break;

    case MESSAGE_TYPE_DISCONNECT:
      message = cc_comm_build_message (
        "urn:x-cast:com.google.cast.tp.connection",
        CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
        NULL,
        "{ \"type\": \"CLOSE\" }");
      break;

    case MESSAGE_TYPE_PING:
      message = cc_comm_build_message (
        "urn:x-cast:com.google.cast.tp.heartbeat",
        CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
        NULL,
        "{ \"type\": \"PING\" }");
      break;

    case MESSAGE_TYPE_PONG:
      message = cc_comm_build_message (
        "urn:x-cast:com.google.cast.tp.heartbeat",
        CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
        NULL,
        "{ \"type\": \"PONG\" }");
      break;

    case MESSAGE_TYPE_RECEIVER:
      message = cc_comm_build_message (
        "urn:x-cast:com.google.cast.receiver",
        CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
        NULL,
        utf8_payload);
      break;

    case MESSAGE_TYPE_MEDIA:
      message = cc_comm_build_message (
        "urn:x-cast:com.google.cast.media",
        CASTCHANNEL__CAST_MESSAGE__PAYLOAD_TYPE__STRING,
        NULL,
        utf8_payload);
      message.destination_id = comm->destination_id;
      break;

    default:
      return FALSE;
    }

  packed_size = castchannel__cast_message__get_packed_size (&message);
  sock_buffer = malloc (4 + packed_size);

  guint32 packed_size_be = GUINT32_TO_BE (packed_size);

  memcpy (sock_buffer, &packed_size_be, 4);
  castchannel__cast_message__pack (&message, 4 + sock_buffer);

  if (message_type != MESSAGE_TYPE_PING && message_type != MESSAGE_TYPE_PONG)
    {
      g_debug ("CcComm: Sending message:");
      cc_comm_dump_json_message (&message);
    }

  return cc_comm_tls_send (comm,
                           sock_buffer,
                           packed_size + 4,
                           error);
}

gboolean
cc_comm_send_ping (CcComm * comm)
{
  g_autoptr(GError) error = NULL;

  // if this errors out, we cancel the periodic ping by returning FALSE
  if (!cc_comm_send_request (comm, MESSAGE_TYPE_PING, NULL, &error))
    {
      if (error != NULL)
        {
          g_warning ("CcComm: Failed to send ping message: %s", error->message);
          return G_SOURCE_REMOVE;
        }
      g_warning ("CcComm: Failed to send ping message");
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}
