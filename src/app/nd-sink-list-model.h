/* nd-sink-list-model.h
 *
 * Copyright 2018 Benjamin Berg <bberg@redhat.com>
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

#include <gtk/gtk.h>
#include "nd-provider.h"

G_BEGIN_DECLS

#define ND_TYPE_SINK_LIST_MODEL (nd_sink_list_model_get_type ())
G_DECLARE_FINAL_TYPE (NdSinkListModel, nd_sink_list_model, ND, SINK_LIST_MODEL, GObject)

NdSinkListModel * nd_sink_list_model_new (NdProvider * provider);

void nd_sink_list_model_set_provider (NdSinkListModel *sink_list,
                                      NdProvider      *provider);
NdProvider *nd_sink_list_model_get_provider (NdSinkListModel *sink_list);

G_END_DECLS
