/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

typedef void (*NdClientEventFunc) (GObject *owner,
                                   GObject *client);

/* Always queue, even when the caller owns context. NULL selects the default
 * main context. Both objects remain alive until dispatch or source removal.
 * The callback must reject events from a canceled or replaced client. */
guint nd_client_event_queue (GMainContext     *context,
                             GObject          *owner,
                             GObject          *client,
                             NdClientEventFunc callback);

/* Queue every @signal_name emission on @client, from whichever thread emits
 * it, for @callback on the default main context. @owner is only held weakly
 * until an emission queues an event, so it may be finalized while an emission
 * is running. @owner is the handler data: disconnect with
 * g_signal_handlers_disconnect_by_data (client, owner). */
gulong nd_client_event_connect (GObject          *client,
                                const gchar      *signal_name,
                                GObject          *owner,
                                NdClientEventFunc callback);

G_END_DECLS
