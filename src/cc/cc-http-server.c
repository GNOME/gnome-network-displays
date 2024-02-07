/* cc-http-server.c
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

#include "cc-http-server.h"
#include "cc-common.h"

typedef struct _CcHttpServer
{
  GObject          parent_instance;

  SoupServer      *server;
  gchar           *remote_address;
  guint            port;

  WfdMediaFactory *factory;
  GstElement      *pipeline;
  GstElement      *multisocketsink;
} CcHttpServer;

G_DEFINE_TYPE (CcHttpServer, cc_http_server, G_TYPE_OBJECT)

enum {
  SIGNAL_CREATE_SOURCE,
  SIGNAL_CREATE_AUDIO_SOURCE,
  SIGNAL_STREAM_STARTED,
  SIGNAL_END_STREAM,
  NR_SIGNALS
};

static guint signals[NR_SIGNALS];

/* DECLS */

void cc_http_server_set_pipeline_state (CcHttpServer *self,
                                        GstState      state);

/* GSTREAMER */

static gboolean
gst_bus_message_cb (GstBus *bus, GstMessage *msg, CcHttpServer *self)
{
  switch (GST_MESSAGE_TYPE (msg))
    {
    case GST_MESSAGE_STATE_CHANGED:
      {
        if (GST_MESSAGE_SRC (msg) != GST_OBJECT (self->pipeline))
          break;

        GstState old_state, new_state;
        gst_message_parse_state_changed (msg, &old_state, &new_state, NULL);

        g_debug ("CcHttpServer: State changed: from %s to %s",
                 gst_element_state_get_name (old_state),
                 gst_element_state_get_name (new_state));

        break;
      }

    case GST_MESSAGE_EOS:
      {
        g_debug ("CcHttpServer: EOS, halting pipeline");
        cc_http_server_set_pipeline_state (self, GST_STATE_NULL);
        g_signal_emit_by_name (self->multisocketsink, "clear");
        break;
      }

    case GST_MESSAGE_INFO:
    case GST_MESSAGE_WARNING:
    case GST_MESSAGE_ERROR:
      {
        GError *error;
        gchar *debug_info;

        switch (GST_MESSAGE_TYPE (msg))
          {
          case GST_MESSAGE_INFO:
            gst_message_parse_info (msg, &error, &debug_info);
            g_debug ("CcHttpServer: INFO: %s, debug info: %s", error->message, debug_info);
            break;

          case GST_MESSAGE_WARNING:
            gst_message_parse_warning (msg, &error, &debug_info);
            g_debug ("CcHttpServer: WARNING: %s, debug info: %s", error->message, debug_info);
            break;

          case GST_MESSAGE_ERROR:
            gst_message_parse_error (msg, &error, &debug_info);
            g_debug ("CcHttpServer: ERROR: %s, debug info: %s", error->message, debug_info);
            break;

          default:
            g_assert_not_reached ();
          }

        if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR)
          {
            GError *error_ = g_error_new (CC_ERROR,
                                          CC_ERROR_GST_PIPELINE_FAULT,
                                          "Pipeline fault: %s",
                                          error ? error->message : "none");
            g_warning ("CcHttpServer: %s", error_->message);
            g_signal_emit_by_name (self->multisocketsink, "clear");
            cc_http_server_set_pipeline_state (self, GST_STATE_NULL);
            g_signal_emit_by_name (self, "end-stream", error_);
            g_clear_error (&error_);
          }

        g_clear_error (&error);
        g_clear_pointer (&debug_info, g_free);

        break;
      }

    case GST_MESSAGE_REQUEST_STATE:
      {
        GstState requested_state;
        gst_message_parse_request_state (msg, &requested_state);

        g_debug ("CcHttpServer: State change to %s was requested by %s",
                 gst_element_state_get_name (requested_state),
                 GST_MESSAGE_SRC_NAME (msg));

        cc_http_server_set_pipeline_state (self, requested_state);
        break;
      }

    case GST_MESSAGE_LATENCY:
      {
        g_debug ("CcHttpServer: Redistributing latency");
        gst_bin_recalculate_latency (GST_BIN (self->pipeline));
        break;
      }

    default:
      break;
    }

  return TRUE;
}

