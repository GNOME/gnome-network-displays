#include <glib/gi18n.h>
#include <gst/pbutils/pbutils.h>
#include "nd-codec-install.h"

struct _NdCodecInstall
{
  GtkWidget      parent_instance;

  GtkStringList *codecs;

  GtkFrame      *frame;
  GtkListBox    *listbox;
  GtkLabel      *header;
};

G_DEFINE_TYPE (NdCodecInstall, nd_codec_install, GTK_TYPE_WIDGET)

enum {
  PROP_0,
  PROP_TITLE,
  PROP_CODECS,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

NdCodecInstall *
nd_codec_install_new (void)
{
  return g_object_new (ND_TYPE_CODEC_INSTALL, NULL);
}

static void
nd_codec_install_dispose (GObject *object)
{
  NdCodecInstall *self = (NdCodecInstall *) object;

  gtk_widget_unparent (GTK_WIDGET (self->frame));
  gtk_widget_unparent (GTK_WIDGET (self->header));

  G_OBJECT_CLASS (nd_codec_install_parent_class)->dispose (object);
}

static void
nd_codec_install_finalize (GObject *object)
{
  NdCodecInstall *self = (NdCodecInstall *) object;

  g_clear_object (&self->codecs);

  G_OBJECT_CLASS (nd_codec_install_parent_class)->finalize (object);
}

static gchar *
get_description (const gchar *codec)
{
  /* TODO: update it once all elements are final */
  /* video encoders */
  if (g_strcmp0 (codec, "openh264enc") == 0)
    return g_strdup_printf (_("GStreamer OpenH264 video encoder (%s)"), codec);
  else if (g_strcmp0 (codec, "x264enc") == 0)
    return g_strdup_printf (_("GStreamer x264 video encoder (%s)"), codec);
  else if (g_strcmp0 (codec, "vah264enc") == 0)
    return g_strdup_printf (_("GStreamer VA H264 video encoder (%s)"), codec);
  else if (g_strcmp0 (codec, "vaapih264enc") == 0)
    return g_strdup_printf (_("GStreamer VA-API H264 video encoder (%s)"), codec);
  else if (g_strcmp0 (codec, "vp8enc") == 0)
    return g_strdup_printf (_("GStreamer On2 VP8 video encoder (%s)"), codec);
  else if (g_strcmp0 (codec, "vp9enc") == 0)
    return g_strdup_printf (_("GStreamer On2 VP9 video encoder (%s)"), codec);
  /* audio encoders */
  else if (g_strcmp0 (codec, "fdkaacenc") == 0)
    return g_strdup_printf (_("GStreamer FDK AAC audio encoder (%s)"), codec);
  else if (g_strcmp0 (codec, "avenc_aac") == 0)
    return g_strdup_printf (_("GStreamer libav AAC audio encoder (%s)"), codec);
  else if (g_strcmp0 (codec, "faac") == 0)
    return g_strdup_printf (_("GStreamer Free AAC audio encoder (%s)"), codec);
  else if (g_strcmp0 (codec, "vorbisenc") == 0)
    return g_strdup_printf (_("GStreamer Vorbis audio encoder (%s)"), codec);
  else if (g_strcmp0 (codec, "opusenc") == 0)
    return g_strdup_printf (_("GStreamer Opus audio encoder (%s)"), codec);
  /* muxers */
  else if (g_strcmp0 (codec, "webmmux") == 0)
    return g_strdup_printf (_("GStreamer WebM muxer (%s)"), codec);
  else if (g_strcmp0 (codec, "matroskamux") == 0)
    return g_strdup_printf (_("GStreamer Matroska muxer (%s)"), codec);
  else if (g_strcmp0 (codec, "mpegtsmux") == 0)
    return g_strdup_printf (_("GStreamer MPEG Transport Stream muxer (%s)"), codec);

  return g_strdup_printf (_("GStreamer Element “%s”"), codec);
}

static GtkWidget *
create_listbox_row (gpointer item,
                    gpointer user_data)
{
  GtkStringObject *object = item;
  const char *str = gtk_string_object_get_string (object);
  GtkWidget *row;
  GtkWidget *label;
  g_autofree gchar *description = NULL;

  description = get_description (str);
  label = gtk_label_new (description);

  row = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), label);

  g_object_set_data_full (G_OBJECT (row), "codec", g_strdup (str), g_free);
  return row;
}

static void
nd_codec_install_update (NdCodecInstall *self)
{
  guint codec_count;

  if (!self->codecs)
    {
      g_warning ("codec list not initialized");
      return;
    }

  codec_count = g_list_model_get_n_items (G_LIST_MODEL (self->codecs));

  gtk_list_box_bind_model (self->listbox,
                           G_LIST_MODEL (self->codecs),
                           (GtkListBoxCreateWidgetFunc) create_listbox_row,
                           NULL,
                           NULL);

  gtk_widget_set_visible (GTK_WIDGET (self), codec_count > 0);
}

