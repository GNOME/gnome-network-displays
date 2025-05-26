/* nd-wfd-p2p-provider.c
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
#include "nd-wfd-p2p-provider.h"
#include "nd-wfd-p2p-sink.h"

struct _NdWFDP2PProvider
{
  GObject    parent_instance;

  GPtrArray *sinks;

  NMClient  *nm_client;
  NMDevice  *nm_device;

  gboolean   discover;
  guint      p2p_find_source_id;
};

enum {
  PROP_CLIENT = 1,
  PROP_DEVICE,

  PROP_DISCOVER,

  PROP_LAST = PROP_DISCOVER,
};

static void nd_wfd_p2p_provider_provider_iface_init (NdProviderIface *iface);
static GList * nd_wfd_p2p_provider_provider_get_sinks (NdProvider *provider);

static void peer_added_cb (NdWFDP2PProvider *provider,
                           NMWifiP2PPeer    *peer,
                           NMDevice         *device);

G_DEFINE_TYPE_EXTENDED (NdWFDP2PProvider, nd_wfd_p2p_provider, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (ND_TYPE_PROVIDER,
                                               nd_wfd_p2p_provider_provider_iface_init);
                       )

static GParamSpec * props[PROP_LAST] = { NULL, };

static void
on_peer_wfd_ie_notify_cb (NdWFDP2PProvider *provider,
                          GParamSpec       *pspec,
                          NMWifiP2PPeer    *peer)
{
  g_debug ("WFDP2PProvider: WFDIEs for ignored peer \"%s\" (%s) changed, trying to re-add",
           nm_wifi_p2p_peer_get_name (peer),
           nm_wifi_p2p_peer_get_hw_address (peer));

  g_signal_handlers_disconnect_by_func (peer, on_peer_wfd_ie_notify_cb, provider);

  peer_added_cb (provider, peer, provider->nm_device);
}

static gboolean
compare_sinks (NdWFDP2PSink *a, NdWFDP2PSink *b)
{
  return nd_wfd_p2p_sink_get_peer (a) == nd_wfd_p2p_sink_get_peer (b);
}

static void
peer_added_cb (NdWFDP2PProvider *provider, NMWifiP2PPeer *peer, NMDevice *device)
{
  NdWFDP2PSink *sink = NULL;
  GBytes *wfd_ies;

  wfd_ies = nm_wifi_p2p_peer_get_wfd_ies (peer);

  /* Assume this is not a WFD Peer if there are no WFDIEs set. */
  if (!wfd_ies || g_bytes_get_size (wfd_ies) == 0)
    {
      g_debug ("WFDP2PProvider: Ignoring peer \"%s\" (%s) for now as it has no WFDIEs set",
               nm_wifi_p2p_peer_get_name (peer),
               nm_wifi_p2p_peer_get_hw_address (peer));

      g_signal_connect_object (peer, "notify::" NM_WIFI_P2P_PEER_WFD_IES,
                               G_CALLBACK (on_peer_wfd_ie_notify_cb),
                               provider,
                               G_CONNECT_SWAPPED);
      return;
    }
  sink = nd_wfd_p2p_sink_new (provider->nm_client, provider->nm_device, peer);
  if (g_ptr_array_find_with_equal_func (provider->sinks, sink,
                                        (GEqualFunc) compare_sinks, NULL))
    {
      g_debug ("WFDP2PProvider: Repeat peer \"%s\" (%s)",
               nm_wifi_p2p_peer_get_name (peer),
               nm_wifi_p2p_peer_get_hw_address (peer));
      g_object_unref (sink);
      return;
    }
  g_debug ("WFDP2PProvider: Found a new sink with peer \"%s\" (%s) on device %p",
           nm_wifi_p2p_peer_get_name (peer),
           nm_wifi_p2p_peer_get_hw_address (peer),
           device);
  g_ptr_array_add (provider->sinks, sink);
  g_signal_emit_by_name (provider, "sink-added", sink);
}

