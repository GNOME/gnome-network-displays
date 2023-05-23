/* nd-cc-sink.c
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

#include "gnome-network-displays-config.h"
#include "nd-cc-sink.h"
#include "wfd/wfd-client.h"
#include "wfd/wfd-media-factory.h"
#include "cc/cc-ctrl.h"
#include "cc/cc-common.h"

struct _NdCCSink
{
  GObject        parent_instance;

  NdSinkState    state;

  GCancellable  *cancellable;

  GStrv          missing_video_codec;
  GStrv          missing_audio_codec;
  char          *missing_firewall_zone;

  gchar         *remote_address;
  gchar         *remote_name;

  GSocketClient *client;

  CcCtrl         ctrl;
  CcHttpServer  *http_server;
};

enum {
  PROP_CLIENT = 1,
  PROP_NAME,
  PROP_ADDRESS,

  PROP_DISPLAY_NAME,
  PROP_MATCHES,
  PROP_PRIORITY,
  PROP_STATE,
  PROP_MISSING_VIDEO_CODEC,
  PROP_MISSING_AUDIO_CODEC,
  PROP_MISSING_FIREWALL_ZONE,

  PROP_LAST = PROP_DISPLAY_NAME,
};

/* interface related functions */
static void nd_cc_sink_sink_iface_init (NdSinkIface *iface);
static NdSink * nd_cc_sink_sink_start_stream (NdSink *sink);
static void nd_cc_sink_sink_stop_stream (NdSink *sink);

static void nd_cc_sink_sink_stop_stream_int (NdCCSink *self);

G_DEFINE_TYPE_EXTENDED (NdCCSink, nd_cc_sink, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (ND_TYPE_SINK,
                                               nd_cc_sink_sink_iface_init);
                       )

static GParamSpec * props[PROP_LAST] = { NULL, };

