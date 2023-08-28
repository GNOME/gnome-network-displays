#pragma once

#include <glib-object.h>
#include <gst/gst.h>
#include "cc-common.h"

G_BEGIN_DECLS

#define CC_TYPE_MEDIA_FACTORY (cc_media_factory_get_type ())
G_DECLARE_FINAL_TYPE (CcMediaFactory, cc_media_factory, CC, MEDIA_FACTORY, GObject)

/* future profiles might have 10 bit video and other codecs like H.265 */
typedef enum {
  /* video + audio profiles */
  PROFILE_BASE_VP8,
  PROFILE_BASE_H264,

  /* audio only profiles */
  PROFILE_AUDIO_AAC,
  PROFILE_AUDIO_VORBIS,
  PROFILE_AUDIO_OPUS,

  PROFILE_LAST,
} CcMediaProfile;

/* TODO: categorise profiles in Chromecast device types */
/* vp8, h264, vaapivp8, vappih264 */

typedef enum {
  ELEMENT_VP8,
  ELEMENT_X264,
  ELEMENT_VIDEO_NONE,

  ELEMENT_AAC_FDK,
  ELEMENT_AAC_AVENC,
  ELEMENT_AAC_FAAC,
  ELEMENT_VORBIS,
  ELEMENT_OPUS,
  ELEMENT_AUDIO_NONE,

  ELEMENT_WEBM,
  ELEMENT_MATROSKA,
  ELEMENT_MP4,

  ELEMENT_NONE,
} CcGstElement;

typedef struct {
  CcMediaProfile profile;
  CcGstElement video_encoder;
  CcGstElement audio_encoder;
  CcGstElement muxer;
} CcMediaProfileInfo;

/* TODO: looks like the problem is vp8/its profile.
  webmmux and vorbis are supported */

/* video encoder, audio encoder, muxer */
static const CcMediaProfileInfo cc_media_profiles[] = {
  {PROFILE_BASE_VP8,     ELEMENT_VP8,        ELEMENT_VORBIS,     ELEMENT_WEBM},
  {PROFILE_BASE_VP8,     ELEMENT_VP8,        ELEMENT_VORBIS,     ELEMENT_MATROSKA},
  {PROFILE_BASE_VP8,     ELEMENT_VP8,        ELEMENT_OPUS,       ELEMENT_WEBM},
  {PROFILE_BASE_VP8,     ELEMENT_VP8,        ELEMENT_OPUS,       ELEMENT_MATROSKA},
  {PROFILE_BASE_VP8,     ELEMENT_VP8,        ELEMENT_AUDIO_NONE, ELEMENT_WEBM},
  {PROFILE_BASE_VP8,     ELEMENT_VP8,        ELEMENT_AUDIO_NONE, ELEMENT_MATROSKA},
  {PROFILE_BASE_VP8,     ELEMENT_VP8,        ELEMENT_AAC_AVENC,  ELEMENT_MP4},
  {PROFILE_BASE_VP8,     ELEMENT_VP8,        ELEMENT_AAC_FAAC,   ELEMENT_MP4},
  {PROFILE_BASE_VP8,     ELEMENT_VP8,        ELEMENT_AAC_FAAC,   ELEMENT_MP4},
  {PROFILE_BASE_VP8,     ELEMENT_VP8,        ELEMENT_OPUS,       ELEMENT_MP4},
  {PROFILE_BASE_VP8,     ELEMENT_VP8,        ELEMENT_AUDIO_NONE, ELEMENT_MP4},
  {PROFILE_BASE_H264,    ELEMENT_X264,       ELEMENT_AAC_FDK,    ELEMENT_MATROSKA},
  {PROFILE_BASE_H264,    ELEMENT_X264,       ELEMENT_AAC_AVENC,  ELEMENT_MATROSKA},
  {PROFILE_BASE_H264,    ELEMENT_X264,       ELEMENT_AAC_FAAC,   ELEMENT_MATROSKA},
  {PROFILE_BASE_H264,    ELEMENT_X264,       ELEMENT_OPUS,       ELEMENT_MATROSKA},
  {PROFILE_BASE_H264,    ELEMENT_X264,       ELEMENT_AUDIO_NONE, ELEMENT_MATROSKA},
  /* TODO: needs to be tested
  {PROFILE_BASE_H264,    ELEMENT_X264,       ELEMENT_AAC_AVENC,  ELEMENT_MP4},
  {PROFILE_BASE_H264,    ELEMENT_X264,       ELEMENT_AAC_FAAC,   ELEMENT_MP4},
  {PROFILE_BASE_H264,    ELEMENT_X264,       ELEMENT_AAC_FAAC,   ELEMENT_MP4},
  {PROFILE_BASE_H264,    ELEMENT_X264,       ELEMENT_OPUS,       ELEMENT_MP4},
  {PROFILE_BASE_H264,    ELEMENT_X264,       ELEMENT_AUDIO_NONE, ELEMENT_MP4},
  */
  {PROFILE_AUDIO_AAC,    ELEMENT_VIDEO_NONE, ELEMENT_AAC_FDK,    ELEMENT_MATROSKA},
  {PROFILE_AUDIO_AAC,    ELEMENT_VIDEO_NONE, ELEMENT_AAC_AVENC,  ELEMENT_MATROSKA},
  {PROFILE_AUDIO_AAC,    ELEMENT_VIDEO_NONE, ELEMENT_AAC_FAAC,   ELEMENT_MATROSKA},
  {PROFILE_AUDIO_VORBIS, ELEMENT_VIDEO_NONE, ELEMENT_VORBIS,     ELEMENT_WEBM},
  {PROFILE_AUDIO_VORBIS, ELEMENT_VIDEO_NONE, ELEMENT_VORBIS,     ELEMENT_MATROSKA},
  {PROFILE_AUDIO_OPUS,   ELEMENT_VIDEO_NONE, ELEMENT_OPUS,       ELEMENT_WEBM},
  {PROFILE_AUDIO_OPUS,   ELEMENT_VIDEO_NONE, ELEMENT_OPUS,       ELEMENT_MATROSKA},
};

/* TODO:
typedef enum {
  CC_QUIRK_NO_IDR = 0x01,
} CcMediaQuirks;
*/

struct _CcMediaFactory
{
  GObject     parent_instance;

  GstElement *pipeline;
  gint        selected_profile;
};

typedef struct _CcMediaFactory CcMediaFactory;

CcMediaFactory * cc_media_factory_new (void);

gboolean
cc_media_factory_lookup_encoders (CcMediaFactory *self,
                                  CcMediaProfile  profile,
                                  GStrv          *missing_video,
                                  GStrv          *missing_audio);

gboolean cc_media_factory_set_pipeline_state (CcMediaFactory *self,
                                              GstState state);
gboolean cc_media_factory_create_pipeline (CcMediaFactory *self);
/* TODO: do this in the renegotiation step or if there are plans for on-the-fly changes (unlikely) */
/* void cc_configure_media_element (GstBin *bin, CcParams *params); */
void cc_media_factory_finalize (GObject *object);

G_END_DECLS
