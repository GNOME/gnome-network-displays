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

#include "cc-ctrl.h"
#include "cc-comm.h"

// SEND HELPER FUNCTIONS

static gboolean
cc_ctrl_send_auth (CcCtrl *ctrl, GError **error)
{
  g_debug ("CcCtrl: Sending auth");

  return cc_comm_send_request (&ctrl->comm,
                               CC_DEFAULT_RECEIVER_ID,
                               CC_MESSAGE_TYPE_AUTH,
                               NULL,
                               error);
}

static gboolean
cc_ctrl_send_connect (CcCtrl *ctrl, gchar *destination_id, GError **error)
{
  g_debug ("CcCtrl: Sending CONNECT");

  return cc_comm_send_request (&ctrl->comm,
                               destination_id,
                               CC_MESSAGE_TYPE_CONNECT,
                               "{ \"type\": \"CONNECT\", \"userAgent\": \"GND/0.90.5  (X11; Linux x86_64)\", \"connType\": 0, \"origin\": {}, \"senderInfo\": { \"sdkType\": 2, \"version\": \"X11; Linux x86_64\", \"browserVersion\": \"X11; Linux x86_64\", \"platform\": 6, \"connectionType\": 1 } }",
                               error);
}

static gboolean
cc_ctrl_send_disconnect (CcCtrl *ctrl, gchar *destination_id, GError **error)
{
  g_debug ("CcCtrl: Sending CLOSE");

  return cc_comm_send_request (&ctrl->comm,
                               destination_id,
                               CC_MESSAGE_TYPE_DISCONNECT,
                               "{ \"type\": \"CLOSE\" }",
                               error);
}

static gboolean
cc_ctrl_send_get_status (CcCtrl *ctrl, gchar *destination_id, GError **error)
{
  g_debug ("CcCtrl: Sending GET_STATUS");

  g_autoptr (GString) json = g_string_new ("{ \"type\": \"GET_STATUS\", ");
  g_string_append_printf (json, "\"requestId\": %d }", ctrl->request_id++);
  return cc_comm_send_request (&ctrl->comm,
                               destination_id,
                               CC_MESSAGE_TYPE_RECEIVER,
                               json->str,
                               error);
}

static gboolean
cc_ctrl_send_get_app_availability (CcCtrl *ctrl, gchar *destination_id, gchar *appId, GError **error)
{
  g_debug ("CcCtrl: Sending GET_APP_AVAILABILITY");

  g_autoptr (GString) json = g_string_new ("{ \"type\": \"GET_APP_AVAILABILITY\", ");
  g_string_append_printf (json, "\"appId\": [\"%s\"], \"requestId\": %d }", appId, ctrl->request_id++);
  return cc_comm_send_request (&ctrl->comm,
                               destination_id,
                               CC_MESSAGE_TYPE_RECEIVER,
                               json->str,
                               error);
}

static gboolean
cc_ctrl_send_launch_app (CcCtrl *ctrl, gchar *destination_id, gchar *appId, GError **error)
{
  g_debug ("CcCtrl: Sending LAUNCH");

  g_autoptr (GString) json = g_string_new ("{ \"type\": \"LAUNCH\", \"language\": \"en-US\", ");
  g_string_append_printf (json, "\"appId\": \"%s\", \"requestId\": %d }", appId, ctrl->request_id++);
  return cc_comm_send_request (&ctrl->comm,
                               destination_id,
                               CC_MESSAGE_TYPE_RECEIVER,
                               json->str,
                               error);
}

static gboolean
cc_ctrl_send_close_app (CcCtrl *ctrl, gchar *sessionId, GError **error)
{
  g_debug ("CcCtrl: Sending STOP");

  g_autoptr (GString) json = g_string_new ("{ \"type\": \"STOP\", ");
  g_string_append_printf (json, "\"sessionId\": \"%s\", \"requestId\": %d }", sessionId, ctrl->request_id++);
  return cc_comm_send_request (&ctrl->comm,
                               sessionId,
                               CC_MESSAGE_TYPE_RECEIVER,
                               json->str,
                               error);
}

