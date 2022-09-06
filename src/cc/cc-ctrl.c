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

/* FUNCTION DECLS */
static void cc_ctrl_fatal_error (CcCtrl *ctrl);

/* WAITING FOR */

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

/* SEND HELPER FUNCTIONS */

static gboolean
cc_ctrl_send_auth (CcCtrl *ctrl)
{
  if (!cc_comm_send_request (&ctrl->comm,
                             CC_DEFAULT_RECEIVER_ID,
                             CC_MESSAGE_TYPE_AUTH,
                             NULL))
    {
      g_warning ("CcCtrl: Failed to send auth message");
      cc_ctrl_fatal_error (ctrl);
      return FALSE;
    }

  return TRUE;
}

static gboolean
cc_ctrl_send_connect (CcCtrl *ctrl, gchar *destination_id)
{
  gchar *json = cc_json_helper_build_string (
    "type", CC_JSON_TYPE_STRING, "CONNECT",
    "userAgent", CC_JSON_TYPE_STRING, "GND/0.90.5  (X11; Linux x86_64)",
    "connType", CC_JSON_TYPE_INT, 0,
    "origin", CC_JSON_TYPE_OBJECT, cc_json_helper_build_node (NULL),
    "senderInfo", CC_JSON_TYPE_OBJECT, cc_json_helper_build_node (
      "sdkType", CC_JSON_TYPE_INT, 2,
      "version", CC_JSON_TYPE_STRING, "X11; Linux x86_64",
      "browserVersion", CC_JSON_TYPE_STRING, "X11; Linux x86_64",
      "platform", CC_JSON_TYPE_INT, 6,
      "connectionType", CC_JSON_TYPE_INT, 1,
      NULL),
    NULL);

  if (!cc_comm_send_request (&ctrl->comm,
                             destination_id,
                             CC_MESSAGE_TYPE_CONNECT,
                             json))
    {
      g_warning ("CcCtrl: Failed to send connect message");
      cc_ctrl_fatal_error (ctrl);
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
      g_warning ("CcCtrl: Failed to send disconnect message");
      cc_ctrl_fatal_error (ctrl);
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
      g_warning ("CcCtrl: Failed to send get status message");
      cc_ctrl_fatal_error (ctrl);
      return FALSE;
    }

  return TRUE;
}

static gboolean
cc_ctrl_send_get_app_availability (CcCtrl *ctrl, gchar *destination_id, gchar *appId)
{
  g_autoptr(GArray) appIds = g_array_new (FALSE, FALSE, sizeof (gchar *));
  g_array_append_val (appIds, CC_MIRRORING_APP_ID);

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
      g_warning ("CcCtrl: Failed to send get app availability message");
      cc_ctrl_fatal_error (ctrl);
      return FALSE;
    }

  cc_ctrl_set_waiting_for (ctrl, CC_RWAIT_TYPE_GET_APP_AVAILABILITY);

  return TRUE;
}

static gboolean
cc_ctrl_send_launch_app (CcCtrl *ctrl, gchar *destination_id, gchar *appId)
{
  gchar *json = cc_json_helper_build_string (
    "type", CC_JSON_TYPE_STRING, "LAUNCH",
    "launguage", CC_JSON_TYPE_STRING, "en-US",
    "appId", CC_JSON_TYPE_STRING, appId,
    "requestId", CC_JSON_TYPE_INT, ctrl->request_id++,
    NULL);

  if (!cc_comm_send_request (&ctrl->comm,
                             destination_id,
                             CC_MESSAGE_TYPE_RECEIVER,
                             json))
    {
      g_warning ("CcCtrl: Failed to send launch app message");
      cc_ctrl_fatal_error (ctrl);
      return FALSE;
    }

  cc_ctrl_set_waiting_for (ctrl, CC_RWAIT_TYPE_RECEIVER_STATUS);

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
                             sessionId,
                             CC_MESSAGE_TYPE_RECEIVER,
                             json))
    {
      g_warning ("CcCtrl: Failed to send close app message");
      cc_ctrl_fatal_error (ctrl);
      return FALSE;
    }

  cc_ctrl_set_waiting_for (ctrl, CC_RWAIT_TYPE_RECEIVER_STATUS);

  return TRUE;
}

