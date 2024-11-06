#include "gnome-network-displays-config.h"
#include "cc-media-factory.h"

G_DEFINE_TYPE (CcMediaFactory, cc_media_factory, G_TYPE_OBJECT)

static const gchar * cc_gst_elements[ELEMENT_NONE + 1] = {
  [ELEMENT_VP8] = "vp8enc",
  [ELEMENT_X264] = "x264enc",
  [ELEMENT_VAH264] = "vah264enc",
  [ELEMENT_VAAPIH264] = "vaapih264enc",
  [ELEMENT_VIDEO_NONE] = NULL,

  [ELEMENT_AAC_FDK] = "fdkaacenc",
  [ELEMENT_AAC_AVENC] = "avenc_aac",
  [ELEMENT_AAC_FAAC] = "faac",
  [ELEMENT_VORBIS] = "vorbisenc",
  [ELEMENT_OPUS] = "opusenc",
  [ELEMENT_AUDIO_NONE] = NULL,

  [ELEMENT_WEBM] = "webmmux",
  [ELEMENT_MATROSKA] = "matroskamux",
  [ELEMENT_MP4] = "mp4mux",

  [ELEMENT_NONE] = NULL,
};

/* TODO: lookup if this is needed in Cc */
/*
   typedef struct
   {
   GstSegment *segment;
   } QOSData;
 */

enum {
  SIGNAL_CREATE_SOURCE,
  SIGNAL_CREATE_AUDIO_SOURCE,
  SIGNAL_END_STREAM,
  NR_SIGNALS,
};

static guint signals[NR_SIGNALS];

