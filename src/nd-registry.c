/* nd-registry.c
 *
 * Copyright 2023 Pedro Sader Azevedo <pedro.saderazevedo@proton.me>
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

#include <glib-object.h>
#include "nd-provider.h"
#include "nd-registry.h"
#include "nd-sink.h"
#include "nd-dbus-registry.h"

struct _NdRegistry
{
  GObject         parent_instance;

  NdProvider     *provider;

  NdDBusRegistry *dbus_interface;
  guint           dbus_name_id;
};

enum {
  PROP_PROVIDER = 1,
  PROP_LAST,
};

G_DEFINE_TYPE (NdRegistry, nd_registry, G_TYPE_OBJECT)

static GParamSpec * props[PROP_LAST] = { NULL, };

void
nd_registry_sink_to_variant (NdSink *sink, GVariantBuilder *sinks_builder)
{
  GVariantBuilder *sink_builder;
  g_autofree gchar *uuid = NULL;
  g_autofree gchar *display_name = NULL;
  gint priority;
  gint state;
  gint protocol;
  GVariant *value;

  g_assert (ND_IS_SINK (sink));

  sink_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

  g_object_get (sink, "uuid", &uuid, NULL);
  g_variant_builder_add (sink_builder, "{sv}",
                         "uuid", g_variant_new ("s", uuid));

  g_object_get (sink, "display-name", &display_name, NULL);
  g_variant_builder_add (sink_builder, "{sv}",
                         "display-name", g_variant_new ("s", display_name));

  g_object_get (sink, "priority", &priority, NULL);
  g_variant_builder_add (sink_builder, "{sv}",
                         "priority", g_variant_new ("u", priority));

  g_object_get (sink, "state", &state, NULL);
  g_variant_builder_add (sink_builder, "{sv}",
                         "state", g_variant_new ("u", state));

  g_object_get (sink, "protocol", &protocol, NULL);
  g_variant_builder_add (sink_builder, "{sv}",
                         "protocol", g_variant_new ("u", protocol));

  value = g_variant_new ("a{sv}", sink_builder);

  g_variant_builder_add_value (sinks_builder, value);
}

void
nd_registry_update_exposed_sinks (NdRegistry *registry)
{
  GVariantBuilder *sinks_builder;
  GVariant *value;

  g_autoptr(GList) sinks = NULL;
  sinks = nd_provider_get_sinks (registry->provider);

  sinks_builder = g_variant_builder_new (G_VARIANT_TYPE ("aa{sv}"));

  g_list_foreach (sinks, (GFunc) nd_registry_sink_to_variant, sinks_builder);

  value = g_variant_new ("aa{sv}", sinks_builder);
  nd_dbus_registry_set_sinks (registry->dbus_interface, value);
}

static void
sink_added_cb (NdRegistry *registry,
               NdSink     *sink,
               NdProvider *provider)
{
  nd_registry_update_exposed_sinks (registry);
  g_debug ("NdRegistry: Adding a sink");
}

static void
sink_removed_cb (NdRegistry *registry,
                 NdSink     *sink,
                 NdProvider *provider)
{
  nd_registry_update_exposed_sinks (registry);
  g_debug ("NdRegistry: Removing a sink");
}

static void
nd_registry_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  NdRegistry *registry = ND_REGISTRY (object);

  switch (prop_id)
    {
    case PROP_PROVIDER:
      g_value_set_object (value, registry->provider);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_registry_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  NdRegistry *registry = ND_REGISTRY (object);

  switch (prop_id)
    {
    case PROP_PROVIDER:
      nd_registry_set_provider (registry, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_registry_finalize (GObject *object)
{
  NdRegistry *registry = ND_REGISTRY (object);

  nd_registry_set_provider (registry, NULL);

  G_OBJECT_CLASS (nd_registry_parent_class)->finalize (object);
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const char      *name,
                 gpointer         user_data)
{
  NdRegistry *registry = user_data;

  g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (registry->dbus_interface),
                                    connection,
                                    "/org/gnome/NetworkDisplays/Registry",
                                    NULL);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const char      *name,
                  gpointer         user_data)
{
  g_info ("Acquired name %s", name);
}

static void
on_name_lost (GDBusConnection *connection,
              const char      *name,
              gpointer         user_data)
{
  g_warning ("Lost or failed to acquire name %s", name);
}

static void
initialize_dbus_interface (NdRegistry *registry)
{
  registry->dbus_name_id =
    g_bus_own_name (G_BUS_TYPE_SESSION,
                    "org.gnome.NetworkDisplays.Registry",
                    G_BUS_NAME_OWNER_FLAGS_NONE,
                    on_bus_acquired,
                    on_name_acquired,
                    on_name_lost,
                    g_object_ref (registry),
                    g_object_unref);
}

static void
nd_registry_constructed (GObject *object)
{
  NdRegistry *registry = ND_REGISTRY (object);

  registry->dbus_interface = nd_dbus_registry_skeleton_new ();
  initialize_dbus_interface (registry);
}

static void
nd_registry_class_init (NdRegistryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = nd_registry_get_property;
  object_class->set_property = nd_registry_set_property;
  object_class->constructed = nd_registry_constructed;
  object_class->finalize = nd_registry_finalize;

  props[PROP_PROVIDER] =
    g_param_spec_object ("provider", "The sink provider",
                         "The sink provider (usually a MetaProvider) that finds the available sinks.",
                         ND_TYPE_PROVIDER,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, props);
}

static void
nd_registry_init (NdRegistry *self)
{
}


/**
 * nd_registry_get_provider
 * @registry: an #NdRegistry
 *
 * Retrieve the sink provider that is used to populate the registry list.
 *
 * Returns: (transfer none): The sink provider
 */
NdProvider *
nd_registry_get_provider (NdRegistry *registry)
{
  return registry->provider;
}

/**
 * nd_registry_set_provider
 * @registry: a #NdRegistry
 * @provider: an #NdProvider
 *
 * Set the sink provider that is used to populate the registry list.
 */
void
nd_registry_set_provider (NdRegistry *registry,
                          NdProvider *provider)
{
  if (registry->provider)
    {
      g_signal_handlers_disconnect_by_data (registry->provider, registry);
      g_clear_object (&registry->provider);
    }

  if (provider)
    {
      registry->provider = g_object_ref (provider);

      g_signal_connect_object (registry->provider,
                               "sink-added",
                               (GCallback) sink_added_cb,
                               registry,
                               G_CONNECT_SWAPPED);

      g_signal_connect_object (registry->provider,
                               "sink-removed",
                               (GCallback) sink_removed_cb,
                               registry,
                               G_CONNECT_SWAPPED);

    }
}

/**
 * nd_registry_new
 * @provider: an #NdProvider
 *
 * Create an #NdRegistry using the given provider
 *
 * Returns: The newly created #NdRegistry
 */
NdRegistry *
nd_registry_new (NdProvider *provider)
{
  return g_object_new (ND_TYPE_REGISTRY,
                       "provider", provider,
                       NULL);
}
