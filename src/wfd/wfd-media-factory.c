#include "gnome-network-displays-config.h"
#include "wfd-media-factory.h"
#include "wfd-media.h"

static const gchar * wfd_gst_elements[ELEMENT_NONE + 1] = {
  [ELEMENT_OPENH264] = "openh264enc",
  [ELEMENT_X264] = "x264enc",
  [ELEMENT_VAH264] = "vah264enc",
  [ELEMENT_VAAPIH264] = "vaapih264enc",
  [ELEMENT_VIDEO_NONE] = NULL,

  [ELEMENT_AAC_FDK] = "fdkaacenc",
  [ELEMENT_AAC_AVENC] = "avenc_aac",
  [ELEMENT_AAC_FAAC] = "faac",
  [ELEMENT_AUDIO_NONE] = NULL,

  [ELEMENT_MPEGTS] = "mpegtsmux",

  [ELEMENT_NONE] = NULL,
};

struct _WfdMediaFactory
{
  GstRTSPMediaFactory parent_instance;

  gint                factory_profile;
};

G_DEFINE_TYPE (WfdMediaFactory, wfd_media_factory, GST_TYPE_RTSP_MEDIA_FACTORY)

typedef struct
{
  GstSegment *segment;
} QOSData;

enum {
  SIGNAL_CREATE_SOURCE,
  SIGNAL_CREATE_AUDIO_SOURCE,
  NR_SIGNALS
};

static guint signals[NR_SIGNALS];

static void
encoding_perf_handoff_cb (GstElement *elem, GstBuffer *buf, gpointer user_data)
{
  g_autoptr(GstClock) clock = NULL;
  QOSData *qos_data = user_data;
  GstClockTime now;

  clock = gst_element_get_clock (elem);
  if (!clock)
    return;

  now = MAX (0, gst_clock_get_time (clock) - gst_element_get_base_time (elem));
  if (buf->pts != GST_CLOCK_TIME_NONE)
    {
      GstEvent *qos_event;
      gdouble proportion;
      GstClockTimeDiff pts;
      GstClockTimeDiff late;

      if (qos_data->segment)
        pts = gst_segment_to_running_time (qos_data->segment, GST_FORMAT_TIME, buf->pts);
      else
        pts = buf->pts;

      late = MAX (0, now - pts);

      /* Ignore the first few frames. */
      if (pts > 100 * GST_MSECOND && (late - 50 * GST_MSECOND >= 0 || -(late - 50 * GST_MSECOND) <= pts))
        {
          /* We stop accepting things at more than 50ms delay;
           * Just use late / 50ms for the long term proportion. */
          proportion = late / (gdouble) (50 * GST_MSECOND);

          /* g_debug ("Sending QOS event with proportion %.2f", proportion); */
          qos_event = gst_event_new_qos (GST_QOS_TYPE_UNDERFLOW,
                                         proportion,
                                         late - 50 * GST_MSECOND,
                                         pts);

          gst_element_send_event (elem, qos_event);
        }
    }
}

GstPadProbeReturn
encoding_perf_probe_cb (GstPad          *pad,
                        GstPadProbeInfo *info,
                        gpointer         user_data)
{
  const GstSegment *segment;
  QOSData *qos_data = user_data;
  GstEvent *event;

  if (info->type != GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM)
    return GST_PAD_PROBE_OK;

  event = gst_pad_probe_info_get_event (info);

  if (GST_EVENT_TYPE (event) != GST_EVENT_SEGMENT)
    return GST_PAD_PROBE_OK;

  gst_event_parse_segment (event, &segment);

  g_clear_pointer (&qos_data->segment, gst_segment_free);
  qos_data->segment = gst_segment_copy (segment);

  return GST_PAD_PROBE_OK;
}

static void
free_qos_data (QOSData *qos_data)
{
  g_clear_pointer (&qos_data->segment, gst_segment_free);
  g_free (qos_data);
}

