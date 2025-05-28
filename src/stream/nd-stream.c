/* nd-stream.c
 *
 * Copyright 2024 Pedro Sader Azevedo <pedro.saderazevedo@proton.me>
 * Copyright 2024 Christian Glombek <lorbus@fedoraproject.org>
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
#include <glib-unix.h>
#include <libportal-gtk4/portal-gtk4.h>
#include <gst/base/base.h>
#include <gst/gst.h>

#include "gnome-network-displays-config.h"
#include "nd-stream.h"
#include "nd-pulseaudio.h"
#include "nd-sink.h"

struct _NdStream
{
  GApplication           parent_instance;

  GSource               *sigterm_source;
  GSource               *sigint_source;

  XdpPortal             *portal;
  XdpSession            *session;
  NdPulseaudio          *pulse;
  NdScreenCastSourceType screencast_type;

  GCancellable          *cancellable;

  NdSink                *sink;
};

enum {
  PROP_SINK = 1,
  PROP_LAST,
};

G_DEFINE_TYPE (NdStream, nd_stream, G_TYPE_APPLICATION)

static GParamSpec * props[PROP_LAST] = { NULL, };

static void
nd_stream_get_property (GObject    *object,
                        guint       prop_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
  NdStream *self = ND_STREAM (object);

  switch (prop_id)
    {
    case PROP_SINK:
      g_value_set_object (value, self->sink);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_stream_set_property (GObject      *object,
                        guint         prop_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
  NdStream *self = ND_STREAM (object);

  switch (prop_id)
    {
    case PROP_SINK:
      if (!self->sink)
        {
          self->sink = g_value_dup_object (value);
          g_object_notify (G_OBJECT (self), "sink");
        }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static GstElement *
nd_stream_get_source (NdStream *self)
{
  g_autoptr(GVariant) stream_properties = NULL;
  g_autoptr(GError) error = NULL;
  GstElement *src = NULL;
  GVariant *streams = NULL;
  GVariantIter iter;
  gchar *uuid = NULL;
  guint32 node_id;
  guint32 screencast_type;

  g_debug ("NdStream: Getting a source");

  if (!self->session)
    g_error ("NdStream: XDP session not found!");

  streams = xdp_session_get_streams (self->session);
  if (streams == NULL)
    g_error ("NdStream: XDP session streams not found!");

  g_variant_iter_init (&iter, streams);
  g_variant_iter_loop (&iter, "(u@a{sv})", &node_id, &stream_properties);
  g_variant_lookup (stream_properties, "source_type", "u", &screencast_type);

  g_debug ("NdStream: Got a stream with node ID: %d", node_id);
  g_debug ("NdStream: Got a stream of type: %d", screencast_type);

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

  g_assert (ND_IS_SINK (self->sink));

  g_object_get (self->sink, "uuid", &uuid, NULL);

  src = gst_element_factory_make ("pipewiresrc", g_strdup_printf ("portal-pipewire-source-%.8s", uuid));
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

void
session_closed_cb (NdStream * self, NdSink * sink)
{
  g_debug ("NdStream: Session closed cb");

  if (self->sink)
    nd_sink_stop_stream (self->sink);

  g_clear_object (&self->session);
}

static GstElement *
sink_create_source_cb (NdStream * self, NdSink * sink)
{
  g_autoptr(GstCaps) caps = NULL;
  GstBin *bin;
  GstElement *src, *filter, *dst, *res;

  g_debug ("NdStream: Sink create source cb");

  bin = GST_BIN (gst_bin_new ("screencast source bin"));
  src = nd_stream_get_source (self);

  if (!src)
    g_error ("NdStream: Error creating video source element, likely a missing dependency!");

  gst_bin_add (bin, src);

  dst = gst_element_factory_make ("intervideosink", "inter video sink");
  if (!dst)
    g_error ("NdStream: Error creating intervideosink, missing dependency!");
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
sink_create_audio_source_cb (NdStream * self, NdSink * sink)
{
  GstElement *res;

  g_debug ("NdStream: Sink create audio source cb");

  if (!self->pulse)
    return NULL;

  res = nd_pulseaudio_get_source (self->pulse);

  return g_object_ref_sink (res);
}

static void
nd_stream_cleanup (GApplication *app)
{
  NdStream *self = ND_STREAM (app);

  g_debug ("NdStream: Cleanup");

  g_clear_object (&self->portal);
  if (self->session)
    {
      g_debug ("NdStream: Closing screencast session");
      xdp_session_close (self->session);
    }

  if (self->session)
    g_clear_object (&self->session);

  if (self->pulse)
    {
      g_debug ("NdStream: Unloading PulseAudio client");
      nd_pulseaudio_unload (self->pulse);
      g_clear_object (&self->pulse);
    }

  g_clear_object (&self->sink);

  if (self->sigterm_source)
    {
      g_source_destroy (self->sigterm_source);
      g_clear_pointer (&self->sigterm_source, g_source_unref);
    }

  if (self->sigint_source)
    {
      g_source_destroy (self->sigint_source);
      g_clear_pointer (&self->sigint_source, g_source_unref);
    }

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
}

static void
nd_stream_startup (GApplication *app)
{
  g_debug ("NdStream: Startup");
  /* Run indefinitely, until told to exit. */
  g_application_hold (app);

  G_APPLICATION_CLASS (nd_stream_parent_class)->startup (app);
}