/* OFFER MESSAGE */

JsonNode *
build_audio_source (AudioStream *audio_stream)
{
  JsonNode *node = cc_json_helper_build_node (
    "aesIvMask", CC_JSON_TYPE_STRING, audio_stream->stream.aes_iv_mask,
    "aesKey", CC_JSON_TYPE_STRING, audio_stream->stream.aes_key,
    "bitRate", CC_JSON_TYPE_INT, audio_stream->bit_rate,
    "codecName", CC_JSON_TYPE_STRING, audio_stream->codec,
    "index", CC_JSON_TYPE_INT, audio_stream->stream.index,
    "receiverRtcpEventLog", CC_JSON_TYPE_BOOLEAN, audio_stream->stream.receiver_rtcp_event_log,
    "rtpExtensions", CC_JSON_TYPE_STRING, "adaptive_playout_delay",
    "rtpPayloadType", CC_JSON_TYPE_INT, audio_stream->stream.rtp_payload_type,
    "rtpProfile", CC_JSON_TYPE_STRING, audio_stream->profile,
    "sampleRate", CC_JSON_TYPE_INT, audio_stream->sample_rate,
    "ssrc", CC_JSON_TYPE_INT, audio_stream->stream.ssrc,
    "targetDelay", CC_JSON_TYPE_INT, audio_stream->stream.target_delay,
    "timeBase", CC_JSON_TYPE_STRING, audio_stream->stream.rtp_timebase,
    "type", CC_JSON_TYPE_STRING, "audio_source",
    NULL);

  return g_steal_pointer (&node);
}

JsonNode *
build_video_source (VideoStream *video_stream)
{
  g_autoptr(GArray) resolutions = g_array_new (FALSE, FALSE, sizeof (JsonNode *));

  JsonNode *resolution = cc_json_helper_build_node (
    "height", CC_JSON_TYPE_INT, 1080,
    "width", CC_JSON_TYPE_INT, 1920,
    NULL);
  g_array_append_val (resolutions, resolution);

  JsonNode *node = cc_json_helper_build_node (
    "aesIvMask", CC_JSON_TYPE_STRING, video_stream->stream.aes_iv_mask,
    "aesKey", CC_JSON_TYPE_STRING, video_stream->stream.aes_key,
    "codecName", CC_JSON_TYPE_STRING, video_stream->codec,
    "index", CC_JSON_TYPE_INT, video_stream->stream.index,
    "maxBitRate", CC_JSON_TYPE_INT, video_stream->max_bit_rate,
    "maxFrameRate", CC_JSON_TYPE_STRING, video_stream->max_frame_rate,
    "receiverRtcpEventLog", CC_JSON_TYPE_BOOLEAN, video_stream->stream.receiver_rtcp_event_log,
    "renderMode", CC_JSON_TYPE_STRING, "video",
    "resolutions", CC_JSON_TYPE_ARRAY_OBJECT, resolutions,
    "rtpExtensions", CC_JSON_TYPE_STRING, "adaptive_playout_delay",
    "rtpPayloadType", CC_JSON_TYPE_INT, video_stream->stream.rtp_payload_type,
    "rtpProfile", CC_JSON_TYPE_STRING, video_stream->profile,
    "ssrc", CC_JSON_TYPE_INT, video_stream->stream.ssrc,
    "targetDelay", CC_JSON_TYPE_INT, video_stream->stream.target_delay,
    "timeBase", CC_JSON_TYPE_STRING, video_stream->stream.rtp_timebase,
    "type", CC_JSON_TYPE_STRING, "video_source",
    NULL);

  return g_steal_pointer (&node);
}