static void
peer_removed_cb (NdWFDP2PProvider *provider, NMWifiP2PPeer *peer, NMDevice *device)
{
  g_debug ("WFDP2PProvider: Peer removed \"%s\" (%s)",
           nm_wifi_p2p_peer_get_hw_address (peer),
           nm_wifi_p2p_peer_get_name (peer));

  /* Otherwise we may see properties changing to NULL before the object is destroyed. */
  g_signal_handlers_disconnect_by_func (peer, on_peer_wfd_ie_notify_cb, provider);

  for (gint i = 0; i < provider->sinks->len; i++)
    {
      g_autoptr(NdWFDP2PSink) sink = g_object_ref (g_ptr_array_index (provider->sinks, i));

      if (nd_wfd_p2p_provider_get_device (provider) != device)
        continue;

      if (nd_wfd_p2p_sink_get_peer (sink) == peer)
        {
          g_debug ("WFDP2PProvider: Removing sink \"%s\" (%s)",
                   nm_wifi_p2p_peer_get_hw_address (nd_wfd_p2p_sink_get_peer (sink)),
                   nm_wifi_p2p_peer_get_name (nd_wfd_p2p_sink_get_peer (sink)));
          g_ptr_array_remove_index (provider->sinks, i);
          g_signal_emit_by_name (provider, "sink-removed", sink);
          break;
        }
    }
}