static void
nd_stream_shutdown (GApplication *app)
{
  nd_stream_cleanup (app);

  g_debug ("NdStream: Shutdown");
  G_APPLICATION_CLASS (nd_stream_parent_class)->shutdown (app);
  /* Stop running */

  g_debug ("NdStream: Release");
  g_application_release (app);
}

static void
nd_stream_class_init (NdStreamClass *klass)
{
  GApplicationClass *g_application_class = G_APPLICATION_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  g_application_class->startup = nd_stream_startup;
  g_application_class->shutdown = nd_stream_shutdown;

  object_class->set_property = nd_stream_set_property;
  object_class->get_property = nd_stream_get_property;

  props[PROP_SINK] =
    g_param_spec_object ("sink", "The stream sink",
                         "The sink used to stream, usually not a MetaSink",
                         ND_TYPE_SINK,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, props);
}

static void
sink_notify_state_cb (NdStream *self, GParamSpec *pspec, NdSink *sink)
{
  NdSinkState state;

  g_object_get (sink, "state", &state, NULL);
  g_debug ("NdStream: State changed to %s",
           g_enum_to_string (ND_TYPE_SINK_STATE, state));

  switch (state)
    {
    case ND_SINK_STATE_ENSURE_FIREWALL:
      g_debug ("NdStream: Checking and installing required firewall zones.");
      break;

    case ND_SINK_STATE_WAIT_P2P:
      g_debug ("NdStream: Waiting for P2P connection");
      break;

    case ND_SINK_STATE_WAIT_SOCKET:
      g_debug ("NdStream: Establishing connection to sink");
      break;

    case ND_SINK_STATE_WAIT_STREAMING:
      g_debug ("NdStream: Starting to stream");
      break;

    case ND_SINK_STATE_STREAMING:
      g_debug ("NdStream: Streaming");
      break;

    case ND_SINK_STATE_ERROR:
      g_warning ("NdStream: Sink error");

    case ND_SINK_STATE_DISCONNECTED:
      g_debug ("NdStream: Sink disconnected");

      /* Quit the application */
      g_debug ("NdStream: Quitting");
      g_application_quit (G_APPLICATION (self));

      break;
    }
}

static void
nd_screencast_started_cb (GObject      *source_object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  XdpSession *session = XDP_SESSION (source_object);
  NdStream *self = ND_STREAM (user_data);

  g_debug ("NdStream: Screencast started cb");

  if (!xdp_session_start_finish (session, result, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("NdStream: Error initializing screencast portal: %s", error->message);

          /* Unknown method means the portal does not exist, give a slightly
           * more specific warning then.
           */
          if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
            g_warning ("NdStream: Screencasting portal is unavailable! It is required to select the monitor to stream!");
        }

      g_warning ("NdStream: Failed to start screencast session: %s\nNdStream: Quitting...", error->message);
      g_application_quit (G_APPLICATION (self));
      return;
    }

  g_debug ("NdStream: Created screencast session");
  g_signal_connect_object (self->session,
                           "closed",
                           (GCallback) session_closed_cb,
                           self,
                           G_CONNECT_SWAPPED);

  /* change pointer from meta_sink to current_sink */
  self->sink = nd_sink_start_stream (self->sink);

  if (!self->sink)
    {
      g_warning ("NdStream: Could not start streaming! Quitting...");
      g_application_quit (G_APPLICATION (self));
      return;
    }

  g_signal_connect_object (self->sink,
                           "create-source",
                           (GCallback) sink_create_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->sink,
                           "create-audio-source",
                           (GCallback) sink_create_audio_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->sink,
                           "notify::state",
                           (GCallback) sink_notify_state_cb,
                           self,
                           G_CONNECT_SWAPPED);

  /* We might have moved into the error state in the meantime. */
  sink_notify_state_cb (self, NULL, self->sink);
}

