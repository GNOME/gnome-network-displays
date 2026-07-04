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

#define WPA_SUPPLICANT_DBUS_NAME "fi.w1.wpa_supplicant1"
#define WPA_SUPPLICANT_DBUS_PATH "/fi/w1/wpa_supplicant1"
#define WPA_SUPPLICANT_DBUS_INTERFACE "fi.w1.wpa_supplicant1"
#define WPA_SUPPLICANT_INTERFACE "fi.w1.wpa_supplicant1.Interface"
#define WPA_SUPPLICANT_WPS_INTERFACE "fi.w1.wpa_supplicant1.Interface.WPS"
#define WPA_SUPPLICANT_P2P_DEVICE_INTERFACE "fi.w1.wpa_supplicant1.Interface.P2PDevice"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define P2P_DEVICE_IFACE_PREFIX "p2p-dev-"

static const guint8 wfd_discovery_ies[] = {
  0x00, 0x00, 0x06, 0x01, 0x10, 0x1c, 0x44, 0x00, 0x32
};

static const guint8 wps_primary_device_type[] = {
  0x00, 0x0a, 0x00, 0x50, 0xf2, 0x04, 0x00, 0x05
};

static const guint8 wfd_sink_device_type[] = {
  0x00, 0x07, 0x00, 0x50, 0xf2, 0x04, 0x00, 0x01
};

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

static gboolean
set_supplicant_property (GDBusConnection  *connection,
                         const gchar      *object_path,
                         const gchar      *interface_name,
                         const gchar      *property_name,
                         GVariant         *value,
                         GError          **error)
{
  g_autoptr(GVariant) ret = NULL;

  ret = g_dbus_connection_call_sync (connection,
                                     WPA_SUPPLICANT_DBUS_NAME,
                                     object_path,
                                     DBUS_PROPERTIES_INTERFACE,
                                     "Set",
                                     g_variant_new ("(ssv)",
                                                    interface_name,
                                                    property_name,
                                                    value),
                                     NULL,
                                     G_DBUS_CALL_FLAGS_NONE,
                                     -1,
                                     NULL,
                                     error);

  return ret != NULL;
}

static gchar *
get_supplicant_iface_name (NMDevice *nm_device)
{
  const gchar *ifname = nm_device_get_iface (nm_device);

  if (!ifname)
    return NULL;

  if (g_str_has_prefix (ifname, P2P_DEVICE_IFACE_PREFIX))
    return g_strdup (ifname + strlen (P2P_DEVICE_IFACE_PREFIX));

  return g_strdup (ifname);
}

static gchar *
get_supplicant_interface_path (GDBusConnection  *connection,
                               NMDevice         *nm_device,
                               GError          **error)
{
  g_autoptr(GVariant) ret = NULL;
  g_autofree gchar *supplicant_iface = NULL;
  gchar *supplicant_path = NULL;

  supplicant_iface = get_supplicant_iface_name (nm_device);
  if (!supplicant_iface)
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_FAILED,
                           "Could not determine the supplicant interface name");
      return NULL;
    }

  ret = g_dbus_connection_call_sync (connection,
                                     WPA_SUPPLICANT_DBUS_NAME,
                                     WPA_SUPPLICANT_DBUS_PATH,
                                     WPA_SUPPLICANT_DBUS_INTERFACE,
                                     "GetInterface",
                                     g_variant_new ("(s)", supplicant_iface),
                                     G_VARIANT_TYPE ("(o)"),
                                     G_DBUS_CALL_FLAGS_NONE,
                                     -1,
                                     NULL,
                                     error);
  if (!ret)
    return NULL;

  g_variant_get (ret, "(o)", &supplicant_path);

  return supplicant_path;
}

static void
warn_and_clear (const gchar *message, GError **error)
{
  if (error && *error)
    {
      g_warning ("%s: %s", message, (*error)->message);
      g_clear_error (error);
    }
  else
    {
      g_warning ("%s", message);
    }
}

