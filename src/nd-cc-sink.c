/* nd-cc-sink.c
 *
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
  GObject          parent_instance;

  NdSinkState      state;

  GCancellable    *cancellable;

  GStrv            missing_video_codec;
  GStrv            missing_audio_codec;
  char            *missing_firewall_zone;

  gchar           *remote_address;
  gchar           *remote_name;

  GSocketClient   *comm_client;
  CcCtrl           ctrl;

  WfdMediaFactory *factory;
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
  NdCCSink *sink = ND_CC_SINK (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      g_value_set_object (value, sink->comm_client);
      break;

    case PROP_NAME:
      g_value_set_string (value, sink->remote_name);
      break;

    case PROP_ADDRESS:
      g_value_set_string (value, sink->remote_address);
      break;

    case PROP_DISPLAY_NAME:
      g_object_get_property (G_OBJECT (sink), "name", value);
      break;

    case PROP_MATCHES:
      {
        g_autoptr(GPtrArray) res = NULL;
        res = g_ptr_array_new_with_free_func (g_free);

        if (sink->remote_name)
          g_ptr_array_add (res, g_strdup (sink->remote_name));

        g_value_take_boxed (value, g_steal_pointer (&res));
        break;
      }

    case PROP_PRIORITY:
      g_value_set_int (value, 100);
      break;

    case PROP_STATE:
      g_value_set_enum (value, sink->state);
      break;

    case PROP_MISSING_VIDEO_CODEC:
      g_value_set_boxed (value, sink->missing_video_codec);
      break;

    case PROP_MISSING_AUDIO_CODEC:
      g_value_set_boxed (value, sink->missing_audio_codec);
      break;

    case PROP_MISSING_FIREWALL_ZONE:
      g_value_set_string (value, sink->missing_firewall_zone);
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
  NdCCSink *sink = ND_CC_SINK (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      /* Construct only */
      sink->comm_client = g_value_dup_object (value);
      break;

    case PROP_NAME:
      sink->remote_name = g_value_dup_string (value);
      g_object_notify (G_OBJECT (sink), "display-name");
      break;

    case PROP_ADDRESS:
      g_assert (sink->remote_address == NULL);
      sink->remote_address = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

