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

#include <NetworkManager.h>

#include "gnome-network-displays-config.h"
#include "nd-nm-device-registry.h"
#include "nd-wfd-p2p-provider.h"

#ifdef HAVE_SYSTEMD_RESOLVED
#include "nd-sd-wfd-mice-provider.h"
#include "nd-sd-cc-provider.h"
#endif

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
add_p2p_provider (NdNMDeviceRegistry *registry, NMDevice *device)
{
  /* Check if we already have a provider for this device */
  for (gint i = 0; i < registry->providers->len; i++)
    {
      NdProvider *provider = g_ptr_array_index (registry->providers, i);
      if (!ND_IS_WFD_P2P_PROVIDER (provider))
        continue;

      if (nd_wfd_p2p_provider_get_device (ND_WFD_P2P_PROVIDER (provider)) == device)
        return; /* Already have a provider for this device */
    }

  g_debug ("NdNMDeviceRegistry: Creating Wi-Fi P2P provider: "
           "iface=%s, driver=%s, udi=%s",
           nm_device_get_iface (device),
           nm_device_get_driver (device),
           nm_device_get_udi (device));

  NdWFDP2PProvider *wfd_p2p_provider = nd_wfd_p2p_provider_new (registry->nm_client, device);

  g_ptr_array_add (registry->providers, g_object_ref (wfd_p2p_provider));
  nd_meta_provider_add_provider (registry->meta_provider,
                                 ND_PROVIDER (wfd_p2p_provider));
}

static void
remove_p2p_provider (NdNMDeviceRegistry *registry, NMDevice *device)
{
  for (gint i = 0; i < registry->providers->len; i++)
    {
      NdProvider *provider = g_ptr_array_index (registry->providers, i);
      if (!ND_IS_WFD_P2P_PROVIDER (provider))
        continue;

      if (nd_wfd_p2p_provider_get_device (ND_WFD_P2P_PROVIDER (provider)) != device)
        continue;

      g_debug ("NdNMDeviceRegistry: Removing Wi-Fi P2P provider: "
               "iface=%s, driver=%s, udi=%s",
               nm_device_get_iface (device),
               nm_device_get_driver (device),
               nm_device_get_udi (device));

      nd_meta_provider_remove_provider (registry->meta_provider, provider);
      g_ptr_array_remove_index (registry->providers, i);
      break;
    }
}

static void
p2p_device_state_changed_cb (NdNMDeviceRegistry *registry,
                             NMDeviceState       new_state,
                             NMDeviceState       old_state,
                             NMDeviceStateReason reason,
                             NMDevice           *device)
{
  g_debug ("NdNMDeviceRegistry: P2P device state changed: %s (%d -> %d, reason: %d)",
           nm_device_get_iface (device), old_state, new_state, reason);

  if (new_state > NM_DEVICE_STATE_UNAVAILABLE)
    add_p2p_provider (registry, device);
  else
    remove_p2p_provider (registry, device);
}

#ifdef HAVE_SYSTEMD_RESOLVED
static void
add_sd_wfd_mice_provider (NdNMDeviceRegistry *registry, NMDevice *device)
{
  const char *device_type = NM_IS_DEVICE_WIFI (device) ? "Wi-Fi" : "Ethernet";

  /* Check if we already have a provider for this device */
  for (gint i = 0; i < registry->providers->len; i++)
    {
      NdProvider *provider = g_ptr_array_index (registry->providers, i);
      if (!ND_IS_SD_WFD_MICE_PROVIDER (provider))
        continue;

      NMDevice *provider_device = NULL;
      g_object_get (ND_SD_WFD_MICE_PROVIDER (provider), "device", &provider_device, NULL);
      if (provider_device == device)
        return; /* Already have a provider for this device */
    }

  g_debug ("NdNMDeviceRegistry: Creating %s systemd-resolved MICE provider: "
           "iface=%s, driver=%s, udi=%s",
           device_type,
           nm_device_get_iface (device),
           nm_device_get_driver (device),
           nm_device_get_udi (device));

  g_autoptr(GError) error = NULL;
  g_autoptr(NdSdWfdMiceProvider) sd_wfd_mice_provider = nd_sd_wfd_mice_provider_new (device);

  if (!sd_wfd_mice_provider)
    {
      g_warning ("NdNMDeviceRegistry: Failed to create systemd-resolved MICE provider");
      return;
    }

  g_ptr_array_add (registry->providers, g_object_ref (sd_wfd_mice_provider));
  nd_meta_provider_add_provider (registry->meta_provider, ND_PROVIDER (sd_wfd_mice_provider));
  if (!nd_sd_wfd_mice_provider_browse (sd_wfd_mice_provider, &error))
    {
      g_warning ("NdNMDeviceRegistry: systemd-resolved provider failed to browse: %s",
                 error ? error->message : "unknown error");
      return;
    }

  g_debug ("NdNMDeviceRegistry: Using systemd-resolved mDNS browser for WFD MICE");
}

