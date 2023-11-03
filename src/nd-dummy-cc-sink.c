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
#include "gnome-network-displays-config.h"
#include "nd-dummy-cc-sink.h"
#include "nd-enum-types.h"
#include "nd-uri-helpers.h"

struct _NdDummyCCSink
{
  GObject        parent_instance;

  NdSinkState    state;

  gchar         *uuid;

  GtkStringList *missing_video_codec;
  GtkStringList *missing_audio_codec;
  char          *missing_firewall_zone;

  CcHttpServer  *http_server;
};

enum {
  PROP_CLIENT = 1,
  PROP_DEVICE,
  PROP_PEER,

  PROP_UUID,
  PROP_DISPLAY_NAME,
  PROP_MATCHES,
  PROP_PRIORITY,
  PROP_STATE,
  PROP_PROTOCOL,
  PROP_MISSING_VIDEO_CODEC,
  PROP_MISSING_AUDIO_CODEC,
  PROP_MISSING_FIREWALL_ZONE,

  PROP_LAST = PROP_UUID,
};

const static NdSinkProtocol protocol = ND_SINK_PROTOCOL_DUMMY_CC;

static void nd_dummy_cc_sink_sink_iface_init (NdSinkIface *iface);
static NdSink * nd_dummy_cc_sink_sink_start_stream (NdSink *sink);
static void nd_dummy_cc_sink_sink_stop_stream (NdSink *sink);
static gchar * nd_dummy_cc_sink_sink_to_uri (NdSink *sink);

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
    case PROP_UUID:
      g_value_set_string (value, sink->uuid);
      break;

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

    case PROP_PROTOCOL:
      g_value_set_enum (value, protocol);
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

  g_object_class_override_property (object_class, PROP_UUID, "uuid");
  g_object_class_override_property (object_class, PROP_DISPLAY_NAME, "display-name");
  g_object_class_override_property (object_class, PROP_MATCHES, "matches");
  g_object_class_override_property (object_class, PROP_PRIORITY, "priority");
  g_object_class_override_property (object_class, PROP_STATE, "state");
  g_object_class_override_property (object_class, PROP_PROTOCOL, "protocol");
  g_object_class_override_property (object_class, PROP_MISSING_VIDEO_CODEC, "missing-video-codec");
  g_object_class_override_property (object_class, PROP_MISSING_AUDIO_CODEC, "missing-audio-codec");
  g_object_class_override_property (object_class, PROP_MISSING_FIREWALL_ZONE, "missing-firewall-zone");
}

static void
nd_dummy_cc_sink_init (NdDummyCCSink *sink)
{
  sink->uuid = g_uuid_string_random ();
  sink->state = ND_SINK_STATE_DISCONNECTED;
}

static gchar *
nd_dummy_cc_sink_sink_to_uri (NdSink *sink)
{
  GHashTable *params = g_hash_table_new (g_str_hash, g_str_equal);

  /* protocol */
  g_hash_table_insert (params, "protocol", (gpointer *) g_strdup_printf ("%d", protocol));

  return nd_uri_helpers_generate_uri (params);
}

/******************************************************************
* NdSink interface implementation
******************************************************************/

static void
nd_dummy_cc_sink_sink_iface_init (NdSinkIface *iface)
{
  iface->start_stream = nd_dummy_cc_sink_sink_start_stream;
  iface->stop_stream = nd_dummy_cc_sink_sink_stop_stream;
  iface->to_uri = nd_dummy_cc_sink_sink_to_uri;
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
  guint port;

  if (!cc_http_server_start_server (self->http_server, &error))
    {
      g_warning ("Error starting http server: %s", error->message);
      self->state = ND_SINK_STATE_ERROR;
      g_object_notify (G_OBJECT (self), "state");
    }

  g_object_get (self->http_server, "port", &port, NULL);
  g_message ("NdDummyCCSink: You should now be able to connect to http://localhost:%d/", port);

  return G_SOURCE_REMOVE;
}

static NdSink *
nd_dummy_cc_sink_sink_start_stream (NdSink *sink)
{
  NdDummyCCSink *self = ND_DUMMY_CC_SINK (sink);
  gboolean have_cc_codecs;
  GStrv missing_video = NULL, missing_audio = NULL;

  g_return_val_if_fail (self->state == ND_SINK_STATE_DISCONNECTED, NULL);

  g_assert (self->http_server == NULL);
  self->http_server = cc_http_server_new ("dummy-sink");

  have_cc_codecs = cc_http_server_lookup_encoders (self->http_server,
                                                   &missing_video,
                                                   &missing_audio);
  g_clear_object (&self->missing_video_codec);
  g_clear_object (&self->missing_audio_codec);

  self->missing_video_codec = gtk_string_list_new ((const char *const *) missing_video);
  self->missing_audio_codec = gtk_string_list_new ((const char *const *) missing_audio);

  g_object_notify (G_OBJECT (self), "missing-video-codec");
  g_object_notify (G_OBJECT (self), "missing-audio-codec");

  if (!have_cc_codecs)
    {
      g_warning ("NdDummyCCSink: Essential codecs are missing!");
      goto error;
    }

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

  self->state = ND_SINK_STATE_WAIT_SOCKET;
  g_object_notify (G_OBJECT (self), "state");

  g_idle_add (G_SOURCE_FUNC (start_server), self);

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
      cc_http_server_finalize (G_OBJECT (self->http_server));
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

/**
 * nd_dummy_cc_sink_from_uri
 * @uri: a URI string
 *
 * Construct a #NdDummyCCSink using the information encoded in the URI string
 *
 * Returns: The newly constructed #NdDummyCCSink
 */
NdDummyCCSink *
nd_dummy_cc_sink_from_uri (gchar *uri)
{
  GHashTable *params = nd_uri_helpers_parse_uri (uri);

  /* protocol */
  const gchar *protocol_in_uri_str = g_hash_table_lookup (params, "protocol");

  ;
  NdSinkProtocol protocol_in_uri = g_ascii_strtoll (protocol_in_uri_str, NULL, 10);
  if (protocol != protocol_in_uri)
    {
      g_warning ("NdDummyCCSink: Attempted to create sink whose protocol (%s) doesn't match the URI (%s)",
                 g_enum_to_string (ND_TYPE_SINK_PROTOCOL, protocol),
                 g_enum_to_string (ND_TYPE_SINK_PROTOCOL, protocol_in_uri));
      return NULL;
    }

  return nd_dummy_cc_sink_new ();
}