GstElement *
wfd_media_factory_create_video_element (WfdMediaFactory *self, GstBin *bin)
{
  g_autoptr(GstCaps) caps = NULL;
  g_autoptr(GstPad) encoding_perf_sink = NULL;
  g_autoptr(GstElement) source = NULL;

  QOSData *qos_data;

  GstElement *scale;
  GstElement *sinkfilter;
  GstElement *convert;
  GstElement *queue_pre_encoder;
  GstElement *encoder;
  GstElement *encoder_elem;
  GstElement *encoding_perf;
  GstElement *parse;
  GstElement *codecfilter;
  GstElement *queue_mpegmux_video;

  gboolean success = TRUE;

  /* Test input, will be replaced by real source */
  g_signal_emit (self, signals[SIGNAL_CREATE_SOURCE], 0, &source);
  g_assert (source);
  success &= gst_bin_add (bin, source);

  scale = gst_element_factory_make ("videoscale", "wfd-scale");
  g_object_set (scale,
                "qos", TRUE,
                NULL);
  success &= gst_bin_add (bin, scale);

  caps = gst_caps_new_simple ("video/x-raw",
                              "framerate", GST_TYPE_FRACTION, 30, 1,
                              "width", G_TYPE_INT, 1920,
                              "height", G_TYPE_INT, 1080,
                              NULL);
  sinkfilter = gst_element_factory_make ("capsfilter", "wfd-sinkfilter");
  success &= gst_bin_add (bin, sinkfilter);
  g_object_set (sinkfilter,
                "caps", caps,
                NULL);
  g_clear_pointer (&caps, gst_caps_unref);

  convert = gst_element_factory_make ("videoconvert", "wfd-videoconvert");
  g_object_set (convert,
                "qos", TRUE,
                NULL);
  success &= gst_bin_add (bin, convert);

  queue_pre_encoder = gst_element_factory_make ("queue", "wfd-pre-encoder-queue");
  g_object_set (queue_pre_encoder,
                "max-size-buffers", (guint) 1,
                "leaky", 0,
                NULL);
  success &= gst_bin_add (bin, queue_pre_encoder);

  switch (wfd_media_factory_profiles[self->factory_profile].video_encoder)
    {
    case ELEMENT_OPENH264:
      encoder = gst_element_factory_make ("openh264enc", "wfd-encoder");
      encoder_elem = encoder;
      success &= gst_bin_add (bin, encoder);
      g_object_set (encoder,
                    "qos", TRUE,
                    "multi-thread", 1,
                    "usage-type", 1, /* screen */
                    "slice-mode", 1, /* n-slices */
                    "num-slices", 1,
                    "rate-control", 1, /* bitrate */
                    "gop-size", 30,
                    /* If frame skipping is too aggressive, then audio will
                     * drop out. So don't enable it. */
                    "enable-frame-skip", FALSE,
                    "scene-change-detection", TRUE,
                    "background-detection", TRUE,
                    /*"adaptive-quantization", FALSE,*/
                    /*"max-slice-size", 5000,*/
                    "complexity", 0,
                    /*"deblocking", 2,*/
                    NULL);

      /* Maybe try:
       *  - rate-control: 2, buffer*/
      break;

    case ELEMENT_X264:
      encoder = gst_element_factory_make ("x264enc", "wfd-encoder");
      encoder_elem = encoder;
      success &= gst_bin_add (bin, encoder);

      gst_preset_load_preset (GST_PRESET (encoder), "Profile Baseline");
      break;

    case ELEMENT_VAH264:
      encoder = gst_element_factory_make ("vah264enc", "wfd-encoder");
      encoder_elem = encoder;
      success &= gst_bin_add (bin, encoder);
      g_object_set (encoder,
                    "qos", TRUE,
                    "key-int-max", 30,
                    "num-slices", 1,
                    "cabac", FALSE,
                    "dct8x8", FALSE,
                    NULL);

      break;

    case ELEMENT_VAAPIH264:
      {
        GstElement *vaapi_encoder;
        GstElement *vaapi_convert;

        encoder = gst_bin_new ("wfd-encoder-bin");

        vaapi_convert = gst_element_factory_make ("vaapipostproc", "wfd-vaapi-convert");
        success &= gst_bin_add (GST_BIN (encoder), vaapi_convert);

        vaapi_encoder = gst_element_factory_make ("vaapih264enc", "wfd-encoder");
        encoder_elem = vaapi_encoder;
        success &= gst_bin_add (GST_BIN (encoder), vaapi_encoder);

        g_object_set (vaapi_encoder,
                      "qos", TRUE,
                      "rate-control", 2, /* constant bitrate */
                      "keyframe-period", 30,
                      "max-bframes", 0,
                      "refs", 1,
                      "num-slices", 1,
                      "cabac", FALSE,
                      "dct8x8", FALSE,
                      "compliance-mode", 0, /* strict */
                      NULL);

        gst_element_link (vaapi_convert, vaapi_encoder);


        gst_element_add_pad (encoder,
                             gst_ghost_pad_new ("sink",
                                                gst_element_get_static_pad (vaapi_convert,
                                                                            "sink")));
        gst_element_add_pad (encoder,
                             gst_ghost_pad_new ("src",
                                                gst_element_get_static_pad (vaapi_encoder,
                                                                            "src")));

        success &= gst_bin_add (bin, encoder);
        break;
      }

    default:
      g_assert_not_reached ();
    }
  g_object_set_data (G_OBJECT (encoder_elem), "wfd-encoder-impl", GINT_TO_POINTER (wfd_media_factory_profiles[self->factory_profile].video_encoder));

  encoding_perf = gst_element_factory_make ("identity", "wfd-measure-encoder-realtime");
  success &= gst_bin_add (bin, encoding_perf);
  qos_data = g_new0 (QOSData, 1);
  g_object_set_data_full (G_OBJECT (encoding_perf), "wfd-qos-data", qos_data, (GDestroyNotify) free_qos_data);
  g_signal_connect (encoding_perf, "handoff", G_CALLBACK (encoding_perf_handoff_cb), qos_data);
  encoding_perf_sink = gst_element_get_static_pad (encoding_perf, "sink");
  gst_pad_add_probe (encoding_perf_sink,
                     GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                     encoding_perf_probe_cb,
                     qos_data,
                     NULL);

  /* Repack the H264 stream */
  parse = gst_element_factory_make ("h264parse", "wfd-h264parse");
  success &= gst_bin_add (bin, parse);
  g_object_set (parse,
                "config-interval", (gint) - 1,
                NULL);

  /* Permit both constrained-baseline and baseline. Would constrained-baseline be sufficient? */
  caps = gst_caps_from_string ("video/x-h264,stream-format=byte-stream,profile=constrained-baseline");
  gst_caps_append (caps,
                   gst_caps_from_string ("video/x-h264,stream-format=byte-stream,profile=baseline"));
  codecfilter = gst_element_factory_make ("capsfilter", "wfd-codecfilter");
  g_object_set (codecfilter,
                "caps", caps,
                NULL);
  g_clear_pointer (&caps, gst_caps_unref);
  success &= gst_bin_add (bin, codecfilter);

  queue_mpegmux_video = gst_element_factory_make ("queue", "wfd-mpegmux-video-queue");
  success &= gst_bin_add (bin, queue_mpegmux_video);
  g_object_set (queue_mpegmux_video,
                "max-size-buffers", (guint) 1000,
                "max-size-time", 500 * GST_MSECOND,
                NULL);

  success &= gst_element_link_many (source,
                                    scale,
                                    sinkfilter,
                                    convert,
                                    queue_pre_encoder,
                                    encoder,
                                    encoding_perf,
                                    parse,
                                    codecfilter,
                                    queue_mpegmux_video,
                                    NULL);

  if (!success)
    {
      GST_DEBUG_BIN_TO_DOT_FILE (bin,
                                 GST_DEBUG_GRAPH_SHOW_MEDIA_TYPE,
                                 "wfd-video-encoder-bin");
      g_error ("WfdMediaFactory: Error creating video encoding pipeline. If gstreamer is compiled with debugging and GST_DEBUG_DUMP_DOT_DIR is set, then the pipeline will have been dumped.");
    }

  return (GstElement *) g_steal_pointer (&queue_mpegmux_video);
}

