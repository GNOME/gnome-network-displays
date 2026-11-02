/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "nd-client-event.h"

typedef struct
{
  GMainContext *context;
  GThread      *thread;
  GObject      *owner;
  GObject      *client;
  guint         calls;
  GString      *order;
} TestState;

static TestState *
check_dispatch (GObject *owner, GObject *client)
{
  TestState *state = g_object_get_data (owner, "test-state");

  g_assert_true (g_thread_self () == state->thread);
  g_assert_true (g_main_context_is_owner (state->context));
  g_assert_true (owner == state->owner);
  g_assert_true (client == state->client);
  state->calls++;
  return state;
}

static void
received (GObject *owner, GObject *client)
{
  check_dispatch (owner, client);
}

static void
play (GObject *owner, GObject *client)
{
  g_string_append_c (check_dispatch (owner, client)->order, 'P');
}

static void
closed (GObject *owner, GObject *client)
{
  g_string_append_c (check_dispatch (owner, client)->order, 'C');
}

static gpointer
queue_from_worker (gpointer data)
{
  TestState *state = data;

  g_autoptr(GMainContext) worker_context = g_main_context_new ();

  g_main_context_push_thread_default (worker_context);
  nd_client_event_queue (state->context, state->owner, state->client, received);
  g_main_context_pop_thread_default (worker_context);
  return NULL;
}

static TestState
state_new (GMainContext *context, GObject *owner, GObject *client)
{
  TestState state = { context, g_thread_self (), owner, client, 0, NULL };

  return state;
}

static void
test_worker_context_and_lifetime (void)
{
  g_autoptr(GMainContext) context = g_main_context_new ();
  GObject *owner = g_object_new (G_TYPE_OBJECT, NULL);
  GObject *client = g_object_new (G_TYPE_OBJECT, NULL);
  gpointer weak_owner = owner, weak_client = client;
  TestState state = state_new (context, owner, client);
  g_object_set_data (owner, "test-state", &state);
  g_object_add_weak_pointer (owner, &weak_owner);
  g_object_add_weak_pointer (client, &weak_client);

  GThread *worker = g_thread_new ("rtsp-test", queue_from_worker, &state);
  g_thread_join (worker);
  g_assert_cmpuint (state.calls, ==, 0);
  g_object_unref (owner);
  g_object_unref (client);
  g_assert_nonnull (weak_owner);
  g_assert_nonnull (weak_client);

  g_assert_true (g_main_context_iteration (context, FALSE));
  g_assert_cmpuint (state.calls, ==, 1);
  g_assert_null (weak_owner);
  g_assert_null (weak_client);
  g_assert_false (g_main_context_pending (context));
}

static void
test_always_async_default (void)
{
  g_autoptr(GObject) owner = g_object_new (G_TYPE_OBJECT, NULL);
  g_autoptr(GObject) client = g_object_new (G_TYPE_OBJECT, NULL);
  GMainContext *context = g_main_context_default ();
  TestState state = state_new (context, owner, client);
  g_object_set_data (owner, "test-state", &state);

  /* invoke_full would run inline when the caller owns this context. */
  g_assert_true (g_main_context_acquire (context));
  nd_client_event_queue (NULL, owner, client, received);
  g_assert_cmpuint (state.calls, ==, 0);
  g_main_context_release (context);
  g_assert_true (g_main_context_iteration (context, FALSE));
  g_assert_cmpuint (state.calls, ==, 1);
}

static void
test_source_removal_releases_objects (void)
{
  g_autoptr(GMainContext) context = g_main_context_new ();
  GObject *owner = g_object_new (G_TYPE_OBJECT, NULL);
  GObject *client = g_object_new (G_TYPE_OBJECT, NULL);
  gpointer weak_owner = owner, weak_client = client;
  TestState state = state_new (context, owner, client);
  g_object_set_data (owner, "test-state", &state);
  g_object_add_weak_pointer (owner, &weak_owner);
  g_object_add_weak_pointer (client, &weak_client);

  guint id = nd_client_event_queue (context, owner, client, received);
  g_object_unref (owner);
  g_object_unref (client);
  GSource *source = g_main_context_find_source_by_id (context, id);
  g_assert_nonnull (source);
  g_source_destroy (source);
  g_assert_null (weak_owner);
  g_assert_null (weak_client);
  g_assert_cmpuint (state.calls, ==, 0);
  g_assert_false (g_main_context_pending (context));
}

static void
test_event_order (void)
{
  g_autoptr(GMainContext) context = g_main_context_new ();
  g_autoptr(GObject) owner = g_object_new (G_TYPE_OBJECT, NULL);
  g_autoptr(GObject) client = g_object_new (G_TYPE_OBJECT, NULL);
  g_autoptr(GString) order = g_string_new (NULL);
  TestState state = state_new (context, owner, client);
  state.order = order;
  g_object_set_data (owner, "test-state", &state);

  nd_client_event_queue (context, owner, client, play);
  nd_client_event_queue (context, owner, client, closed);
  g_assert_cmpuint (state.calls, ==, 0);
  while (g_main_context_iteration (context, FALSE))
    ;
  g_assert_cmpuint (state.calls, ==, 2);
  g_assert_cmpstr (order->str, ==, "PC");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/client-event/worker-context-and-lifetime", test_worker_context_and_lifetime);
  g_test_add_func ("/client-event/always-async-default", test_always_async_default);
  g_test_add_func ("/client-event/source-removal-releases-objects", test_source_removal_releases_objects);
  g_test_add_func ("/client-event/event-order", test_event_order);
  return g_test_run ();
}
