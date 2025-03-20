/* nd-manager.c
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

#include <gio/gio.h>
#include <glib-object.h>
#include "nd-provider.h"
#include "nd-manager.h"
#include "nd-sink.h"
#include "nd-dbus-manager.h"
#include "nd-systemd-helpers.h"
#include "nd-dbus-systemd.h"

struct _NdManager
{
  GObject parent_instance;

  NdProvider *provider;

  GHashTable *uuid_to_sink;

  NdDBusManager                       *manager_proxy;
  NdDBusOrgFreedesktopSystemd1Manager *systemd_proxy;

  guint manager_name_watch_id;
  guint systemd_name_watch_id;
};

enum {
  PROP_PROVIDER = 1,
  PROP_LAST,
};

G_DEFINE_TYPE (NdManager, nd_manager, G_TYPE_OBJECT)

static GParamSpec * props[PROP_LAST] = { NULL, };

static void
sink_added_cb (NdManager  *manager,
               NdSink     *sink,
               NdProvider *provider)
{
  g_autofree gchar *uuid = NULL;
  g_object_get (sink, "uuid", &uuid, NULL);
  g_hash_table_insert(manager->uuid_to_sink, g_strdup(uuid), sink);

  g_debug ("NdManager: Adding a sink");
}

static void
sink_removed_cb (NdManager  *manager,
                 NdSink     *sink,
                 NdProvider *provider)
{
  g_autofree gchar *uuid = NULL;
  g_object_get (sink, "uuid", &uuid, NULL);
  g_hash_table_remove(manager->uuid_to_sink, g_strdup(uuid));

  g_debug ("NdManager: Removing a sink");
}

static gchar *
nd_manager_start_transient_unit (NdManager   *manager,
                                 const gchar *uri,
                                 const gchar *uuid)
{
  const gchar *unit_name = g_strdup_printf ("gnome-network-displays-stream-%s.service", uuid);

  g_autoptr (GError) error = NULL;

  g_autofree gchar *job = NULL;

  GVariant *properties;
  GVariant *aux;

  properties = build_properties (uuid, uri);
  aux = build_aux ();

  nd_dbus_org_freedesktop_systemd1_manager_call_start_transient_unit_sync (
    manager->systemd_proxy,
    unit_name,
    "replace",
    properties,
    aux,
    &job,
    NULL,
    &error);

  if (!job)
    g_warning ("NdManager: Unable to spawn nd-stream with StartTransientUnit: %s",
               error ? error->message : "none");

  return g_strdup (unit_name);
}

void
nd_manager_stop_transient_unit (NdManager   *manager,
                                const gchar *unit_name)
{
  g_autoptr (GError) error = NULL;
  g_autofree gchar *job = NULL;
  gboolean is_stream_unit;

  is_stream_unit = g_str_has_prefix (unit_name, "gnome-network-displays-stream");
  if (!is_stream_unit)
    {
      g_warning ("NdManager: unit %s is not a GNOME Network Displays stream", unit_name);
      return;
    }

  nd_dbus_org_freedesktop_systemd1_manager_call_kill_unit_sync(
    manager->systemd_proxy,
    unit_name,
    "all",
    SIGTERM,
    NULL,
    &error);

  if (error != NULL)
    g_warning ("NdManager: Error stopping unit %s: %s", unit_name, error->message);
}

static gboolean
handle_start_stream_cb (NdDBusManager         *dbus_manager,
                        GDBusMethodInvocation *invocation,
                        const gchar           *uuid,
                        gpointer               user_data)
{
  NdManager *manager = ND_MANAGER(user_data);

  NdSink *sink = NULL;
  g_autofree gchar *unit_name = NULL;
  g_autofree gchar *uri = NULL;

  sink = g_hash_table_lookup(manager->uuid_to_sink, uuid);

  if (sink == NULL)
    {
      g_warning("Failed to find sink with uuid %s", uuid);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  uri = nd_sink_to_uri(sink);
  unit_name = nd_manager_start_transient_unit(manager, uri, uuid);

  nd_dbus_manager_complete_start_stream(manager->manager_proxy, invocation, unit_name);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_stop_stream_cb (NdDBusManager         *dbus_manager,
                       GDBusMethodInvocation *invocation,
                       const gchar           *unit_name,
                       gpointer               user_data)
{
  NdManager *manager = ND_MANAGER(user_data);

  nd_manager_stop_transient_unit(manager, unit_name);

  nd_dbus_manager_complete_stop_stream(dbus_manager, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
nd_manager_get_property (GObject    *object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  NdManager *manager = ND_MANAGER (object);

  switch (prop_id)
    {
    case PROP_PROVIDER:
      g_value_set_object (value, manager->provider);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_manager_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  NdManager *manager = ND_MANAGER (object);

  switch (prop_id)
    {
    case PROP_PROVIDER:
      nd_manager_set_provider (manager, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_manager_finalize (GObject *object)
{
  NdManager *manager = ND_MANAGER (object);

  nd_manager_set_provider (manager, NULL);

  G_OBJECT_CLASS (nd_manager_parent_class)->finalize (object);
}

static void
on_manager_bus_acquired (GDBusConnection *connection,
                         const char      *name,
                         gpointer         user_data)
{
  NdManager *manager = user_data;

  g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (manager->manager_proxy),
                                    connection,
                                    "/org/gnome/NetworkDisplays/Manager",
                                    NULL);
}

static void
on_manager_name_acquired (GDBusConnection *connection,
                          const char      *name,
                          gpointer         user_data)
{
  g_info ("Acquired name %s", name);
}

static void
on_manager_name_lost (GDBusConnection *connection,
                      const char      *name,
                      gpointer         user_data)
{
  g_warning ("Lost or failed to acquire name %s", name);
}

static void
on_systemd_name_appeared (GDBusConnection *connection,
                          const char *name,
                          const char *name_owner,
                          gpointer user_data)
{
  NdManager *manager = ND_MANAGER (user_data);
  g_autoptr(GError) error = NULL;

  manager->systemd_proxy =
    nd_dbus_org_freedesktop_systemd1_manager_proxy_new_sync (connection,
                                                             G_DBUS_PROXY_FLAGS_NONE,
                                                             "org.freedesktop.systemd1",
                                                             "/org/freedesktop/systemd1",
                                                             NULL,
                                                             &error);

  if (!manager->systemd_proxy)
    {
      g_warning ("NdManager: Failed to acquire org.freedesktop.systemd1.Manager proxy: %s",
                 error ? error->message : "none");
      return;
    }
    g_info ("NdManager: Acquired org.freedesktop.systemd1.Manager proxy");
}

static void
on_systemd_name_vanished (GDBusConnection *connection,
                          const char *name,
                          gpointer user_data)
{
  NdManager *manager = ND_MANAGER (user_data);

  g_clear_object (&manager->systemd_proxy);
}

static void
nd_manager_acquire_dbus_proxies (NdManager *manager)
{
  manager->manager_name_watch_id =
    g_bus_own_name (G_BUS_TYPE_SESSION,
                    "org.gnome.NetworkDisplays.Manager",
                    G_BUS_NAME_OWNER_FLAGS_NONE,
                    on_manager_bus_acquired,
                    on_manager_name_acquired,
                    on_manager_name_lost,
                    g_object_ref (manager),
                    g_object_unref);

  manager->systemd_name_watch_id =
    g_bus_watch_name (G_BUS_TYPE_SESSION,
                      "org.freedesktop.systemd1",
                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                      on_systemd_name_appeared,
                      on_systemd_name_vanished,
                      manager,
                      NULL);
}

static void
nd_manager_constructed (GObject *object)
{
  NdManager *manager = ND_MANAGER (object);

  manager->manager_proxy = nd_dbus_manager_skeleton_new ();
  nd_manager_acquire_dbus_proxies (manager);

  g_signal_connect (manager->manager_proxy,
                    "handle-start-stream",
                    G_CALLBACK (handle_start_stream_cb),
                    manager);

  g_signal_connect (manager->manager_proxy,
                    "handle-stop-stream",
                    G_CALLBACK (handle_stop_stream_cb),
                    manager);
}

static void
nd_manager_class_init (NdManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = nd_manager_get_property;
  object_class->set_property = nd_manager_set_property;
  object_class->constructed = nd_manager_constructed;
  object_class->finalize = nd_manager_finalize;

  props[PROP_PROVIDER] =
    g_param_spec_object ("provider", "The sink provider",
                         "The sink provider (usually a MetaProvider) that finds the available sinks.",
                         ND_TYPE_PROVIDER,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, props);
}

static void
nd_manager_init (NdManager *manager)
{
  manager->uuid_to_sink = g_hash_table_new(g_str_hash, g_str_equal);
}


/******************************************************************
* NdManager public functions
******************************************************************/

