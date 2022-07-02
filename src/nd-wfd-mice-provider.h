/* nd-wfd-mice-provider.h
 *
 * Copyright 2022 Christian Glombek <lorbus@fedoraproject.org>
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

#include <avahi-gobject/ga-client.h>
#include <gio/gio.h>
#include "nd-provider.h"

G_BEGIN_DECLS

#define ND_TYPE_WFD_MICE_PROVIDER (nd_wfd_mice_provider_get_type ())
G_DECLARE_FINAL_TYPE (NdWFDMiceProvider, nd_wfd_mice_provider, ND, WFD_MICE_PROVIDER, GObject)

NdWFDMiceProvider * nd_wfd_mice_provider_new (GaClient * client);

GaClient *  nd_wfd_mice_provider_get_client (NdWFDMiceProvider *provider);

gboolean nd_wfd_mice_provider_browse (NdWFDMiceProvider *provider,
                                      GError           * error);

G_END_DECLS
