/* gnome-nd-window.c
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

#include <avahi-gobject/ga-client.h>
#include <avahi-gobject/ga-service-browser.h>
#include <glib/gi18n.h>
#include <gst/base/base.h>
#include <gst/gst.h>
#include <libportal-gtk4/portal-gtk4.h>
#include "gnome-network-displays-config.h"
#include "nd-cc-provider.h"
#include "nd-codec-install.h"
#include "nd-dummy-provider.h"
#include "nd-meta-provider.h"
#include "nd-nm-device-registry.h"
#include "nd-pulseaudio.h"
#include "nd-sink-list-model.h"
#include "nd-sink-row.h"
#include "nd-wfd-mice-provider.h"
#include "nd-window.h"

struct _NdWindow
{
  AdwApplicationWindow   parent_instance;

  GaClient              *avahi_client;
  NdMetaProvider        *meta_provider;
  NdNMDeviceRegistry    *nm_device_registry;

  XdpPortal             *portal;
  XdpSession            *session;
  NdScreenCastSourceType screencast_type;
  gboolean               use_x11;

  NdPulseaudio          *pulse;

  GCancellable          *cancellable;

  NdSink                *stream_sink;

  GPtrArray             *sink_property_bindings;

  /* Template widgets */
  GtkStack        *has_providers_stack;
  GtkStack        *step_stack;

  GtkListBox      *find_sink_list;
  NdSinkListModel *find_sink_list_model;

  GtkListBox      *connect_sink_list;
  GListStore      *connect_sink_list_model;

  GtkLabel        *connect_state_label;
  GtkButton       *connect_cancel;

  GtkListBox      *stream_sink_list;
  GListStore      *stream_sink_list_model;

  GtkLabel        *stream_state_label;
  GtkButton       *stream_cancel;

  NdCodecInstall  *codec_install_video;
  NdCodecInstall  *codec_install_audio;

  GtkListBox      *error_sink_list;
  GListStore      *error_sink_list_model;
  GtkBox          *error_firewall_zone;
  GtkButton       *error_return;
};

G_DEFINE_TYPE (NdWindow, gnome_nd_window, ADW_TYPE_APPLICATION_WINDOW)

static GstElement *
nd_window_screencast_get_source (NdWindow * self)
{
  g_autoptr(GVariant) stream_properties = NULL;
  g_autoptr(GError) error = NULL;
  GstElement *src = NULL;
  GVariant *streams = NULL;
  GVariantIter iter;
  guint32 node_id;
  guint32 screencast_type;

  if (!self->session)
    g_error ("XDP session not found!");

  streams = xdp_session_get_streams (self->session);
  if (streams == NULL)
    g_error ("XDP session streams not found!");

  g_variant_iter_init (&iter, streams);
  g_variant_iter_loop (&iter, "(u@a{sv})", &node_id, &stream_properties);
  g_variant_lookup (stream_properties, "source_type", "u", &screencast_type);

  g_debug ("Got a stream with node ID: %d", node_id);
  g_debug ("Got a stream of type: %d", screencast_type);

  switch (screencast_type)
    {
    case ND_SCREEN_CAST_SOURCE_TYPE_MONITOR:
    case ND_SCREEN_CAST_SOURCE_TYPE_WINDOW:
    case ND_SCREEN_CAST_SOURCE_TYPE_VIRTUAL:
      self->screencast_type = screencast_type;
      break;

    default:
      g_assert_not_reached ();
    }

  src = gst_element_factory_make ("pipewiresrc", "portal-pipewire-source");
  if (src == NULL)
    g_error ("GStreamer element \"pipewiresrc\" could not be created!");

  g_object_set (src,
                "fd", xdp_session_open_pipewire_remote (self->session),
                "path", g_strdup_printf ("%u", node_id),
                "do-timestamp", TRUE,
                NULL);

  gst_base_src_set_live (GST_BASE_SRC (src), TRUE);

  return g_steal_pointer (&src);
}