static gboolean
cc_ctrl_send_offer (CcCtrl *ctrl, gchar *destination_id, GError **error)
{
  g_debug ("CcCtrl: Sending OFFER");

  /* look into [ adaptive_playout_delay, rtpExtensions, rtpPayloadType, rtpProfile, aes stuff, ssrc increment in received msg ] */

  Offer *offer = ctrl->closure->get_offer_message (ctrl->closure->userdata);
  JsonNode *audio_source_node = build_audio_source (&offer->audio_stream);
  JsonNode *video_source_node = build_video_source (&offer->video_stream);

  g_autoptr(GArray) streams = g_array_new (FALSE, FALSE, sizeof (JsonNode *));
  g_array_append_val (streams, audio_source_node);
  g_array_append_val (streams, video_source_node);

  JsonNode *offer_key = cc_json_helper_build_node (
    "castMode", CC_JSON_TYPE_STRING, offer->cast_mode,
    "receiverGetStatus", CC_JSON_TYPE_BOOLEAN, offer->receiver_get_status,
    "supportedStreams", CC_JSON_TYPE_ARRAY_OBJECT, streams,
    NULL);

  JsonNode *root = cc_json_helper_build_node (
    "offer", CC_JSON_TYPE_OBJECT, offer_key,
    "seqNum", CC_JSON_TYPE_INT, offer->seq_num,
    "type", CC_JSON_TYPE_STRING, "OFFER",
    NULL);

  if (!cc_comm_send_request (&ctrl->comm,
                             destination_id,
                             CC_MESSAGE_TYPE_WEBRTC,
                             cc_json_helper_node_to_string (root)))
    {
      g_warning ("CcCtrl: Failed to send OFFER message");
      cc_ctrl_fatal_error (ctrl);
      return FALSE;
    }

  cc_ctrl_set_waiting_for (ctrl, CC_RWAIT_TYPE_ANSWER);

  return TRUE;
}

/* INTERVAL FUNCTIONS */

/* if we are waiting for something for longer than said interval, then our connection has some problem */
static gboolean
cc_ctrl_check_waiting_for (CcCtrl *ctrl)
{
  if (ctrl->waiting_for == CC_RWAIT_TYPE_NONE)
    return G_SOURCE_CONTINUE;

  g_error ("CcCtrl: Timed out waiting for %d", ctrl->waiting_for);
  cc_ctrl_fatal_error (ctrl);

  return G_SOURCE_REMOVE;
}

static gboolean
cc_ctrl_send_ping (CcCtrl *ctrl)
{
  g_debug ("CcCtrl: Sending PING");
  g_autoptr(GError) error = NULL;

  /* if this errors out, we cancel the periodic ping by returning FALSE */
  if (!cc_comm_send_request (&ctrl->comm,
                             CC_DEFAULT_RECEIVER_ID,
                             CC_MESSAGE_TYPE_PING,
                             NULL))
    {
      g_error ("CcCtrl: Failed to send ping message");
      return G_SOURCE_REMOVE;
    }

  cc_ctrl_set_waiting_for (ctrl, CC_RWAIT_TYPE_PONG);

  return G_SOURCE_CONTINUE;
}

static gboolean
cc_ctrl_send_offer_cb (CcCtrl *ctrl)
{
  g_autoptr(GError) error = NULL;
  if (!cc_ctrl_send_offer (ctrl, ctrl->session_id, &error))
    {
      if (error)
        g_warning ("CcCtrl: Failed to send OFFER to the mirroring app: %s", error->message);
      else
        g_warning ("CcCtrl: Failed to send OFFER to the mirroring app");
    }
  return G_SOURCE_REMOVE;
}

