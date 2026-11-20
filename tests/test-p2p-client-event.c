/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Exercise the real private callbacks without NetworkManager or a network
 * connection: the sink is created without NM objects and nm_ac stays NULL. */
#include "../src/nd-wfd-p2p-sink.c"

gchar *
nd_uri_helpers_generate_uri (GHashTable *params)
{
  g_assert_not_reached ();
}

GHashTable *
nd_uri_helpers_parse_uri (gchar *uri)
{
  g_assert_not_reached ();
}

typedef struct
{
  GThread *thread;
  guint    notifications;
} Observer;

typedef struct
{
  WfdClient *client;
  gboolean   close;
} WorkerEvent;

static void
state_notified (GObject *sink, GParamSpec *pspec, Observer *observer)
{
  g_assert_true (g_thread_self () == observer->thread);
  g_assert_true (g_main_context_is_owner (g_main_context_default ()));
  observer->notifications++;
}

static gpointer
send_event (gpointer data)
{
  WorkerEvent *event = data;

  if (event->close)
    g_signal_emit_by_name (event->client, "closed");
  else
    g_signal_emit_by_name (event->client, "play-request", NULL);
  return NULL;
}

/* Emit the RTSP client signal from another thread, as gst-rtsp-server does. */
static void
queue_event (WfdClient *client, gboolean close)
{
  WorkerEvent event = { client, close };

  g_thread_join (g_thread_new ("rtsp-test", send_event, &event));
}

static NdWFDP2PSink *
new_sink (void)
{
  NdWFDP2PSink *sink = g_object_new (ND_TYPE_WFD_P2P_SINK, NULL);

  sink->state = ND_SINK_STATE_WAIT_STREAMING;
  return sink;
}

static void
drain (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static void
test_play_and_close (void)
{
  g_autoptr(NdWFDP2PSink) sink = new_sink ();
  g_autoptr(WfdClient) client = wfd_client_new ();
  Observer observer = { g_thread_self (), 0 };

  watch_client (sink, client);
  g_signal_connect (sink, "notify::state", G_CALLBACK (state_notified), &observer);

  queue_event (client, FALSE);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_WAIT_STREAMING);
  drain ();
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_STREAMING);
  g_assert_cmpuint (observer.notifications, ==, 1);

  /* A second PLAY while streaming is ignored. */
  queue_event (client, FALSE);
  drain ();
  g_assert_cmpuint (observer.notifications, ==, 1);

  queue_event (client, TRUE);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_STREAMING);
  drain ();
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_DISCONNECTED);
  g_assert_null (sink->wfd_client);
  g_assert_cmpuint (observer.notifications, ==, 2);
}

static void
test_replaced_client_is_ignored (void)
{
  g_autoptr(NdWFDP2PSink) sink = new_sink ();
  g_autoptr(WfdClient) old = wfd_client_new ();
  g_autoptr(WfdClient) current = wfd_client_new ();
  Observer observer = { g_thread_self (), 0 };

  g_signal_connect (sink, "notify::state", G_CALLBACK (state_notified), &observer);
  watch_client (sink, old);

  queue_event (old, FALSE);
  queue_event (old, TRUE);
  g_signal_handlers_disconnect_by_data (old, sink);
  g_clear_object (&sink->wfd_client);
  watch_client (sink, current);
  drain ();
  g_assert_true (sink->wfd_client == current);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_WAIT_STREAMING);
  g_assert_cmpuint (observer.notifications, ==, 0);

  sink->state = ND_SINK_STATE_ERROR;
  queue_event (current, FALSE);
  drain ();
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_ERROR);
  g_assert_cmpuint (observer.notifications, ==, 0);
}

static void
test_cancel_before_dispatch (void)
{
  NdWFDP2PSink *sink = new_sink ();
  WfdClient *client = wfd_client_new ();
  gpointer weak_sink = sink, weak_client = client;

  g_object_add_weak_pointer (G_OBJECT (sink), &weak_sink);
  g_object_add_weak_pointer (G_OBJECT (client), &weak_client);
  watch_client (sink, client);

  queue_event (client, FALSE);
  queue_event (client, TRUE);
  nd_sink_stop_stream (ND_SINK (sink));
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_DISCONNECTED);
  g_assert_null (sink->wfd_client);
  g_object_unref (sink);
  g_object_unref (client);
  g_assert_nonnull (weak_sink);
  g_assert_nonnull (weak_client);
  drain ();
  g_assert_null (weak_sink);
  g_assert_null (weak_client);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  gst_init (&argc, &argv);
  g_test_add_func ("/p2p-client-event/play-close", test_play_and_close);
  g_test_add_func ("/p2p-client-event/replaced-client", test_replaced_client_is_ignored);
  g_test_add_func ("/p2p-client-event/cancel-before-dispatch", test_cancel_before_dispatch);
  return g_test_run ();
}
