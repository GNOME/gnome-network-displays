/* nd-dummy-cc-sink.c
 *
 * Copyright 2018 Benjamin Berg <bberg@redhat.com>
 * Copyright 2023 Anupam Kumar <kyteinsky@gmail.com>
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

#include "cc/cc-http-server.h"
#include "wfd/wfd-media-factory.h"
#include "gnome-network-displays-config.h"
#include "nd-dummy-cc-sink.h"

struct _NdDummyCCSink
{
  GObject        parent_instance;

  NdSinkState    state;

  GtkStringList *missing_video_codec;
  GtkStringList *missing_audio_codec;
  char          *missing_firewall_zone;

  CcHttpServer  *http_server;
};

enum {
  PROP_CLIENT = 1,
  PROP_DEVICE,
  PROP_PEER,

  PROP_DISPLAY_NAME,
  PROP_MATCHES,
  PROP_PRIORITY,
  PROP_STATE,
  PROP_MISSING_VIDEO_CODEC,
  PROP_MISSING_AUDIO_CODEC,
  PROP_MISSING_FIREWALL_ZONE,

  PROP_LAST = PROP_DISPLAY_NAME,
};

static void nd_dummy_cc_sink_sink_iface_init (NdSinkIface *iface);
static NdSink * nd_dummy_cc_sink_sink_start_stream (NdSink *sink);
static void nd_dummy_cc_sink_sink_stop_stream (NdSink *sink);

G_DEFINE_TYPE_EXTENDED (NdDummyCCSink, nd_dummy_cc_sink, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (ND_TYPE_SINK,
                                               nd_dummy_cc_sink_sink_iface_init);
                       )


static void
nd_dummy_cc_sink_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  NdDummyCCSink *sink = ND_DUMMY_CC_SINK (object);

  switch (prop_id)
    {
    case PROP_DISPLAY_NAME:
      g_value_set_string (value, "Dummy ChromeCast Sink");
      break;

    case PROP_MATCHES:
      {
        g_autoptr(GPtrArray) res = NULL;
        res = g_ptr_array_new_with_free_func (g_free);

        g_ptr_array_add (res, g_strdup ("dummy-cc-sink"));

        g_value_take_boxed (value, g_steal_pointer (&res));
        break;
      }

    case PROP_PRIORITY:
      g_value_set_int (value, 0);
      break;

    case PROP_STATE:
      g_value_set_enum (value, sink->state);
      break;

    case PROP_MISSING_VIDEO_CODEC:
      g_value_set_object (value, sink->missing_video_codec);
      break;

    case PROP_MISSING_AUDIO_CODEC:
      g_value_set_object (value, sink->missing_audio_codec);
      break;

    case PROP_MISSING_FIREWALL_ZONE:
      g_value_set_static_string (value, NULL);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

void
nd_dummy_cc_sink_finalize (GObject *object)
{
  NdDummyCCSink *self = ND_DUMMY_CC_SINK (object);

  nd_dummy_cc_sink_sink_stop_stream (ND_SINK (object));

  g_clear_object (&self->missing_video_codec);
  g_clear_object (&self->missing_audio_codec);
  g_clear_pointer (&self->missing_firewall_zone, g_free);

  G_OBJECT_CLASS (nd_dummy_cc_sink_parent_class)->finalize (object);
}

static void
nd_dummy_cc_sink_class_init (NdDummyCCSinkClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = nd_dummy_cc_sink_get_property;
  object_class->finalize = nd_dummy_cc_sink_finalize;

  g_object_class_override_property (object_class, PROP_DISPLAY_NAME, "display-name");
  g_object_class_override_property (object_class, PROP_MATCHES, "matches");
  g_object_class_override_property (object_class, PROP_PRIORITY, "priority");
  g_object_class_override_property (object_class, PROP_STATE, "state");
  g_object_class_override_property (object_class, PROP_MISSING_VIDEO_CODEC, "missing-video-codec");
  g_object_class_override_property (object_class, PROP_MISSING_AUDIO_CODEC, "missing-audio-codec");
  g_object_class_override_property (object_class, PROP_MISSING_FIREWALL_ZONE, "missing-firewall-zone");
}

static void
nd_dummy_cc_sink_init (NdDummyCCSink *sink)
{
  sink->state = ND_SINK_STATE_DISCONNECTED;
}

/******************************************************************
* NdSink interface implementation
******************************************************************/