static void
cc_ctrl_mirroring_app_init (CcCtrl *ctrl)
{
  g_autoptr(GError) err = NULL;
  if (!cc_ctrl_send_connect (ctrl, ctrl->session_id))
    {
      g_error ("CcCtrl: Failed to send CONNECT to the mirroring app");
      return;
    }

  /* send offer message after 1 second */
  g_timeout_add_seconds (1, G_SOURCE_FUNC (cc_ctrl_send_offer_cb), ctrl);
}

/* HANDLE MESSAGE */

static void
cc_ctrl_handle_get_app_availability (CcCtrl *ctrl, JsonReader *reader)
{
  g_autoptr(GError) error = NULL;

  if (json_reader_read_member (reader, "availability"))
    {
      if (json_reader_read_member (reader, CC_MIRRORING_APP_ID))
        {
          const gchar *available = json_reader_get_string_value (reader);
          if (g_strcmp0 (available, "APP_AVAILABLE"))
            {
              /* launch the app now */
              if (!cc_ctrl_send_launch_app (ctrl, CC_DEFAULT_RECEIVER_ID, CC_MIRRORING_APP_ID))
                {
                  g_error ("CcCtrl: Failed to launch the app");
                  return;
                }
            }

          /* since the app is not available, stop attempts */
          g_warning ("CcCtrl: %s app is not available, quiting", CC_MIRRORING_APP_ID);
          cc_ctrl_fatal_error (ctrl);
        }
    }
}

/* handler messages for received messages */
static void
cc_ctrl_handle_receiver_status (CcCtrl *ctrl, JsonParser *parser)
{
  /* reports all the open apps (the relevant stuff)
   * if the app is open, it has a sessionId: opened by this app or not, it is hijackable
   * connect to it, and then propose an offer
   */

  g_autoptr(GError) error = NULL;
  g_autoptr(JsonNode) app_status = NULL;
  g_autoptr(JsonPath) path = json_path_new ();
  json_path_compile (path, "$.status.applications[0]", NULL);
  app_status = json_path_match (path, json_parser_get_root (parser));

  g_autoptr(JsonGenerator) generator = json_generator_new ();
  json_generator_set_root (generator, app_status);
  gsize size;
  json_generator_to_data (generator, &size);

  if (size == 2) /* empty array [] */
    {
      g_debug ("CcCtrl: No apps open");
      if (ctrl->state == CC_CTRL_STATE_LAUNCH_SENT)
        return;

      if (ctrl->state >= CC_CTRL_STATE_APP_OPEN) /* app closed unexpectedly */
        g_debug ("CcCtrl: App closed unexpectedly");

      if (!cc_ctrl_send_launch_app (ctrl, CC_DEFAULT_RECEIVER_ID, CC_MIRRORING_APP_ID))
        {
          g_error ("CcCtrl: Failed to launch the app");
          return;
        }

      ctrl->state = CC_CTRL_STATE_LAUNCH_SENT;
      return;
    }

  /* one or more apps is/are open */
  g_autoptr(JsonReader) reader = json_reader_new (app_status);

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
                  /* takeover the session, doesn't matter which sender opened it */
                  g_debug ("CcCtrl: Mirroring app is open");
                  ctrl->state = CC_CTRL_STATE_APP_OPEN;
                  g_clear_pointer (&ctrl->session_id, g_free);
                  ctrl->session_id = g_strdup (sessionId);

                  cc_ctrl_mirroring_app_init (ctrl);
                  return;
                }

              /* some other app is open, check if `CC_MIRRORING_APP_ID` is available */
              if (!cc_ctrl_send_get_app_availability (ctrl, CC_MIRRORING_APP_ID, CC_DEFAULT_RECEIVER_ID))
                {
                  g_error ("CcCtrl: Failed to send GET_APP_AVAILABILITY");
                  return;
                }
            }
        }
    }
}

