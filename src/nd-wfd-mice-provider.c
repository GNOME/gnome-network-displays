/* nd-wfd-mice-provider.c
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

#include <avahi-gobject/ga-service-browser.h>
#include <avahi-gobject/ga-service-resolver.h>
#include <avahi-common/address.h>
#include <avahi-common/malloc.h>
#include "gnome-network-displays-config.h"
#include "nd-wfd-mice-provider.h"
#include "nd-sink.h"
#include "nd-wfd-mice-sink.h"

struct _NdWFDMiceProvider
{
  GObject         parent_instance;

  GPtrArray      *sinks;
  GaClient       *avahi_client;

  GSocketService *signalling_server;

  gboolean        discover;
};

enum {
  PROP_CLIENT = 1,

  PROP_DISCOVER,

  PROP_LAST = PROP_DISCOVER,
};

static void nd_wfd_mice_provider_provider_iface_init (NdProviderIface *iface);
static GList * nd_wfd_mice_provider_provider_get_sinks (NdProvider *provider);

G_DEFINE_TYPE_EXTENDED (NdWFDMiceProvider, nd_wfd_mice_provider, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (ND_TYPE_PROVIDER,
                                               nd_wfd_mice_provider_provider_iface_init);
                       )

static GParamSpec * props[PROP_LAST] = { NULL, };

static void
nd_wfd_mice_provider_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  NdWFDMiceProvider *provider = ND_WFD_MICE_PROVIDER (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      g_value_set_object (value, provider->avahi_client);
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
nd_wfd_mice_provider_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  NdWFDMiceProvider *provider = ND_WFD_MICE_PROVIDER (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      /* Construct only */
      provider->avahi_client = g_value_dup_object (value);
      break;

    case PROP_DISCOVER:
      provider->discover = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}


static void
nd_wfd_mice_provider_finalize (GObject *object)
{
  NdWFDMiceProvider *provider = ND_WFD_MICE_PROVIDER (object);

  g_clear_pointer (&provider->sinks, g_ptr_array_unref);
  g_clear_object (&provider->avahi_client);
  g_clear_object (&provider->signalling_server);

  G_OBJECT_CLASS (nd_wfd_mice_provider_parent_class)->finalize (object);
}

static void
nd_wfd_mice_provider_class_init (NdWFDMiceProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = nd_wfd_mice_provider_get_property;
  object_class->set_property = nd_wfd_mice_provider_set_property;
  object_class->finalize = nd_wfd_mice_provider_finalize;

  props[PROP_CLIENT] =
    g_param_spec_object ("client", "Client",
                         "The AvahiClient used to find sinks.",
                         GA_TYPE_CLIENT,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, props);

  g_object_class_override_property (object_class, PROP_DISCOVER, "discover");
}

static gboolean
compare_sinks (NdWFDMiceSink *a, NdWFDMiceSink *b)
{
  gchar *a_name = NULL;
  gchar *b_name = NULL;
  gchar *a_ip = NULL;
  gchar *b_ip = NULL;
  gint a_iface = 0;
  gint b_iface = 0;

  g_object_get (a, "name", &a_name, NULL);
  g_object_get (b, "name", &b_name, NULL);
  g_object_get (a, "ip", &a_ip, NULL);
  g_object_get (b, "ip", &b_ip, NULL);
  g_object_get (a, "interface", &a_iface, NULL);
  g_object_get (b, "interface", &b_iface, NULL);

  return g_str_equal (a_name, b_name) &&
         g_str_equal (a_ip, b_ip) &&
         (a_iface == b_iface);
}