static void
nd_pulseaudio_init_async_cb (GObject      *source_object,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  NdStream *self;

  g_debug ("NdStream: Pulseaudio init async cb");

  if (!g_async_initable_init_finish (G_ASYNC_INITABLE (source_object), res, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("NdStream: Error initializing pulse audio sink: %s", error->message);

      g_object_unref (source_object);
      return;
    }

  self = ND_STREAM (user_data);
  self->pulse = ND_PULSEAUDIO (source_object);
}

static void
nd_screencast_init_cb (GObject      *source_object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  XdpPortal *portal = XDP_PORTAL (source_object);
  NdStream *self = ND_STREAM (user_data);
  NdSink *sink = ND_SINK (self->sink);
  gchar *name = NULL;
  gchar *uuid = NULL;

  g_debug ("NdStream: Screencast init cb");

  g_assert (ND_IS_SINK (sink));

  self->portal = portal;
  self->session = xdp_portal_create_screencast_session_finish (self->portal, result, &error);
  if (self->session == NULL)
    {
      g_warning ("NdStream: Failed to create screencast session: %s", error->message);
      g_application_quit (G_APPLICATION (self));
      return;
    }

  xdp_session_start (self->session, NULL, NULL, nd_screencast_started_cb, self);

  g_object_get (sink, "display-name", &name, NULL);
  g_object_get (sink, "uuid", &uuid, NULL);

  self->pulse = nd_pulseaudio_new (g_strdup (name), g_strdup (uuid));
  g_async_initable_init_async (G_ASYNC_INITABLE (self->pulse),
                               G_PRIORITY_LOW,
                               self->cancellable,
                               nd_pulseaudio_init_async_cb,
                               self);
}


static void
nd_stream_init (NdStream *self)
{
  g_autoptr(GError) error = NULL;
  self->cancellable = g_cancellable_new ();

  g_debug ("GNOME Network Displays Stream v%s started", PACKAGE_VERSION);

  self->portal = xdp_portal_initable_new (&error);
  if (error)
    {
      g_warning ("NdStream: Failed to create screencast portal: %s", error->message);
      g_application_quit (G_APPLICATION (self));
    }

  if (!self->portal)
    {
      g_debug ("NdStream: Couldn't aquire portal. Quitting...");
      g_application_quit (G_APPLICATION (self));
      return;
    }

  xdp_portal_create_screencast_session (self->portal,
                                        XDP_OUTPUT_MONITOR | XDP_OUTPUT_WINDOW | XDP_OUTPUT_VIRTUAL,
                                        XDP_SCREENCAST_FLAG_NONE,
                                        XDP_CURSOR_MODE_EMBEDDED,
                                        XDP_PERSIST_MODE_NONE,
                                        NULL,
                                        self->cancellable,
                                        nd_screencast_init_cb,
                                        self);
  g_debug ("NdStream: Got a portal");
}

static gboolean
on_signal (NdStream *self, GSource *source, const char *signal_name)
{
  g_debug ("NdStream: Received %s signal. Exiting...", signal_name);

  nd_sink_stop_stream (self->sink);

  return G_SOURCE_REMOVE;
}

static gboolean
on_sigterm (gpointer user_data)
{
  NdStream *self = ND_STREAM (user_data);

  return on_signal (self, self->sigterm_source, "SIGTERM");
}

static gboolean
on_sigint (gpointer user_data)
{
  NdStream *self = ND_STREAM (user_data);

  return on_signal (self, self->sigint_source, "SIGINT");
}

void
nd_stream_register_unix_signals (NdStream *self)
{
  self->sigterm_source = g_unix_signal_source_new (SIGTERM);
  g_source_set_callback (self->sigterm_source, on_sigterm, self, NULL);
  g_source_attach (self->sigterm_source, NULL);

  self->sigint_source = g_unix_signal_source_new (SIGINT);
  g_source_set_callback (self->sigint_source, on_sigint, self, NULL);
  g_source_attach (self->sigint_source, NULL);
}

NdStream *
nd_stream_new ()
{
  NdStream *stream;
  gchar * uuid = g_uuid_string_random ();
  gchar * id = g_strdup_printf ("org.gnome.NetworkDisplays.Stream_%.8s", uuid);

  g_debug ("NdStream: Starting with app-id: %s", id);
  stream = g_object_new (ND_TYPE_STREAM,
                         "application-id", g_strdup (id),
                         "flags", G_APPLICATION_HANDLES_OPEN,
                         NULL);

  if (!stream)
    {
      g_warning ("NdStream: Failed to construct NdStream object");
      return NULL;
    }

  nd_stream_register_unix_signals (stream);

  return stream;
}