static void
cc_ctrl_handle_media_status (CcCtrl *ctrl, Cast__Channel__CastMessage *message, JsonReader *reader)
{
  /* since answer and media_status are received one after another, we discard this
   * for the mirroring app
   * and since this stream is LIVE, we won't need any of it
   * if (g_strcmp0 (message->source_id, ctrl->session_id))
   *   return;
   */
}

static void
cc_ctrl_handle_close (CcCtrl *ctrl, Cast__Channel__CastMessage *message)
{
  g_autoptr(GError) error = NULL;

  if (g_strcmp0 (message->source_id, CC_DEFAULT_RECEIVER_ID) == 0)
    {
      g_warning ("CcCtrl: Receiver closed the connection");
      cc_ctrl_fatal_error (ctrl);
      return;
    }

  /* the app closed */
  g_debug ("CcCtrl: App sent a close message, launching again");
  if (!cc_ctrl_send_launch_app (ctrl, CC_DEFAULT_RECEIVER_ID, CC_MIRRORING_APP_ID))
    g_error ("CcCtrl: Failed to launch app");
}

void
cc_ctrl_handle_received_msg (gpointer                    userdata,
                             Cast__Channel__CastMessage *message)
{
  CcCtrl *ctrl = (CcCtrl *) userdata;

  g_autoptr(GError) error = NULL;
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(JsonReader) reader = NULL;
  CcReceivedMessageType type;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, message->payload_utf8, -1, &error))
    {
      g_warning ("CcCtrl: Error parsing received messaage JSON: %s", error->message);
      cc_json_helper_dump_message (message, TRUE);
      return;
    }

  reader = json_reader_new (json_parser_get_root (parser));

  type = cc_json_helper_get_message_type (message, reader);

  if (!(type == CC_RWAIT_TYPE_PING || type == CC_RWAIT_TYPE_PONG || type == -1))
    {
      g_debug ("CcComm: Received message:");
      cc_json_helper_dump_message (message, FALSE);
    }

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
      g_warning ("CcCtrl: Failed to launch app");
      cc_ctrl_fatal_error (ctrl);
      break;

    case CC_RWAIT_TYPE_ANSWER:
      cc_ctrl_unset_waiting_for (ctrl, CC_RWAIT_TYPE_ANSWER);
      /* cc_ctrl_handle_answer (ctrl, reader); */
      break;

    case CC_RWAIT_TYPE_MEDIA_STATUS:
      cc_ctrl_unset_waiting_for (ctrl, CC_RWAIT_TYPE_MEDIA_STATUS);
      cc_ctrl_handle_media_status (ctrl, message, reader);
      break;

    case CC_RWAIT_TYPE_PING:
      cc_ctrl_unset_waiting_for (ctrl, CC_RWAIT_TYPE_PING);
      break;

    case CC_RWAIT_TYPE_PONG:
      cc_ctrl_unset_waiting_for (ctrl, CC_RWAIT_TYPE_PONG);
      break;

    case CC_RWAIT_TYPE_CLOSE:
      cc_ctrl_handle_close (ctrl, message);
      break;

    case CC_RWAIT_TYPE_UNKNOWN:
    default:
      g_warning ("CcCtrl: Unknown message type");
      cc_ctrl_fatal_error (ctrl);
      break;
    }
}

static void
cc_ctrl_fatal_error (CcCtrl *ctrl)
{
  if (ctrl->state == CC_CTRL_STATE_ERROR) /* function has already been called */
    return;

  ctrl->closure->end_stream (ctrl->closure->userdata);
  ctrl->state = CC_CTRL_STATE_ERROR;
}

void
cc_ctrl_fatal_error_closure (gpointer userdata, GError *error)
{
  CcCtrl *ctrl;

  /* XXX: add error arg in end_stream and display an error message to user */
  if (error)
    g_warning ("CcCtrl: Fatal error: %s", error->message);
  else
    g_error ("CcCtrl: Fatal error");

  ctrl = (CcCtrl *) userdata;
  cc_ctrl_fatal_error (ctrl);
}

