/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "nd-client-event.h"

typedef struct
{
  GObject          *owner;
  GObject          *client;
  NdClientEventFunc callback;
} ClientEvent;

static void
client_event_free (gpointer data)
{
  ClientEvent *event = data;

  g_object_unref (event->client);
  g_object_unref (event->owner);
  g_free (event);
}

static gboolean
client_event_dispatch (gpointer data)
{
  ClientEvent *event = data;

  event->callback (event->owner, event->client);
  return G_SOURCE_REMOVE;
}

guint
nd_client_event_queue (GMainContext     *context,
                       GObject          *owner,
                       GObject          *client,
                       NdClientEventFunc callback)
{
  g_autoptr(GSource) source = NULL;
  ClientEvent *event;

  g_return_val_if_fail (G_IS_OBJECT (owner), 0);
  g_return_val_if_fail (G_IS_OBJECT (client), 0);
  g_return_val_if_fail (callback != NULL, 0);

  event = g_new (ClientEvent, 1);
  event->owner = g_object_ref (owner);
  event->client = g_object_ref (client);
  event->callback = callback;

  /* RTSP emits PLAY while holding its media lock. Never invoke inline or
   * wait for the owning context from that callback. */
  source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_callback (source, client_event_dispatch, event, client_event_free);
  return g_source_attach (source, context);
}

typedef struct
{
  GClosure          closure;
  GWeakRef          owner;
  NdClientEventFunc callback;
} ClientEventClosure;

static void
client_event_closure_marshal (GClosure     *closure,
                              GValue       *return_value,
                              guint         n_param_values,
                              const GValue *param_values,
                              gpointer      invocation_hint,
                              gpointer      marshal_data)
{
  ClientEventClosure *self = (ClientEventClosure *) closure;

  g_autoptr(GObject) owner = g_weak_ref_get (&self->owner);

  /* Runs on the emitting thread, where the owner may be finalized at any
   * time: an emission can still be running after the main thread has
   * disconnected the handler and dropped the owner. */
  if (owner)
    nd_client_event_queue (NULL, owner, g_value_get_object (&param_values[0]), self->callback);
}

static void
client_event_closure_finalize (gpointer data, GClosure *closure)
{
  g_weak_ref_clear (&((ClientEventClosure *) closure)->owner);
}

gulong
nd_client_event_connect (GObject          *client,
                         const gchar      *signal_name,
                         GObject          *owner,
                         NdClientEventFunc callback)
{
  GClosure *closure;
  ClientEventClosure *self;

  g_return_val_if_fail (G_IS_OBJECT (client), 0);
  g_return_val_if_fail (G_IS_OBJECT (owner), 0);
  g_return_val_if_fail (callback != NULL, 0);

  /* The closure data only identifies the handler; it is never dereferenced. */
  closure = g_closure_new_simple (sizeof (ClientEventClosure), owner);
  self = (ClientEventClosure *) closure;
  g_weak_ref_init (&self->owner, owner);
  self->callback = callback;
  g_closure_set_marshal (closure, client_event_closure_marshal);
  g_closure_add_finalize_notifier (closure, NULL, client_event_closure_finalize);

  return g_signal_connect_closure (client, signal_name, closure, FALSE);
}
