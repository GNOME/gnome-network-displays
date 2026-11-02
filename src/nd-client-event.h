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

G_END_DECLS
