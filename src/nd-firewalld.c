#include <gio/gio.h>
#include "nd-firewalld.h"

#define FIREWALLD_NAME "org.fedoraproject.FirewallD1"
#define FIREWALLD_PATH "/org/fedoraproject/FirewallD1"

#define ZONE_TYPE "(sssbsasa(ss)asba(ssss)asasasasa(ss)b)"

struct _NdFirewalld
{
  GObject parent_instance;
};

G_DEFINE_TYPE (NdFirewalld, nd_firewalld, G_TYPE_OBJECT)

NdFirewalld *
nd_firewalld_new (void)
{
  return g_object_new (ND_TYPE_FIREWALLD, NULL);
}

static void
nd_firewalld_finalize (GObject *object)
{
  G_OBJECT_CLASS (nd_firewalld_parent_class)->finalize (object);
}

static void
nd_firewalld_class_init (NdFirewalldClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = nd_firewalld_finalize;
}

static void
nd_firewalld_init (NdFirewalld *self)
{
}

static void
loaded_wfd_zone_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GDBusConnection *connection = G_DBUS_CONNECTION (source_object);

  g_autoptr(GTask) task = G_TASK (user_data);
  g_autoptr(GVariant) val = NULL;
  GError *error = NULL;

  val = g_dbus_connection_call_finish (connection, res, &error);
  if (!val)
    {
      g_task_return_error (task, error);
      return;
    }

  /* Assume everything is fine (though, we should maybe double check the configuration). */
  g_task_return_boolean (task, TRUE);
}

static void
create_wfd_zone_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GDBusConnection *connection = G_DBUS_CONNECTION (source_object);
  GCancellable *cancellable = NULL;

  g_autoptr(GTask) task = G_TASK (user_data);
  g_autoptr(GVariant) val = NULL;
  GError *error = NULL;

  val = g_dbus_connection_call_finish (connection, res, &error);
  if (!val)
    {
      g_task_return_error (task, error);
      return;
    }

  /* We created a zone, but it will not be loaded yet. We also need to
   * trigger a reload after doing this. */
  cancellable = g_task_get_cancellable (task);
  g_dbus_connection_call (connection,
                          FIREWALLD_NAME,
                          FIREWALLD_PATH,
                          FIREWALLD_NAME,
                          "reload",
                          NULL,
                          G_VARIANT_TYPE_UNIT,
                          G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                          60000,
                          cancellable,
                          loaded_wfd_zone_cb,
                          g_steal_pointer (&task));
}