static gboolean
cc_ctrl_send_offer (CcCtrl *ctrl, gchar *destination_id, GError **error)
{
  g_debug ("CcCtrl: Sending OFFER");

  /* look into [ adaptive_playout_delay, rtpExtensions, rtpPayloadType, rtpProfile, aes stuff, ssrc increment in received msg ] */
  return cc_comm_send_request (&ctrl->comm,
                               destination_id,
                               CC_MESSAGE_TYPE_WEBRTC,
                               "{ \"offer\": { \"castMode\": \"mirroring\", \"receiverGetStatus\": true, \"supportedStreams\": [ { \"aesIvMask\": \"1D20EA1C710E5598ECF80FB26ABC57B0\", \"aesKey\": \"BB0CAE24F76EA1CAC9A383CFB1CFD54E\", \"bitRate\": 102000, \"channels\": 2, \"codecName\": \"aac\", \"index\": 0, \"receiverRtcpEventLog\": true, \"rtpExtensions\": \"adaptive_playout_delay\", \"rtpPayloadType\": 127, \"rtpProfile\": \"cast\", \"sampleRate\": 48000, \"ssrc\": 144842, \"targetDelay\": 400, \"timeBase\": \"1/48000\", \"type\": \"audio_source\" }, { \"aesIvMask\": \"1D20EA1C710E5598ECF80FB26ABC57B0\", \"aesKey\": \"BB0CAE24F76EA1CAC9A383CFB1CFD54E\", \"codecName\": \"h264\", \"index\": 1, \"maxBitRate\": 5000000, \"maxFrameRate\": \"30000/1000\", \"receiverRtcpEventLog\": true, \"renderMode\": \"video\", \"resolutions\": [{ \"height\": 1080, \"width\": 1920 }], \"rtpExtensions\": \"adaptive_playout_delay\", \"rtpPayloadType\": 96, \"rtpProfile\": \"cast\", \"ssrc\": 545579, \"targetDelay\": 400, \"timeBase\": \"1/90000\", \"type\": \"video_source\" } ] }, \"seqNum\": 730137397, \"type\": \"OFFER\" }",
                               error);
}

// WAITING FOR

static void
cc_ctrl_set_waiting_for (CcCtrl *ctrl, CcWaitingFor waiting_for)
{
  ctrl->waiting_for |= waiting_for;
}

static void
cc_ctrl_unset_waiting_for (CcCtrl *ctrl, CcWaitingFor waiting_for)
{
  ctrl->waiting_for &= ~waiting_for;
}

static gboolean
cc_ctrl_is_waiting_for (CcCtrl *ctrl, CcWaitingFor waiting_for)
{
  return (ctrl->waiting_for & waiting_for) > CC_RWAIT_TYPE_NONE;
}

// INTERVAL FUNCTIONS

// if we are waiting for something for longer than said interval, then our connection has some problem
static gboolean
cc_ctrl_check_waiting_for (CcCtrl *ctrl)
{
  if (ctrl->waiting_for == CC_RWAIT_TYPE_NONE)
    return G_SOURCE_CONTINUE;

  g_warning ("CcCtrl: Timed out waiting for %d", ctrl->waiting_for);
  ctrl->closure->end_stream (ctrl->closure);

  return G_SOURCE_REMOVE;
}

static gboolean
cc_ctrl_send_ping (CcCtrl *ctrl)
{
  g_debug ("CcCtrl: Sending PING");
  g_autoptr(GError) error = NULL;

  // if this errors out, we cancel the periodic ping by returning FALSE
  if (!cc_comm_send_request (&ctrl->comm,
                             CC_DEFAULT_RECEIVER_ID,
                             CC_MESSAGE_TYPE_PING,
                             NULL,
                             &error))
    {
      if (error != NULL)
        {
          g_warning ("CcCtrl: Failed to send ping message: %s", error->message);
          return G_SOURCE_REMOVE;
        }
      g_warning ("CcCtrl: Failed to send ping message");
      return G_SOURCE_REMOVE;
    }

  cc_ctrl_set_waiting_for (ctrl, CC_RWAIT_TYPE_PONG);

  return G_SOURCE_CONTINUE;
}