GstBin *
wfd_media_factory_create_audio_element (WfdMediaFactory *self)
{
  g_autoptr(GstElement) audio_source = NULL;
  gboolean success = TRUE;

  if (wfd_media_factory_profiles[self->factory_profile].audio_encoder == ELEMENT_AUDIO_NONE)
    return NULL;

  g_signal_emit (self, signals[SIGNAL_CREATE_AUDIO_SOURCE], 0, &audio_source);

  if (!audio_source)
    return NULL;

  g_autoptr(GstCaps) caps = NULL;
  g_autoptr(GstBin) audio_pipeline = NULL;

  GstElement *audioencoder;
  GstElement *audioresample;
  GstElement *audioconvert;
  GstElement *queue_mpegmux_audio;

  audio_pipeline = GST_BIN (gst_bin_new ("wfd-audio"));
  /* The audio pipeline is disabled by default, we hook it up and
   * enable it during configuration. */
  gst_element_set_locked_state (GST_ELEMENT (audio_pipeline), TRUE);

  success &= gst_bin_add (audio_pipeline, audio_source);

  audioresample = gst_element_factory_make ("audioresample", "wfd-audio-resample");
  success &= gst_bin_add (audio_pipeline, audioresample);

  audioconvert = gst_element_factory_make ("audioconvert", "wfd-audio-convert");
  success &= gst_bin_add (audio_pipeline, audioconvert);

  switch (wfd_media_factory_profiles[self->factory_profile].audio_encoder)
    {
    case ELEMENT_AAC_FDK:
      audioencoder = gst_element_factory_make ("fdkaacenc", "wfd-audio-aac-enc");
      break;

    case ELEMENT_AAC_FAAC:
      audioencoder = gst_element_factory_make ("faac", "wfd-audio-aac-enc");
      break;

    case ELEMENT_AAC_AVENC:
      audioencoder = gst_element_factory_make ("avenc_aac", "wfd-audio-aac-enc");
      break;

    default:
      g_assert_not_reached ();
    }
  success &= gst_bin_add (audio_pipeline, audioencoder);

  queue_mpegmux_audio = gst_element_factory_make ("queue", "wfd-mpegmux-audio-queue");
  g_object_set (queue_mpegmux_audio,
                "max-size-buffers", (guint) 100000,
                "max-size-time", 500 * GST_MSECOND,
                "leaky", 0,
                NULL);
  success &= gst_bin_add (audio_pipeline, queue_mpegmux_audio);

  caps = gst_caps_new_simple ("audio/mpeg",
                              "channels", G_TYPE_INT, 2,
                              "rate", G_TYPE_INT, 48000,
                              NULL);

  success &= gst_element_link_many (audio_source, audioresample, audioconvert, NULL);
  success &= gst_element_link (audioconvert, audioencoder);
  success &= gst_element_link_filtered (audioencoder, queue_mpegmux_audio, caps);
  g_clear_pointer (&caps, gst_caps_unref);

  gst_element_add_pad (GST_ELEMENT (audio_pipeline),
                       gst_ghost_pad_new ("src",
                                          gst_element_get_static_pad (queue_mpegmux_audio,
                                                                      "src")));

  if (!success)
    {
      GST_DEBUG_BIN_TO_DOT_FILE (audio_pipeline,
                                 GST_DEBUG_GRAPH_SHOW_MEDIA_TYPE,
                                 "wfd-audio-encoder-bin");
      g_error ("WfdMediaFactory: Error creating audio encoding pipeline. If gstreamer is compiled with debugging and GST_DEBUG_DUMP_DOT_DIR is set, then the pipeline will have been dumped.");
    }

  return (GstBin *) g_steal_pointer (&audio_pipeline);
}