static void
nd_wfd_p2p_provider_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  NdWFDP2PProvider *provider = ND_WFD_P2P_PROVIDER (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      g_assert (provider->nm_client == NULL);
      g_value_set_object (value, provider->nm_client);
      break;

    case PROP_DEVICE:
      g_assert (provider->nm_device == NULL);
      g_value_set_object (value, provider->nm_device);
      break;

    case PROP_DISCOVER:
      g_value_set_boolean (value, provider->discover);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
log_start_find_error (GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr(GError) error = NULL;
  NMDeviceWifiP2P *p2p_dev = NM_DEVICE_WIFI_P2P (source);

  if (!nm_device_wifi_p2p_start_find_finish (p2p_dev, res, &error))
    g_warning ("WFDP2PProvider: Could not start P2P find: %s", error->message);
  else
    g_debug ("WFDP2PProvider: Started P2P discovery");
}

static gboolean
device_restart_find_timeout (gpointer user_data)
{
  NdWFDP2PProvider *provider = ND_WFD_P2P_PROVIDER (user_data);

  g_debug ("WFDP2PProvider: Restarting P2P discovery");
  nm_device_wifi_p2p_start_find (NM_DEVICE_WIFI_P2P (provider->nm_device), NULL, NULL, log_start_find_error, NULL);

  return G_SOURCE_CONTINUE;
}

static void
discovery_start_stop (NdWFDP2PProvider *provider, NMDeviceState state)
{
  if (provider->discover && state > NM_DEVICE_STATE_UNAVAILABLE)
    {
      g_debug ("WFDP2PProvider: Starting P2P discovery.");
      nm_device_wifi_p2p_start_find (NM_DEVICE_WIFI_P2P (provider->nm_device), NULL, NULL, log_start_find_error, NULL);
      if (!provider->p2p_find_source_id)
        provider->p2p_find_source_id = g_timeout_add_seconds (20, device_restart_find_timeout, provider);
    }
  else
    {
      if (provider->p2p_find_source_id)
        {
          g_debug ("WFDP2PProvider: Stopping P2P discovery.");
          g_source_remove (provider->p2p_find_source_id);
          provider->p2p_find_source_id = 0;

          /* FIXME (upstream): Calling stop find for every state changes causes
           * the connection to fail. While this was never a good idea,
           * it does mean that something is going wrong elsewhere (likely
           * in wpa_supplicant).
           */
          nm_device_wifi_p2p_stop_find (NM_DEVICE_WIFI_P2P (provider->nm_device), NULL, NULL, NULL);
        }
    }
}

static void
device_state_changed_cb (NdWFDP2PProvider   *provider,
                         NMDeviceState       new_state,
                         NMDeviceState       old_state,
                         NMDeviceStateReason reason,
                         NMDevice           *device)
{
  g_debug ("WFDP2PProvider: Device state changed. It is now %i. Reason: %i", new_state, reason);

  discovery_start_stop (provider, new_state);
}

static void
nd_wfd_p2p_provider_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  NdWFDP2PProvider *provider = ND_WFD_P2P_PROVIDER (object);
  const GPtrArray *peers;
  NMDeviceState state = NM_DEVICE_STATE_UNKNOWN;

  if (provider->nm_device)
    state = nm_device_get_state (provider->nm_device);

  switch (prop_id)
    {
    case PROP_CLIENT:
      /* Construct only */
      provider->nm_client = g_value_dup_object (value);
      break;

    case PROP_DEVICE:
      /* Construct only */
      provider->nm_device = g_value_dup_object (value);

      g_signal_connect_object (provider->nm_device,
                               "peer-added",
                               (GCallback) peer_added_cb,
                               provider,
                               G_CONNECT_SWAPPED);

      g_signal_connect_object (provider->nm_device,
                               "peer-removed",
                               (GCallback) peer_removed_cb,
                               provider,
                               G_CONNECT_SWAPPED);

      g_signal_connect_object (provider->nm_device,
                               "state-changed",
                               (GCallback) device_state_changed_cb,
                               provider,
                               G_CONNECT_SWAPPED);

      discovery_start_stop (provider, state);

      peers = nm_device_wifi_p2p_get_peers (NM_DEVICE_WIFI_P2P (provider->nm_device));
      for (gint i = 0; i < peers->len; i++)
        peer_added_cb (provider, g_ptr_array_index (peers, i), provider->nm_device);

      break;

    case PROP_DISCOVER:
      provider->discover = g_value_get_boolean (value);
      g_debug ("WFDP2PProvider: Discover is now set to %d", provider->discover);

      discovery_start_stop (provider, state);

      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_wfd_p2p_provider_finalize (GObject *object)
{
  NdWFDP2PProvider *provider = ND_WFD_P2P_PROVIDER (object);

  if (provider->p2p_find_source_id)
    g_source_remove (provider->p2p_find_source_id);
  provider->p2p_find_source_id = 0;

  g_clear_pointer (&provider->sinks, g_ptr_array_unref);
  g_clear_object (&provider->nm_client);
  g_clear_object (&provider->nm_device);

  G_OBJECT_CLASS (nd_wfd_p2p_provider_parent_class)->finalize (object);
}

static void
nd_wfd_p2p_provider_class_init (NdWFDP2PProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = nd_wfd_p2p_provider_get_property;
  object_class->set_property = nd_wfd_p2p_provider_set_property;
  object_class->finalize = nd_wfd_p2p_provider_finalize;

  props[PROP_CLIENT] =
    g_param_spec_object ("client", "Client",
                         "The NMClient used to find the sink.",
                         NM_TYPE_CLIENT,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  props[PROP_DEVICE] =
    g_param_spec_object ("device", "Device",
                         "The NMDevice the sink was found on.",
                         NM_TYPE_DEVICE,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, props);

  g_object_class_override_property (object_class, PROP_DISCOVER, "discover");
}

static void
nd_wfd_p2p_provider_init (NdWFDP2PProvider *provider)
{
  provider->sinks = g_ptr_array_new_with_free_func (g_object_unref);
}

/******************************************************************
* NdProvider interface implementation
******************************************************************/

static void
nd_wfd_p2p_provider_provider_iface_init (NdProviderIface *iface)
{
  iface->get_sinks = nd_wfd_p2p_provider_provider_get_sinks;
}

static GList *
nd_wfd_p2p_provider_provider_get_sinks (NdProvider *provider)
{
  NdWFDP2PProvider *wfd_p2p_provider = ND_WFD_P2P_PROVIDER (provider);
  GList *res = NULL;

  for (gint i = 0; i < wfd_p2p_provider->sinks->len; i++)
    res = g_list_prepend (res, g_ptr_array_index (wfd_p2p_provider->sinks, i));

  return res;
}

/******************************************************************
* NdWFDP2PProvider public functions
******************************************************************/

/**
 * nd_wfd_p2p_provider_get_client
 * @provider: a #NdWFDP2PProvider
 *
 * Retrieve the #NMClient used to find the device.
 *
 * Returns: (transfer none): The #NMClient
 */
NMClient *
nd_wfd_p2p_provider_get_client (NdWFDP2PProvider *provider)
{
  return provider->nm_client;
}

/**
 * nd_wfd_p2p_provider_get_device
 * @provider: a #NdWFDP2PProvider
 *
 * Retrieve the #NMDevice the provider is for.
 *
 * Returns: (transfer none): The #NMDevice
 */
NMDevice *
nd_wfd_p2p_provider_get_device (NdWFDP2PProvider *provider)
{
  return provider->nm_device;
}


NdWFDP2PProvider *
nd_wfd_p2p_provider_new (NMClient *client, NMDevice *device)
{
  return g_object_new (ND_TYPE_WFD_P2P_PROVIDER,
                       "client", client,
                       "device", device,
                       NULL);
}