GstBin *
cc_media_factory_create_video_element (CcMediaFactory *self)
{
  g_autoptr(GstBin) bin = NULL;
  g_autoptr(GstCaps) caps = NULL;
  g_autoptr(GstElement) source = NULL;

  GstElement *scale;
  GstElement *sinkfilter;
  GstElement *convert;
  GstElement *queue_pre_encoder;
  GstElement *encoder;
  GstElement *parser = NULL;
  GstElement *codecfilter;
  GstElement *queue_post_encoder;

  gboolean success = TRUE;

  if (cc_media_factory_profiles[self->factory_profile].video_encoder == ELEMENT_VIDEO_NONE)
    return NULL;

  /* Test input, will be replaced by real source */
  g_signal_emit (self, signals[SIGNAL_CREATE_SOURCE], 0, &source);

  if (!source)
    return NULL;

  bin = GST_BIN (gst_bin_new ("cc-video-bin"));

  success &= gst_bin_add (bin, source);

  scale = gst_element_factory_make ("videoscale", "cc-scale");
  success &= gst_bin_add (bin, scale);

  /* TODO: this is the initial config, change it once the info is
   * available from the source in gst bus events
   */
  caps = gst_caps_new_simple ("video/x-raw",
                              "framerate", GST_TYPE_FRACTION, 30, 1,
                              "width", G_TYPE_INT, 1920,
                              "height", G_TYPE_INT, 1080,
                              NULL);
  sinkfilter = gst_element_factory_make ("capsfilter", "cc-sinkfilter");
  success &= gst_bin_add (bin, sinkfilter);
  g_object_set (sinkfilter,
                "caps", caps,
                NULL);
  g_clear_pointer (&caps, gst_caps_unref);

  convert = gst_element_factory_make ("videoconvert", "cc-videoconvert");
  success &= gst_bin_add (bin, convert);

  queue_pre_encoder = gst_element_factory_make ("queue", "cc-pre-encoder-queue");
  /* TODO: play with the queue max buffers */
  success &= gst_bin_add (bin, queue_pre_encoder);

  switch (cc_media_factory_profiles[self->factory_profile].video_encoder)
    {
    case ELEMENT_X264:
      encoder = gst_element_factory_make ("x264enc", "cc-video-encoder");
      gst_preset_load_preset (GST_PRESET (encoder), "Zero Latency");
      gst_preset_load_preset (GST_PRESET (encoder), "Profile High");
      g_object_set (encoder,
                    "ref", (guint) 5,
                    "speed-preset", (guint) 1,
                    "tune", 0x00000004,
                    NULL);

      parser = gst_element_factory_make ("h264parse", "cc-h264parse");
      caps = gst_caps_from_string ("video/x-h264,stream-format=avc,alignment=au,profile=high");
      break;

    case ELEMENT_VAH264:
      encoder = gst_element_factory_make ("vah264enc", "cc-video-encoder");
      g_object_set (encoder,
                    "rate-control", 2,
                    NULL);

      parser = gst_element_factory_make ("h264parse", "cc-h264parse");
      caps = gst_caps_from_string ("video/x-h264,stream-format=avc,alignment=au,profile=high");
      break;

    case ELEMENT_VAAPIH264:
      encoder = gst_element_factory_make ("vaapih264enc", "cc-video-encoder");
      g_object_set (encoder,
                    "prediction-type", 1,
                    "rate-control", 2,
                    "compliance-mode", 0,
                    NULL);

      parser = gst_element_factory_make ("h264parse", "cc-h264parse");
      caps = gst_caps_from_string ("video/x-h264,stream-format=avc,alignment=au,profile=high");
      break;

    case ELEMENT_VP8:
      encoder = gst_element_factory_make ("vp8enc", "cc-video-encoder");

      /* TODO: in search of the perfect settings */
      /* TODO: change the encoder settings on renegotiation of caps */
      g_object_set (encoder,
                    "arnr-maxframes", 0, /* AltRef maximum number of frames: 0 */
                    "arnr-strength", 0, /* AltRef strength: 0 */
                    "auto-alt-ref", TRUE, /* Automatically generate AltRef frames */
                    "cpu-used", -16, /* CPU used: maximum quality */
                    "end-usage", 1, /* Rate control mode: Constant Bit Rate (CBR) */
                    "keyframe-mode", 1, /* Keyframe placement: disabled */
                    "static-threshold", 100, /* Motion detection threshold for screen sharing: 100 */
                    "target-bitrate", 3000000, /* Target bitrate: 3 Mbps */
                    /* TODO: this is not working, looks like a bug in gstreamer
                        "target-bitrate", 0,  // Target bitrate: 3 Mbps
                        "bits-per-pixel", 0.1,  // Bits per pixel: 0.1
                     */
                    "deadline", 1,
                    "resize-allowed", FALSE, /* Disable spatial resampling */
                    NULL
                   );

      /* TODO: This was 2, but that doesn't work because "Codec bit-depth 8 not supported in profile > 1" (see gstvpxenc.c) */
      caps = gst_caps_from_string ("video/x-vp8,profile=(string)1");
      break;

    default:
      g_assert_not_reached ();
    }

  success &= gst_bin_add (bin, encoder);
  /* TODO: what is this? */
  /* g_object_set_data (G_OBJECT (encoder_elem), "wfd-encoder-impl", GINT_TO_POINTER (self->encoder)); */

  codecfilter = gst_element_factory_make ("capsfilter", "cc-codecfilter");
  g_object_set (codecfilter,
                "caps", caps,
                NULL);
  g_clear_pointer (&caps, gst_caps_unref);
  success &= gst_bin_add (bin, codecfilter);

  queue_post_encoder = gst_element_factory_make ("queue", "cc-post-encoder-queue");
  success &= gst_bin_add (bin, queue_post_encoder);

  success &= gst_element_link_many (source,
                                    scale,
                                    sinkfilter,
                                    convert,
                                    queue_pre_encoder,
                                    encoder,
                                    NULL);

  if (parser != NULL)
    {
      success &= gst_bin_add (bin, parser);
      success &= gst_element_link_many (encoder,
                                        parser,
                                        codecfilter,
                                        queue_post_encoder,
                                        NULL);
    }
  else
    success &= gst_element_link_many (encoder,
                                      codecfilter,
                                      queue_post_encoder,
                                      NULL);

  gst_element_add_pad (GST_ELEMENT (bin),
                       gst_ghost_pad_new ("src",
                                          gst_element_get_static_pad (queue_post_encoder,
                                                                      "src")));

  GST_DEBUG_BIN_TO_DOT_FILE (bin,
                             GST_DEBUG_GRAPH_SHOW_ALL,
                             "cc-video-encoder-bin");
  if (!success)
    g_error ("CcMediaFactory: Error creating a video element for the media encoding pipeline. If gstreamer is compiled with debugging and GST_DEBUG_DUMP_DOT_DIR is set, then the pipeline will have been dumped as \"cc-video-encoder-bin\".");

  return (GstBin *) g_steal_pointer (&bin);
}

