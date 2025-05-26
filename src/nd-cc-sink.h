/* nd-cc-sink.h
 *
 * Copyright 2022 Christian Glombek <lorbus@fedoraproject.org>
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

#pragma once

#include <gio/gio.h>
#include "nd-sink.h"

G_BEGIN_DECLS

#define ND_TYPE_CC_SINK (nd_cc_sink_get_type ())
G_DECLARE_FINAL_TYPE (NdCCSink, nd_cc_sink, ND, CC_SINK, GObject)

NdCCSink * nd_cc_sink_new (GSocketClient * client,
                           gchar         * name,
                           gchar         * ip,
                           gchar         * display_name,
                           gint            interface);

NdCCSink * nd_cc_sink_from_uri (gchar *uri);

G_END_DECLS