static gboolean
cc_ctrl_send_gaa_cb (CcCtrl *ctrl)
{
  g_autoptr (GError) error = NULL;
  if (!cc_ctrl_send_get_app_availability (ctrl, CC_DEFAULT_RECEIVER_ID, CC_MIRRORING_APP_ID, &error))
    g_warning ("CcCtrl: Failed to send GET_APP_AVAILABILITY to the mirroring app: %s", error->message);
  return FALSE;
}

static void
cc_ctrl_mirroring_app_init (CcCtrl *ctrl, GError **error)
{
  if (!cc_ctrl_send_connect (ctrl, ctrl->session_id, error))
    {
      g_warning ("CcCtrl: Failed to send CONNECT to the mirroring app: %s", (*error)->message);
      return;
    }

  // send get_app_availability message after 2 seconds
  g_timeout_add_seconds (2, G_SOURCE_FUNC (cc_ctrl_send_gaa_cb), ctrl);
}

// HANDLE MESSAGE

// should be status received callback
static void
cc_ctrl_handle_get_app_availability (CcCtrl *ctrl, JsonReader *reader)
{
  g_autoptr (GError) error = NULL;

  // TODO: reader
  if (!cc_ctrl_send_offer (ctrl, ctrl->session_id, &error))
    {
      g_warning ("CcCtrl: Failed to send offer: %s", error->message);
      return;
    }
}

// handler messages for received messages
static void
cc_ctrl_handle_receiver_status (CcCtrl *ctrl, JsonParser *parser)
{
  // reports all the open apps (the relevant stuff)
  // if the app is open, it has a sessionId: hijack the session
  // connect to it, send a stop, and then propose an offer

  g_autoptr (GError) error = NULL;
	g_autoptr (JsonNode) app_status = NULL;
	g_autoptr (JsonPath) path = json_path_new();
	json_path_compile(path, "$.status.applications[0]", NULL);
	app_status = json_path_match(path, json_parser_get_root(parser));

	g_autoptr (JsonGenerator) generator = json_generator_new();
	json_generator_set_root(generator, app_status);
  gsize size;
	json_generator_to_data(generator, &size);

  if (size == 2) // empty array []
    {
      g_debug ("CcCtrl: No apps open");
      if (ctrl->state == CC_CTRL_STATE_LAUNCH_SENT)
        return;

      if (ctrl->state >= CC_CTRL_STATE_APP_OPEN) // app closed unexpectedly
        g_debug ("CcCtrl: App closed unexpectedly");

      if (!cc_ctrl_send_launch_app (ctrl, CC_DEFAULT_RECEIVER_ID, CC_MIRRORING_APP_ID, &error))
        {
          g_warning ("CcCtrl: Failed to launch the app: %s", error->message);
          return;
        }

      cc_ctrl_set_waiting_for (ctrl, CC_RWAIT_TYPE_RECEIVER_STATUS);
      ctrl->state = CC_CTRL_STATE_LAUNCH_SENT;
      return;
    }

  // one or more apps is/are open
  g_autoptr (JsonReader) reader = json_reader_new(app_status);

  if (json_reader_read_element (reader, 0))
    {
      if (json_reader_read_member (reader, "appId"))
        {
          const gchar *appId = json_reader_get_string_value (reader);
          json_reader_end_member (reader);

          if (json_reader_read_member (reader, "sessionId"))
            {
              const gchar *sessionId = json_reader_get_string_value (reader); /* pointer can be modified */
              g_debug ("CcCtrl: Session id for app %s: %s", appId, sessionId);
              json_reader_end_member (reader);

              if (g_strcmp0 (appId, CC_MIRRORING_APP_ID) == 0)
                {
                  // takeover the session, doesn't matter which sender opened it
                  g_debug ("CcCtrl: Mirroring app is open,");
                  ctrl->state = CC_CTRL_STATE_APP_OPEN;
                  /* is this freed automatically? */
                  ctrl->session_id = g_strdup (sessionId);

                  cc_ctrl_mirroring_app_init (ctrl, &error);
                  cc_ctrl_set_waiting_for (ctrl, CC_RWAIT_TYPE_GET_APP_AVAILABILITY);

                  return;
                }

              if (!cc_ctrl_send_launch_app (ctrl, CC_DEFAULT_RECEIVER_ID, CC_MIRRORING_APP_ID, &error))
                {
                  g_warning ("CcCtrl: Failed to launch the app: %s", error->message);
                  return;
                }

              cc_ctrl_set_waiting_for (ctrl, CC_RWAIT_TYPE_RECEIVER_STATUS);
              ctrl->state = CC_CTRL_STATE_LAUNCH_SENT;
            }
        }
    }
}