static void
configure_supplicant_wfd_discovery (NdWFDP2PProvider *provider)
{
  g_autoptr(GDBusConnection) connection = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *supplicant_path = NULL;
  GVariantBuilder p2p_config;

  connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
  if (!connection)
    {
      warn_and_clear ("WFDP2PProvider: Could not connect to the system bus", &error);
      return;
    }

  if (!set_supplicant_property (connection,
                                WPA_SUPPLICANT_DBUS_PATH,
                                WPA_SUPPLICANT_DBUS_INTERFACE,
                                "WFDIEs",
                                g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                           wfd_discovery_ies,
                                                           G_N_ELEMENTS (wfd_discovery_ies),
                                                           sizeof (guint8)),
                                &error))
    warn_and_clear ("WFDP2PProvider: Could not set wpa_supplicant WFDIEs", &error);

  supplicant_path = get_supplicant_interface_path (connection, provider->nm_device, &error);
  if (!supplicant_path)
    {
      warn_and_clear ("WFDP2PProvider: Could not resolve the supplicant interface", &error);
      return;
    }

  if (!set_supplicant_property (connection,
                                supplicant_path,
                                WPA_SUPPLICANT_WPS_INTERFACE,
                                "DeviceName",
                                g_variant_new_string ("GNOME-Network-Displays"),
                                &error))
    warn_and_clear ("WFDP2PProvider: Could not set WPS device name", &error);

  if (!set_supplicant_property (connection,
                                supplicant_path,
                                WPA_SUPPLICANT_WPS_INTERFACE,
                                "Manufacturer",
                                g_variant_new_string ("GNOME"),
                                &error))
    warn_and_clear ("WFDP2PProvider: Could not set WPS manufacturer", &error);

  if (!set_supplicant_property (connection,
                                supplicant_path,
                                WPA_SUPPLICANT_WPS_INTERFACE,
                                "ModelName",
                                g_variant_new_string ("Network-Displays"),
                                &error))
    warn_and_clear ("WFDP2PProvider: Could not set WPS model name", &error);

  if (!set_supplicant_property (connection,
                                supplicant_path,
                                WPA_SUPPLICANT_WPS_INTERFACE,
                                "ConfigMethods",
                                g_variant_new_string ("virtual_push_button physical_display keypad ext_nfc_token nfc_interface"),
                                &error))
    warn_and_clear ("WFDP2PProvider: Could not set WPS config methods", &error);

  if (!set_supplicant_property (connection,
                                supplicant_path,
                                WPA_SUPPLICANT_WPS_INTERFACE,
                                "DeviceType",
                                g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                           wps_primary_device_type,
                                                           G_N_ELEMENTS (wps_primary_device_type),
                                                           sizeof (guint8)),
                                &error))
    warn_and_clear ("WFDP2PProvider: Could not set WPS device type", &error);

  g_variant_builder_init (&p2p_config, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&p2p_config,
                         "{sv}",
                         "DeviceName",
                         g_variant_new_string ("GNOME-Network-Displays"));
  g_variant_builder_add (&p2p_config,
                         "{sv}",
                         "PrimaryDeviceType",
                         g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                    wps_primary_device_type,
                                                    G_N_ELEMENTS (wps_primary_device_type),
                                                    sizeof (guint8)));
  g_variant_builder_add (&p2p_config, "{sv}", "GOIntent", g_variant_new_uint32 (7));
  g_variant_builder_add (&p2p_config, "{sv}", "PersistentReconnect", g_variant_new_boolean (FALSE));

  if (!set_supplicant_property (connection,
                                supplicant_path,
                                WPA_SUPPLICANT_P2P_DEVICE_INTERFACE,
                                "P2PDeviceConfig",
                                g_variant_builder_end (&p2p_config),
                                &error))
    warn_and_clear ("WFDP2PProvider: Could not set P2P device config", &error);
}

static gboolean
start_supplicant_wfd_find (NdWFDP2PProvider *provider,
                           guint             timeout)
{
  g_autoptr(GDBusConnection) connection = NULL;
  g_autoptr(GVariant) ret = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *supplicant_path = NULL;
  GVariantBuilder options;
  GVariantBuilder requested_device_types;

  connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
  if (!connection)
    {
      warn_and_clear ("WFDP2PProvider: Could not connect to the system bus", &error);
      return FALSE;
    }

  supplicant_path = get_supplicant_interface_path (connection, provider->nm_device, &error);
  if (!supplicant_path)
    {
      warn_and_clear ("WFDP2PProvider: Could not resolve the supplicant interface", &error);
      return FALSE;
    }

  g_variant_builder_init (&requested_device_types, G_VARIANT_TYPE ("aay"));
  g_variant_builder_add (&requested_device_types,
                         "@ay",
                         g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                    wfd_sink_device_type,
                                                    G_N_ELEMENTS (wfd_sink_device_type),
                                                    sizeof (guint8)));

  g_variant_builder_init (&options, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&options, "{sv}", "Timeout", g_variant_new_int32 (timeout));
  g_variant_builder_add (&options, "{sv}", "DiscoveryType", g_variant_new_string ("social"));
  g_variant_builder_add (&options,
                         "{sv}",
                         "RequestedDeviceTypes",
                         g_variant_builder_end (&requested_device_types));

  ret = g_dbus_connection_call_sync (connection,
                                     WPA_SUPPLICANT_DBUS_NAME,
                                     supplicant_path,
                                     WPA_SUPPLICANT_P2P_DEVICE_INTERFACE,
                                     "Find",
                                     g_variant_new ("(a{sv})", &options),
                                     G_VARIANT_TYPE ("()"),
                                     G_DBUS_CALL_FLAGS_NONE,
                                     -1,
                                     NULL,
                                     &error);
  if (!ret)
    {
      warn_and_clear ("WFDP2PProvider: Could not start targeted supplicant P2P discovery", &error);
      return FALSE;
    }

  g_debug ("WFDP2PProvider: Started targeted supplicant P2P discovery");

  return TRUE;
}

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

static GVariant *
build_p2p_find_options (void)
{
  GVariantBuilder options;

  g_variant_builder_init (&options, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&options, "{sv}", "timeout", g_variant_new_int32 (30));

  return g_variant_ref_sink (g_variant_builder_end (&options));
}

static void
start_p2p_find (NdWFDP2PProvider *provider)
{
  configure_supplicant_wfd_discovery (provider);

  if (start_supplicant_wfd_find (provider, 30))
    return;

  g_autoptr(GVariant) options = build_p2p_find_options ();

  nm_device_wifi_p2p_start_find (NM_DEVICE_WIFI_P2P (provider->nm_device),
                                 options,
                                 NULL,
                                 log_start_find_error,
                                 NULL);
}

static gboolean
device_restart_find_timeout (gpointer user_data)
{
  NdWFDP2PProvider *provider = ND_WFD_P2P_PROVIDER (user_data);

  g_debug ("WFDP2PProvider: Restarting P2P discovery");
  start_p2p_find (provider);

  return G_SOURCE_CONTINUE;
}

static void
discovery_start_stop (NdWFDP2PProvider *provider, NMDeviceState state)
{
  if (provider->discover && state > NM_DEVICE_STATE_UNAVAILABLE)
    {
      g_debug ("WFDP2PProvider: Starting P2P discovery.");
      start_p2p_find (provider);
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