static void
client_socket_removed_cb (GstElement   *multisocketsink,
                          GSocket      *socket,
                          CcHttpServer *self)
{
  gboolean ok;

  g_autoptr(GError) err = NULL;

  /* close the socket */
  ok = g_socket_close (socket, &err);
  if (!ok)
    g_warning ("CcHttpServer: Error closing the socket: %s",
               err ? err->message : "none");
}

static void
configure_gst_elements (CcHttpServer *self, GstBin *bin)
{
  WfdParams params;
  WfdVideoCodec codec;
  WfdAudioCodec audio_codec;
  WfdResolution resolution;

  codec = (WfdVideoCodec){
    .profile = WFD_H264_PROFILE_CHROMECAST,
    .level = 4,
    .latency = 0,
    .frame_skipping_allowed = 1,
  };

  audio_codec = (WfdAudioCodec){
    .type = WFD_AUDIO_AAC,
    .modes = 0x1,
  };

  resolution = (WfdResolution){
    .height = 1080,
    .width = 1920,
    .refresh_rate = 30,
    .interlaced = FALSE,
  };

  params.selected_codec = &codec;
  params.selected_audio_codec = &audio_codec;
  params.selected_resolution = &resolution;
  params.idr_request_capability = FALSE;

  wfd_configure_media_element (bin, &params);
}

static void
create_gst_elements (CcHttpServer *self)
{
  gboolean success = TRUE;
  GstElement *matroskamux;
  GstElement *queue_post_muxer;
  GstElement *queue_mpegmux_video;
  GstBin *bin;

  g_autoptr(GstBin) audio_pipeline = NULL;

  bin = GST_BIN (self->pipeline);

  queue_mpegmux_video = wfd_media_factory_create_video_element (self->factory, bin);

  audio_pipeline = wfd_media_factory_create_audio_element (self->factory);
  if (audio_pipeline != NULL)
    success &= gst_bin_add (bin, GST_ELEMENT (g_object_ref (audio_pipeline)));

  /* set encoder element name "wfd-mpegtsmux" to make it compatible with wfd-media-factory */
  matroskamux = gst_element_factory_make ("matroskamux", "wfd-mpegtsmux");
  success &= gst_bin_add (bin, matroskamux);
  g_object_set (matroskamux,
                "streamable", TRUE,
                NULL);

  queue_post_muxer = gst_element_factory_make ("queue", "queue-post-muxer");
  success &= gst_bin_add (bin, queue_post_muxer);
  g_object_set (queue_post_muxer,
                "max-size-buffers", (guint) 100000,
                "max-size-time", 500 * GST_MSECOND,
                NULL);

  self->multisocketsink = gst_element_factory_make ("multisocketsink", "multisocketsink");
  success &= gst_bin_add (bin, self->multisocketsink);

  g_object_set (self->multisocketsink,
                "qos", TRUE,
                "recover-policy", 3,
                "sync-method", 2, /* unclear */
                NULL);

  g_signal_connect (self->multisocketsink, "client-socket-removed", G_CALLBACK (client_socket_removed_cb), self);

  configure_gst_elements (self, bin);

  /* link the elements last to ensure correct caps are set in the config step */
  success &= gst_element_link_pads (queue_mpegmux_video, "src", matroskamux, "video_%u");
  success &= gst_element_link_pads (GST_ELEMENT (audio_pipeline), "src", matroskamux, "audio_%u");

  success &= gst_element_link_many (matroskamux,
                                    queue_post_muxer,
                                    self->multisocketsink,
                                    NULL);

  GST_DEBUG_BIN_TO_DOT_FILE (bin,
                             GST_DEBUG_GRAPH_SHOW_ALL,
                             "nd-cc-bin");

  if (!success)
    g_error ("CcHttpServer: Failed to create gst elements");

  g_debug ("CcHttpServer: Created video pipeline");
}