GstBin *
cc_media_factory_create_audio_element (CcMediaFactory *self)
{
  g_autoptr(GstElement) audio_source = NULL;
  g_autoptr(GstCaps) caps = NULL;
  g_autoptr(GstBin) bin = NULL;

  GstElement *audioencoder;
  GstElement *audioresample;
  GstElement *audioconvert;
  GstElement *queue_post_encoder;

  gboolean success = TRUE;

  if (cc_media_factory_profiles[self->factory_profile].audio_encoder == ELEMENT_AUDIO_NONE)
    return NULL;

  g_signal_emit (self, signals[SIGNAL_CREATE_AUDIO_SOURCE], 0, &audio_source);

  if (!audio_source)
    return NULL;

  bin = GST_BIN (gst_bin_new ("cc-audio-bin"));

  success &= gst_bin_add (bin, audio_source);

  audioresample = gst_element_factory_make ("audioresample", "cc-audio-resample");
  g_object_set (audioresample,
                "quality", (guint) 8, /* 10 is best */
                NULL);
  success &= gst_bin_add (bin, audioresample);

  audioconvert = gst_element_factory_make ("audioconvert", "cc-audio-convert");
  success &= gst_bin_add (bin, audioconvert);

  switch (cc_media_factory_profiles[self->factory_profile].audio_encoder)
    {
    case ELEMENT_AAC_FDK:
      audioencoder = gst_element_factory_make ("fdkaacenc", "cc-audio-encoder");
      g_object_set (audioencoder,
                    "bitrate", (gint) 128000,
                    NULL);
      break;

    case ELEMENT_AAC_AVENC:
      audioencoder = gst_element_factory_make ("avenc_aac", "cc-audio-encoder");
      g_object_set (audioencoder,
                    "bitrate", (gint) 128000,
                    NULL);
      break;

    case ELEMENT_AAC_FAAC:
      audioencoder = gst_element_factory_make ("faac", "cc-audio-encoder");
      /* default bitrate is 128000 */
      break;

    case ELEMENT_VORBIS:
      audioencoder = gst_element_factory_make ("vorbisenc", "cc-audio-encoder");
      g_object_set (audioencoder,
                    "quality", (gfloat) 0.8,
                    NULL);
      break;

    case ELEMENT_OPUS:
      audioencoder = gst_element_factory_make ("opusenc", "cc-audio-encoder");
      g_object_set (audioencoder,
                    "bitrate", (gint) 128000,
                    "bitrate-type", (guint) 0, /* CBR */
                    NULL);
      break;

    default:
      g_assert_not_reached ();
    }
  success &= gst_bin_add (bin, audioencoder);

  queue_post_encoder = gst_element_factory_make ("queue", "cc-post-audio-encoder-queue");
  success &= gst_bin_add (bin, queue_post_encoder);

  /* no caps, we use the provided channels and rate */
  success &= gst_element_link_many (audio_source,
                                    audioresample,
                                    audioconvert,
                                    audioencoder,
                                    queue_post_encoder,
                                    NULL);

  gst_element_add_pad (GST_ELEMENT (bin),
                       gst_ghost_pad_new ("src",
                                          gst_element_get_static_pad (queue_post_encoder,
                                                                      "src")));

  if (!success)
    {
      GST_DEBUG_BIN_TO_DOT_FILE (bin,
                                 GST_DEBUG_GRAPH_SHOW_ALL,
                                 "cc-audio-encoder-bin");
      g_error ("CcMediaFactory: Error creating an audio element for the media encoding pipeline. If gstreamer is compiled with debugging and GST_DEBUG_DUMP_DOT_DIR is set, then the pipeline will have been dumped at \"cc-audio-encoder-bin\".");
    }

  return (GstBin *) g_steal_pointer (&bin);
}