void
cc_ctrl_handle_received_msg (CcCommClosure *closure,
                             Cast__Channel__CastMessage *message)
{
  CcCtrl *ctrl = (CcCtrl *) closure->userdata;
  g_autoptr(GError) error = NULL;
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(JsonReader) reader = NULL;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, message->payload_utf8, -1, &error))
    {
      cc_json_helper_dump_message (message);
      g_warning ("CcCtrl: Error parsing received messaage JSON: %s", error->message);
      return;
    }

  reader = json_reader_new (json_parser_get_root (parser));

  CcReceivedMessageType type = cc_json_helper_get_message_type (message, reader);

  switch (type)
    {
    case CC_RWAIT_TYPE_RECEIVER_STATUS:
      cc_ctrl_unset_waiting_for (ctrl, CC_RWAIT_TYPE_RECEIVER_STATUS);
      cc_ctrl_handle_receiver_status (ctrl, parser);
      break;
    case CC_RWAIT_TYPE_GET_APP_AVAILABILITY:
      cc_ctrl_unset_waiting_for (ctrl, CC_RWAIT_TYPE_GET_APP_AVAILABILITY);
      cc_ctrl_handle_get_app_availability (ctrl, reader);
      break;
    case CC_RWAIT_TYPE_LAUNCH_ERROR:
      // cc_ctrl_handle_launch_error (ctrl, reader);
      break;
    case CC_RWAIT_TYPE_ANSWER:
      cc_ctrl_unset_waiting_for (ctrl, CC_RWAIT_TYPE_ANSWER);
      // cc_ctrl_handle_answer (ctrl, reader);
      break;
    case CC_RWAIT_TYPE_MEDIA_STATUS:
      cc_ctrl_unset_waiting_for (ctrl, CC_RWAIT_TYPE_MEDIA_STATUS);
      // cc_ctrl_handle_media_status (ctrl, reader);
      break;
    case CC_RWAIT_TYPE_PING:
      cc_ctrl_unset_waiting_for (ctrl, CC_RWAIT_TYPE_PING);
      break;
    case CC_RWAIT_TYPE_PONG:
      cc_ctrl_unset_waiting_for (ctrl, CC_RWAIT_TYPE_PONG);
      break;
    case CC_RWAIT_TYPE_CLOSE:
      // cc_ctrl_handle_close (ctrl, reader);
      break;
    case CC_RWAIT_TYPE_UNKNOWN:
    default:
      g_warning ("CcCtrl: Unknown message type");
      break;
    }
}

void
cc_ctrl_fatal_error (CcCommClosure *closure, GError **error)
{
  // XXX
  CcCtrl *ctrl = (CcCtrl *) closure->userdata;
  ctrl->closure->end_stream (ctrl->closure);
}