static void
setup_gst (CcHttpServer *self)
{
  self->pipeline = gst_pipeline_new ("nd-cc-pipeline");
  create_gst_elements (self);
  gst_pipeline_set_latency (GST_PIPELINE (self->pipeline), 500 * GST_MSECOND);

  g_autoptr(GstBus) bus = gst_element_get_bus (self->pipeline);
  gst_bus_add_signal_watch (bus);

  g_signal_connect (G_OBJECT (bus), "message", (GCallback) gst_bus_message_cb, self);

  cc_http_server_set_pipeline_state (self, GST_STATE_READY);
}

/* SOUP SERVER */

static gboolean
set_ui_streaming_state_cb (CcHttpServer *self)
{
  g_signal_emit_by_name (self, "stream-started");
  return G_SOURCE_REMOVE;
}

static void
wrote_headers_cb (SoupServerMessage *msg, CcHttpServer *self)
{
  GSocket *socket = soup_server_message_get_socket (msg);

  g_signal_emit_by_name (self->multisocketsink, "add", socket);
  cc_http_server_set_pipeline_state (self, GST_STATE_PLAYING);

  g_main_context_invoke (NULL,
                         G_SOURCE_FUNC (set_ui_streaming_state_cb),
                         self);
}

static void
server_callback (SoupServer        *server,
                 SoupServerMessage *msg,
                 const char        *path,
                 GHashTable        *query,
                 gpointer           user_data)
{
  GInetSocketAddress *inet_sock_addr = NULL;

  g_autoptr(GInetAddress) inet_addr = NULL;
  gchar *remote_address;

  CcHttpServer *self = (CcHttpServer *) user_data;

  if (soup_server_message_get_method (msg) != SOUP_METHOD_GET ||
      g_strcmp0 (path, "/") != 0)
    {
      soup_server_message_set_status (msg, SOUP_STATUS_FORBIDDEN, NULL); /* 403 */
      return;
    }

  inet_sock_addr = G_INET_SOCKET_ADDRESS (soup_server_message_get_remote_address (msg));
  if (!inet_sock_addr)
    {
      soup_server_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL); /* 500 */
      return;
    }

  inet_addr = g_inet_socket_address_get_address (inet_sock_addr);
  remote_address = g_inet_address_to_string (inet_addr);

  /* only allow connections from the connected Chromecast */
  /* allow access to all the devices through the dummy sink */
  if (g_strcmp0 (self->remote_address, remote_address) != 0 && g_strcmp0 (self->remote_address, "dummy-sink") != 0)
    {
      soup_server_message_set_status (msg, SOUP_STATUS_FORBIDDEN, NULL); /* 403 */
      g_clear_pointer (&remote_address, g_free);
      return;
    }

  g_clear_pointer (&remote_address, g_free);

  soup_message_headers_set_encoding (soup_server_message_get_response_headers (msg),
                                     SOUP_ENCODING_EOF);
  soup_message_headers_set_content_type (soup_server_message_get_response_headers (msg),
                                         "video/x-matroska", NULL);
  soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);

  g_signal_connect (G_OBJECT (msg), "wrote-headers", G_CALLBACK (wrote_headers_cb), self);
}

/*  */

static GstElement *
server_create_source_cb (CcHttpServer *self, WfdMediaFactory *factory)
{
  GstElement *res;

  g_signal_emit_by_name (self, "create-source", &res);
  g_debug ("CcHttpServer: Create source signal emitted");

  return res;
}

static GstElement *
server_create_audio_source_cb (CcHttpServer *self, WfdMediaFactory *factory)
{
  GstElement *res;

  g_signal_emit_by_name (self, "create-audio-source", &res);
  g_debug ("CcHttpServer: Create audio source signal emitted");

  return res;
}

/* PUBLIC FUNCTIONS */

