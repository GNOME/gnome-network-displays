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