static void
cc_media_factory_create_element (CcMediaFactory *self)
{
  g_autoptr(GstBin) video_pipeline = NULL;
  g_autoptr(GstBin) audio_pipeline = NULL;

  GstBin *bin;

  GstElement *muxer;
  GstElement *queue_post_muxer;
  GstElement *multisocketsink;

  gboolean success = TRUE;

  bin = GST_BIN (self->pipeline);

  video_pipeline = cc_media_factory_create_video_element (self);
  if (video_pipeline != NULL)
    success &= gst_bin_add (bin, GST_ELEMENT (g_object_ref (video_pipeline)));

  audio_pipeline = cc_media_factory_create_audio_element (self);
  if (audio_pipeline != NULL)
    success &= gst_bin_add (bin, GST_ELEMENT (g_object_ref (audio_pipeline)));

  if (video_pipeline == NULL && audio_pipeline == NULL)
    g_error ("CcMediaFactory: Error creating the media encoding pipeline, no video or audio pipeline was created.");

  switch (cc_media_factory_profiles[self->factory_profile].muxer)
    {
    /* TODO: some muxer settings? */
    case ELEMENT_WEBM:
      muxer = gst_element_factory_make ("webmmux", "cc-muxer");
      g_object_set (muxer,
                    "streamable", TRUE,
                    NULL);
      break;

    case ELEMENT_MATROSKA:
      muxer = gst_element_factory_make ("matroskamux", "cc-muxer");
      g_object_set (muxer,
                    "streamable", TRUE,
                    NULL);
      break;

    case ELEMENT_MP4:
      muxer = gst_element_factory_make ("mp4mux", "cc-muxer");
      g_object_set (muxer,
                    "streamable", TRUE,
                    NULL);
      break;

    default:
      g_assert_not_reached ();
    }

  success &= gst_bin_add (bin, muxer);

  queue_post_muxer = gst_element_factory_make ("queue", "cc-queue-post-muxer");
  success &= gst_bin_add (bin, queue_post_muxer);

  multisocketsink = gst_element_factory_make ("multisocketsink", "cc-multisocketsink");
  success &= gst_bin_add (bin, multisocketsink);

  g_object_set (multisocketsink,
                "blocksize", (guint) 32768, /* 32 KiB */
                "burst-format", (guint) 4, /* buffers */
                "recover-policy", 3, /* most recent keyframe */
                "sync-method", 2, /* latest keyframe */
                NULL);

  if (video_pipeline != NULL)
    success &= gst_element_link_pads (GST_ELEMENT (video_pipeline), "src", muxer, "video_%u");
  if (audio_pipeline != NULL)
    success &= gst_element_link_pads (GST_ELEMENT (audio_pipeline), "src", muxer, "audio_%u");

  success &= gst_element_link_many (muxer,
                                    queue_post_muxer,
                                    multisocketsink,
                                    NULL);

  GST_DEBUG_BIN_TO_DOT_FILE (bin,
                             GST_DEBUG_GRAPH_SHOW_ALL,
                             "nd-cc-bin");

  if (!success)
    g_error ("CcMediaFactory: Error creating a muxer element for the media encoding pipeline. If gstreamer is compiled with debugging and GST_DEBUG_DUMP_DOT_DIR is set, then the pipeline will have been dumped at \"nd-cc-bin.dot\".");

  g_debug ("CcMediaFactory: Successfully created the media pipeline");
}

/* TODO: pipeline configuration */

gboolean
cc_media_factory_set_pipeline_state (CcMediaFactory *self, GstState state)
{
  GError *error;
  gboolean success;

  if (!self->pipeline)
    {
      g_warning ("CcMediaFactory: Pipeline state change requested when the pipeline is invalid, ignoring.");
      return FALSE;
    }

  success = gst_element_set_state (self->pipeline, state) != GST_STATE_CHANGE_FAILURE;

  if (!success)
    {
      error = g_error_new (CC_ERROR,
                           CC_ERROR_GST_PIPELINE_SET_STATE_FAILED,
                           "Unable to set the pipeline to %s state.",
                           gst_element_state_get_name (state));
      g_warning ("CcMediaFactory: Pipeline state change failure: %s", error->message);
      g_signal_emit_by_name (self, "end-stream", error);
      g_clear_error (&error);
      return FALSE;
    }

  return TRUE;
}

