/* cc-ctrl.c
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

#include "gnome-network-displays-config.h"
#include "cc-ctrl.h"
#include "cc-comm.h"

/* FUNCTION DECLS */

static void cc_ctrl_close_connection (CcCtrl *ctrl,
                                      GError *error);

/* SEND HELPER FUNCTIONS */

static gboolean
cc_ctrl_send_auth (CcCtrl *ctrl)
{
  if (!cc_comm_send_request (&ctrl->comm,
                             CC_DEFAULT_RECEIVER_ID,
                             CC_MESSAGE_TYPE_AUTH,
                             NULL))
    {
      GError *error_ = g_error_new (CC_ERROR,
                                    CC_ERROR_MESSAGE_SEND_FAILED,
                                    "Failed to send the auth message");
      g_warning ("CcCtrl: %s", error_->message);
      cc_ctrl_close_connection (ctrl, error_);
      return FALSE;
    }

  return TRUE;
}

static gboolean
cc_ctrl_send_connect (CcCtrl *ctrl, gchar *destination_id)
{
#ifdef __aarch64__
  gchar *platform = "Wayland; Linux aarch64";
#else
  gchar *platform = "Wayland; Linux x86_64";
#endif
  gchar *json = cc_json_helper_build_string (
    "type", CC_JSON_TYPE_STRING, "CONNECT",
    "userAgent", CC_JSON_TYPE_STRING, g_strdup_printf ("GND/%s (%s)", PACKAGE_VERSION, platform),
    "connType", CC_JSON_TYPE_INT, 0,
    "origin", CC_JSON_TYPE_OBJECT, cc_json_helper_build_node (NULL),
    "senderInfo", CC_JSON_TYPE_OBJECT, cc_json_helper_build_node (
      "sdkType", CC_JSON_TYPE_INT, 2,
      "version", CC_JSON_TYPE_STRING, platform,
      "browserVersion", CC_JSON_TYPE_STRING, platform,
      "platform", CC_JSON_TYPE_INT, 6,
      "connectionType", CC_JSON_TYPE_INT, 1,
      NULL),
    NULL);

  if (!cc_comm_send_request (&ctrl->comm,
                             destination_id,
                             CC_MESSAGE_TYPE_CONNECT,
                             json))
    {
      GError *error_ = g_error_new (CC_ERROR,
                                    CC_ERROR_MESSAGE_SEND_FAILED,
                                    "Failed to send the virtual connection message");
      g_warning ("CcCtrl: %s", error_->message);
      cc_ctrl_close_connection (ctrl, error_);
      return FALSE;
    }

  return TRUE;
}

static gboolean
cc_ctrl_send_disconnect (CcCtrl *ctrl, gchar *destination_id)
{
  gchar *json = cc_json_helper_build_string (
    "type", CC_JSON_TYPE_STRING, "CLOSE",
    NULL);

  if (!cc_comm_send_request (&ctrl->comm,
                             destination_id,
                             CC_MESSAGE_TYPE_DISCONNECT,
                             json))
    {
      GError *error_ = g_error_new (CC_ERROR,
                                    CC_ERROR_MESSAGE_SEND_FAILED,
                                    "Failed to send the disconnect message");
      g_warning ("CcCtrl: %s", error_->message);
      cc_ctrl_close_connection (ctrl, error_);
      return FALSE;
    }

  return TRUE;
}

static gboolean
cc_ctrl_send_get_status (CcCtrl *ctrl, gchar *destination_id)
{
  gchar *json = cc_json_helper_build_string (
    "type", CC_JSON_TYPE_STRING, "GET_STATUS",
    "requestId", CC_JSON_TYPE_INT, ctrl->request_id++,
    NULL);

  if (!cc_comm_send_request (&ctrl->comm,
                             destination_id,
                             CC_MESSAGE_TYPE_RECEIVER,
                             json))
    {
      GError *error_ = g_error_new (CC_ERROR,
                                    CC_ERROR_MESSAGE_SEND_FAILED,
                                    "Failed to send the get status message");
      g_warning ("CcCtrl: %s", error_->message);
      cc_ctrl_close_connection (ctrl, error_);
      return FALSE;
    }

  return TRUE;
}

