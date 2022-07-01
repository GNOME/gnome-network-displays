/* nd-cc-provider.h
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

#include <avahi-gobject/ga-client.h>
#include <gio/gio.h>
#include "nd-provider.h"

G_BEGIN_DECLS

#define ND_TYPE_CC_PROVIDER (nd_cc_provider_get_type ())
G_DECLARE_FINAL_TYPE (NdCCProvider, nd_cc_provider, ND, CC_PROVIDER, GObject)

NdCCProvider * nd_cc_provider_new (GaClient * client);

GaClient * nd_cc_provider_get_client (NdCCProvider *provider);

GSocketClient * nd_cc_provider_get_signalling_client (NdCCProvider *provider);

gboolean nd_cc_provider_browse (NdCCProvider * provider,
                                GError       * error);

G_END_DECLS
