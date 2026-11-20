/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Exercise the real private callbacks without creating a network connection. */
#include "../src/nd-wfd-mice-sink.c"

/* URI factories are unrelated to these callbacks; do not instantiate other
 * transports or discover/connect to any receiver in this unit test. */
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
  GMainContext *context;
  GThread      *thread;
  guint         notifications;
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
  g_assert_true (g_main_context_is_owner (observer->context));
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
  GThread *thread = g_thread_new ("rtsp-test", send_event, &event);

  g_thread_join (thread);
}

static NdWFDMiceSink *
new_sink (void)
{
  NdWFDMiceSink *sink = g_object_new (ND_TYPE_WFD_MICE_SINK, NULL);

  sink->state = ND_SINK_STATE_WAIT_STREAMING;
  return sink;
}

static void
drain (GMainContext *context)
{
  while (g_main_context_iteration (context, FALSE))
    ;
}

static void
test_play_and_close_on_default_context (void)
{
  g_autoptr(GMainContext) context = g_main_context_ref (g_main_context_default ());
  g_autoptr(NdWFDMiceSink) sink = new_sink ();
  g_autoptr(WfdClient) client = wfd_client_new ();
  Observer observer = { context, g_thread_self (), 0 };
  watch_client (sink, client);
  g_signal_connect (sink, "notify::state", G_CALLBACK (state_notified), &observer);

  queue_event (client, FALSE);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_WAIT_STREAMING);
  g_assert_cmpuint (observer.notifications, ==, 0);
  g_autoptr(GMainContext) unrelated_context = g_main_context_new ();
  drain (unrelated_context);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_WAIT_STREAMING);
  drain (context);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_STREAMING);
  g_assert_cmpuint (observer.notifications, ==, 1);

  queue_event (client, FALSE);
  drain (context);
  g_assert_cmpuint (observer.notifications, ==, 1);

  queue_event (client, TRUE);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_STREAMING);
  drain (context);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_DISCONNECTED);
  g_assert_null (sink->wfd_client);
  g_assert_cmpuint (observer.notifications, ==, 2);
}

static void
test_replaced_client_is_ignored (void)
{
  g_autoptr(GMainContext) context = g_main_context_ref (g_main_context_default ());
  g_autoptr(NdWFDMiceSink) sink = new_sink ();
  g_autoptr(WfdClient) old = wfd_client_new ();
  g_autoptr(WfdClient) current = wfd_client_new ();
  gpointer weak_old = old;
  Observer observer = { context, g_thread_self (), 0 };
  g_object_add_weak_pointer (G_OBJECT (old), &weak_old);
  g_signal_connect (sink, "notify::state", G_CALLBACK (state_notified), &observer);
  watch_client (sink, old);

  queue_event (old, FALSE);
  queue_event (old, TRUE);
  g_signal_handlers_disconnect_by_data (old, sink);
  g_clear_object (&sink->wfd_client);
  watch_client (sink, current);
  g_clear_object (&old);
  g_assert_nonnull (weak_old);
  drain (context);
  g_assert_null (weak_old);
  g_assert_true (sink->wfd_client == current);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_WAIT_STREAMING);
  g_assert_cmpuint (observer.notifications, ==, 0);

  sink->state = ND_SINK_STATE_ERROR;
  queue_event (current, FALSE);
  drain (context);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_ERROR);
  g_assert_cmpuint (observer.notifications, ==, 0);

  sink->state = ND_SINK_STATE_WAIT_STREAMING;
  queue_event (current, FALSE);
  drain (context);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_STREAMING);
  queue_event (current, TRUE);
  drain (context);
  g_assert_cmpuint (observer.notifications, ==, 2);
}

static void
test_cancel_before_dispatch (void)
{
  g_autoptr(GMainContext) context = g_main_context_ref (g_main_context_default ());
  NdWFDMiceSink *sink = new_sink ();
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
  drain (context);
  g_assert_null (weak_sink);
  g_assert_null (weak_client);
}

static void
test_emission_after_sink_finalized (void)
{
  NdWFDMiceSink *sink = new_sink ();

  g_autoptr(WfdClient) client = wfd_client_new ();
  gpointer weak_sink = sink;

  g_object_add_weak_pointer (G_OBJECT (sink), &weak_sink);
  watch_client (sink, client);

  /* Leave the handlers connected, as for an emission that is already running
   * on the RTSP thread when the main thread drops the sink. */
  g_clear_object (&sink->wfd_client);
  g_object_unref (sink);
  g_assert_null (weak_sink);

  queue_event (client, FALSE);
  queue_event (client, TRUE);
  g_assert_false (g_main_context_pending (g_main_context_default ()));
}

static void
test_failed_start_releases_server (void)
{
  g_autoptr(GSocketClient) probe = g_socket_client_new ();
  g_autoptr(GSocketConnection) listener = g_socket_client_connect_to_host (probe, "127.0.0.1", 7250, NULL, NULL);

  /* A running MICE source, e.g. the GND daemon, would accept the connection. */
  if (listener)
    {
      g_test_skip ("Something is listening on 127.0.0.1:7250");
      return;
    }

  /* With nothing listening on the MICE signalling port, the start fails after
   * the RTSP server has been attached. */
  g_autoptr(NdWFDMiceSink) sink = g_object_new (ND_TYPE_WFD_MICE_SINK, "ip", "127.0.0.1", NULL);
  GLogLevelFlags fatal = g_log_set_always_fatal (G_LOG_LEVEL_CRITICAL);
  NdSink *started = nd_sink_start_stream (ND_SINK (sink));

  g_log_set_always_fatal (fatal);
  g_object_unref (started);

  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_ERROR);
  g_assert_cmpuint (sink->server_source_id, ==, 0);
  g_assert_null (sink->server);
  g_assert_null (sink->signalling_client_conn);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  gst_init (&argc, &argv);
  g_test_add_func ("/mice-client-event/global-default-play-close", test_play_and_close_on_default_context);
  g_test_add_func ("/mice-client-event/replaced-client", test_replaced_client_is_ignored);
  g_test_add_func ("/mice-client-event/cancel-before-dispatch", test_cancel_before_dispatch);
  g_test_add_func ("/mice-client-event/emission-after-sink-finalized", test_emission_after_sink_finalized);
  g_test_add_func ("/mice-client-event/failed-start-releases-server", test_failed_start_releases_server);
  return g_test_run ();
}
