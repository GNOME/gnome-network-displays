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

/* TODO: check if only audio streams can go through */
const gchar *content_types[ELEMENT_NONE] = {
  [ELEMENT_WEBM] = "video/webm",
  [ELEMENT_MATROSKA] = "video/x-matroska",
  [ELEMENT_MP4] = "video/mp4",
};

struct _CcHttpServer
{
  CcMediaFactory parent_instance;

  SoupServer    *server;
  gchar         *remote_address;
  guint          port;
};

G_DEFINE_TYPE (CcHttpServer, cc_http_server, CC_TYPE_MEDIA_FACTORY)

enum {
  PROP_PORT = 1,
  NR_PROPS,
};

static GParamSpec * props[NR_PROPS] = { NULL, };

enum {
  SIGNAL_STREAM_STARTED,
  NR_SIGNALS
};

static guint signals[NR_SIGNALS];

static void
cc_http_server_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  CcHttpServer *self = CC_HTTP_SERVER (object);

  switch (prop_id)
    {
    case PROP_PORT:
      g_value_set_uint (value, self->port);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* GSTREAMER */

static void
client_socket_removed_cb (GstElement   *multisocketsink,
                          GSocket      *socket,
                          CcHttpServer *self)
{
  gboolean ok;

  g_autoptr(GError) err = NULL;

  /* TODO: using READY or NULL state does not let pipeline start again, needs investigation */
  /*
     GstMessage *message;
     g_autoptr(GstBus) bus = NULL;
     CcMediaFactory *factory;

     factory = CC_MEDIA_FACTORY (self);
     message = gst_message_new_request_state (GST_OBJECT (multisocketsink), GST_STATE_NULL);
     bus = gst_pipeline_get_bus (GST_PIPELINE (factory->pipeline));
     gst_bus_post (bus, message);
   */

  /* close the socket */
  ok = g_socket_close (socket, &err);
  if (!ok)
    g_warning ("CcHttpServer: Error closing the socket: %s",
               err ? err->message : "none");
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
  GSocket *socket;
  GstElement *multisocketsink;

  socket = soup_server_message_get_socket (msg);

  multisocketsink = gst_bin_get_by_name (GST_BIN (CC_MEDIA_FACTORY (self)->pipeline), "cc-multisocketsink");
  g_signal_emit_by_name (multisocketsink, "add", socket);
  gst_object_unref (multisocketsink);

  if (!cc_media_factory_set_pipeline_state (CC_MEDIA_FACTORY (self), GST_STATE_PLAYING))
    return;

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

  /* TODO: debugging */
#if 0
  /* only allow connections from the connected Chromecast */
  /* allow access to all the devices through the dummy sink */
  if (g_strcmp0 (self->remote_address, remote_address) != 0 && g_strcmp0 (self->remote_address, "dummy-sink") != 0)
    {
      soup_server_message_set_status (msg, SOUP_STATUS_FORBIDDEN, NULL); /* 403 */
      g_clear_pointer (&remote_address, g_free);
      return;
    }
#endif

  g_clear_pointer (&remote_address, g_free);

  soup_message_headers_set_encoding (soup_server_message_get_response_headers (msg),
                                     SOUP_ENCODING_EOF);
  soup_message_headers_set_content_type (soup_server_message_get_response_headers (msg),
                                         content_types[cc_media_factory_profiles[CC_MEDIA_FACTORY (self)->factory_profile].muxer],
                                         NULL);
  soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);

  g_signal_connect (G_OBJECT (msg), "wrote-headers", G_CALLBACK (wrote_headers_cb), self);
}

/* PUBLIC FUNCTIONS */

gboolean
cc_http_server_start_server (CcHttpServer *self, GError **error)
{
  g_autoptr(GSList) gslist = NULL;
  g_autoptr(GUri) soup_uri = NULL;

  CcMediaFactory *factory;
  GstElement *multisocketsink;
  gboolean success;

  factory = (CcMediaFactory *) self;

  success = cc_media_factory_create_pipeline (factory);
  if (!success)
    {
      g_set_error (error,
                   CC_ERROR,
                   CC_ERROR_GST_PIPELINE_CREATION_FAILED,
                   "Unable to create the pipeline.");
      return FALSE;
    }

  multisocketsink = gst_bin_get_by_name (GST_BIN (factory->pipeline), "cc-multisocketsink");
  g_signal_connect (multisocketsink,
                    "client-socket-removed",
                    G_CALLBACK (client_socket_removed_cb),
                    self);
  gst_object_unref (multisocketsink);

  self->server = soup_server_new ("server-header",
                                  "simple-httpd ",
                                  NULL);

  success = soup_server_listen_all (self->server, 0, 0, error);
  if (!success)
    {
      g_set_error (error,
                   CC_ERROR,
                   CC_ERROR_HTTP_SERVER_LISTEN_FAILED,
                   "Failed to start the HTTP server");
      return FALSE;
    }

  gslist = soup_server_get_uris (self->server);
  soup_uri = (GUri *) gslist->data;
  self->port = (guint) g_uri_get_port (soup_uri);

  g_debug ("CcHttpServer: Listening on port %d", self->port);

  soup_server_add_handler (self->server, NULL,
                           server_callback, self, NULL);

  return TRUE;
}

CcHttpServer *
cc_http_server_new (gchar *remote_address)
{
  CcHttpServer *self = g_object_new (CC_TYPE_HTTP_SERVER, NULL);

  self->remote_address = g_strdup (remote_address);

  return g_steal_pointer (&self);
}

void
cc_http_server_finalize (GObject *object)
{
  CcHttpServer *self;

  g_debug ("CcHttpServer: Finalizing");

  self = CC_HTTP_SERVER (object);

  if (SOUP_IS_SERVER (self->server))
    soup_server_remove_handler (self->server, NULL);

  g_clear_pointer (&self->remote_address, g_free);

  G_OBJECT_CLASS (cc_http_server_parent_class)->finalize (object);
}

gboolean
cc_http_server_lookup_encoders (CcHttpServer *self,
                                GStrv        *missing_video,
                                GStrv        *missing_audio)
{
  return cc_media_factory_lookup_encoders (CC_MEDIA_FACTORY (self),
                                           PROFILE_LAST,
                                           missing_video,
                                           missing_audio);
}

static void
cc_http_server_class_init (CcHttpServerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = cc_http_server_get_property;
  object_class->finalize = cc_http_server_finalize;

  props[PROP_PORT] = g_param_spec_uint ("port",
                                        "Port Number",
                                        "Port number on which the HTTP server listens",
                                        0, G_MAXUINT, 0,
                                        G_PARAM_READABLE);

  g_object_class_install_properties (object_class, NR_PROPS, props);

  signals[SIGNAL_STREAM_STARTED] =
    g_signal_new ("stream-started", CC_TYPE_HTTP_SERVER, G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

void
cc_http_server_init (CcHttpServer *self)
{
}