static void
nd_cc_sink_get_property (GObject    * object,
                         guint        prop_id,
                         GValue     * value,
                         GParamSpec * pspec)
{
  NdCCSink *self = ND_CC_SINK (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      g_value_set_object (value, self->client);
      break;

    case PROP_NAME:
      g_value_set_string (value, self->remote_name);
      break;

    case PROP_ADDRESS:
      g_value_set_string (value, self->remote_address);
      break;

    case PROP_DISPLAY_NAME:
      g_object_get_property (G_OBJECT (self), "name", value);
      break;

    case PROP_MATCHES:
      {
        g_autoptr(GPtrArray) res = NULL;
        res = g_ptr_array_new_with_free_func (g_free);

        if (self->remote_name)
          g_ptr_array_add (res, g_strdup (self->remote_name));

        g_value_take_boxed (value, g_steal_pointer (&res));
        break;
      }

    case PROP_PRIORITY:
      g_value_set_int (value, 100);
      break;

    case PROP_STATE:
      g_value_set_enum (value, self->state);
      break;

    case PROP_MISSING_VIDEO_CODEC:
      g_value_set_boxed (value, self->missing_video_codec);
      break;

    case PROP_MISSING_AUDIO_CODEC:
      g_value_set_boxed (value, self->missing_audio_codec);
      break;

    case PROP_MISSING_FIREWALL_ZONE:
      g_value_set_string (value, self->missing_firewall_zone);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_cc_sink_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  NdCCSink *self = ND_CC_SINK (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      /* Construct only */
      self->client = g_value_dup_object (value);
      break;

    case PROP_NAME:
      self->remote_name = g_value_dup_string (value);
      g_object_notify (G_OBJECT (self), "display-name");
      break;

    case PROP_ADDRESS:
      g_assert (self->remote_address == NULL);
      self->remote_address = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

void
nd_cc_sink_finalize (GObject *object)
{
  NdCCSink *self = ND_CC_SINK (object);

  g_debug ("NdCCSink: Finalizing");

  nd_cc_sink_sink_stop_stream_int (self);

  g_clear_pointer (&self->missing_video_codec, g_strfreev);
  g_clear_pointer (&self->missing_audio_codec, g_strfreev);
  g_clear_pointer (&self->missing_firewall_zone, g_free);

  g_clear_pointer (&self->remote_address, g_free);
  g_clear_pointer (&self->remote_name, g_free);

  g_clear_object (&self->client);

  G_OBJECT_CLASS (nd_cc_sink_parent_class)->finalize (object);
}

static void
stream_started_callback (gpointer userdata)
{
  NdCCSink *self = (NdCCSink *) userdata;

  self->state = ND_SINK_STATE_STREAMING;
  g_object_notify (G_OBJECT (self), "state");
}

/* TODO: show an error message to user */
static void
end_stream_callback (gpointer userdata, GError *error)
{
  g_debug ("NdCCSink: Error received: %s", error->message);
  g_clear_error (&error);

  nd_cc_sink_sink_stop_stream (ND_SINK (userdata));
}

static void
nd_cc_sink_sink_stop_stream_int (NdCCSink *self)
{
  cc_ctrl_finish (&self->ctrl);

  if (self->http_server)
    {
      cc_http_server_finalize (self->http_server);
      self->http_server = NULL;
    }
}

static void
nd_cc_sink_sink_stop_stream (NdSink *sink)
{
  NdCCSink *self = ND_CC_SINK (sink);

  nd_cc_sink_sink_stop_stream_int (self);

  self->state = ND_SINK_STATE_DISCONNECTED;
  g_object_notify (G_OBJECT (self), "state");
}

static void
nd_cc_sink_class_init (NdCCSinkClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = nd_cc_sink_get_property;
  object_class->set_property = nd_cc_sink_set_property;
  object_class->finalize = nd_cc_sink_finalize;

  props[PROP_CLIENT] =
    g_param_spec_object ("client", "Communication Client",
                         "Unused client",
                         G_TYPE_SOCKET_CLIENT,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  props[PROP_NAME] =
    g_param_spec_string ("name", "Sink Name",
                         "The sink name found by the Avahi Client.",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  props[PROP_ADDRESS] =
    g_param_spec_string ("address", "Sink Address",
                         "The address the sink was found on.",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, props);

  g_object_class_override_property (object_class, PROP_DISPLAY_NAME, "display-name");
  g_object_class_override_property (object_class, PROP_MATCHES, "matches");
  g_object_class_override_property (object_class, PROP_PRIORITY, "priority");
  g_object_class_override_property (object_class, PROP_STATE, "state");
  g_object_class_override_property (object_class, PROP_MISSING_VIDEO_CODEC, "missing-video-codec");
  g_object_class_override_property (object_class, PROP_MISSING_AUDIO_CODEC, "missing-audio-codec");
  g_object_class_override_property (object_class, PROP_MISSING_FIREWALL_ZONE, "missing-firewall-zone");
}

static void
nd_cc_sink_init (NdCCSink *self)
{
  CcCtrlClosure *closure;

  self->state = ND_SINK_STATE_DISCONNECTED;
  self->ctrl.state = CC_CTRL_STATE_DISCONNECTED;

  closure = (CcCtrlClosure *) g_malloc (sizeof (CcCtrlClosure));
  closure->userdata = self;
  closure->end_stream = end_stream_callback;
  self->ctrl.closure = closure;

  srand (time (NULL));
}

/******************************************************************
* NdSink interface implementation
******************************************************************/

static void
nd_cc_sink_sink_iface_init (NdSinkIface *iface)
{
  iface->start_stream = nd_cc_sink_sink_start_stream;
  iface->stop_stream = nd_cc_sink_sink_stop_stream;
}

static GstElement *
server_create_source_cb (NdCCSink *self, CcHttpServer *server)
{
  GstElement *res;

  g_signal_emit_by_name (self, "create-source", &res);
  g_debug ("NdCCSink: Create source signal emitted");

  return res;
}

static GstElement *
server_create_audio_source_cb (NdCCSink *self, CcHttpServer *server)
{
  GstElement *res;

  g_signal_emit_by_name (self, "create-audio-source", &res);
  g_debug ("NdCCSink: Create audio source signal emitted");

  return res;
}

static NdSink *
nd_cc_sink_sink_start_stream (NdSink *sink)
{
  NdCCSink *self = ND_CC_SINK (sink);
  gboolean have_basic_codecs;
  GStrv missing_video, missing_audio;

  g_autoptr(GError) error = NULL;

  g_return_val_if_fail (self->state == ND_SINK_STATE_DISCONNECTED, NULL);

  // TODO: use the cc version of this function
  have_basic_codecs = wfd_get_missing_codecs (&missing_video, &missing_audio);

  g_clear_pointer (&self->missing_video_codec, g_strfreev);
  g_clear_pointer (&self->missing_audio_codec, g_strfreev);

  self->missing_video_codec = g_strdupv (missing_video);
  self->missing_audio_codec = g_strdupv (missing_audio);

  g_object_notify (G_OBJECT (self), "missing-video-codec");
  g_object_notify (G_OBJECT (self), "missing-audio-codec");

  if (!have_basic_codecs)
    {
      g_warning ("NdCCSink: Basic codecs missing");
      goto error;
    }

  self->state = ND_SINK_STATE_WAIT_SOCKET;
  g_object_notify (G_OBJECT (self), "state");

  self->cancellable = g_cancellable_new ();
  self->ctrl.cancellable = self->cancellable;
  self->ctrl.comm.cancellable = self->cancellable;

  g_debug ("NdCCSink: Attempting connection to Chromecast: %s", self->remote_name);
  if (!cc_ctrl_connection_init (&self->ctrl, self->remote_address))
    {
      g_warning ("NdCCSink: Failed to init cc-ctrl");
      goto error;
    }

  self->http_server = cc_http_server_new ();
  cc_http_server_set_remote_address (self->http_server, self->remote_address);

  /* copy the pointer to ctrl */
  self->ctrl.http_server = self->http_server;

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

  return g_object_ref (sink);

error:
  g_warning ("Error starting stream!");
  self->state = ND_SINK_STATE_ERROR;
  g_object_notify (G_OBJECT (self), "state");

  return NULL;
}

/******************************************************************
* NdCCSink public functions
******************************************************************/

/* XXX: no use for client */
NdCCSink *
nd_cc_sink_new (GSocketClient *client,
                gchar         *name,
                gchar         *remote_address)
{
  return g_object_new (ND_TYPE_CC_SINK,
                       "client", client,
                       "name", name,
                       "address", remote_address,
                       NULL);
}

NdSinkState
nd_cc_sink_get_state (NdCCSink *self)
{
  return self->state;
}