static gboolean
cc_ctrl_send_get_app_availability (CcCtrl *ctrl, gchar *destination_id, gchar *appId)
{
  GArray *appIds = g_array_new (FALSE, FALSE, sizeof (gchar *));

  g_array_append_val (appIds, appId);

  gchar *json = cc_json_helper_build_string (
    "type", CC_JSON_TYPE_STRING, "GET_APP_AVAILABILITY",
    "appId", CC_JSON_TYPE_ARRAY_STRING, appIds,
    "requestId", CC_JSON_TYPE_INT, ctrl->request_id++,
    NULL);

  if (!cc_comm_send_request (&ctrl->comm,
                             destination_id,
                             CC_MESSAGE_TYPE_RECEIVER,
                             json))
    {
      GError *error_ = g_error_new (CC_ERROR,
                                    CC_ERROR_MESSAGE_SEND_FAILED,
                                    "Failed to send the app availability message");
      g_warning ("CcCtrl: %s", error_->message);
      cc_ctrl_close_connection (ctrl, error_);
      return FALSE;
    }

  return TRUE;
}

static gboolean
cc_ctrl_send_launch_app (CcCtrl *ctrl, gchar *appId)
{
  gchar *json = cc_json_helper_build_string (
    "type", CC_JSON_TYPE_STRING, "LAUNCH",
    "launguage", CC_JSON_TYPE_STRING, "en-US",
    "appId", CC_JSON_TYPE_STRING, appId,
    "requestId", CC_JSON_TYPE_INT, ctrl->request_id++,
    NULL);

  if (!cc_comm_send_request (&ctrl->comm,
                             CC_DEFAULT_RECEIVER_ID,
                             CC_MESSAGE_TYPE_RECEIVER,
                             json))
    {
      GError *error_ = g_error_new (CC_ERROR,
                                    CC_ERROR_MESSAGE_SEND_FAILED,
                                    "Failed to send launch app message");
      g_warning ("CcCtrl: %s", error_->message);
      cc_ctrl_close_connection (ctrl, error_);
      return FALSE;
    }

  return TRUE;
}

static gboolean
cc_ctrl_send_close_app (CcCtrl *ctrl, gchar *sessionId)
{
  gchar *json = cc_json_helper_build_string (
    "type", CC_JSON_TYPE_STRING, "STOP",
    "sessionId", CC_JSON_TYPE_STRING, sessionId,
    "requestId", CC_JSON_TYPE_INT, ctrl->request_id++,
    NULL);

  if (!cc_comm_send_request (&ctrl->comm,
                             CC_DEFAULT_RECEIVER_ID,
                             CC_MESSAGE_TYPE_RECEIVER,
                             json))
    {
      GError *error_ = g_error_new (CC_ERROR,
                                    CC_ERROR_MESSAGE_SEND_FAILED,
                                    "Failed to send close app message");
      g_warning ("CcCtrl: %s", error_->message);
      cc_ctrl_close_connection (ctrl, error_);
      return FALSE;
    }

  return TRUE;
}

static gboolean
cc_ctrl_send_load (CcCtrl *ctrl, gchar *sessionId)
{
  guint port;
  CcMediaFactory *factory;

  g_object_get (ctrl->http_server, "port", &port, NULL);
  factory = (CcMediaFactory *) ctrl->http_server;

  GArray *tracks = g_array_new (FALSE, FALSE, sizeof (JsonNode *));
  JsonNode *track_node = cc_json_helper_build_node (
    "trackContentType", CC_JSON_TYPE_STRING, content_types[cc_media_factory_profiles[factory->factory_profile].muxer],
    "trackId", CC_JSON_TYPE_INT, 1,
    "type", CC_JSON_TYPE_STRING, "VIDEO",
    NULL);

  g_array_append_val (tracks, track_node);

  gchar *json = cc_json_helper_build_string (
    "type", CC_JSON_TYPE_STRING, "LOAD",
    "media", CC_JSON_TYPE_OBJECT, cc_json_helper_build_node (
      "contentUrl", CC_JSON_TYPE_STRING, g_strdup_printf ("http://%s:%d/",
                                                          ctrl->comm.local_address,
                                                          port),
      "streamType", CC_JSON_TYPE_STRING, "LIVE",
      "contentType", CC_JSON_TYPE_STRING, content_types[cc_media_factory_profiles[factory->factory_profile].muxer],
      NULL),
    "requestId", CC_JSON_TYPE_INT, ctrl->request_id++,
    NULL);

  if (!cc_comm_send_request (&ctrl->comm,
                             sessionId,
                             CC_MESSAGE_TYPE_MEDIA,
                             json))
    {
      GError *error_ = g_error_new (CC_ERROR,
                                    CC_ERROR_MESSAGE_SEND_FAILED,
                                    "Failed to send load app message");
      g_warning ("CcCtrl: %s", error_->message);
      cc_ctrl_close_connection (ctrl, error_);
      return FALSE;
    }

  return TRUE;
}