GstElement *
wfd_media_factory_create_element (GstRTSPMediaFactory *factory, const GstRTSPUrl *url)
{
  g_autoptr(GstBin) bin = NULL;
  g_autoptr(GstBin) audio_pipeline = NULL;
  WfdMediaFactory *self = WFD_MEDIA_FACTORY (factory);

  GstElement *queue_mpegmux_video;
  GstElement *mpegmux;
  GstElement *queue_pre_payloader;
  GstElement *payloader;
  gboolean success = TRUE;

  bin = GST_BIN (gst_bin_new ("nd-wfd-pipeline"));

  queue_mpegmux_video = wfd_media_factory_create_video_element (self, bin);

  /* TODO: With gstreamer 1.17 we should be able to set things up so
   *       that audio frames are still send even if there are no new
   *       video frames. At that point, some semantics should be
   *       changed like doing proper variable framerate and such.
   *       This is possible as mpegtsmux was being ported to GstAggregator.
   */
  mpegmux = gst_element_factory_make ("mpegtsmux", "wfd-mpegtsmux");
  success &= gst_bin_add (bin, mpegmux);
  g_object_set (mpegmux,
                "alignment", (gint) 7, /* Force the correct alignment for UDP */
                NULL);


  queue_pre_payloader = gst_element_factory_make ("queue", "wfd-pre-payloader-queue");
  success &= gst_bin_add (bin, queue_pre_payloader);
  g_object_set (queue_pre_payloader,
                "max-size-buffers", (guint) 1,
                "leaky", 0,
                NULL);

  payloader = gst_element_factory_make ("rtpmp2tpay", "pay0");
  success &= gst_bin_add (bin, payloader);
  g_object_set (payloader,
                "ssrc", 1,
                /* Perfect is in relation to the input buffers, but we want the
                 * proper clock from when the packet was sent. */
                "perfect-rtptime", FALSE,
                "timestamp-offset", (guint) 0,
                "seqnum-offset", (gint) 0,
                NULL);

  /* The WFD specification says we should use stream ID 0x1011. */
  success &= gst_element_link_pads (queue_mpegmux_video, "src", mpegmux, "sink_4113");
  success &= gst_element_link_many (mpegmux,
                                    queue_pre_payloader,
                                    payloader,
                                    NULL);


  /* Add audio elements */
  audio_pipeline = wfd_media_factory_create_audio_element (self);
  if (audio_pipeline != NULL)
    success &= gst_bin_add (bin, GST_ELEMENT (g_object_ref (audio_pipeline)));

  GST_DEBUG_BIN_TO_DOT_FILE (bin,
                             GST_DEBUG_GRAPH_SHOW_MEDIA_TYPE,
                             "nd-wfd-pipeline");
  if (!success)
    {
      g_error ("WfdMediaFactory: Error creating encoding pipeline. If gstreamer is compiled with debugging and GST_DEBUG_DUMP_DOT_DIR is set, then the pipeline will have been dumped.");
      g_clear_object (&bin);
    }

  return (GstElement *) g_steal_pointer (&bin);
}