static GstElement *
sink_create_source_cb (NdWindow * self, NdSink * sink)
{
  g_autoptr(GstCaps) caps = NULL;
  GstBin *bin;
  GstElement *src, *filter, *dst, *res;

  bin = GST_BIN (gst_bin_new ("screencast source bin"));
  g_debug ("use x11: %d", self->use_x11);
  if (self->use_x11)
    src = gst_element_factory_make ("ximagesrc", "X11 screencast source");
  else
    src = nd_window_screencast_get_source (self);

  if (!src)
    g_error ("Error creating video source element, likely a missing dependency!");

  gst_bin_add (bin, src);

  dst = gst_element_factory_make ("intervideosink", "inter video sink");
  if (!dst)
    g_error ("Error creating intervideosink, missing dependency!");
  g_object_set (dst,
                "channel", "nd-inter-video",
                "max-lateness", (gint64) - 1,
                "sync", FALSE,
                NULL);
  gst_bin_add (bin, dst);

  if (self->screencast_type == ND_SCREEN_CAST_SOURCE_TYPE_VIRTUAL)
    {
      /* Initial caps for virtual display */
      caps = gst_caps_new_simple ("video/x-raw",
                                  "max-framerate", GST_TYPE_FRACTION, 30, 1,
                                  "width", G_TYPE_INT, 1920,
                                  "height", G_TYPE_INT, 1080,
                                  NULL);
      filter = gst_element_factory_make ("capsfilter", "srcfilter");
      gst_bin_add (bin, filter);
      g_object_set (filter,
                    "caps", caps,
                    NULL);
      g_clear_pointer (&caps, gst_caps_unref);

      gst_element_link_many (src, filter, dst, NULL);
    }
  else
    gst_element_link_many (src, dst, NULL);

  res = gst_element_factory_make ("intervideosrc", "screencastsrc");
  g_object_set (res,
                "do-timestamp", FALSE,
                "timeout", (guint64) G_MAXUINT64,
                "channel", "nd-inter-video",
                NULL);

  gst_bin_add (bin, res);

  gst_element_add_pad (GST_ELEMENT (bin),
                       gst_ghost_pad_new ("src",
                                          gst_element_get_static_pad (res,
                                                                      "src")));

  g_object_ref_sink (bin);
  return GST_ELEMENT (bin);
}

static GstElement *
sink_create_audio_source_cb (NdWindow * self, NdSink * sink)
{
  GstElement *res;

  if (!self->pulse)
    return NULL;

  res = nd_pulseaudio_get_source (self->pulse);

  return g_object_ref_sink (res);
}

static void
sink_notify_state_cb (NdWindow *self, GParamSpec *pspec, NdSink *sink)
{
  NdSinkState state;

  g_object_get (sink, "state", &state, NULL);
  g_debug ("Got state change notification from streaming sink to state %s",
           g_enum_to_string (ND_TYPE_SINK_STATE, state));

  switch (state)
    {
    case ND_SINK_STATE_ENSURE_FIREWALL:
      gtk_label_set_text (self->connect_state_label,
                          _("Checking and installing required firewall zones."));

      gtk_stack_set_visible_child_name (self->step_stack, "connect");
      break;

    case ND_SINK_STATE_WAIT_P2P:
      gtk_label_set_text (self->connect_state_label,
                          _("Making P2P connection"));

      gtk_stack_set_visible_child_name (self->step_stack, "connect");
      break;

    case ND_SINK_STATE_WAIT_SOCKET:
      gtk_label_set_text (self->connect_state_label,
                          _("Establishing connection to sink"));

      gtk_stack_set_visible_child_name (self->step_stack, "connect");
      break;

    case ND_SINK_STATE_WAIT_STREAMING:
      gtk_label_set_text (self->connect_state_label,
                          _("Starting to stream"));

      gtk_stack_set_visible_child_name (self->step_stack, "connect");
      break;

    case ND_SINK_STATE_STREAMING:
      g_list_store_remove_all (self->connect_sink_list_model);
      g_list_store_remove_all (self->stream_sink_list_model);
      g_list_store_remove_all (self->error_sink_list_model);

      g_list_store_append (self->stream_sink_list_model, self->stream_sink);

      gtk_stack_set_visible_child_name (self->step_stack, "stream");
      break;

    case ND_SINK_STATE_ERROR:
      g_list_store_remove_all (self->connect_sink_list_model);
      g_list_store_remove_all (self->stream_sink_list_model);
      g_list_store_remove_all (self->error_sink_list_model);

      g_list_store_append (self->error_sink_list_model, self->stream_sink);

      gtk_stack_set_visible_child_name (self->step_stack, "error");
      break;

    case ND_SINK_STATE_DISCONNECTED:
      g_list_store_remove_all (self->connect_sink_list_model);
      g_list_store_remove_all (self->stream_sink_list_model);
      g_list_store_remove_all (self->error_sink_list_model);

      gtk_stack_set_visible_child_name (self->step_stack, "find");
      g_object_set (self->meta_provider, "discover", TRUE, NULL);

      g_ptr_array_set_size (self->sink_property_bindings, 0);

      g_signal_handlers_disconnect_by_data (self->stream_sink, self);
      g_clear_object (&self->stream_sink);
      break;
    }
}

