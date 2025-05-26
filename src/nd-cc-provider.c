/* nd-cc-provider.c
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

#include <avahi-gobject/ga-service-browser.h>
#include <avahi-gobject/ga-service-resolver.h>
#include <avahi-common/address.h>
#include "gnome-network-displays-config.h"
#include "nd-cc-provider.h"
#include "nd-sink.h"
#include "nd-cc-sink.h"

struct _NdCCProvider
{
  GObject        parent_instance;

  GPtrArray     *sinks;
  GaClient      *avahi_client;

  GSocketClient *signalling_client;

  gboolean       discover;
};

enum {
  PROP_CLIENT = 1,

  PROP_DISCOVER,

  PROP_LAST = PROP_DISCOVER,
};

static void nd_cc_provider_provider_iface_init (NdProviderIface *iface);
static GList *nd_cc_provider_provider_get_sinks (NdProvider *provider);

G_DEFINE_TYPE_EXTENDED (NdCCProvider, nd_cc_provider, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (ND_TYPE_PROVIDER,
                                               nd_cc_provider_provider_iface_init);
                       )

static GParamSpec *props[PROP_LAST] = {
  NULL,
};

static void
nd_cc_provider_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  NdCCProvider *provider = ND_CC_PROVIDER (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      g_assert (provider->avahi_client == NULL);
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
nd_cc_provider_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  NdCCProvider *provider = ND_CC_PROVIDER (object);

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
nd_cc_provider_finalize (GObject *object)
{
  NdCCProvider *provider = ND_CC_PROVIDER (object);

  g_clear_pointer (&provider->sinks, g_ptr_array_unref);

  if (provider->signalling_client)
    g_object_unref (provider->signalling_client);

  G_OBJECT_CLASS (nd_cc_provider_parent_class)->finalize (object);
}

static void
nd_cc_provider_class_init (NdCCProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = nd_cc_provider_get_property;
  object_class->set_property = nd_cc_provider_set_property;
  object_class->finalize = nd_cc_provider_finalize;

  props[PROP_CLIENT] =
    g_param_spec_object ("client", "Client",
                         "The AvahiClient used to find sinks.",
                         GA_TYPE_CLIENT,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, props);

  g_object_class_override_property (object_class, PROP_DISCOVER, "discover");
}

static gboolean
compare_sinks (NdCCSink *a, NdCCSink *b)
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
                   NdCCProvider       *provider)
{
  NdCCSink *sink = NULL;
  gchar ip[AVAHI_ADDRESS_STR_MAX];
  AvahiStringList *cc_name, *cc_model;
  gchar *display_name = NULL;
  gint interface = iface;

  if (avahi_address_snprint (ip, sizeof (ip), addr) == NULL)
    g_warning ("NdCCProvider: Failed to convert AvahiAddress to string");

  g_debug ("NdCCProvider: Found entry \"%s\" at %s:%d (%s) on interface %i", name, hostname, port, ip, iface);

  /* chromecast has pretty name and model in txt */
  cc_name = avahi_string_list_find (txt, "fn");
  cc_model = avahi_string_list_find (txt, "md");
  if (cc_name && cc_model)
    display_name = g_strdup_printf ("%s - %s", cc_name->text + 3, cc_model->text + 3);
  else if (cc_name)
    display_name = g_strdup_printf ("%s", cc_name->text + 3);
  else
    display_name = name;

  sink = nd_cc_sink_new (provider->signalling_client, name, ip, display_name, interface);
  if (g_ptr_array_find_with_equal_func (provider->sinks, sink,
                                        (GEqualFunc) compare_sinks, NULL))
    {
      g_debug ("NdCCProvider: Duplicate entry \"%s\" (%s) on interface %i",
               display_name,
               ip,
               interface);
      g_object_unref (sink);
      return;
    }
  g_object_unref (resolver);

  g_debug ("NdCCProvider: Creating sink \"%s\" (%s on IP %s) on interface %i", display_name, name, ip, interface);
  g_ptr_array_add (provider->sinks, sink);
  g_signal_emit_by_name (provider, "sink-added", sink);
}

static void
resolver_failure_cb (GaServiceResolver *resolver,
                     GError            *error,
                     NdCCProvider      *provider)
{
  g_warning ("NdCCProvider: Failed to resolve Avahi service: %s", error->message);
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
                  NdCCProvider       *provider)
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
      g_warning ("NdCCProvider: Failed to attach Avahi resolver: %s", error->message);
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
                    NdCCProvider       *provider)
{
  g_debug ("NdCCProvider: mDNS service \"%s\" removed from interface %i", name, iface);

  for (gint i = 0; i < provider->sinks->len; i++)
    {
      g_autoptr(NdCCSink) sink = g_object_ref (g_ptr_array_index (provider->sinks, i));

      gchar *remote_name = NULL;
      gint interface = 0;
      g_object_get (sink, "name", &remote_name, NULL);
      g_object_get (sink, "interface", &interface, NULL);
      if (g_str_equal (remote_name, name) && (iface == interface))
        {
          g_debug ("NdCCProvider: Removing sink \"%s\" from interface %i", remote_name, interface);
          g_ptr_array_remove_index (provider->sinks, i);
          g_signal_emit_by_name (provider, "sink-removed", sink);
          break;
        }
      else
        g_debug ("NdCCProvider: Keeping sink \"%s\" on interface %i", remote_name, interface);
    }
}

static void
nd_cc_provider_init (NdCCProvider *provider)
{
  provider->discover = TRUE;
  provider->sinks = g_ptr_array_new_with_free_func (g_object_unref);
  provider->signalling_client = g_socket_client_new ();
}

/******************************************************************
* NdProvider interface implementation
******************************************************************/

static void
nd_cc_provider_provider_iface_init (NdProviderIface *iface)
{
  iface->get_sinks = nd_cc_provider_provider_get_sinks;
}

static GList *
nd_cc_provider_provider_get_sinks (NdProvider *provider)
{
  NdCCProvider *cc_provider = ND_CC_PROVIDER (provider);
  GList *res = NULL;

  for (gint i = 0; i < cc_provider->sinks->len; i++)
    res = g_list_prepend (res, g_ptr_array_index (cc_provider->sinks, i));

  return res;
}

/******************************************************************
* NdCCProvider public functions
******************************************************************/

GaClient *
nd_cc_provider_get_client (NdCCProvider *provider)
{
  return provider->avahi_client;
}

GSocketClient *
nd_cc_provider_get_signalling_client (NdCCProvider *provider)
{
  return provider->signalling_client;
}

NdCCProvider *
nd_cc_provider_new (GaClient *client)
{
  return g_object_new (ND_TYPE_CC_PROVIDER,
                       "client", client,
                       NULL);
}

gboolean
nd_cc_provider_browse (NdCCProvider *provider, GError *error)
{
  GaServiceBrowser *avahi_browser;

  avahi_browser = ga_service_browser_new ("_googlecast._tcp");

  if (provider->avahi_client == NULL)
    {
      g_warning ("NdCCProvider: No Avahi client found");
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
      g_warning ("NdCCProvider: Failed to attach Avahi Service Browser: %s", error->message);
      return FALSE;
    }

  return TRUE;
}