/* TODO: get media caps after starting it for a bit and configure the pipeline */
static gboolean
cc_media_factory_gst_bus_message_cb (GstBus *bus, GstMessage *msg, CcMediaFactory *self)
{
  switch (GST_MESSAGE_TYPE (msg))
    {
    case GST_MESSAGE_STATE_CHANGED:
      {
        if (GST_MESSAGE_SRC (msg) != GST_OBJECT (self->pipeline))
          break;

        GstState old_state, new_state;
        gst_message_parse_state_changed (msg, &old_state, &new_state, NULL);

        g_debug ("CcMediaFactory: State changed: from %s to %s",
                 gst_element_state_get_name (old_state),
                 gst_element_state_get_name (new_state));

        break;
      }

    case GST_MESSAGE_EOS:
      {
        GstElement *multisocketsink;

        g_debug ("CcMediaFactory: EOS, halting pipeline");
        cc_media_factory_set_pipeline_state (self, GST_STATE_NULL);

        multisocketsink = gst_bin_get_by_name (GST_BIN (self->pipeline), "cc-multisocketsink");
        g_signal_emit_by_name (multisocketsink, "clear");
        gst_object_unref (multisocketsink);

        break;
      }

    case GST_MESSAGE_INFO:
    case GST_MESSAGE_WARNING:
    case GST_MESSAGE_ERROR:
      {
        GError *error = NULL;
        gchar *debug_info;
        GstElement *multisocketsink;

        switch (GST_MESSAGE_TYPE (msg))
          {
          case GST_MESSAGE_INFO:
            gst_message_parse_info (msg, &error, &debug_info);
            g_debug ("CcMediaFactory: INFO: %s, debug info: %s", error->message, debug_info);
            break;

          case GST_MESSAGE_WARNING:
            gst_message_parse_warning (msg, &error, &debug_info);
            g_debug ("CcMediaFactory: WARNING: %s, debug info: %s", error->message, debug_info);
            break;

          case GST_MESSAGE_ERROR:
            gst_message_parse_error (msg, &error, &debug_info);
            g_debug ("CcMediaFactory: ERROR: %s, debug info: %s", error->message, debug_info);
            break;

          default:
            g_assert_not_reached ();
          }

        if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR)
          {
            GError *_error = g_error_new (CC_ERROR,
                                          CC_ERROR_GST_PIPELINE_FAULT,
                                          "Pipeline fault: %s",
                                          error ? error->message : "none");
            multisocketsink = gst_bin_get_by_name (GST_BIN (self->pipeline), "cc-multisocketsink");
            g_signal_emit_by_name (multisocketsink, "clear");
            gst_object_unref (multisocketsink);

            cc_media_factory_set_pipeline_state (self, GST_STATE_NULL);
            g_signal_emit_by_name (self, "end-stream", _error);
            g_clear_error (&_error);
          }

        g_clear_error (&error);
        g_clear_pointer (&debug_info, g_free);

        break;
      }

    case GST_MESSAGE_REQUEST_STATE:
      {
        GstState requested_state;
        gst_message_parse_request_state (msg, &requested_state);

        g_debug ("CcMediaFactory: State change to %s was requested by %s",
                 gst_element_state_get_name (requested_state),
                 GST_MESSAGE_SRC_NAME (msg));

        cc_media_factory_set_pipeline_state (self, requested_state);
        break;
      }

    case GST_MESSAGE_LATENCY:
      {
        g_debug ("CcMediaFactory: Redistributing latency");
        gst_bin_recalculate_latency (GST_BIN (self->pipeline));
        break;
      }

    default:
      break;
    }

  return TRUE;
}