static void
nd_codec_install_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  NdCodecInstall *self = ND_CODEC_INSTALL (object);

  switch (prop_id)
    {
    case PROP_TITLE:
      g_value_set_string (value, gtk_label_get_text (self->header));
      break;

    case PROP_CODECS:
      g_value_set_object (value, self->codecs);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
nd_codec_install_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  NdCodecInstall *self = ND_CODEC_INSTALL (object);

  switch (prop_id)
    {
    case PROP_TITLE:
      gtk_label_set_text (self->header, g_value_get_string (value));
      break;

    case PROP_CODECS:
      self->codecs = g_value_dup_object (value);

      nd_codec_install_update (self);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
nd_codec_install_class_init (NdCodecInstallClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nd_codec_install_dispose;
  object_class->finalize = nd_codec_install_finalize;
  object_class->get_property = nd_codec_install_get_property;
  object_class->set_property = nd_codec_install_set_property;

  properties[PROP_TITLE] =
    g_param_spec_string ("title", "Title",
                         "String for the title of the list",
                         _("Please install one of the following GStreamer plugins by clicking below"),
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_CODECS] =
    g_param_spec_object ("codecs", "Codecs",
                         "List of required codecs",
                         G_TYPE_LIST_MODEL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
on_install_plugins_done_cb (GstInstallPluginsReturn result,
                            gpointer                user_data)
{
  g_debug ("gst_install_plugins_async operation finished");
}

static void
on_row_activated_cb (NdCodecInstall *self, GtkListBoxRow *row)
{
  GstInstallPluginsContext *ctx = NULL;
  const gchar *codecs[2] = { NULL };
  GstInstallPluginsReturn res;
  GstElement *elem;
  GstMessage *msg;

  ctx = gst_install_plugins_context_new ();
  gst_install_plugins_context_set_desktop_id (ctx, "org.gnome.NetworkDisplays");

  elem = gst_pipeline_new ("dummy");
  msg = gst_missing_element_message_new (elem, g_object_get_data (G_OBJECT (row), "codec"));
  codecs[0] = gst_missing_plugin_message_get_installer_detail (msg);

  res = gst_install_plugins_async (codecs, ctx, on_install_plugins_done_cb, NULL);

  /* Try to fall back to calling into gnome-software directly in case the
   * GStreamer plugin helper is not installed.
   * Not sure if this is sane to do, but it may be worth a shot. */
  if (res == GST_INSTALL_PLUGINS_HELPER_MISSING)
    {
      GDBusConnection *con = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
      GVariantBuilder params;
      g_autofree gchar *desc = NULL;

      desc = g_strdup_printf ("%s|gstreamer1(element-%s)()(%dbit)",
                              (const gchar *) g_object_get_data (G_OBJECT (row), "desc"),
                              (const gchar *) g_object_get_data (G_OBJECT (row), "codec"),
                              (gint) sizeof (gpointer) * 8);

      g_variant_builder_init (&params, G_VARIANT_TYPE ("(asssa{sv})"));
      g_variant_builder_open (&params, G_VARIANT_TYPE ("as"));
      g_variant_builder_add (&params, "s", desc);
      g_variant_builder_close (&params);
      g_variant_builder_add (&params, "s", "");
      g_variant_builder_add (&params, "s", "org.gnome.NetworkDisplays");
      g_variant_builder_open (&params, G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_close (&params);

      g_dbus_connection_call (con,
                              "org.gnome.Software",
                              "/org/freedesktop/PackageKit",
                              "org.freedesktop.PackageKit.Modify2",
                              "InstallGStreamerResources",
                              g_variant_builder_end (&params),
                              NULL,
                              G_DBUS_CALL_FLAGS_NONE,
                              -1,
                              NULL,
                              NULL,
                              NULL);
    }
}

static void
nd_codec_install_init (NdCodecInstall *self)
{
  GtkBoxLayout *box_layout;

  box_layout = GTK_BOX_LAYOUT (gtk_box_layout_new (GTK_ORIENTATION_VERTICAL));
  gtk_box_layout_set_spacing (box_layout, 6);
  gtk_widget_set_layout_manager (GTK_WIDGET (self), GTK_LAYOUT_MANAGER (box_layout));
  gtk_widget_set_visible (GTK_WIDGET (self), TRUE);

  self->header = GTK_LABEL (gtk_label_new (g_value_get_string (g_param_spec_get_default_value (properties[PROP_TITLE]))));
  g_object_set (self->header,
                "wrap", TRUE,
                "wrap-mode", GTK_WRAP_WORD,
                "xalign", 0.0,
                NULL);
  gtk_widget_set_parent (GTK_WIDGET (self->header), GTK_WIDGET (self));
  gtk_widget_set_visible (GTK_WIDGET (self->header), TRUE);

  self->frame = GTK_FRAME (gtk_frame_new (NULL));
  gtk_widget_set_parent (GTK_WIDGET (self->frame), GTK_WIDGET (self));
  gtk_widget_set_visible (GTK_WIDGET (self->frame), TRUE);

  self->listbox = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_frame_set_child (self->frame, GTK_WIDGET (self->listbox));

  self->codecs = gtk_string_list_new (NULL);

  gtk_widget_set_visible (GTK_WIDGET (self->listbox), TRUE);

  g_signal_connect_object (self->listbox,
                           "row-activated",
                           G_CALLBACK (on_row_activated_cb),
                           self,
                           G_CONNECT_SWAPPED);
}