static void
nd_dummy_cc_sink_sink_iface_init (NdSinkIface *iface)
{
  iface->start_stream = nd_dummy_cc_sink_sink_start_stream;
  iface->stop_stream = nd_dummy_cc_sink_sink_stop_stream;
}

static void
stream_started_callback (gpointer userdata)
{
  NdDummyCCSink *self = ND_DUMMY_CC_SINK (userdata);

  self->state = ND_SINK_STATE_STREAMING;
  g_object_notify (G_OBJECT (self), "state");
}

static void
end_stream_callback (gpointer userdata, GError *error)
{
  g_debug ("NdDummyCCSink: Error received: %s", error->message);

  nd_dummy_cc_sink_sink_stop_stream (ND_SINK (userdata));
}

static GstElement *
server_create_source_cb (NdDummyCCSink *sink, CcHttpServer *server)
{
  GstElement *res;

  g_signal_emit_by_name (sink, "create-source", &res);

  return res;
}

static GstElement *
server_create_audio_source_cb (NdDummyCCSink *sink, CcHttpServer *server)
{
  GstElement *res;

  g_signal_emit_by_name (sink, "create-audio-source", &res);

  return res;
}

static gboolean
start_server (NdDummyCCSink *self)
{
  g_autoptr(GError) error = NULL;

  if (!cc_http_server_start_server (self->http_server, &error))
    {
      g_warning ("Error starting http server: %s", error->message);
      self->state = ND_SINK_STATE_ERROR;
      g_object_notify (G_OBJECT (self), "state");
    }

  g_debug ("NdDummyCCSink: You should now be able to connect to http://localhost:%d/",
           cc_http_server_get_port (self->http_server));

  return G_SOURCE_REMOVE;
}

static NdSink *
nd_dummy_cc_sink_sink_start_stream (NdSink *sink)
{
  NdDummyCCSink *self = ND_DUMMY_CC_SINK (sink);
  gboolean have_basic_codecs;
  GStrv missing_video, missing_audio;

  g_return_val_if_fail (self->state == ND_SINK_STATE_DISCONNECTED, NULL);

  have_basic_codecs = wfd_get_missing_codecs (&missing_video, &missing_audio);

  g_clear_object (&self->missing_video_codec);
  g_clear_object (&self->missing_audio_codec);

  self->missing_video_codec = gtk_string_list_new ((const char *const *) missing_video);
  self->missing_audio_codec = gtk_string_list_new ((const char *const *) missing_audio);

  g_object_notify (G_OBJECT (self), "missing-video-codec");
  g_object_notify (G_OBJECT (self), "missing-audio-codec");

  if (!have_basic_codecs)
    goto error;

  self->http_server = cc_http_server_new ();
  cc_http_server_set_remote_address (self->http_server, "dummy-sink");

  g_signal_connect_object (self->http_server,
                           "create-source",
                           (GCallback) server_create_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->http_server,
                           "create-audio-source",
                           (GCallback) server_create_audio_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->http_server,
                           "stream-started",
                           (GCallback) stream_started_callback,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->http_server,
                           "end-stream",
                           (GCallback) end_stream_callback,
                           self,
                           G_CONNECT_SWAPPED);

  g_idle_add (G_SOURCE_FUNC (start_server), self);

  self->state = ND_SINK_STATE_WAIT_SOCKET;
  g_object_notify (G_OBJECT (self), "state");

  return g_object_ref (sink);

error:
  g_warning ("Error starting stream!");
  self->state = ND_SINK_STATE_ERROR;
  g_object_notify (G_OBJECT (self), "state");

  return g_object_ref (sink);
}

void
nd_dummy_cc_sink_sink_stop_stream (NdSink *sink)
{
  NdDummyCCSink *self = ND_DUMMY_CC_SINK (sink);

  if (self->http_server)
    {
      cc_http_server_finalize (self->http_server);
      self->http_server = NULL;
    }

  self->state = ND_SINK_STATE_DISCONNECTED;
  g_object_notify (G_OBJECT (self), "state");
}

NdDummyCCSink *
nd_dummy_cc_sink_new (void)
{
  return g_object_new (ND_TYPE_DUMMY_CC_SINK,
                       NULL);
}