gboolean
cc_media_factory_create_pipeline (CcMediaFactory *self)
{
  GstBus *bus;

  self->pipeline = gst_pipeline_new ("nd-cc-pipeline");
  cc_media_factory_create_element (self);

  bus = gst_element_get_bus (self->pipeline);
  gst_bus_add_signal_watch (bus);
  gst_object_unref (bus);

  g_signal_connect (G_OBJECT (bus),
                    "message",
                    G_CALLBACK (cc_media_factory_gst_bus_message_cb),
                    self);

  return cc_media_factory_set_pipeline_state (self, GST_STATE_READY);
}

CcMediaFactory *
cc_media_factory_new (void)
{
  return g_object_new (CC_TYPE_MEDIA_FACTORY, NULL);
}

void
cc_media_factory_finalize (GObject *object)
{
  CcMediaFactory *self;
  GstBus *bus;
  GstState state;

  self = CC_MEDIA_FACTORY (object);

  g_debug ("CcMediaFactory: Finalizing");

  if (GST_IS_ELEMENT (self->pipeline))
    {
      bus = gst_element_get_bus (self->pipeline);
      gst_bus_remove_signal_watch (bus);
      gst_object_unref (bus);

      gst_element_get_state (self->pipeline, &state, NULL, 100 * GST_MSECOND);
      if (state != GST_STATE_NULL)
        cc_media_factory_set_pipeline_state (self, GST_STATE_NULL);

      gst_object_unref (self->pipeline);
    }

  G_OBJECT_CLASS (cc_media_factory_parent_class)->finalize (object);
}

static gboolean
cc_gst_element_present (CcGstElement element)
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

  encoder_factory = gst_element_factory_find (cc_gst_elements[element]);

  if (encoder_factory != NULL)
    {
      g_debug ("CcMediaFactory: Found %s gst element.", cc_gst_elements[element]);
      found_elements[element] = 1;
      return TRUE;
    }

  found_elements[element] = -1;
  return FALSE;
}

static gint
cc_gst_media_profile_present (CcMediaProfile media_profile)
{
  gint factory_profile;

  g_debug ("CcMediaFactory: Checking profile %d", media_profile);

  if (media_profile < 0 || media_profile >= PROFILE_LAST)
    return -1;

  for (factory_profile = 0; factory_profile < (sizeof (cc_media_factory_profiles) / sizeof (cc_media_factory_profiles[0])); ++factory_profile)
    {
      if (cc_media_factory_profiles[factory_profile].media_profile == media_profile &&
          cc_gst_element_present (cc_media_factory_profiles[factory_profile].video_encoder) &&
          cc_gst_element_present (cc_media_factory_profiles[factory_profile].audio_encoder) &&
          cc_gst_element_present (cc_media_factory_profiles[factory_profile].muxer))
        return factory_profile;
    }

  return -1;
}