static void
resolver_found_cb (GaServiceResolver  *resolver,
                   AvahiIfIndex        iface,
                   GaProtocol          proto,
                   gchar              *name,
                   gchar              *type,
                   gchar              *domain,
                   gchar              *hostname,
                   AvahiAddress       *addr,
                   gint                port,
                   AvahiStringList    *txt,
                   GaLookupResultFlags flags,
                   NdWFDMiceProvider  *provider)
{
  NdWFDMiceSink * sink = NULL;
  AvahiStringList *l;
  gchar ip[AVAHI_ADDRESS_STR_MAX];
  gchar *p2p_mac = NULL;
  gint interface = iface;

  if (avahi_address_snprint (ip, sizeof (ip), addr) == NULL)
    g_warning ("NdWFDMiceProvider: Failed to convert AvahiAddress to string");

  g_debug ("NdWFDMiceProvider: Found entry \"%s\" at %s:%d (%s) on interface %i", name, hostname, port, ip, iface);

  for (l = txt; l; l = l->next)
    {
      char *key, *value;

      if (avahi_string_list_get_pair (l, &key, &value, NULL) != 0)
        break;

      if (g_str_equal (key, "p2pMAC"))
        p2p_mac = g_strdup (value);

      avahi_free (key);
      avahi_free (value);
    }

  sink = nd_wfd_mice_sink_new (name, ip, p2p_mac, interface);
  if (g_ptr_array_find_with_equal_func (provider->sinks, sink,
                                        (GEqualFunc) compare_sinks, NULL))
    {
      g_debug ("NdWFDMiceProvider: Duplicate entry \"%s\" (%s) on interface %i",
               name,
               ip,
               interface);
      g_object_unref (sink);
      return;
    }
  g_object_unref (resolver);

  g_debug ("NdWFDMiceProvider: Creating sink \"%s\" (%s) on interface %i", name, ip, interface);
  g_ptr_array_add (provider->sinks, sink);
  g_signal_emit_by_name (provider, "sink-added", sink);
}

static void
resolver_failure_cb (GaServiceResolver *resolver,
                     GError            *error,
                     NdWFDMiceProvider *provider)
{
  g_warning ("NdWFDMiceProvider: Failed to resolve Avahi service: %s", error->message);
  g_object_unref (resolver);
}

static void
service_added_cb (GaServiceBrowser   *browser,
                  AvahiIfIndex        iface,
                  GaProtocol          proto,
                  gchar              *name,
                  gchar              *type,
                  gchar              *domain,
                  GaLookupResultFlags flags,
                  NdWFDMiceProvider  *provider)
{
  GaServiceResolver *resolver;
  GError *error = NULL;

  resolver = ga_service_resolver_new (iface,
                                      proto,
                                      name,
                                      type,
                                      domain,
                                      GA_PROTOCOL_INET,
                                      GA_LOOKUP_NO_FLAGS);

  g_signal_connect (resolver,
                    "found",
                    (GCallback) resolver_found_cb,
                    provider);

  g_signal_connect (resolver,
                    "failure",
                    (GCallback) resolver_failure_cb,
                    provider);

  if (!ga_service_resolver_attach (resolver,
                                   provider->avahi_client,
                                   &error))
    {
      g_warning ("NdWFDMiceProvider: Failed to attach Avahi resolver: %s", error->message);
      g_error_free (error);
    }
}

static void
service_removed_cb (GaServiceBrowser   *browser,
                    AvahiIfIndex        iface,
                    GaProtocol          proto,
                    gchar              *name,
                    gchar              *type,
                    gchar              *domain,
                    GaLookupResultFlags flags,
                    NdWFDMiceProvider  *provider)
{
  g_debug ("NdWFDMiceProvider: mDNS service \"%s\" removed from interface %i", name, iface);

  for (gint i = 0; i < provider->sinks->len; i++)
    {
      g_autoptr(NdWFDMiceSink) sink = g_object_ref (g_ptr_array_index (provider->sinks, i));

      gchar *remote_name = NULL;
      gint interface = 0;
      g_object_get (sink, "name", &remote_name, NULL);
      g_object_get (sink, "interface", &interface, NULL);
      if (g_str_equal (remote_name, name) && (iface == interface))
        {
          g_debug ("NdWFDMiceProvider: Removing sink \"%s\" from interface %i", remote_name, interface);
          g_ptr_array_remove_index (provider->sinks, i);
          g_signal_emit_by_name (provider, "sink-removed", sink);
          break;
        }
      else
        g_debug ("NdWFDMiceProvider: Keeping sink \"%s\" on interface %i", remote_name, interface);
    }
}