static GstElement *
wfd_media_factory_create_pipeline (GstRTSPMediaFactory *factory, GstRTSPMedia *media)
{
  GstElement *pipeline;

  pipeline = GST_RTSP_MEDIA_FACTORY_CLASS (wfd_media_factory_parent_class)->create_pipeline (factory, media);

  /* We need a high latency for the openh264 encoder at least when the
   * usage-type is set to "screen". After e.g. scene changes the latency will
   * be very high for short periods of time, and this prevents further issues. */
  gst_pipeline_set_latency (GST_PIPELINE (pipeline), 500 * GST_MSECOND);

  return pipeline;
}

WfdMediaQuirks
wfd_configure_media_element (GstBin *bin, WfdParams *params)
{
  g_autoptr(GstCaps) caps = NULL;
  g_autoptr(GstElement) srcfilter = NULL;
  g_autoptr(GstElement) sinkfilter = NULL;
  g_autoptr(GstElement) codecfilter = NULL;
  g_autoptr(GstElement) encoder = NULL;
  g_autoptr(GstElement) audio_pipeline = NULL;
  g_autoptr(GstElement) mpegmux = NULL;
  WfdMediaQuirks quirks = 0;
  WfdVideoCodec *codec = params->selected_codec;
  WfdResolution *resolution = params->selected_resolution;
  WfdGstElement encoder_impl;
  WfdH264ProfileFlags profile;
  guint gop_size = resolution->refresh_rate;
  guint bitrate_kbit = wfd_video_codec_get_max_bitrate_kbit (codec);

  /* Limit initial video bitrate to 512kBit/s to ensure we don't
   * saturate the wifi link.
   * This is a rather bad method, but it kind of works. */
  bitrate_kbit = MIN (bitrate_kbit, 512 * 8);

  if (resolution->interlaced)
    g_warning ("Resolution should never be set to interlaced as that is not supported with all codecs.");

  encoder = gst_bin_get_by_name (bin, "wfd-encoder");
  encoder_impl = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (encoder), "wfd-encoder-impl"));

  if (encoder_impl == ELEMENT_VAAPIH264)
    quirks = WFD_QUIRK_NO_IDR;

  /* Decrease the number of keyframes if the device is able to request
   * IDRs by itself.
   * Note that VAAPI H264 appears to run into an assertion error in version 1.14.4 */
  if (params->idr_request_capability && !(quirks & WFD_QUIRK_NO_IDR))
    gop_size = 10 * resolution->refresh_rate;

  srcfilter = gst_bin_get_by_name (bin, "srcfilter");
  if (srcfilter != NULL)
    {
      caps = gst_caps_new_simple ("video/x-raw",
                                  "max-framerate", GST_TYPE_FRACTION, resolution->refresh_rate, 1,
                                  "width", G_TYPE_INT, resolution->width,
                                  "height", G_TYPE_INT, resolution->height,
                                  NULL);

      g_object_set (srcfilter,
                    "caps", caps,
                    NULL);
      g_clear_pointer (&caps, gst_caps_unref);
    }

  caps = gst_caps_new_simple ("video/x-raw",
                              "framerate", GST_TYPE_FRACTION, resolution->refresh_rate, 1,
                              "width", G_TYPE_INT, resolution->width,
                              "height", G_TYPE_INT, resolution->height,
                              NULL);

  sinkfilter = gst_bin_get_by_name (bin, "wfd-sinkfilter");
  g_object_set (sinkfilter,
                "caps", caps,
                NULL);
  g_clear_pointer (&caps, gst_caps_unref);

  switch (encoder_impl)
    {
    case ELEMENT_OPENH264:
      /* We could set multi-thread/num-slices to codec->max_slice_num; but not sure
       * if that works realiably, and simply using one slice is on the safe side
       */
      profile = WFD_H264_PROFILE_BASE;
      g_object_set (encoder,
                    "max-bitrate", (guint) bitrate_kbit * 1024,
                    "bitrate", (guint) bitrate_kbit * 1024,
                    "gop-size", gop_size,
                    NULL);
      break;

    case ELEMENT_X264:
      if (codec->profile == WFD_H264_PROFILE_HIGH)
        {
          profile = codec->profile;
          gst_preset_load_preset (GST_PRESET (encoder), "Profile High");
        }
      else
        {
          profile = WFD_H264_PROFILE_BASE;
          gst_preset_load_preset (GST_PRESET (encoder), "Profile Baseline");
        }

      g_object_set (encoder,
                    "qos", TRUE,
                    "pass", 4, /* constant bit rate encoding */
                    "tune", 0x4, /* zero latency */
                    "speed-preset", 1, /* ultrafast */
                    "rc-lookahead", 1,
                    "threads", 1,
                    "vbv-buf-capacity", 50,
                    "dct8x8", FALSE,
                    "ref", 1,
                    "cabac", FALSE,
                    "sync-lookahead", 0,
                    "b-adapt", FALSE,
                    "bframes", (guint) 0,
                    "rc-lookahead", 0,
                    "key-int-max", (guint) gop_size,
                    "interlaced", resolution->interlaced,
                    "bitrate",  bitrate_kbit,
                    "insert-vui", TRUE,
                    "sliced-threads", FALSE,
                    NULL);
      break;

    case ELEMENT_VAH264:
      if (codec->profile == WFD_H264_PROFILE_HIGH)
        profile = WFD_H264_PROFILE_HIGH;
      else
        profile = WFD_H264_PROFILE_BASE;

      g_object_set (encoder,
                    "key-int-max", (guint) gop_size,
                    "bitrate",  bitrate_kbit,
                    NULL);
      break;

    case ELEMENT_VAAPIH264:
      if (codec->profile == WFD_H264_PROFILE_HIGH)
        profile = WFD_H264_PROFILE_HIGH;
      else
        profile = WFD_H264_PROFILE_BASE;

      g_object_set (encoder,
                    "keyframe-period", (guint) gop_size,
                    "bitrate",  bitrate_kbit,
                    NULL);
      break;

    default:
      g_assert_not_reached ();
    }

  if (profile == WFD_H264_PROFILE_HIGH)
    caps = gst_caps_from_string ("video/x-h264,stream-format=byte-stream,profile=high");
  else
    {
      /* Permit both constrained-baseline and baseline. Would constrained-baseline be sufficient? */
      caps = gst_caps_from_string ("video/x-h264,stream-format=byte-stream,profile=constrained-baseline");
      gst_caps_append (caps,
                       gst_caps_from_string ("video/x-h264,stream-format=byte-stream,profile=baseline"));
    }

  codecfilter = gst_bin_get_by_name (bin, "wfd-codecfilter");
  g_object_set (codecfilter,
                "caps", caps,
                NULL);
  g_clear_pointer (&caps, gst_caps_unref);

  g_debug ("An audiocodec has been selected: %s", params->selected_audio_codec ? "yes" : "no");
  audio_pipeline = gst_bin_get_by_name (bin, "wfd-audio");
  mpegmux = gst_bin_get_by_name (bin, "wfd-mpegtsmux");
  if (audio_pipeline)
    {
      gst_element_unlink (audio_pipeline, mpegmux);

      if (params->selected_audio_codec)
        {
          /* We currently only handle AAC with 2 channels and 48kHz */
          g_assert (params->selected_audio_codec->type == WFD_AUDIO_AAC);
          g_assert (params->selected_audio_codec->modes == 0x1);

          gst_element_set_locked_state (GST_ELEMENT (audio_pipeline), FALSE);

          /* Hook up the audio channel */
          gst_element_link_pads (audio_pipeline, "src", mpegmux, "sink_4352");
        }
      else
        {
          gst_element_set_locked_state (GST_ELEMENT (audio_pipeline), TRUE);
          gst_element_set_state (GST_ELEMENT (audio_pipeline), GST_STATE_NULL);
        }
    }

  GST_DEBUG_BIN_TO_DOT_FILE (bin,
                             GST_DEBUG_GRAPH_SHOW_ALL,
                             "nd-wfd-pipeline-configured");

  return quirks;
}