gboolean
transform_str_is_set_to_bool (GBinding     *binding,
                              const GValue *from_value,
                              GValue       *to_value,
                              gpointer      user_data)
{
  g_value_set_boolean (to_value, g_value_get_string (from_value) != NULL);

  return TRUE;
}

static void
nd_screencast_started_cb (GObject      *source_object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  XdpSession *session = XDP_SESSION (source_object);
  NdWindow *window = ND_WINDOW (user_data);

  if (!xdp_session_start_finish (session, result, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("Error initializing screencast portal: %s", error->message);

          /* Unknown method means the portal does not exist, give a slightly
           * more specific warning then.
           */
          if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
            g_warning ("Screencasting portal is unavailable! It is required to select the monitor to stream!");

          g_warning ("Falling back to X11! You need to fix your setup to avoid issues (XDG Portals and/or mutter screencasting support)!");
          window->use_x11 = TRUE;
        }

      g_warning ("Failed to start screencast session: %s", error->message);
      return;
    }
  g_debug ("Created screencast session");
}

static void
session_closed_cb (NdWindow *self)
{
  g_debug ("Session closed");
  if (self->stream_sink)
    nd_sink_stop_stream (self->stream_sink);

  g_clear_object (&self->session);
}

static void
nd_screencast_init_cb (GObject      *source_object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  XdpPortal *portal = XDP_PORTAL (source_object);
  NdWindow *window = ND_WINDOW (user_data);
  XdpParent *parent = NULL;

  window->portal = portal;
  window->session = xdp_portal_create_screencast_session_finish (window->portal, result, &error);
  if (window->session == NULL)
    {
      g_warning ("Failed to create screencast session: %s", error->message);
      window->use_x11 = TRUE;
      return;
    }

  g_signal_connect_object (window->session,
                           "closed",
                           (GCallback) session_closed_cb,
                           window,
                           G_CONNECT_SWAPPED);

  parent = xdp_parent_new_gtk (GTK_WINDOW (window));
  xdp_session_start (window->session, parent, NULL, nd_screencast_started_cb, window);
  xdp_parent_free (parent);
}

static void
find_sink_list_row_activated_cb (NdWindow *self, NdSinkRow *row, GtkListBox *sink_list)
{
  NdSink *sink;

  if (!self->use_x11 && !self->portal)
    {
      g_warning ("Cannot start streaming right now as we don't have a portal!");
      return;
    }

  if (!self->use_x11 && self->portal && !self->session)
    {
      xdp_portal_create_screencast_session (self->portal,
                                            XDP_OUTPUT_MONITOR | XDP_OUTPUT_WINDOW | XDP_OUTPUT_VIRTUAL,
                                            XDP_SCREENCAST_FLAG_NONE,
                                            XDP_CURSOR_MODE_EMBEDDED,
                                            XDP_PERSIST_MODE_NONE,
                                            NULL,
                                            self->cancellable,
                                            nd_screencast_init_cb,
                                            self);
      g_debug ("NdWindow: Re-creating portal session!");
      return;
    }

  g_assert (ND_IS_SINK_ROW (row));

  sink = nd_sink_row_get_sink (row);
  self->stream_sink = nd_sink_start_stream (sink);

  if (!self->stream_sink)
    {
      g_warning ("NdWindow: Could not start streaming!");
      return;
    }

  g_signal_connect_object (self->stream_sink,
                           "create-source",
                           (GCallback) sink_create_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->stream_sink,
                           "create-audio-source",
                           (GCallback) sink_create_audio_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->stream_sink,
                           "notify::state",
                           (GCallback) sink_notify_state_cb,
                           self,
                           G_CONNECT_SWAPPED);

  /* We might have moved into the error state in the meantime. */
  sink_notify_state_cb (self, NULL, self->stream_sink);

  g_ptr_array_add (self->sink_property_bindings,
                   g_object_ref (g_object_bind_property (self->stream_sink,
                                                         "missing-video-codec",
                                                         self->codec_install_video,
                                                         "codecs",
                                                         G_BINDING_SYNC_CREATE)));

  g_ptr_array_add (self->sink_property_bindings,
                   g_object_ref (g_object_bind_property (self->stream_sink,
                                                         "missing-audio-codec",
                                                         self->codec_install_audio,
                                                         "codecs",
                                                         G_BINDING_SYNC_CREATE)));

  g_ptr_array_add (self->sink_property_bindings,
                   g_object_ref (g_object_bind_property_full (self->stream_sink,
                                                              "missing-firewall-zone",
                                                              self->error_firewall_zone,
                                                              "reveal-child",
                                                              G_BINDING_SYNC_CREATE,
                                                              transform_str_is_set_to_bool,
                                                              NULL,
                                                              NULL,
                                                              NULL)));

  g_object_set (self->meta_provider, "discover", FALSE, NULL);
  g_list_store_append (self->connect_sink_list_model, self->stream_sink);
}

