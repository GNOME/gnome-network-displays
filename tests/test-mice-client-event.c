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
  NdWFDMiceSink *sink;
  WfdClient     *client;
  gboolean       close;
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
    closed_cb (event->sink, event->client);
  else
    play_request_cb (event->sink, NULL, event->client);
  return NULL;
}

static void
queue_event (NdWFDMiceSink *sink, WfdClient *client, gboolean close)
{
  WorkerEvent event = { sink, client, close };
  GThread *thread = g_thread_new ("rtsp-test", send_event, &event);

  g_thread_join (thread);
}

static NdWFDMiceSink *
new_sink (void)
{
  NdWFDMiceSink *sink = g_object_new (ND_TYPE_WFD_MICE_SINK, NULL);

  g_assert_true (sink->main_context == g_main_context_default ());
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
  sink->wfd_client = g_object_ref (client);
  g_signal_connect (sink, "notify::state", G_CALLBACK (state_notified), &observer);

  queue_event (sink, client, FALSE);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_WAIT_STREAMING);
  g_assert_cmpuint (observer.notifications, ==, 0);
  g_autoptr(GMainContext) unrelated_context = g_main_context_new ();
  drain (unrelated_context);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_WAIT_STREAMING);
  drain (context);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_STREAMING);
  g_assert_cmpuint (observer.notifications, ==, 1);

  queue_event (sink, client, FALSE);
  drain (context);
  g_assert_cmpuint (observer.notifications, ==, 1);

  queue_event (sink, client, TRUE);
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
  sink->wfd_client = g_object_ref (old);

  queue_event (sink, old, FALSE);
  queue_event (sink, old, TRUE);
  g_set_object (&sink->wfd_client, current);
  g_clear_object (&old);
  g_assert_nonnull (weak_old);
  drain (context);
  g_assert_null (weak_old);
  g_assert_true (sink->wfd_client == current);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_WAIT_STREAMING);
  g_assert_cmpuint (observer.notifications, ==, 0);

  sink->state = ND_SINK_STATE_ERROR;
  queue_event (sink, current, FALSE);
  drain (context);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_ERROR);
  g_assert_cmpuint (observer.notifications, ==, 0);

  sink->state = ND_SINK_STATE_WAIT_STREAMING;
  queue_event (sink, current, FALSE);
  drain (context);
  g_assert_cmpint (sink->state, ==, ND_SINK_STATE_STREAMING);
  queue_event (sink, current, TRUE);
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
  sink->wfd_client = g_object_ref (client);

  queue_event (sink, client, FALSE);
  queue_event (sink, client, TRUE);
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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  gst_init (&argc, &argv);
  g_test_add_func ("/mice-client-event/global-default-play-close", test_play_and_close_on_default_context);
  g_test_add_func ("/mice-client-event/replaced-client", test_replaced_client_is_ignored);
  g_test_add_func ("/mice-client-event/cancel-before-dispatch", test_cancel_before_dispatch);
  return g_test_run ();
}
