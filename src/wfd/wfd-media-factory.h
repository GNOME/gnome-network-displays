#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <gst/rtsp-server/rtsp-media-factory.h>
#pragma GCC diagnostic pop
#include "wfd-params.h"

G_BEGIN_DECLS

#define WFD_TYPE_MEDIA_FACTORY (wfd_media_factory_get_type ())
G_DECLARE_FINAL_TYPE (WfdMediaFactory, wfd_media_factory, WFD, MEDIA_FACTORY, GstRTSPMediaFactory)

typedef enum {
  /* video + audio profiles */
  PROFILE_HIGH_H264,
  PROFILE_BASE_H264,

  /* audio only profiles */
//PROFILE_AUDIO_AAC,

  PROFILE_LAST,
} WfdMediaProfile;

typedef enum {
  ELEMENT_OPENH264,
  ELEMENT_X264,
  ELEMENT_VAH264,
  ELEMENT_VAAPIH264,
  ELEMENT_VIDEO_NONE,

  ELEMENT_AAC_FDK,
  ELEMENT_AAC_AVENC,
  ELEMENT_AAC_FAAC,
  ELEMENT_AUDIO_NONE,

  ELEMENT_MPEGTS,

  ELEMENT_NONE,
} WfdGstElement;

typedef struct
{
  WfdMediaProfile media_profile;
  WfdGstElement   video_encoder;
  WfdGstElement   audio_encoder;
  WfdGstElement   muxer;
} WfdMediaFactoryProfile;

/* video encoder, audio encoder, muxer */
static const WfdMediaFactoryProfile wfd_media_factory_profiles[] = {
  /* video + audio profiles */
  {PROFILE_HIGH_H264,    ELEMENT_VAH264,     ELEMENT_AAC_FDK,    ELEMENT_MPEGTS},
  {PROFILE_HIGH_H264,    ELEMENT_VAH264,     ELEMENT_AAC_AVENC,  ELEMENT_MPEGTS},
  {PROFILE_HIGH_H264,    ELEMENT_VAH264,     ELEMENT_AAC_FAAC,   ELEMENT_MPEGTS},
  {PROFILE_HIGH_H264,    ELEMENT_VAH264,     ELEMENT_AUDIO_NONE, ELEMENT_MPEGTS},
  {PROFILE_HIGH_H264,    ELEMENT_VAAPIH264,  ELEMENT_AAC_FDK,    ELEMENT_MPEGTS},
  {PROFILE_HIGH_H264,    ELEMENT_VAAPIH264,  ELEMENT_AAC_AVENC,  ELEMENT_MPEGTS},
  {PROFILE_HIGH_H264,    ELEMENT_VAAPIH264,  ELEMENT_AAC_FAAC,   ELEMENT_MPEGTS},
  {PROFILE_HIGH_H264,    ELEMENT_VAAPIH264,  ELEMENT_AUDIO_NONE, ELEMENT_MPEGTS},
  {PROFILE_HIGH_H264,    ELEMENT_X264,       ELEMENT_AAC_FDK,    ELEMENT_MPEGTS},
  {PROFILE_HIGH_H264,    ELEMENT_X264,       ELEMENT_AAC_AVENC,  ELEMENT_MPEGTS},
  {PROFILE_HIGH_H264,    ELEMENT_X264,       ELEMENT_AAC_FAAC,   ELEMENT_MPEGTS},
  {PROFILE_HIGH_H264,    ELEMENT_X264,       ELEMENT_AUDIO_NONE, ELEMENT_MPEGTS},
  {PROFILE_BASE_H264,    ELEMENT_OPENH264,   ELEMENT_AAC_FDK,    ELEMENT_MPEGTS},
  {PROFILE_BASE_H264,    ELEMENT_OPENH264,   ELEMENT_AAC_AVENC,  ELEMENT_MPEGTS},
  {PROFILE_BASE_H264,    ELEMENT_OPENH264,   ELEMENT_AAC_FAAC,   ELEMENT_MPEGTS},
  {PROFILE_BASE_H264,    ELEMENT_OPENH264,   ELEMENT_AUDIO_NONE, ELEMENT_MPEGTS},

  /* TODO: audio-only profiles */
//{PROFILE_AUDIO_AAC,    ELEMENT_VIDEO_NONE, ELEMENT_AAC_FDK,    ELEMENT_MPEGTS},
//{PROFILE_AUDIO_AAC,    ELEMENT_VIDEO_NONE, ELEMENT_AAC_AVENC,  ELEMENT_MPEGTS},
//{PROFILE_AUDIO_AAC,    ELEMENT_VIDEO_NONE, ELEMENT_AAC_FAAC,   ELEMENT_MPEGTS},
};

typedef enum {
  WFD_QUIRK_NO_IDR = 0x01,
} WfdMediaQuirks;

WfdMediaFactory * wfd_media_factory_new (void);

gboolean wfd_media_factory_lookup_encoders (WfdMediaFactory *self,
                                            WfdMediaProfile  media_profile,
                                            GStrv           *missing_video,
                                            GStrv           *missing_audio);

/* Just because it is convenient to have next to the pipeline creation code */
GstElement * wfd_media_factory_create_element (GstRTSPMediaFactory *factory,
                                               const GstRTSPUrl    *url);
GstElement * wfd_media_factory_create_video_element (WfdMediaFactory *self,
                                                     GstBin          *bin);
GstBin * wfd_media_factory_create_audio_element (WfdMediaFactory *self);
WfdMediaQuirks wfd_configure_media_element (GstBin    *bin,
                                            WfdParams *params);
void wfd_media_factory_finalize (GObject *object);

G_END_DECLS