static void
create_wfd_zone (GTask *task, GDBusConnection *connection)
{
  GVariantBuilder builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("(s" ZONE_TYPE ")"));

  g_variant_builder_add (&builder, "s", ND_WFD_ZONE);
  g_variant_builder_open (&builder, G_VARIANT_TYPE (ZONE_TYPE));
  /* alright, this is "fun", quoting zone.py:
   *     ( "version",  "" ),                            # s
   *     ( "short", "" ),                               # s
   *     ( "description", "" ),                         # s
   *     ( "UNUSED", False ),                           # b
   *     ( "target", "" ),                              # s
   *     ( "services", [ "", ], ),                      # as
   *     ( "ports", [ ( "", "" ), ], ),                 # a(ss)
   *     ( "icmp_blocks", [ "", ], ),                   # as
   *     ( "masquerade", False ),                       # b
   *     ( "forward_ports", [ ( "", "", "", "" ), ], ), # a(ssss)
   *     ( "interfaces", [ "" ] ),                      # as
   *     ( "sources", [ "" ] ),                         # as
   *     ( "rules_str", [ "" ] ),                       # as
   *     ( "protocols", [ "", ], ),                     # as
   *     ( "source_ports", [ ( "", "" ), ], ),          # a(ss)
   *     ( "icmp_block_inversion", False ),             # b
   */
  g_variant_builder_add (&builder, "s", "");
  g_variant_builder_add (&builder, "s", "GNOME Network Displays WiFi-Display");
  g_variant_builder_add (&builder, "s", "A zone intended to be used by GNOME Network Displays when establishing P2P connections to Wi-Fi Display (Miracast) sinks.");
  g_variant_builder_add (&builder, "b", FALSE);
  g_variant_builder_add (&builder, "s", "ACCEPT"); /* Target name */

  g_variant_builder_open (&builder, G_VARIANT_TYPE ("as")); /* Services*/
  g_variant_builder_add (&builder, "s", "dhcp");
  g_variant_builder_add (&builder, "s", "dns");
  g_variant_builder_close (&builder);

  g_variant_builder_open (&builder, G_VARIANT_TYPE ("a(ss)")); /* Ports */
  g_variant_builder_add (&builder, "(ss)", "7236", "tcp");
  g_variant_builder_close (&builder);

  g_variant_builder_open (&builder, G_VARIANT_TYPE ("as")); /* ICMP blocks */
  g_variant_builder_close (&builder);

  g_variant_builder_add (&builder, "b", FALSE); /* Masquarade (no need, but also setup by NM anyway) */

  g_variant_builder_open (&builder, G_VARIANT_TYPE ("a(ssss)")); /* Forward Ports */
  g_variant_builder_close (&builder);

  g_variant_builder_open (&builder, G_VARIANT_TYPE ("as")); /* Interfaces */
  g_variant_builder_close (&builder);

  g_variant_builder_open (&builder, G_VARIANT_TYPE ("as")); /* Sources */
  g_variant_builder_close (&builder);

  g_variant_builder_open (&builder, G_VARIANT_TYPE ("as")); /* Rules */
  g_variant_builder_add (&builder, "s", "rule priority=\"32767\" reject");
  g_variant_builder_close (&builder);

  g_variant_builder_open (&builder, G_VARIANT_TYPE ("as")); /* Protocols */
  g_variant_builder_add (&builder, "s", "icmp");
  g_variant_builder_add (&builder, "s", "ipv6-icmp");
  g_variant_builder_close (&builder);

  g_variant_builder_open (&builder, G_VARIANT_TYPE ("a(ss)")); /* Source Ports */
  /* We leave this empty and just allow outgoing connections. */
  g_variant_builder_close (&builder);

  g_variant_builder_add (&builder, "b", FALSE); /* ICMP block inversion */

  g_variant_builder_close (&builder);

  g_dbus_connection_call (connection,
                          FIREWALLD_NAME,
                          FIREWALLD_PATH "/config",
                          FIREWALLD_NAME ".config",
                          "addZone",
                          g_variant_builder_end (&builder),
                          G_VARIANT_TYPE ("(o)"),
                          G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                          60000,
                          g_task_get_cancellable (task),
                          create_wfd_zone_cb,
                          task);
}

static void
get_zone_settings_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GDBusConnection *connection = G_DBUS_CONNECTION (source_object);

  g_autoptr(GTask) task = G_TASK (user_data);
  g_autoptr(GVariant) val = NULL;
  GError *error = NULL;

  val = g_dbus_connection_call_finish (connection, res, &error);
  if (!val)
    {
      if (g_dbus_error_is_remote_error (error))
        {
          g_autofree char *remote = NULL;

          remote = g_dbus_error_get_remote_error (error);

          if (g_str_equal (remote, "org.freedesktop.DBus.Error.ServiceUnknown"))
            {
              g_debug ("NdFirewalld: Firewalld does not seem to be installed. Code will assume that no firewall will be configured.");
              g_task_return_boolean (task, TRUE);
              return;
            }

          g_debug ("Received error: %s", remote);
          g_debug ("Will try to create the zone!");
          g_clear_error (&error);
          create_wfd_zone (g_steal_pointer (&task), connection);

          return;
        }

      g_task_return_error (task, error);
      return;
    }

  /* Assume everything is fine (though, we should maybe double check the configuration). */
  g_task_return_boolean (task, TRUE);
}

void
nd_firewalld_ensure_wfd_zone (NdFirewalld        *self,
                              GCancellable       *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer            user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GDBusConnection) connection = NULL;
  GError *error = NULL;

  /* We only support a single zone (for WFD) for now, so don't bother
   * with creating a more generic API as it is only internal anyway.
   */

  /* In reality, this API is mostly stateless and does not need an object. */
  task = g_task_new (self, cancellable, callback, user_data);

  connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, cancellable, &error);
  if (!connection)
    {
      g_task_return_error (task, error);
      return;
    }

  g_dbus_connection_call (connection,
                          FIREWALLD_NAME,
                          FIREWALLD_PATH,
                          FIREWALLD_NAME,
                          "getZoneSettings",
                          g_variant_new ("(s)", ND_WFD_ZONE),
                          G_VARIANT_TYPE ("(" ZONE_TYPE ")"),
                          G_DBUS_CALL_FLAGS_NONE, /* We do not allow password prompting here. */
                          500,
                          cancellable,
                          get_zone_settings_cb,
                          g_steal_pointer (&task));
}

gboolean
nd_firewalld_ensure_wfd_zone_finish (NdFirewalld  *self,
                                     GAsyncResult *res,
                                     GError      **error)
{
  return g_task_propagate_boolean (G_TASK (res), error);
}