static void
remove_sd_wfd_mice_provider (NdNMDeviceRegistry *registry, NMDevice *device)
{
  const char *device_type = NM_IS_DEVICE_WIFI (device) ? "Wi-Fi" : "Ethernet";

  for (gint i = 0; i < registry->providers->len; i++)
    {
      NdProvider *provider = g_ptr_array_index (registry->providers, i);
      if (!ND_IS_SD_WFD_MICE_PROVIDER (provider))
        continue;

      NMDevice *provider_device = NULL;
      g_object_get (ND_SD_WFD_MICE_PROVIDER (provider), "device", &provider_device, NULL);
      if (provider_device != device)
        continue;

      g_debug ("NdNMDeviceRegistry: Removing %s systemd-resolved MICE provider: "
               "iface=%s, driver=%s, udi=%s",
               device_type,
               nm_device_get_iface (device),
               nm_device_get_driver (device),
               nm_device_get_udi (device));

      nd_meta_provider_remove_provider (registry->meta_provider, provider);
      g_ptr_array_remove_index (registry->providers, i);
      break;
    }
}

static void
add_sd_cc_provider (NdNMDeviceRegistry *registry, NMDevice *device)
{
  const char *device_type = NM_IS_DEVICE_WIFI (device) ? "Wi-Fi" : "Ethernet";

  /* Check if we already have a provider for this device */
  for (gint i = 0; i < registry->providers->len; i++)
    {
      NdProvider *provider = g_ptr_array_index (registry->providers, i);
      if (!ND_IS_SD_CC_PROVIDER (provider))
        continue;

      NMDevice *provider_device = NULL;
      g_object_get (ND_SD_CC_PROVIDER (provider), "nm-device", &provider_device, NULL);
      if (provider_device == device)
        return; /* Already have a provider for this device */
    }

  g_debug ("NdNMDeviceRegistry: Creating %s systemd-resolved Chromecast provider: "
           "iface=%s, driver=%s, udi=%s",
           device_type,
           nm_device_get_iface (device),
           nm_device_get_driver (device),
           nm_device_get_udi (device));

  g_autoptr(GError) error = NULL;
  g_autoptr(NdSdCCProvider) sd_cc_provider = nd_sd_cc_provider_new (device);

  if (!sd_cc_provider)
    {
      g_warning ("NdNMDeviceRegistry: Failed to create systemd-resolved Chromecast provider");
      return;
    }

  g_ptr_array_add (registry->providers, g_object_ref (sd_cc_provider));
  nd_meta_provider_add_provider (registry->meta_provider, ND_PROVIDER (sd_cc_provider));
  if (!nd_sd_cc_provider_browse (sd_cc_provider, &error))
    {
      g_warning ("NdNMDeviceRegistry: systemd-resolved provider failed to browse for Chromecast: %s",
                 error ? error->message : "unknown error");
      return;
    }

  g_debug ("NdNMDeviceRegistry: Using systemd-resolved mDNS browser for Chromecast");
}

static void
remove_sd_cc_provider (NdNMDeviceRegistry *registry, NMDevice *device)
{
  const char *device_type = NM_IS_DEVICE_WIFI (device) ? "Wi-Fi" : "Ethernet";

  for (gint i = 0; i < registry->providers->len; i++)
    {
      NdProvider *provider = g_ptr_array_index (registry->providers, i);
      if (!ND_IS_SD_CC_PROVIDER (provider))
        continue;

      NMDevice *provider_device = NULL;
      g_object_get (ND_SD_CC_PROVIDER (provider), "nm-device", &provider_device, NULL);
      if (provider_device != device)
        continue;

      g_debug ("NdNMDeviceRegistry: Removing %s systemd-resolved Chromecast provider: "
               "iface=%s, driver=%s, udi=%s",
               device_type,
               nm_device_get_iface (device),
               nm_device_get_driver (device),
               nm_device_get_udi (device));

      nd_meta_provider_remove_provider (registry->meta_provider, provider);
      g_ptr_array_remove_index (registry->providers, i);
      break;
    }
}