CcCommClosure *
cc_ctrl_get_callback_closure (CcCtrl *ctrl)
{
  CcCommClosure *closure = (CcCommClosure *) g_malloc (sizeof (CcCommClosure));

  closure->userdata = ctrl;
  closure->message_received_cb = cc_ctrl_handle_received_msg;
  closure->fatal_error_cb = cc_ctrl_fatal_error_closure;
  return closure;
}

gboolean
cc_ctrl_connection_init (CcCtrl *ctrl, gchar *remote_address)
{
  g_autoptr(GError) error = NULL;

  ctrl->state = CC_CTRL_STATE_DISCONNECTED;
  ctrl->comm.cancellable = ctrl->cancellable;

  /* register all the callbacks */
  ctrl->comm.closure = cc_ctrl_get_callback_closure (ctrl);

  if (!cc_comm_make_connection (&ctrl->comm, remote_address, &error))
    {
      if (error != NULL)
        g_warning ("CcCtrl: Failed to make connection to %s: %s", remote_address, error->message);
      else
        g_warning ("CcCtrl: Failed to make connection to %s", remote_address);
      return FALSE;
    }

  if (!cc_ctrl_send_auth (ctrl))
    return FALSE;

  if (!cc_ctrl_send_connect (ctrl, CC_DEFAULT_RECEIVER_ID))
    {
      g_warning ("CcCtrl: Failed to send connect");
      return FALSE;
    }

  /* since tls_send is a synchronous call */
  ctrl->state = CC_CTRL_STATE_CONNECTED;

  /* send pings to device every 5 seconds */
  ctrl->ping_timeout_handle = g_timeout_add_seconds (5, G_SOURCE_FUNC (cc_ctrl_send_ping), ctrl);

  /* check waiting for every 15 seconds */
  ctrl->waiting_check_timeout_handle = g_timeout_add_seconds (CC_MAX_MESSAGE_TIMEOUT,
                                                              G_SOURCE_FUNC (cc_ctrl_check_waiting_for),
                                                              ctrl);

  /* we can skip some message interchange if the mirroring app is already up */
  if (!cc_ctrl_send_get_status (ctrl, CC_DEFAULT_RECEIVER_ID))
    {
      g_error ("CcCtrl: Failed to send get status");
      return FALSE;
    }

  return TRUE;
}

void
cc_ctrl_finish (CcCtrl *ctrl)
{
  if (ctrl->state == CC_CTRL_STATE_DISCONNECTED)
    return;

  g_clear_handle_id (&ctrl->ping_timeout_handle, g_source_remove);
  g_clear_handle_id (&ctrl->waiting_check_timeout_handle, g_source_remove);

  /* close app if open */
  if (ctrl->state >= CC_CTRL_STATE_APP_OPEN)
    {
      if (!cc_ctrl_send_disconnect (ctrl, CC_DEFAULT_RECEIVER_ID))
        g_error ("CcCtrl: Error closing virtual connection to app");
      if (!cc_ctrl_send_close_app (ctrl, ctrl->session_id))
        g_error ("CcCtrl: Error closing app");
      g_clear_pointer (&ctrl->session_id, g_free);
    }

  /* close the virtual connection */
  if (!cc_ctrl_send_disconnect (ctrl, CC_DEFAULT_RECEIVER_ID))
    g_error ("CcCtrl: Error closing virtual connection");

  /* free up the resources */
  g_clear_pointer (&ctrl->comm.closure, g_free);

  g_cancellable_cancel (ctrl->cancellable);
  g_clear_object (&ctrl->cancellable);

  /* close the socket connection */
  cc_comm_close_connection (&ctrl->comm);

  ctrl->state = CC_CTRL_STATE_DISCONNECTED;
}

/* TODO: make the code less coupled with the mirroring app */
/* TODO: use waiting_for for error messages */
/* TODO: free all the vars in structs */