static gboolean
cc_ctrl_send_ping (CcCtrl *ctrl)
{
  g_debug ("CcCtrl: Sending PING");
  gchar *json = cc_json_helper_build_string (
    "type", CC_JSON_TYPE_STRING, "PING",
    "requestId", CC_JSON_TYPE_INT, ctrl->request_id++,
    NULL);

  if (!cc_comm_send_request (&ctrl->comm,
                             CC_DEFAULT_RECEIVER_ID,
                             CC_MESSAGE_TYPE_PING,
                             json))
    {
      GError *error_ = g_error_new (CC_ERROR,
                                    CC_ERROR_MESSAGE_SEND_FAILED,
                                    "Failed to send ping message");
      g_warning ("CcCtrl: %s", error_->message);
      cc_ctrl_close_connection (ctrl, error_);
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

/* */

static gboolean
cc_ctrl_app_init (CcCtrl *ctrl, GError **error)
{
  /* a new virtual connection for the app */
  if (!cc_ctrl_send_connect (ctrl, ctrl->session_id))
    return FALSE;

  /* start the server before we send a request to the Chromecast */
  if (!cc_http_server_start_server (ctrl->http_server, error))
    return FALSE;

  if (!cc_ctrl_send_load (ctrl, ctrl->session_id))
    return FALSE;

  ctrl->state = CC_CTRL_STATE_STREAMING;

  return TRUE;
}

/* HANDLE MESSAGE */

static void
cc_ctrl_handle_get_app_availability (CcCtrl *ctrl, JsonReader *reader)
{
  g_autoptr(GError) error = NULL;

  if (json_reader_read_member (reader, "availability"))
    {
      if (json_reader_read_member (reader, CC_APP_ID))
        {
          const gchar *available = json_reader_get_string_value (reader);
          if (g_strcmp0 (available, "APP_AVAILABLE") == 0)
            {
              /* launch the app now */
              if (cc_ctrl_send_launch_app (ctrl, CC_APP_ID))
                return;
            }

          /* the app is not available */
          GError *error_ = g_error_new (CC_ERROR,
                                        CC_ERROR_APP_UNAVAILABLE,
                                        "%s app is not available on the Chromecast device",
                                        CC_APP_ID);
          g_warning ("CcCtrl: %s", error_->message);
          cc_ctrl_close_connection (ctrl, error_);
        }
    }
}

/* handler messages for received messages */
/* reports all the open apps (the relevant stuff) */
static void
cc_ctrl_handle_receiver_status (CcCtrl *ctrl, JsonParser *parser)
{
  g_autoptr(JsonNode) app_status = NULL;
  g_autoptr(JsonPath) path = json_path_new ();

  gchar *parsed_payload;

  json_path_compile (path, "$.status.applications", NULL);
  app_status = json_path_match (path, json_parser_get_root (parser));

  g_autoptr(JsonGenerator) generator = json_generator_new ();
  json_generator_set_root (generator, app_status);

  parsed_payload = json_generator_to_data (generator, NULL);

  /* applications key not found = []
   * applications key found but empty = [[]]
   */
  if (g_str_equal (parsed_payload, "[]") || g_str_equal (parsed_payload, "[[]]"))
    {
      g_debug ("CcCtrl: No apps open");
      if (ctrl->state >= CC_CTRL_STATE_LAUNCH_SENT)
        return;

      if (!cc_ctrl_send_launch_app (ctrl, CC_APP_ID))
        return;

      ctrl->state = CC_CTRL_STATE_LAUNCH_SENT;
      return;
    }

  /* one or more apps is/are open */
  g_autoptr(JsonReader) reader = json_reader_new (app_status);

  if (json_reader_read_element (reader, 0) && json_reader_read_element (reader, 0))
    {
      if (json_reader_read_member (reader, "appId"))
        {
          const gchar *appId = json_reader_get_string_value (reader);
          json_reader_end_member (reader);

          if (json_reader_read_member (reader, "sessionId"))
            {
              gchar *sessionId = (gchar *) json_reader_get_string_value (reader); /* pointer can be modified */
              g_debug ("CcCtrl: Session id for app %s: %s", appId, sessionId);
              json_reader_end_member (reader);

              if (g_strcmp0 (appId, CC_APP_ID) == 0)
                {
                  GError *error = NULL;

                  g_debug ("CcCtrl: App is open");
                  ctrl->state = CC_CTRL_STATE_APP_OPEN;
                  ctrl->session_id = g_strdup (sessionId);

                  if (!cc_ctrl_app_init (ctrl, &error))
                    {
                      if (!error)
                        {
                          error = g_error_new (CC_ERROR,
                                               CC_ERROR_APP_COMMUNICATION_FAILED,
                                               "Could not initialize \"%s\" app",
                                               CC_APP_ID);
                        }
                      g_warning ("CcCtrl: %s", error->message);
                      cc_ctrl_close_connection (ctrl, error);
                      return;
                    }

                  ctrl->state = CC_CTRL_STATE_READY;
                  return;
                }

              /* some other app is open, check if `CC_APP_ID` is available */
              if (!cc_ctrl_send_get_app_availability (ctrl, CC_DEFAULT_RECEIVER_ID, CC_APP_ID))
                return;
            }
        }
    }
}

static void
cc_ctrl_handle_media_status (CcCtrl *ctrl, Cast__Channel__CastMessage *message, JsonReader *reader)
{
}

static void
cc_ctrl_handle_close (CcCtrl *ctrl, Cast__Channel__CastMessage *message)
{
  if (g_strcmp0 (message->source_id, CC_DEFAULT_RECEIVER_ID) == 0)
    {
      GError *error_ = g_error_new (CC_ERROR,
                                    CC_ERROR_HTTP_SERVER_LISTEN_FAILED,
                                    "Receiver closed the connection");
      g_warning ("CcCtrl: %s", error_->message);
      cc_ctrl_close_connection (ctrl, error_);
      return;
    }

  /* the app closed */
  g_debug ("CcCtrl: App sent a close message, launching again");
  cc_ctrl_send_launch_app (ctrl, CC_APP_ID);
}

void
cc_ctrl_handle_received_msg (gpointer                    userdata,
                             Cast__Channel__CastMessage *message)
{
  CcCtrl *ctrl = (CcCtrl *) userdata;

  GError *error_;

  g_autoptr(GError) error = NULL;
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(JsonReader) reader = NULL;
  CcReceivedMessageTypeEnum type;

  if (message->payload_utf8 == NULL)
    {
      g_warning ("CcCtrl: Received message with no payload");
      cc_json_helper_dump_message (message, TRUE);
      return;
    }

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, message->payload_utf8, -1, &error))
    {
      g_warning ("CcCtrl: Error parsing received messaage JSON: %s",
                 error ? error->message : "none");
      cc_json_helper_dump_message (message, TRUE);
      return;
    }

  reader = json_reader_new (json_parser_get_root (parser));
  type = cc_json_helper_get_message_type (message, reader);

  if (!(type == CC_REC_MSG_TYPE_PING || type == CC_REC_MSG_TYPE_PONG))
    {
      g_debug ("CcComm: Received message:");
      cc_json_helper_dump_message (message, FALSE);
    }

  switch (type)
    {
    case CC_REC_MSG_TYPE_RECEIVER_STATUS:
      cc_ctrl_handle_receiver_status (ctrl, parser);
      break;

    case CC_REC_MSG_TYPE_GET_APP_AVAILABILITY:
      cc_ctrl_handle_get_app_availability (ctrl, reader);
      break;

    case CC_REC_MSG_TYPE_LAUNCH_ERROR:
      error_ = g_error_new (CC_ERROR,
                            CC_ERROR_LAUNCH_ERROR_MSG_RECEIVED,
                            "Failed to launch app");
      g_warning ("CcCtrl: %s", error_->message);
      cc_ctrl_close_connection (ctrl, error_);
      break;

    /* more info on what each error code stands for are listed here:
       https://developers.google.com/cast/docs/web_receiver/error_codes */
    /* TODO: parse and return error code in error struct */
    case CC_REC_MSG_TYPE_ERROR:
      error_ = g_error_new (CC_ERROR,
                            CC_ERROR_ERROR_MSG_RECEIVED,
                            "Some error occurred");
      g_warning ("CcCtrl: %s", error_->message);
      cc_ctrl_close_connection (ctrl, error_);
      break;

    case CC_REC_MSG_TYPE_LOAD_FAILED:
      error_ = g_error_new (CC_ERROR,
                            CC_ERROR_LOAD_FAILED_MSG_RECEIVED,
                            "Media load failed");
      g_warning ("CcCtrl: %s", error_->message);
      cc_ctrl_close_connection (ctrl, error_);
      break;

    case CC_REC_MSG_TYPE_MEDIA_STATUS:
      cc_ctrl_handle_media_status (ctrl, message, reader);
      break;

    case CC_REC_MSG_TYPE_PONG:
      break;

    case CC_REC_MSG_TYPE_CLOSE:
      cc_ctrl_handle_close (ctrl, message);
      break;

    case CC_REC_MSG_TYPE_UNKNOWN:
    default:
      g_debug ("CcCtrl: Unknown message type");
      break;
    }
}