static void
mdns_device_state_changed_cb (NdNMDeviceRegistry *registry,
                               NMDeviceState       new_state,
                               NMDeviceState       old_state,
                               NMDeviceStateReason reason,
                               NMDevice           *device)
{
  const char *device_type = NM_IS_DEVICE_WIFI (device) ? "Wi-Fi" : "Ethernet";
  g_debug ("NdNMDeviceRegistry: %s device state changed: %s (%d -> %d, reason: %d)",
           device_type, nm_device_get_iface (device), old_state, new_state, reason);

  /* WiFi/Ethernet devices need to be connected (state > DISCONNECTED) for mDNS to work */
  if (new_state > NM_DEVICE_STATE_DISCONNECTED)
    {
      add_sd_wfd_mice_provider (registry, device);
      add_sd_cc_provider (registry, device);
    }
  else
    {
      remove_sd_wfd_mice_provider (registry, device);
      remove_sd_cc_provider (registry, device);
    }

  /* Update has_adapters since WiFi/Ethernet device state changed */
  nd_nm_device_registry_update_has_adapters (registry);
}
#endif

static void
device_added_cb (NdNMDeviceRegistry *registry, NMDevice *device, NMClient *client)
{
  if (NM_IS_DEVICE_WIFI_P2P (device))
    {
      NMDeviceState state = nm_device_get_state (device);

      g_debug ("NdNMDeviceRegistry: Found a new Wi-Fi P2P device: "
               "iface=%s, driver=%s, udi=%s, state=%d",
               nm_device_get_iface (device),
               nm_device_get_driver (device),
               nm_device_get_udi (device),
               state);

      /* Connect to state changes to add/remove provider based on availability */
      g_signal_connect_object (device,
                               "state-changed",
                               (GCallback) p2p_device_state_changed_cb,
                               registry,
                               G_CONNECT_SWAPPED);

      /* Only create provider if device is available */
      if (state > NM_DEVICE_STATE_UNAVAILABLE)
        add_p2p_provider (registry, device);
      else
        g_debug ("NdNMDeviceRegistry: Wi-Fi P2P device not yet available (state=%d), waiting...", state);
    }

#ifdef HAVE_SYSTEMD_RESOLVED
  if (NM_IS_DEVICE_WIFI (device) || NM_IS_DEVICE_ETHERNET (device))
    {
      NMDeviceState state = nm_device_get_state (device);
      const char *device_type = NM_IS_DEVICE_WIFI (device) ? "Wi-Fi" : "Ethernet";

      g_debug ("NdNMDeviceRegistry: Found a new %s device: "
               "iface=%s, driver=%s, udi=%s, state=%d",
               device_type,
               nm_device_get_iface (device),
               nm_device_get_driver (device),
               nm_device_get_udi (device),
               state);

      /* Connect to state changes to add/remove provider based on availability */
      g_signal_connect_object (device,
                               "state-changed",
                               (GCallback) mdns_device_state_changed_cb,
                               registry,
                               G_CONNECT_SWAPPED);

      /* Only create provider if device is available */
      if (state > NM_DEVICE_STATE_DISCONNECTED)
        {
          add_sd_wfd_mice_provider (registry, device);
          add_sd_cc_provider (registry, device);
        }
      else
        g_debug ("NdNMDeviceRegistry: %s device not yet available (state=%d), waiting...",
                 device_type, state);
    }
#endif
}

static void
device_removed_cb (NdNMDeviceRegistry *registry, NMDevice *device, NMClient *client)
{
  if (NM_IS_DEVICE_WIFI_P2P (device))
    {
      g_debug ("NdNMDeviceRegistry: Lost Wi-Fi P2P device: iface=%s, driver=%s, udi=%s",
               nm_device_get_iface (device),
               nm_device_get_driver (device),
               nm_device_get_udi (device));

      /* Disconnect state change handler */
      g_signal_handlers_disconnect_by_func (device, p2p_device_state_changed_cb, registry);

      remove_p2p_provider (registry, device);
    }

#ifdef HAVE_SYSTEMD_RESOLVED
  if (NM_IS_DEVICE_WIFI (device) || NM_IS_DEVICE_ETHERNET (device))
    {
      const char *device_type = NM_IS_DEVICE_WIFI (device) ? "Wi-Fi" : "Ethernet";
      g_debug ("NdNMDeviceRegistry: Lost %s device: iface=%s, driver=%s, udi=%s",
               device_type,
               nm_device_get_iface (device),
               nm_device_get_driver (device),
               nm_device_get_udi (device));

      /* Disconnect state change handler */
      g_signal_handlers_disconnect_by_func (device, mdns_device_state_changed_cb, registry);

      remove_sd_wfd_mice_provider (registry, device);
      remove_sd_cc_provider (registry, device);
    }
#endif
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