static void
gnome_nd_window_constructed (GObject *obj)
{
  G_OBJECT_CLASS (gnome_nd_window_parent_class)->constructed (obj);

  g_autoptr(GError) error = NULL;
  g_autoptr(NdWFDMiceProvider) mice_provider = NULL;
  g_autoptr(NdCCProvider) cc_provider = NULL;
  NdWindow *self = ND_WINDOW (obj);

  self->cancellable = g_cancellable_new ();
  self->avahi_client = ga_client_new (GA_CLIENT_FLAG_NO_FLAGS);

  if (!ga_client_start (self->avahi_client, &error))
    {
      g_warning ("NdWindow: Failed to start Avahi Client");
      if (error != NULL)
        g_warning ("NdWindow: Error: %s", error->message);
      return;
    }

  g_debug ("NdWindow: Got avahi client");

  mice_provider = nd_wfd_mice_provider_new (self->avahi_client);
  cc_provider = nd_cc_provider_new (self->avahi_client);

  if (!nd_wfd_mice_provider_browse (mice_provider, error) || !nd_cc_provider_browse (cc_provider, error))
    {
      g_warning ("NdWindow: Avahi client failed to browse: %s", error->message);
      return;
    }

  g_debug ("NdWindow: Got avahi browser");
  nd_meta_provider_add_provider (self->meta_provider, ND_PROVIDER (mice_provider));
  nd_meta_provider_add_provider (self->meta_provider, ND_PROVIDER (cc_provider));
  if (g_strcmp0 (g_getenv ("NETWORK_DISPLAYS_DUMMY"), "1") == 0)
    {
      g_autoptr(NdDummyProvider) dummy_provider = NULL;

      g_debug ("Adding dummy provider");
      dummy_provider = nd_dummy_provider_new ();
      nd_meta_provider_add_provider (self->meta_provider, ND_PROVIDER (dummy_provider));
    }
}

static void
gnome_nd_window_finalize (GObject *obj)
{
  NdWindow *self = ND_WINDOW (obj);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  nd_pulseaudio_unload (self->pulse);
  g_clear_object (&self->portal);
  g_clear_object (&self->pulse);

  g_clear_object (&self->stream_sink);

  g_clear_object (&self->meta_provider);
  g_clear_object (&self->nm_device_registry);
  g_clear_object (&self->avahi_client);

  g_clear_pointer (&self->sink_property_bindings, g_ptr_array_unref);

  if (self->session)
    xdp_session_close (self->session);

  if (self->session)
    g_clear_object (&self->session);

  G_OBJECT_CLASS (gnome_nd_window_parent_class)->finalize (obj);
}

static void
gnome_nd_window_dispose (GObject *obj)
{
  NdWindow *self = ND_WINDOW (obj);

  g_object_run_dispose (G_OBJECT (self->avahi_client));

  G_OBJECT_CLASS (gnome_nd_window_parent_class)->dispose (obj);
}

static void
gnome_nd_window_class_init (NdWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = gnome_nd_window_constructed;
  object_class->finalize = gnome_nd_window_finalize;
  object_class->dispose = gnome_nd_window_dispose;

  ND_TYPE_CODEC_INSTALL;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/NetworkDisplays/nd-window.ui");
  gtk_widget_class_bind_template_child (widget_class, NdWindow, has_providers_stack);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, step_stack);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, find_sink_list);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, connect_sink_list);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, connect_state_label);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, connect_cancel);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, stream_sink_list);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, stream_state_label);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, codec_install_audio);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, codec_install_video);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, stream_cancel);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, error_sink_list);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, error_firewall_zone);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, error_return);
}