static void
cc_ctrl_close_connection (CcCtrl *ctrl, GError *error)
{
  /* avoid multiple calls */
  if (ctrl->state == CC_CTRL_STATE_ERROR)
    return;

  ctrl->closure->end_stream (ctrl->closure->userdata, error);
  ctrl->state = CC_CTRL_STATE_ERROR;
}

void
cc_ctrl_error_close_connection_cb (gpointer userdata, GError *error)
{
  CcCtrl *ctrl = (CcCtrl *) userdata;

  cc_ctrl_close_connection (ctrl, error);
}

void
cc_ctrl_handshake_completed (gpointer userdata)
{
  CcCtrl *ctrl = (CcCtrl *) userdata;

  if (!cc_ctrl_send_auth (ctrl))
    return;

  if (!cc_ctrl_send_connect (ctrl, CC_DEFAULT_RECEIVER_ID))
    return;

  /* since all network write calls are synchronous */
  ctrl->state = CC_CTRL_STATE_CONNECTED;

  /* keeps the connection alive */
  ctrl->ping_timeout_handle = g_timeout_add_seconds (5, G_SOURCE_FUNC (cc_ctrl_send_ping), ctrl);

  /* we can skip some message interchange if the mirroring app is already up */
  if (!cc_ctrl_send_get_status (ctrl, CC_DEFAULT_RECEIVER_ID))
    return;
}

