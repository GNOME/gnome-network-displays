/* nd-nm-device-registry.c
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

#include "gnome-network-displays-config.h"
#include "NetworkManager.h"
#include "nd-nm-device-registry.h"
#include "nd-wfd-p2p-provider.h"

struct _NdNMDeviceRegistry
{
  GObject         parent_instance;

  GPtrArray      *providers;
  NdMetaProvider *meta_provider;

  GCancellable   *cancellable;
  NMClient       *nm_client;
};

enum {
  PROP_META_PROVIDER = 1,
  PROP_LAST,
};

G_DEFINE_TYPE (NdNMDeviceRegistry, nd_nm_device_registry, G_TYPE_OBJECT)

static GParamSpec * props[PROP_LAST] = { NULL, };

static void
device_added_cb (NdNMDeviceRegistry *registry, NMDevice *device, NMClient *client)
{
  g_autoptr(NdWFDP2PProvider) provider = NULL;

  if (!NM_IS_DEVICE_WIFI_P2P (device))
    return;

  g_debug ("NdNMDeviceRegistry: Found a new device, creating provider");

  provider = nd_wfd_p2p_provider_new (client, device);

  g_ptr_array_add (registry->providers, g_object_ref (provider));
  nd_meta_provider_add_provider (registry->meta_provider,
                                 ND_PROVIDER (g_steal_pointer (&provider)));
}

static void
device_removed_cb (NdNMDeviceRegistry *registry, NMDevice *device, NMClient *client)
{
  if (!NM_IS_DEVICE_WIFI_P2P (device))
    return;

  g_debug ("NdNMDeviceRegistry: Lost a device, removing provider");

  for (gint i = 0; i < registry->providers->len; i++)
    {
      NdWFDP2PProvider *provider = g_ptr_array_index (registry->providers, i);

      if (nd_wfd_p2p_provider_get_device (provider) != device)
        continue;

      nd_meta_provider_remove_provider (registry->meta_provider, ND_PROVIDER (provider));
      g_ptr_array_remove_index (registry->providers, i);
      break;
    }
}

static void
nd_nm_device_registry_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  NdNMDeviceRegistry *registry = ND_NM_DEVICE_REGISTRY (object);

  switch (prop_id)
    {
    case PROP_META_PROVIDER:
      g_value_set_object (value, registry->meta_provider);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_nm_device_registry_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  NdNMDeviceRegistry *registry = ND_NM_DEVICE_REGISTRY (object);

  switch (prop_id)
    {
    case PROP_META_PROVIDER:
      g_assert (registry->meta_provider == NULL);

      registry->meta_provider = g_value_dup_object (value);

      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
client_init_async_finished (GObject *source, GAsyncResult *res, gpointer user_data)
{
  NdNMDeviceRegistry *registry = NULL;

  g_autoptr(GError) error = NULL;

  if (!g_async_initable_init_finish (G_ASYNC_INITABLE (source), res, &error))
    {
      /* Operation was aborted */
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      registry = ND_NM_DEVICE_REGISTRY (user_data);
      g_clear_object (&registry->nm_client);
      g_warning ("NdNMDeviceRegistry: Error initialising NMClient: %s", error->message);
    }

  g_debug ("NdNMDeviceRegistry: Got NMClient");

  registry = ND_NM_DEVICE_REGISTRY (user_data);

  /* Everything good, we already connected and possibly received
   * the device-added/device-removed signals. */
}

static void
nd_nm_device_registry_constructed (GObject *object)
{
  NdNMDeviceRegistry *registry = ND_NM_DEVICE_REGISTRY (object);

  registry->cancellable = g_cancellable_new ();
  registry->nm_client = g_object_new (NM_TYPE_CLIENT, NULL);

  g_signal_connect_object (registry->nm_client,
                           "device-added",
                           (GCallback) device_added_cb,
                           registry,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (registry->nm_client,
                           "device-removed",
                           (GCallback) device_removed_cb,
                           registry,
                           G_CONNECT_SWAPPED);

  g_async_initable_init_async (G_ASYNC_INITABLE (registry->nm_client),
                               G_PRIORITY_LOW,
                               registry->cancellable,
                               client_init_async_finished,
                               registry);
}

static void
nd_nm_device_registry_finalize (GObject *object)
{
  NdNMDeviceRegistry *registry = ND_NM_DEVICE_REGISTRY (object);

  while (registry->providers->len)
    {
      nd_meta_provider_remove_provider (registry->meta_provider,
                                        ND_PROVIDER (g_ptr_array_index ( registry->providers, 0)));
      g_ptr_array_remove_index_fast (registry->providers, 0);
    }

  g_cancellable_cancel (registry->cancellable);
  g_clear_object (&registry->nm_client);
  g_clear_object (&registry->cancellable);
  g_clear_object (&registry->meta_provider);
  g_clear_pointer (&registry->providers, g_ptr_array_unref);

  G_OBJECT_CLASS (nd_nm_device_registry_parent_class)->finalize (object);
}

static void
nd_nm_device_registry_class_init (NdNMDeviceRegistryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = nd_nm_device_registry_get_property;
  object_class->set_property = nd_nm_device_registry_set_property;
  object_class->constructed = nd_nm_device_registry_constructed;
  object_class->finalize = nd_nm_device_registry_finalize;

  props[PROP_META_PROVIDER] =
    g_param_spec_object ("meta-provider", "MetaProvider",
                         "The meta provider to add found providers to.",
                         ND_TYPE_META_PROVIDER,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, props);
}

static void
nd_nm_device_registry_init (NdNMDeviceRegistry *registry)
{
  registry->providers = g_ptr_array_new_with_free_func (g_object_unref);
}

NdNMDeviceRegistry *
nd_nm_device_registry_new (NdMetaProvider *meta_provider)
{
  return g_object_new (ND_TYPE_NM_DEVICE_REGISTRY,
                       "meta-provider", meta_provider,
                       NULL);
}