static void
nd_pulseaudio_init_async_cb (GObject      *source_object,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  NdWindow *window;

  g_autoptr(GError) error = NULL;

  if (!g_async_initable_init_finish (G_ASYNC_INITABLE (source_object), res, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Error initializing pulse audio sink: %s", error->message);

      g_object_unref (source_object);
      return;
    }

  window = ND_WINDOW (user_data);
  window->pulse = ND_PULSEAUDIO (source_object);
}

static void
stream_stop_clicked_cb (NdWindow *self)
{
  if (!self->stream_sink)
    return;

  nd_sink_stop_stream (self->stream_sink);
}

static void
on_meta_provider_has_provider_changed_cb (NdWindow *self, GParamSpec *pspec, NdMetaProvider *provider)
{
  gboolean has_providers;

  g_object_get (self->meta_provider,
                "has-providers", &has_providers,
                NULL);
  gtk_stack_set_visible_child_name (self->has_providers_stack,
                                    has_providers ? "has-providers" : "no-providers");
}

static void
gnome_nd_window_init (NdWindow *self)
{
  g_autoptr(GError) error = NULL;
  NdPulseaudio *pulse;

  g_debug ("GNOME Network Displays v%s started", PACKAGE_VERSION);

  gtk_widget_init_template (GTK_WIDGET (self));

  self->meta_provider = nd_meta_provider_new ();
  g_signal_connect_object (self->meta_provider,
                           "notify::has-providers",
                           (GCallback) on_meta_provider_has_provider_changed_cb,
                           self,
                           G_CONNECT_SWAPPED);

  self->nm_device_registry = nd_nm_device_registry_new (self->meta_provider);

  self->connect_sink_list_model = g_list_store_new (ND_TYPE_SINK);
  self->stream_sink_list_model = g_list_store_new (ND_TYPE_SINK);
  self->error_sink_list_model = g_list_store_new (ND_TYPE_SINK);
  self->find_sink_list_model = nd_sink_list_model_new (ND_PROVIDER (self->meta_provider));

  gtk_list_box_bind_model (self->connect_sink_list,
                           G_LIST_MODEL (self->connect_sink_list_model),
                           (GtkListBoxCreateWidgetFunc) nd_sink_row_new,
                           NULL,
                           NULL);
  gtk_list_box_bind_model (self->stream_sink_list,
                           G_LIST_MODEL (self->stream_sink_list_model),
                           (GtkListBoxCreateWidgetFunc) nd_sink_row_new,
                           NULL,
                           NULL);
  gtk_list_box_bind_model (self->error_sink_list,
                           G_LIST_MODEL (self->error_sink_list_model),
                           (GtkListBoxCreateWidgetFunc) nd_sink_row_new,
                           NULL,
                           NULL);
  gtk_list_box_bind_model (self->find_sink_list,
                           G_LIST_MODEL (self->find_sink_list_model),
                           (GtkListBoxCreateWidgetFunc) nd_sink_row_new,
                           NULL,
                           NULL);

  g_signal_connect_object (self->find_sink_list,
                           "row-activated",
                           (GCallback) find_sink_list_row_activated_cb,
                           self,
                           G_CONNECT_SWAPPED);

  self->cancellable = g_cancellable_new ();

  /* All of these buttons just stop the stream, which will return us
   * to the DISCONNECTED state and the selection page. */
  g_signal_connect_object (self->connect_cancel,
                           "clicked",
                           (GCallback) stream_stop_clicked_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->stream_cancel,
                           "clicked",
                           (GCallback) stream_stop_clicked_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->error_return,
                           "clicked",
                           (GCallback) stream_stop_clicked_cb,
                           self,
                           G_CONNECT_SWAPPED);

  self->portal = xdp_portal_initable_new (&error);
  if (error)
    {
      g_warning ("Failed to create screencast portal: %s", error->message);
      self->use_x11 = TRUE;
      g_clear_object (&self->portal);
    }

  if (self->portal)
    {
      xdp_portal_create_screencast_session (self->portal,
                                            XDP_OUTPUT_MONITOR | XDP_OUTPUT_WINDOW | XDP_OUTPUT_VIRTUAL,
                                            XDP_SCREENCAST_FLAG_NONE,
                                            XDP_CURSOR_MODE_EMBEDDED,
                                            XDP_PERSIST_MODE_NONE,
                                            NULL,
                                            self->cancellable,
                                            nd_screencast_init_cb,
                                            self);
      g_debug ("NdWindow: Creating portal session!");
    }

  pulse = nd_pulseaudio_new ("GNOME Network Displays", "gnome-network-displays");
  g_async_initable_init_async (G_ASYNC_INITABLE (pulse),
                               G_PRIORITY_LOW,
                               self->cancellable,
                               nd_pulseaudio_init_async_cb,
                               self);

  self->sink_property_bindings = g_ptr_array_new_full (0, (GDestroyNotify) g_binding_unbind);
}
