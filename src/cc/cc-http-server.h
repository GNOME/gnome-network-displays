/* cc-http-server.h
 *
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

#include <glib-object.h>
#include <libsoup/soup.h>
#include <glib/gstdio.h>

#include "../wfd/wfd-media-factory.h"

G_BEGIN_DECLS

#define CC_TYPE_HTTP_SERVER (cc_http_server_get_type ())
G_DECLARE_FINAL_TYPE (CcHttpServer, cc_http_server, CC, HTTP_SERVER, GObject)

CcHttpServer * cc_http_server_new (void);

void cc_http_server_set_pipeline_state (CcHttpServer *self,
                                        GstState      state);

guint cc_http_server_get_port (CcHttpServer *self);
void cc_http_server_set_remote_address (CcHttpServer *self,
                                        gchar        *remote_address);

gboolean cc_http_server_start_server (CcHttpServer *self,
                                      GError      **error);
void cc_http_server_finalize (CcHttpServer *self);

G_END_DECLS
