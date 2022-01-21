#include "wfd-server.h"
#include "wfd-client.h"
#include "wfd-media-factory.h"
#include "wfd-session-pool.h"

struct _WfdServer
{
  GstRTSPServer parent_instance;

  guint         clean_pool_source_id;
};

G_DEFINE_TYPE (WfdServer, wfd_server, GST_TYPE_RTSP_SERVER)

enum {
  SIGNAL_CREATE_SOURCE,
  SIGNAL_CREATE_AUDIO_SOURCE,
  NR_SIGNALS
};

static guint signals[NR_SIGNALS];


WfdServer *
wfd_server_new (void)
{
  /* We use our own session pool. The only reason to do so is
   * to shorten the session ID to 15 characters!
   */
  return g_object_new (WFD_TYPE_SERVER,
                       "session-pool", wfd_session_pool_new (),
                       NULL);
}

static void
wfd_server_finalize (GObject *object)
{
  WfdServer *self = (WfdServer *) object;

  g_debug ("WfdServer: Finalize");

  g_source_remove (self->clean_pool_source_id);
  self->clean_pool_source_id = 0;

  G_OBJECT_CLASS (wfd_server_parent_class)->finalize (object);
}

static GstRTSPClient *
wfd_server_create_client (GstRTSPServer *server)
{
  g_autoptr(WfdClient) client = NULL;
  GstRTSPClient *rtsp_client;

  g_autoptr(GstRTSPSessionPool) session_pool = NULL;
  g_autoptr(GstRTSPMountPoints) mount_points = NULL;
  g_autoptr(GstRTSPAuth) auth = NULL;
  g_autoptr(GstRTSPThreadPool) thread_pool = NULL;

  client = wfd_client_new ();
  rtsp_client = GST_RTSP_CLIENT (client);

  session_pool = gst_rtsp_server_get_session_pool (server);
  gst_rtsp_client_set_session_pool (rtsp_client, session_pool);
  mount_points = gst_rtsp_server_get_mount_points (server);
  gst_rtsp_client_set_mount_points (rtsp_client, mount_points);
  auth = gst_rtsp_server_get_auth (server);
  gst_rtsp_client_set_auth (rtsp_client, auth);
  thread_pool = gst_rtsp_server_get_thread_pool (server);
  gst_rtsp_client_set_thread_pool (rtsp_client, thread_pool);

  return GST_RTSP_CLIENT (g_steal_pointer (&client));
}

static gboolean
timeout_query_wfd_support (gpointer user_data)
{
  WfdClient *client = WFD_CLIENT (user_data);

  g_object_set_data (G_OBJECT (client),
                     "wfd-query-support-timeout",
                     NULL);

  wfd_client_query_support (client);

  return G_SOURCE_REMOVE;
}

static void
wfd_server_client_closed (GstRTSPServer *server, GstRTSPClient *client)
{
  guint query_support_id;

  query_support_id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (client),
                                                          "wfd-query-support-timeout"));

  if (query_support_id)
    g_source_remove (query_support_id);
}

static void
wfd_server_client_connected (GstRTSPServer *server, GstRTSPClient *client)
{
  guint query_support_id;

  query_support_id = g_timeout_add (500, timeout_query_wfd_support, client);

  g_object_set_data (G_OBJECT (client),
                     "wfd-query-support-timeout",
                     GUINT_TO_POINTER (query_support_id));

  g_signal_connect_object (client,
                           "closed",
                           (GCallback) wfd_server_client_closed,
                           server,
                           G_CONNECT_SWAPPED);
}

static void
wfd_server_class_init (WfdServerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstRTSPServerClass *server_class = GST_RTSP_SERVER_CLASS (klass);

  object_class->finalize = wfd_server_finalize;

  server_class->create_client = wfd_server_create_client;
  server_class->client_connected = wfd_server_client_connected;

  signals[SIGNAL_CREATE_SOURCE] =
    g_signal_new ("create-source", WFD_TYPE_SERVER, G_SIGNAL_RUN_LAST,
                  0,
                  g_signal_accumulator_first_wins, NULL,
                  NULL,
                  GST_TYPE_ELEMENT, 0);

  signals[SIGNAL_CREATE_AUDIO_SOURCE] =
    g_signal_new ("create-audio-source", WFD_TYPE_SERVER, G_SIGNAL_RUN_LAST,
                  0,
                  g_signal_accumulator_first_wins, NULL,
                  NULL,
                  GST_TYPE_ELEMENT, 0);
}

static gboolean
clean_pool (gpointer user_data)
{
  GstRTSPServer *server = GST_RTSP_SERVER (user_data);

  g_autoptr(GstRTSPSessionPool) pool = NULL;

  pool = gst_rtsp_server_get_session_pool (server);
  gst_rtsp_session_pool_cleanup (pool);

  return G_SOURCE_CONTINUE;
}

static GstElement *
factory_source_create_cb (WfdMediaFactory *factory, WfdServer *self)
{
  GstElement *res;

  g_signal_emit (self, signals[SIGNAL_CREATE_SOURCE], 0, &res);

  return res;
}

static GstElement *
factory_audio_source_create_cb (WfdMediaFactory *factory, WfdServer *self)
{
  GstElement *res;

  g_signal_emit (self, signals[SIGNAL_CREATE_AUDIO_SOURCE], 0, &res);

  return res;
}

static void
wfd_server_init (WfdServer *self)
{
  g_autoptr(WfdMediaFactory) factory = NULL;
  g_autoptr(GstRTSPMountPoints) mount_points = NULL;
  /* We need to clean up the pool regularly as it does not happen
   * automatically. */
  self->clean_pool_source_id = g_timeout_add_seconds (2, clean_pool, self);

  factory = wfd_media_factory_new ();
  g_signal_connect_object (factory,
                           "create-source",
                           (GCallback) factory_source_create_cb,
                           self,
                           0);

  g_signal_connect_object (factory,
                           "create-audio-source",
                           (GCallback) factory_audio_source_create_cb,
                           self,
                           0);

  mount_points = gst_rtsp_server_get_mount_points (GST_RTSP_SERVER (self));
  gst_rtsp_mount_points_add_factory (mount_points, "/wfd1.0", GST_RTSP_MEDIA_FACTORY (g_steal_pointer (&factory)));

  gst_rtsp_server_set_address (GST_RTSP_SERVER (self), "0.0.0.0");
  gst_rtsp_server_set_service (GST_RTSP_SERVER (self), "7236");
}

static GstRTSPFilterResult
pool_filter_remove_cb (GstRTSPSessionPool *pool,
                       GstRTSPSession     *session,
                       gpointer            user_data)
{
  return GST_RTSP_FILTER_REMOVE;
}

static GstRTSPFilterResult
client_filter_remove_cb (GstRTSPServer *server,
                         GstRTSPClient *client,
                         gpointer       user_data)
{
  return GST_RTSP_FILTER_REMOVE;
}

void
wfd_server_purge (WfdServer *self)
{
  GstRTSPServer *server = GST_RTSP_SERVER (self);

  g_autoptr(GstRTSPSessionPool) session_pool = NULL;
  g_autoptr(GstRTSPThreadPool) thread_pool = NULL;

  session_pool = gst_rtsp_server_get_session_pool (server);
  gst_rtsp_session_pool_filter (session_pool, pool_filter_remove_cb, NULL);

  gst_rtsp_server_client_filter (server, client_filter_remove_cb, NULL);

  thread_pool = gst_rtsp_server_get_thread_pool (server);
  gst_rtsp_session_pool_filter (session_pool, pool_filter_remove_cb, NULL);
}