CcCommClosure *
cc_ctrl_get_callback_closure (CcCtrl *ctrl)
{
  CcCommClosure *closure = (CcCommClosure *) g_malloc (sizeof (CcCommClosure));
  closure->userdata = ctrl;
  closure->message_received_cb = cc_ctrl_handle_received_msg;
  closure->fatal_error_cb = cc_ctrl_fatal_error;
  return closure;
}

gboolean
cc_ctrl_connection_init (CcCtrl *ctrl, gchar *remote_address)
{
  // pay attn to the receiver ids sent before the messages

  g_autoptr (GError) error = NULL;

  ctrl->state = CC_CTRL_STATE_DISCONNECTED;
  ctrl->comm.cancellable = ctrl->cancellable;

  // register all the callbacks
  ctrl->comm.closure = cc_ctrl_get_callback_closure (ctrl);

  if (!cc_comm_make_connection (&ctrl->comm, remote_address, &error))
    {
      g_warning ("CcCtrl: Failed to make connection to %s: %s", remote_address, error->message);
      return FALSE;
    }

  if (!cc_ctrl_send_auth (ctrl, &error))
    {
      g_warning ("CcCtrl: Failed to send auth: %s", error->message);
      return FALSE;
    }

  if (!cc_ctrl_send_connect (ctrl, CC_DEFAULT_RECEIVER_ID, &error))
    {
      g_warning ("CcCtrl: Failed to send connect: %s", error->message);
      return FALSE;
    }
  
  // since tls_send is a synchronous call
  ctrl->state = CC_CTRL_STATE_CONNECTED;
  
  // send pings to device every 5 seconds
  ctrl->ping_timeout_handle = g_timeout_add_seconds (5, G_SOURCE_FUNC (cc_ctrl_send_ping), ctrl);

  // check waiting for every 15 seconds
  ctrl->waiting_check_timeout_handle = g_timeout_add_seconds (15, G_SOURCE_FUNC (cc_ctrl_check_waiting_for), ctrl);

  // we can skip some message interchange if the mirroring app is already up
  if (!cc_ctrl_send_get_status (ctrl, CC_DEFAULT_RECEIVER_ID, &error))
    {
      g_warning ("CcCtrl: Failed to send get status: %s", error->message);
      return FALSE;
    }
  cc_ctrl_set_waiting_for (ctrl, CC_RWAIT_TYPE_RECEIVER_STATUS);

  return TRUE;
}

void
cc_ctrl_finish (CcCtrl *ctrl, GError **r_error)
{
  g_autoptr(GError) err = NULL;

  // stop both the ping and the waiting check timeout
  g_clear_handle_id (&ctrl->ping_timeout_handle, g_source_remove);
  g_clear_handle_id (&ctrl->waiting_check_timeout_handle, g_source_remove);

  // close app if open
  if (ctrl->state >= CC_CTRL_STATE_APP_OPEN)
    {
      if (!cc_ctrl_send_disconnect (ctrl, CC_DEFAULT_RECEIVER_ID, &err))
        {
          g_warning ("CcCtrl: Error closing virtual connection to app: %s", err->message);
          g_clear_error (&err);
        }
      if (!cc_ctrl_send_close_app (ctrl, ctrl->session_id, &err))
        {
          g_warning ("CcCtrl: Error closing app: %s", err->message);
          g_clear_error (&err);
        }
      g_clear_pointer (&ctrl->session_id, g_free);
    }

  // close the virtual connection
  if (!cc_ctrl_send_disconnect (ctrl, CC_DEFAULT_RECEIVER_ID, NULL))
    g_warning ("CcCtrl: Error closing virtual connection: %s", err->message);

  // free up the resources?
  g_clear_pointer (&ctrl->comm.closure, g_free);

  // safe to call multiple times
  g_cancellable_cancel (ctrl->cancellable);
  g_clear_object (&ctrl->cancellable);

  // close the socket connection
  cc_comm_close_connection (&ctrl->comm);
}