static void
register_all_closures (CcCtrl *ctrl)
{
  CcCommClosure *comm_closure = (CcCommClosure *) g_malloc (sizeof (CcCommClosure));

  comm_closure->userdata = ctrl;
  comm_closure->message_received_cb = cc_ctrl_handle_received_msg;
  comm_closure->handshake_completed = cc_ctrl_handshake_completed;
  comm_closure->error_close_connection_cb = cc_ctrl_error_close_connection_cb;

  ctrl->comm.closure = comm_closure;
}

gboolean
cc_ctrl_connection_init (CcCtrl *ctrl, gchar *remote_address)
{
  g_autoptr(GError) error = NULL;

  assert (ctrl->state == CC_CTRL_STATE_DISCONNECTED);

  register_all_closures (ctrl);

  if (!cc_comm_make_connection (&ctrl->comm, remote_address, &error))
    {
      if (error != NULL)
        g_warning ("CcCtrl: Failed to make connection to %s: %s", remote_address, error->message);
      else
        g_warning ("CcCtrl: Failed to make connection to %s", remote_address);
      return FALSE;
    }

  return TRUE;
}

void
cc_ctrl_finish (CcCtrl *ctrl)
{
  if (ctrl->state == CC_CTRL_STATE_DISCONNECTED || ctrl->state == CC_CTRL_STATE_ERROR)
    return;

  g_debug ("CcCtrl: Finishing");

  g_clear_handle_id (&ctrl->ping_timeout_handle, g_source_remove);

  /* close app if open */
  if (ctrl->state >= CC_CTRL_STATE_APP_OPEN)
    {
      if (!cc_ctrl_send_close_app (ctrl, ctrl->session_id))
        g_warning ("CcCtrl: Error closing app");
      g_clear_pointer (&ctrl->session_id, g_free);
    }

  /* close the virtual connection */
  cc_ctrl_send_disconnect (ctrl, CC_DEFAULT_RECEIVER_ID);

  g_cancellable_cancel (ctrl->cancellable);
  g_clear_object (&ctrl->cancellable);

  /* cleanup comm */
  cc_comm_finish (&ctrl->comm);
  g_clear_pointer (&ctrl->comm.closure, g_free);

  ctrl->state = CC_CTRL_STATE_DISCONNECTED;
}