void
nd_cc_sink_finalize (GObject *object)
{
  NdCCSink *sink = ND_CC_SINK (object);

  g_debug ("NdCCSink: Finalizing");

  nd_cc_sink_sink_stop_stream_int (sink);

  g_clear_pointer (&sink->missing_video_codec, g_strfreev);
  g_clear_pointer (&sink->missing_audio_codec, g_strfreev);
  g_clear_pointer (&sink->missing_firewall_zone, g_free);

  g_clear_pointer (&sink->remote_address, g_free);
  g_clear_pointer (&sink->remote_name, g_free);

  g_clear_object (&sink->comm_client);

  G_OBJECT_CLASS (nd_cc_sink_parent_class)->finalize (object);
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
nd_cc_sink_init (NdCCSink *sink)
{
  sink->state = ND_SINK_STATE_DISCONNECTED;
  sink->cancellable = g_cancellable_new ();
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
server_create_source_cb (NdCCSink *sink, WfdMediaFactory *factory)
{
  GstElement *res;

  g_signal_emit_by_name (sink, "create-source", &res);
  g_debug ("NdCCSink: Create source signal emitted");
  return res;
}

static GstElement *
server_create_audio_source_cb (NdCCSink *sink, WfdMediaFactory *factory)
{
  GstElement *res;

  g_signal_emit_by_name (sink, "create-audio-source", &res);
  g_debug ("NdCCSink: Create audio source signal emitted");

  return res;
}

static void
nd_cc_sink_create_gst_elements (NdCCSink *sink, GstBin *bin)
{
  gboolean success = TRUE;
  GstElement *queue_video;
  GstElement *rtph264pay;
  GstElement *rtp_bin;
  GstElement *udp_sink;

  GstPad *src_pad;
  GstPad *sink_pad;

  queue_video = wfd_media_factory_create_video_element (sink->factory, bin);
  /* leave audio out for the time being */
  /* GstElement *queue_mpegmux_audio = wfd_media_factory_create_audio_element (sink->factory, bin); */

  rtph264pay = gst_element_factory_make ("rtph264pay", "rtph264pay");
  success &= gst_bin_add (bin, rtph264pay);
  g_object_set (rtph264pay,
                "ssrc", 2200,
                "seqnum-offset", (guint) 0,
                NULL);

  rtp_bin = gst_element_factory_make ( "gstrtpbin", "rtpbin" );
  success &= gst_bin_add (bin, rtp_bin);
  src_pad = gst_element_request_pad_simple (rtp_bin, "send_rtp_sink_0");

  udp_sink = gst_element_factory_make ("udpsink", "udpsink");
  success &= gst_bin_add (bin, udp_sink);
  g_object_set (udp_sink,
                // "host", sink->remote_address,
                "port", 5000,
                NULL);
  sink_pad = gst_element_get_static_pad (udp_sink, "sink");

  success &= gst_pad_link (src_pad, sink_pad) == GST_PAD_LINK_OK;
  success &= gst_element_link_many (queue_video, rtph264pay, rtp_bin, NULL);

  GST_DEBUG_BIN_TO_DOT_FILE (bin,
                             GST_DEBUG_GRAPH_SHOW_MEDIA_TYPE,
                             "nd-cc-bin");

  if (!success)
    g_error ("NdCCSink: Failed to create gst elements");

  g_debug ("NdCCSink: Created video element");
}

static void
nd_cc_sink_start_webrtc_stream (gpointer userdata)
{
  NdCCSink *sink = ND_CC_SINK (userdata);

  /* TODO */
  g_debug ("Received webrtc stream signal from ctrl");
}

static void
nd_cc_sink_error_in_ctrl (gpointer userdata)
{
  nd_cc_sink_sink_stop_stream (ND_SINK (userdata));
}

CcCtrlClosure *
nd_cc_sink_get_callback_closure (NdCCSink *sink)
{
  CcCtrlClosure *closure = (CcCtrlClosure *) g_malloc (sizeof (CcCtrlClosure));

  closure->userdata = sink;
  closure->start_stream = nd_cc_sink_start_webrtc_stream;
  closure->end_stream = nd_cc_sink_error_in_ctrl;
  return g_steal_pointer (&closure);
}

static NdSink *
nd_cc_sink_sink_start_stream (NdSink *sink)
{
  NdCCSink *self = ND_CC_SINK (sink);

  g_autoptr(GError) error = NULL;
  /* gchar six_digits[6]; */

  g_return_val_if_fail (self->state == ND_SINK_STATE_DISCONNECTED, NULL);

  g_assert (self->factory == NULL);

  self->state = ND_SINK_STATE_WAIT_SOCKET;
  g_object_notify (G_OBJECT (self), "state");

  self->ctrl.cancellable = self->cancellable;
  self->ctrl.closure = nd_cc_sink_get_callback_closure (self);

  /* FIXME */
  // g_debug ("NdCCSink: Attempting connection to Chromecast: %s", self->remote_name);
  // if (!cc_ctrl_connection_init (&self->ctrl, self->remote_address))
  //   {
  //     g_warning ("NdCCSink: Failed to init cc-ctrl");
  //     if (self->state != ND_SINK_STATE_ERROR)
  //       {
  //         self->state = ND_SINK_STATE_ERROR;
  //         g_object_notify (G_OBJECT (self), "state");
  //       }
  //     g_clear_object (&self->factory);

  //     return NULL;
  //   }

  self->state = ND_SINK_STATE_STREAMING;
  g_object_notify (G_OBJECT (self), "state");

  /* TODO */
  self->factory = wfd_media_factory_new ();

  if (self->remote_address == NULL)
    {
      self->state = ND_SINK_STATE_ERROR;
      g_object_notify (G_OBJECT (self), "state");
      return NULL;
    }

  g_signal_connect_object (self->factory,
                           "create-source",
                           (GCallback) server_create_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->factory,
                           "create-audio-source",
                           (GCallback) server_create_audio_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  /*
     self->state = ND_SINK_STATE_WAIT_SOCKET;
     g_object_notify (G_OBJECT (self), "state");

     these were originally here
     1. send connect request
     2. send ping
   */

  GstStateChangeReturn ret;

  GstElement *pipeline = gst_pipeline_new ("pipeline");
  GstBin *bin = GST_BIN (pipeline);
  nd_cc_sink_create_gst_elements (self, bin);

  gst_pipeline_set_latency (GST_PIPELINE (pipeline), 500 * GST_MSECOND);

  /* Start playing */
  ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Unable to set the pipeline to the playing state.\n");
    return NULL;
  }

  /* Wait until error, EOS or State Change */
  GstMessage *msg;
  gboolean terminate = FALSE;
  g_autoptr(GstBus) bus = gst_element_get_bus (pipeline);

  do {
    msg = gst_bus_timed_pop_filtered (bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS |
        GST_MESSAGE_STATE_CHANGED);

    /* Parse message */
    if (msg != NULL) {
      GError *err;
      gchar *debug_info;

      switch (GST_MESSAGE_TYPE (msg)) {
        case GST_MESSAGE_ERROR:
          gst_message_parse_error (msg, &err, &debug_info);
          g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
          g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
          g_clear_error (&err);
          g_free (debug_info);
          terminate = TRUE;
          break;
        case GST_MESSAGE_EOS:
          g_print ("End-Of-Stream reached.\n");
          terminate = TRUE;
          break;
        case GST_MESSAGE_STATE_CHANGED:
          /* We are only interested in state-changed messages from the pipeline */
          if (GST_MESSAGE_SRC (msg) == GST_OBJECT (pipeline)) {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
            g_print ("\nPipeline state changed from %s to %s:\n",
                gst_element_state_get_name (old_state), gst_element_state_get_name (new_state));
          }
          break;
        default:
          /* We should not reach here because we only asked for ERRORs, EOS and STATE_CHANGED */
          g_printerr ("Unexpected message received.\n");
          break;
      }
      gst_message_unref (msg);
    }
  } while (!terminate);

  gst_element_set_state (pipeline, GST_STATE_NULL);

  return g_object_ref (sink);
}

static void
nd_cc_sink_sink_stop_stream_int (NdCCSink *self)
{
  cc_ctrl_finish (&self->ctrl);
  self->cancellable = g_cancellable_new ();
}

static void
nd_cc_sink_sink_stop_stream (NdSink *sink)
{
  NdCCSink *self = ND_CC_SINK (sink);

  nd_cc_sink_sink_stop_stream_int (self);

  self->state = ND_SINK_STATE_DISCONNECTED;
  g_object_notify (G_OBJECT (self), "state");
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
nd_cc_sink_get_state (NdCCSink *sink)
{
  return sink->state;
}
