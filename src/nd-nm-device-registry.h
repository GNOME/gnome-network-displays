/* nd-nm-device-registry.h
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

#include "nd-meta-provider.h"

G_BEGIN_DECLS

#define ND_TYPE_NM_DEVICE_REGISTRY (nd_nm_device_registry_get_type ())
G_DECLARE_FINAL_TYPE (NdNMDeviceRegistry, nd_nm_device_registry, ND, NM_DEVICE_REGISTRY, GObject)

NdNMDeviceRegistry * nd_nm_device_registry_new (NdMetaProvider * meta_provider);

G_END_DECLS
