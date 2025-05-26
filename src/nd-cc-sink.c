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

#include "cc/cc-ctrl.h"
#include "cc/cc-common.h"
#include "cc/cc-http-server.h"
#include "gnome-network-displays-config.h"
#include "nd-cc-sink.h"
#include "nd-enum-types.h"
#include "nd-uri-helpers.h"

struct _NdCCSink
{
  GObject        parent_instance;

  NdSinkState    state;

  GCancellable  *cancellable;

  GtkStringList *missing_video_codec;
  GtkStringList *missing_audio_codec;
  char          *missing_firewall_zone;

  gchar         *uuid;
  gchar         *ip;
  gchar         *name;
  gchar         *display_name;
  gint           interface;

  GSocketClient *client;

  CcCtrl         ctrl;
  CcHttpServer  *http_server;
};

enum {
  PROP_CLIENT = 1,
  PROP_NAME,
  PROP_IP,
  PROP_DISPLAY_NAME,
  PROP_INTERFACE,

  PROP_UUID,
  PROP_MATCHES,
  PROP_PRIORITY,
  PROP_STATE,
  PROP_PROTOCOL,
  PROP_MISSING_VIDEO_CODEC,
  PROP_MISSING_AUDIO_CODEC,
  PROP_MISSING_FIREWALL_ZONE,

  PROP_LAST = PROP_UUID,
};

const static NdSinkProtocol protocol = ND_SINK_PROTOCOL_CC;

/* interface related functions */
static void nd_cc_sink_sink_iface_init (NdSinkIface *iface);
static NdSink * nd_cc_sink_sink_start_stream (NdSink *sink);
static void nd_cc_sink_sink_stop_stream (NdSink *sink);
static gchar * nd_cc_sink_sink_to_uri (NdSink *sink);

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
      g_value_set_string (value, self->name);
      break;

    case PROP_IP:
      g_value_set_string (value, self->ip);
      break;

    case PROP_UUID:
      g_value_set_string (value, self->uuid);
      break;

    case PROP_DISPLAY_NAME:
      g_value_set_string (value, self->display_name);
      break;

    case PROP_INTERFACE:
      g_value_set_int (value, self->interface);
      break;

    case PROP_MATCHES:
      {
        g_autoptr(GPtrArray) res = NULL;
        res = g_ptr_array_new_with_free_func (g_free);

        if (self->ip)
          {
            g_debug ("NdCCSink: Adding IP %s to match list", self->ip);
            g_ptr_array_add (res, g_strdup (self->ip));
          }

        g_value_take_boxed (value, g_steal_pointer (&res));
        break;
      }

    case PROP_PRIORITY:
      g_value_set_int (value, 50);
      break;

    case PROP_STATE:
      g_value_set_enum (value, self->state);
      break;

    case PROP_PROTOCOL:
      g_value_set_enum (value, protocol);
      break;

    case PROP_MISSING_VIDEO_CODEC:
      g_value_set_object (value, self->missing_video_codec);
      break;

    case PROP_MISSING_AUDIO_CODEC:
      g_value_set_object (value, self->missing_audio_codec);
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
      self->name = g_value_dup_string (value);
      break;

    case PROP_IP:
      g_assert (self->ip == NULL);
      self->ip = g_value_dup_string (value);
      break;

    case PROP_DISPLAY_NAME:
      g_assert (self->display_name == NULL);
      self->display_name = g_value_dup_string (value);
      break;

    case PROP_INTERFACE:
      self->interface = g_value_get_int (value);
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

  g_clear_object (&self->missing_video_codec);
  g_clear_object (&self->missing_audio_codec);
  g_clear_pointer (&self->missing_firewall_zone, g_free);

  g_clear_pointer (&self->ip, g_free);
  g_clear_pointer (&self->name, g_free);

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

  nd_cc_sink_sink_stop_stream (ND_SINK (userdata));
}