gboolean
cc_media_factory_lookup_encoders (CcMediaFactory *self,
                                  CcMediaProfile  media_profile,
                                  GStrv          *missing_video,
                                  GStrv          *missing_audio)
{
  gint factory_profile;

  /* check for particular profile */
  if (media_profile != PROFILE_LAST)
    {
      factory_profile = cc_gst_media_profile_present (media_profile);
      if (factory_profile == -1)
        goto missing_elements;

      self->factory_profile = factory_profile;
      return TRUE;
    }

  /* PROFILE_HIGH_H264 */
  if ((factory_profile = cc_gst_media_profile_present (PROFILE_HIGH_H264)) != -1)
    {
      self->factory_profile = factory_profile;
      g_debug ("CcMediaFactory: Selected media profile PROFILE_HIGH_H264. Set factory profile to %d", factory_profile);
      return TRUE;
    }

  /* PROFILE_BASE_VP8 */
  if ((factory_profile = cc_gst_media_profile_present (PROFILE_BASE_VP8)) != -1)
    {
      self->factory_profile = factory_profile;
      g_debug ("CcMediaFactory: Selected media profile PROFILE_BASE_VP8. Set factory profile to %d", factory_profile);
      return TRUE;
    }

  /* PROFILE_AUDIO_VORBIS */
  if ((factory_profile = cc_gst_media_profile_present (PROFILE_AUDIO_VORBIS)) != -1)
    {
      self->factory_profile = factory_profile;
      g_warning ("CcMediaFactory: Selected audio-only media profile PROFILE_AUDIO_VORBIS. Setting factory profile to %d", factory_profile);
      return TRUE;
    }

  /* PROFILE_AUDIO_OPUS */
  if ((factory_profile = cc_gst_media_profile_present (PROFILE_AUDIO_OPUS)) != -1)
    {
      self->factory_profile = factory_profile;
      g_warning ("CcMediaFactory: Selected audio-only media profile PROFILE_AUDIO_OPUS. Setting factory profile to %d", factory_profile);
      return TRUE;
    }

  /* PROFILE_AUDIO_AAC */
  if ((factory_profile = cc_gst_media_profile_present (PROFILE_AUDIO_AAC)) != -1)
    {
      self->factory_profile = factory_profile;
      g_warning ("CcMediaFactory: Selected audio-only media profile PROFILE_AUDIO_AAC. Setting factory profile to %d", factory_profile);
      return TRUE;
    }

  /* no usage profile found */
missing_elements:
  switch (media_profile)
    {
    case PROFILE_LAST:
    case PROFILE_HIGH_H264:
      if (!cc_gst_element_present (ELEMENT_X264))
        {
          gchar *missing[2] = { (gchar *) cc_gst_elements[ELEMENT_X264], NULL };
          *missing_video = (GStrv) g_strdupv (missing);
        }
      else if (!cc_gst_element_present (ELEMENT_MATROSKA))
        {
          gchar *missing[2] = { (gchar *) cc_gst_elements[ELEMENT_MATROSKA], NULL };
          *missing_video = (GStrv) g_strdupv (missing);
        }

      break;

    case PROFILE_BASE_VP8:
      if (!cc_gst_element_present (ELEMENT_VP8))
        {
          gchar *missing[2] = { (gchar *) cc_gst_elements[ELEMENT_VP8], NULL };
          *missing_video = (GStrv) g_strdupv (missing);
        }
      else if (!cc_gst_element_present (ELEMENT_WEBM))
        {
          gchar *missing[2] = { (gchar *) cc_gst_elements[ELEMENT_WEBM], NULL };
          *missing_video = (GStrv) g_strdupv (missing);
        }

      break;

    default:
      break;
    }

  if (!cc_gst_element_present (ELEMENT_VORBIS) &&
      !cc_gst_element_present (ELEMENT_OPUS) &&
      !cc_gst_element_present (ELEMENT_AAC_FDK))
    {
      gchar *missing[4] = { (gchar *) cc_gst_elements[ELEMENT_VORBIS],
                            (gchar *) cc_gst_elements[ELEMENT_OPUS],
                            (gchar *) cc_gst_elements[ELEMENT_AAC_FDK],
                            NULL };
      *missing_audio = (GStrv) g_strdupv (missing);
    }

  return FALSE;
}

static void
cc_media_factory_init (CcMediaFactory *self)
{
}

static void
cc_media_factory_class_init (CcMediaFactoryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  signals[SIGNAL_CREATE_SOURCE] =
    g_signal_new ("create-source", CC_TYPE_MEDIA_FACTORY, G_SIGNAL_RUN_LAST,
                  0,
                  g_signal_accumulator_first_wins, NULL,
                  NULL,
                  GST_TYPE_ELEMENT, 0);

  signals[SIGNAL_CREATE_AUDIO_SOURCE] =
    g_signal_new ("create-audio-source", CC_TYPE_MEDIA_FACTORY, G_SIGNAL_RUN_LAST,
                  0,
                  g_signal_accumulator_first_wins, NULL,
                  NULL,
                  GST_TYPE_ELEMENT, 0);

  signals[SIGNAL_END_STREAM] =
    g_signal_new ("end-stream", CC_TYPE_MEDIA_FACTORY, G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE,
                  1, G_TYPE_ERROR);

  object_class->finalize = cc_media_factory_finalize;
}