GstRTSPMedia *
wfd_media_factory_construct (GstRTSPMediaFactory *factory, const GstRTSPUrl *url)
{
  GstRTSPMedia *res;
  GstRTSPStream *stream;

  res = GST_RTSP_MEDIA_FACTORY_CLASS (wfd_media_factory_parent_class)->construct (factory, url);

  stream = gst_rtsp_media_get_stream (res, 0);
  gst_rtsp_stream_set_control (stream, "streamid=0");
  g_debug ("WfdMedia init: Got %d streams", gst_rtsp_media_n_streams (res));

  return res;
}

WfdMediaFactory *
wfd_media_factory_new (void)
{
  return g_object_new (WFD_TYPE_MEDIA_FACTORY, NULL);
}

void
wfd_media_factory_finalize (GObject *object)
{
  g_debug ("WfdMediaFactory: Finalize");

  G_OBJECT_CLASS (wfd_media_factory_parent_class)->finalize (object);
}

static void
wfd_media_factory_class_init (WfdMediaFactoryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstRTSPMediaFactoryClass *media_factory_class = GST_RTSP_MEDIA_FACTORY_CLASS (klass);

  object_class->finalize = wfd_media_factory_finalize;

  media_factory_class->create_element = wfd_media_factory_create_element;
  media_factory_class->create_pipeline = wfd_media_factory_create_pipeline;
  media_factory_class->construct = wfd_media_factory_construct;

  signals[SIGNAL_CREATE_SOURCE] =
    g_signal_new ("create-source", WFD_TYPE_MEDIA_FACTORY, G_SIGNAL_RUN_LAST,
                  0,
                  g_signal_accumulator_first_wins, NULL,
                  NULL,
                  GST_TYPE_ELEMENT, 0);

  signals[SIGNAL_CREATE_AUDIO_SOURCE] =
    g_signal_new ("create-audio-source", WFD_TYPE_MEDIA_FACTORY, G_SIGNAL_RUN_LAST,
                  0,
                  g_signal_accumulator_first_wins, NULL,
                  NULL,
                  GST_TYPE_ELEMENT, 0);
}