static void
nd_cc_sink_sink_stop_stream_int (NdCCSink *self)
{
  cc_ctrl_finish (&self->ctrl);

  if (self->http_server)
    {
      cc_http_server_finalize (G_OBJECT (self->http_server));
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

  props[PROP_IP] =
    g_param_spec_string ("ip", "Sink IP Address",
                         "The IP address the sink was found on.",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  props[PROP_DISPLAY_NAME] =
    g_param_spec_string ("display-name", "Sink Display Name",
                         "The human-readable display name of the sink",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  props[PROP_INTERFACE] =
    g_param_spec_int ("interface", "Network Interface",
                      "The network interface Avahi discovered this entry on",
                      0, G_MAXINT, 0,
                      G_PARAM_READWRITE);

  g_object_class_install_properties (object_class, PROP_LAST, props);

  g_object_class_override_property (object_class, PROP_UUID, "uuid");
  g_object_class_override_property (object_class, PROP_MATCHES, "matches");
  g_object_class_override_property (object_class, PROP_PRIORITY, "priority");
  g_object_class_override_property (object_class, PROP_STATE, "state");
  g_object_class_override_property (object_class, PROP_PROTOCOL, "protocol");
  g_object_class_override_property (object_class, PROP_MISSING_VIDEO_CODEC, "missing-video-codec");
  g_object_class_override_property (object_class, PROP_MISSING_AUDIO_CODEC, "missing-audio-codec");
  g_object_class_override_property (object_class, PROP_MISSING_FIREWALL_ZONE, "missing-firewall-zone");
}

static void
nd_cc_sink_init (NdCCSink *self)
{
  CcCtrlClosure *closure;

  self->uuid = g_uuid_string_random ();
  self->state = ND_SINK_STATE_DISCONNECTED;
  self->interface = 0;
  self->ctrl.state = CC_CTRL_STATE_DISCONNECTED;

  closure = (CcCtrlClosure *) g_malloc (sizeof (CcCtrlClosure));
  closure->userdata = self;
  closure->end_stream = end_stream_callback;
  self->ctrl.closure = closure;

  srand (time (NULL));
}

static gchar *
nd_cc_sink_sink_to_uri (NdSink *sink)
{
  NdCCSink *self = ND_CC_SINK (sink);
  GHashTable *params = g_hash_table_new (g_str_hash, g_str_equal);

  /* protocol */
  g_hash_table_insert (params, "protocol", (gpointer *) g_strdup_printf ("%d", protocol));

  /* remote name */
  g_hash_table_insert (params, "name", (gpointer *) g_strdup (self->name));

  /* remote address */
  g_hash_table_insert (params, "ip", (gpointer *) g_strdup (self->ip));

  return nd_uri_helpers_generate_uri (params);
}

/******************************************************************
* NdSink interface implementation
******************************************************************/

static void
nd_cc_sink_sink_iface_init (NdSinkIface *iface)
{
  iface->start_stream = nd_cc_sink_sink_start_stream;
  iface->stop_stream = nd_cc_sink_sink_stop_stream;
  iface->to_uri = nd_cc_sink_sink_to_uri;
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
  gboolean have_cc_codecs;
  GStrv missing_video = NULL, missing_audio = NULL;

  g_return_val_if_fail (self->state == ND_SINK_STATE_DISCONNECTED, NULL);

  g_assert (self->http_server == NULL);
  self->http_server = cc_http_server_new (self->ip);

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
      g_warning ("NdCCSink: Essential codecs are missing!");
      goto error;
    }

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

  self->state = ND_SINK_STATE_WAIT_SOCKET;
  g_object_notify (G_OBJECT (self), "state");

  self->cancellable = g_cancellable_new ();
  self->ctrl.cancellable = self->cancellable;
  self->ctrl.comm.cancellable = self->cancellable;

  g_debug ("NdCCSink: Attempting connection to Chromecast %s at IP %s", self->name, self->ip);
  if (!cc_ctrl_connection_init (&self->ctrl, self->ip))
    {
      g_warning ("NdCCSink: Failed to init cc-ctrl");
      goto error;
    }

  return g_object_ref (sink);

error:
  g_warning ("NdCCSink: Error starting screencast!");
  self->state = ND_SINK_STATE_ERROR;
  g_object_notify (G_OBJECT (self), "state");

  return g_object_ref (sink);
}

/******************************************************************
* NdCCSink public functions
******************************************************************/

/* XXX: no use for client */
NdCCSink *
nd_cc_sink_new (GSocketClient *client,
                gchar         *name,
                gchar         *ip,
                gchar         *display_name,
                gint           interface)
{
  return g_object_new (ND_TYPE_CC_SINK,
                       "client", client,
                       "name", name,
                       "ip", ip,
                       "display-name", display_name,
                       "interface", interface,
                       NULL);
}

/**
 * nd_cc_sink_from_uri
 * @uri: a URI string
 *
 * Construct a #NdCCSink using the information encoded in the URI string
 *
 * Returns: The newly constructed #NdCCSink or #NULL if failed
 */
NdCCSink *
nd_cc_sink_from_uri (gchar *uri)
{
  GHashTable *params = nd_uri_helpers_parse_uri (uri);

  /* protocol */
  const gchar *protocol_in_uri_str = g_hash_table_lookup (params, "protocol");

  ;
  NdSinkProtocol protocol_in_uri = g_ascii_strtoll (protocol_in_uri_str, NULL, 10);
  if (protocol != protocol_in_uri)
    {
      g_warning ("NdCCSink: Attempted to create sink whose protocol (%s) doesn't match the URI (%s)",
                 g_enum_to_string (ND_TYPE_SINK_PROTOCOL, protocol),
                 g_enum_to_string (ND_TYPE_SINK_PROTOCOL, protocol_in_uri));
      return NULL;
    }

  /* client */
  GSocketClient *client = g_socket_client_new ();
  if (!client)
    {
      g_warning ("NdCCSink: Failed to instantiate GSocketClient");
      return NULL;
    }

  /* remote name */
  gchar *name = g_hash_table_lookup (params, "name");
  if (!name)
    {
      g_warning ("NdCCSink: Failed to find remote name in the URI %s", uri);
      return NULL;
    }

  /* remote ip */
  gchar *ip = g_hash_table_lookup (params, "ip");
  if (!ip)
    {
      g_warning ("NdCCSink: Failed to find remote IP address in the URI %s", uri);
      return NULL;
    }

  return nd_cc_sink_new (client, name, ip, NULL, 0);
}