/**
 * nd_manager_get_provider
 * @manager: an #NdManager
 *
 * Retrieve the sink provider that is used to populate the manager list.
 *
 * Returns: (transfer none): The sink provider
 */
NdProvider *
nd_manager_get_provider (NdManager *manager)
{
  return manager->provider;
}

 
/**
 * nd_manager_set_provider
 * @manager: a #NdManager
 *
 * Set the sink provider that is used to populate the manager list.
 */
void
nd_manager_set_provider (NdManager  *manager,
                         NdProvider *provider)
{
  if (manager->provider)
    {
      g_signal_handlers_disconnect_by_data (manager->provider, manager);
      g_clear_object (&manager->provider);
    }

  if (provider)
    {
      manager->provider = g_object_ref (provider);

      g_signal_connect_object (manager->provider,
                               "sink-added",
                               (GCallback) sink_added_cb,
                               manager,
                               G_CONNECT_SWAPPED);

      g_signal_connect_object (manager->provider,
                               "sink-removed",
                               (GCallback) sink_removed_cb,
                               manager,
                               G_CONNECT_SWAPPED);

    }
}

NdManager *
nd_manager_new (NdProvider *provider)
{
  return g_object_new (ND_TYPE_MANAGER,
                       "provider", provider,
                       NULL);
}