static gboolean
wfd_gst_element_present (WfdGstElement element)
{
  g_autoptr(GstElementFactory) encoder_factory = NULL;

  /* -1 = not found
   *  0 = not checked
   *  1 = found
   */
  static gint8 found_elements[ELEMENT_NONE] = {0};

  if (element == ELEMENT_VIDEO_NONE || element == ELEMENT_AUDIO_NONE)
    return TRUE; /* is "nothing" found? yes! */

  if (found_elements[element] != 0)
    return found_elements[element] == 1;

  encoder_factory = gst_element_factory_find (wfd_gst_elements[element]);

  if (encoder_factory != NULL)
    {
      g_debug ("WfdMediaFactory: Found %s gst element.", wfd_gst_elements[element]);
      found_elements[element] = 1;
      return TRUE;
    }

  found_elements[element] = -1;
  return FALSE;
}

static gint
wfd_gst_media_profile_present (WfdMediaProfile media_profile)
{
  gint factory_profile;

  g_debug ("WfdMediaFactory: Checking profile: %d", media_profile);

  if (media_profile < 0 || media_profile >= PROFILE_LAST)
    return -1;

  for (factory_profile = 0; factory_profile < (sizeof (wfd_media_factory_profiles) / sizeof (wfd_media_factory_profiles[0])); ++factory_profile)
    {
      if (wfd_media_factory_profiles[factory_profile].media_profile == media_profile &&
          wfd_gst_element_present (wfd_media_factory_profiles[factory_profile].video_encoder) &&
          wfd_gst_element_present (wfd_media_factory_profiles[factory_profile].audio_encoder) &&
          wfd_gst_element_present (wfd_media_factory_profiles[factory_profile].muxer))
        {
          g_debug ("WfdMediaFactory: Found elements for media profile: %d", media_profile);
          return factory_profile;
        }
    }

  return -1;
}