static void
signalling_incoming_cb (GSocketService     *service,
                        GSocketConnection * connection,
                        GObject           * source,
                        gpointer            user_data)
{
  /*
   * XXX: we should read the full, variable-length message,
   * find the respective sink,
   * and appropriately respond with sink's signalling client.
   */

  /* NdWFDMiceProvider * self = ND_WFD_MICE_PROVIDER (user_data); */

  gchar buffer[1024];
  GInputStream * istream;
  GError * error;

  istream = g_io_stream_get_input_stream (G_IO_STREAM (connection));

  g_input_stream_read (istream, buffer, sizeof (buffer), NULL, &error);
  if (error != NULL)
    g_warning ("NdWFDMiceProvider: Failed to connect to signalling host: %s", error->message);

  g_debug ("NdWFDMiceProvider: Received Message: %s", buffer);

  return;
}

static void
nd_wfd_mice_provider_init (NdWFDMiceProvider *provider)
{
  g_autoptr(GError) error = NULL;
  GSocketService * server;

  provider->discover = TRUE;
  provider->sinks = g_ptr_array_new_with_free_func (g_object_unref);
  server = g_socket_service_new ();

  g_socket_listener_add_inet_port ((GSocketListener *) server,
                                   7250,
                                   NULL,
                                   &error);
  if (error != NULL)
    {
      g_warning ("NdWFDMiceProvider: Error starting signal listener: %s", error->message);
      return;
    }

  g_signal_connect (server,
                    "incoming",
                    G_CALLBACK (signalling_incoming_cb),
                    provider);

  g_socket_service_start (server);

  provider->signalling_server = server;
}

/******************************************************************
* NdProvider interface implementation
******************************************************************/

static void
nd_wfd_mice_provider_provider_iface_init (NdProviderIface *iface)
{
  iface->get_sinks = nd_wfd_mice_provider_provider_get_sinks;
}

static GList *
nd_wfd_mice_provider_provider_get_sinks (NdProvider *provider)
{
  NdWFDMiceProvider *wfd_mice_provider = ND_WFD_MICE_PROVIDER (provider);
  GList *res = NULL;

  for (gint i = 0; i < wfd_mice_provider->sinks->len; i++)
    res = g_list_prepend (res, g_ptr_array_index (wfd_mice_provider->sinks, i));

  return res;
}

/******************************************************************
* NdWFDMiceProvider public functions
******************************************************************/

GaClient *
nd_wfd_mice_provider_get_client (NdWFDMiceProvider *provider)
{
  return provider->avahi_client;
}

GSocketService *
nd_wfd_mice_provider_get_signalling_server (NdWFDMiceProvider *provider)
{
  return provider->signalling_server;
}

NdWFDMiceProvider *
nd_wfd_mice_provider_new (GaClient *client)
{
  return g_object_new (ND_TYPE_WFD_MICE_PROVIDER,
                       "client", client,
                       NULL);
}

gboolean
nd_wfd_mice_provider_browse (NdWFDMiceProvider *provider, GError * error)
{
  GaServiceBrowser * avahi_browser;

  avahi_browser = ga_service_browser_new ("_display._tcp");

  if (provider->avahi_client == NULL)
    {
      g_warning ("NdWFDMiceProvider: No Avahi client found");
      return FALSE;
    }

  g_signal_connect (avahi_browser,
                    "new-service",
                    (GCallback) service_added_cb,
                    provider);

  g_signal_connect (avahi_browser,
                    "removed-service",
                    (GCallback) service_removed_cb,
                    provider);

  if (!ga_service_browser_attach (avahi_browser,
                                  provider->avahi_client,
                                  &error))
    {
      g_warning ("NdWFDMiceProvider: Failed to attach Avahi Service Browser: %s", error->message);
      return FALSE;
    }

  return TRUE;
}
