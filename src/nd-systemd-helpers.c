#include <gio/gio.h>
#include <glib-object.h>

static GVariant *
build_execstart (const gchar *uuid,
                 const gchar *uri)
{
  const gchar *path = "/usr/libexec/gnome-network-displays-stream";

  GVariantBuilder execstart_builder;
  GVariantBuilder command_builder;

  GVariant *execstart;

  g_variant_builder_init (&execstart_builder, G_VARIANT_TYPE ("a(sasb)"));

  g_variant_builder_init (&command_builder, G_VARIANT_TYPE ("as"));
  g_variant_builder_add (&command_builder, "s", path);
  g_variant_builder_add (&command_builder, "s", uri);

  g_variant_builder_add (&execstart_builder, "(sasb)",
                         path,
                         &command_builder,
                         FALSE);

  execstart = g_variant_builder_end (&execstart_builder);

  return g_steal_pointer (&execstart);
}

static GVariant *
build_description (const gchar *name)
{
  const gchar *desc = g_strdup_printf ("GNOME Network Displays stream for %s", name);

  GVariant *description;

  description = g_variant_new_string (desc);

  return g_steal_pointer (&description);
}

GVariant *
build_properties (const gchar *uuid,
                  const gchar *uri,
                  const gchar *display_name)
{
  GVariantBuilder properties_builder;

  GVariant *properties;
  GVariant *execstart;
  GVariant *description;

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE ("a(sv)"));

  execstart = build_execstart (uuid, uri);
  g_variant_builder_add (&properties_builder, "(sv)", "ExecStart", execstart);

  description = build_description (display_name);
  g_variant_builder_add (&properties_builder, "(sv)", "Description", description);

  properties = g_variant_builder_end (&properties_builder);

  return g_steal_pointer (&properties);
}

GVariant *
build_aux ()
{
  GVariantBuilder aux_builder;
  GVariant *aux;

  g_variant_builder_init (&aux_builder, G_VARIANT_TYPE ("a(sa(sv))"));
  aux = g_variant_builder_end (&aux_builder);

  return g_steal_pointer (&aux);
}