gboolean
wfd_media_factory_lookup_encoders (WfdMediaFactory *self,
                                   WfdMediaProfile  media_profile,
                                   GStrv           *missing_video,
                                   GStrv           *missing_audio)
{
  gint factory_profile;

  *missing_video = NULL;
  *missing_audio = NULL;

  /* check for particular profile */
  if (media_profile != PROFILE_LAST)
    {
      factory_profile = wfd_gst_media_profile_present (media_profile);
      if (factory_profile == -1)
        goto missing_elements;

      self->factory_profile = factory_profile;
      return TRUE;
    }

  /* PROFILE_HIGH_H264 */
  if ((factory_profile = wfd_gst_media_profile_present (PROFILE_HIGH_H264)) != -1)
    {
      self->factory_profile = factory_profile;
      return TRUE;
    }

  /* PROFILE_BASE_H264 */
  if ((factory_profile = wfd_gst_media_profile_present (PROFILE_BASE_H264)) != -1)
    {
      self->factory_profile = factory_profile;
      return TRUE;
    }

missing_elements:
  switch (media_profile)
    {
    case PROFILE_LAST:
    case PROFILE_BASE_H264:
      if  (!wfd_gst_element_present (ELEMENT_OPENH264) &&
           !wfd_gst_element_present (ELEMENT_X264) &&
           !wfd_gst_element_present (ELEMENT_VAH264) &&
           !wfd_gst_element_present (ELEMENT_VAAPIH264))
        {
          gchar *missing[5] = { (gchar *) wfd_gst_elements[ELEMENT_OPENH264],
                                (gchar *) wfd_gst_elements[ELEMENT_X264],
                                (gchar *) wfd_gst_elements[ELEMENT_VAH264],
                                (gchar *) wfd_gst_elements[ELEMENT_VAAPIH264],
                                NULL };
          *missing_video = (GStrv) g_strdupv (missing);
        }
      /* use missing_video for muxer as well (legacy reasons) */
      else if (!wfd_gst_element_present (ELEMENT_MPEGTS))
        {
          gchar *missing[2] = { (gchar *) wfd_gst_elements[ELEMENT_MPEGTS], NULL };
          *missing_video = (GStrv) g_strdupv (missing);
        }

      break;

    case PROFILE_HIGH_H264:
      if (!wfd_gst_element_present (ELEMENT_X264) &&
          !wfd_gst_element_present (ELEMENT_VAH264) &&
          !wfd_gst_element_present (ELEMENT_VAAPIH264))
        {
          gchar *missing[4] = { (gchar *) wfd_gst_elements[ELEMENT_X264],
                                (gchar *) wfd_gst_elements[ELEMENT_VAH264],
                                (gchar *) wfd_gst_elements[ELEMENT_VAAPIH264],
                                NULL };
          *missing_video = (GStrv) g_strdupv (missing);
        }
      /* use missing_video for muxer as well (legacy reasons) */
      else if (!wfd_gst_element_present (ELEMENT_MPEGTS))
        {
          gchar *missing[2] = { (gchar *) wfd_gst_elements[ELEMENT_MPEGTS], NULL };
          *missing_video = (GStrv) g_strdupv (missing);
        }

      break;

    default:
      break;
    }

  if (!wfd_gst_element_present (ELEMENT_AAC_FDK) &&
      !wfd_gst_element_present (ELEMENT_AAC_AVENC) &&
      !wfd_gst_element_present (ELEMENT_AAC_FAAC))
    {
      gchar *missing[4] = { (gchar *) wfd_gst_elements[ELEMENT_AAC_FDK],
                            (gchar *) wfd_gst_elements[ELEMENT_AAC_AVENC],
                            (gchar *) wfd_gst_elements[ELEMENT_AAC_FAAC],
                            NULL };
      *missing_audio = (GStrv) g_strdupv (missing);
    }

  return FALSE;
}

static void
wfd_media_factory_init (WfdMediaFactory *self)
{
  GstRTSPMediaFactory *media_factory = GST_RTSP_MEDIA_FACTORY (self);

  gst_rtsp_media_factory_set_media_gtype (media_factory, WFD_TYPE_MEDIA);
  gst_rtsp_media_factory_set_suspend_mode (media_factory, GST_RTSP_SUSPEND_MODE_RESET);
  gst_rtsp_media_factory_set_buffer_size (media_factory, 65536);
}