void
cc_http_server_set_pipeline_state (CcHttpServer *self, GstState state)
{
  if (!self->pipeline)
    {
      g_warning ("CcHttpServer: State change requested when the pipeline is invalid, ignoring.");
      return;
    }

  if (gst_element_set_state (self->pipeline, state) == GST_STATE_CHANGE_FAILURE &&
      state != GST_STATE_NULL)
    {
      GError *error_ = g_error_new (CC_ERROR,
                                    CC_ERROR_GST_PIPELINE_SET_STATE_FAILED,
                                    "Unable to set the pipeline to %s state.",
                                    gst_element_state_get_name (state));
      g_warning ("CcHttpServer: %s", error_->message);
      g_signal_emit_by_name (self, "end-stream", error_);
      g_clear_error(&error_);
    }
}

guint
cc_http_server_get_port (CcHttpServer *self)
{
  return self->port;
}

void
cc_http_server_set_remote_address (CcHttpServer *self, gchar *remote_address)
{
  if (!remote_address)
    {
      self->remote_address = g_strdup ("");
      return;
    }

  self->remote_address = g_strdup (remote_address);
}

gboolean
cc_http_server_start_server (CcHttpServer *self, GError **error)
{
  g_autoptr(GSList) gslist = NULL;
  g_autoptr(GUri) soup_uri = NULL;
  gboolean success = TRUE;

  setup_gst (self);

  self->server = soup_server_new ("server-header",
                                  "simple-httpd ",
                                  NULL);

  success &= soup_server_listen_all (self->server, 0, 0, error);

  if (!success || (error && *error))
    return FALSE;

  gslist = soup_server_get_uris (self->server);
  soup_uri = (GUri *) gslist->data;
  self->port = (guint) g_uri_get_port (soup_uri);

  g_debug ("CcHttpServer: Listening on port %d", self->port);

  soup_server_add_handler (self->server, NULL,
                           server_callback, self, NULL);

  return TRUE;
}

CcHttpServer *
cc_http_server_new (void)
{
  CcHttpServer *self = g_object_new (CC_TYPE_HTTP_SERVER, NULL);

  self->factory = wfd_media_factory_new ();

  g_signal_connect_object (self->factory,
                           "create-source",
                           (GCallback) server_create_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->factory,
                           "create-audio-source",
                           (GCallback) server_create_audio_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  return g_steal_pointer (&self);
}

void
cc_http_server_finalize (CcHttpServer *self)
{
  GstBus *bus;
  GstState state;

  g_debug ("CcHttpServer: Finalizing");

  if (GST_IS_ELEMENT (self->pipeline))
    {
      bus = gst_element_get_bus (self->pipeline);
      gst_bus_remove_signal_watch (bus);
      gst_object_unref (bus);

      gst_element_get_state (self->pipeline, &state, NULL, 100 * GST_MSECOND);
      if (state != GST_STATE_NULL)
        cc_http_server_set_pipeline_state (self, GST_STATE_NULL);

      gst_object_unref (self->pipeline);
    }

  wfd_media_factory_finalize (G_OBJECT (self->factory));

  if (SOUP_IS_SERVER (self->server))
    soup_server_remove_handler (self->server, NULL);

  g_clear_pointer (&self->remote_address, g_free);
}

static void
cc_http_server_class_init (CcHttpServerClass *klass)
{
  signals[SIGNAL_CREATE_SOURCE] =
    g_signal_new ("create-source", CC_TYPE_HTTP_SERVER, G_SIGNAL_RUN_LAST,
                  0,
                  g_signal_accumulator_first_wins, NULL,
                  NULL,
                  GST_TYPE_ELEMENT, 0);

  signals[SIGNAL_CREATE_AUDIO_SOURCE] =
    g_signal_new ("create-audio-source", CC_TYPE_HTTP_SERVER, G_SIGNAL_RUN_LAST,
                  0,
                  g_signal_accumulator_first_wins, NULL,
                  NULL,
                  GST_TYPE_ELEMENT, 0);

  signals[SIGNAL_STREAM_STARTED] =
    g_signal_new ("stream-started", CC_TYPE_HTTP_SERVER, G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_END_STREAM] =
    g_signal_new ("end-stream", CC_TYPE_HTTP_SERVER, G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE,
                  1, G_TYPE_ERROR);
}

void
cc_http_server_init (CcHttpServer *self)
{
}
