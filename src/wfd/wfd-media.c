#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "gst/rtsp-server/rtsp-media.h"
#pragma GCC diagnostic pop
#include "wfd-media.h"

struct _WfdMedia
{
  GstRTSPMedia parent_instance;
};

G_DEFINE_TYPE (WfdMedia, wfd_media, GST_TYPE_RTSP_MEDIA)

WfdMedia *
wfd_media_new (void)
{
  return g_object_new (WFD_TYPE_MEDIA, NULL);
}

static void
wfd_media_finalize (GObject *object)
{
  g_debug ("WfdMedia: Finalize");

  G_OBJECT_CLASS (wfd_media_parent_class)->finalize (object);
}

static gboolean
wfd_media_setup_rtpbin (GstRTSPMedia *media, GstElement *rtpbin)
{
  g_object_set (rtpbin,
                "rtp-profile", 1, /* avp */
                "do-retransmission", TRUE,
                "ntp-time-source", 3, /* clock time */
                "max-misorder-time", 50,
                "buffer-mode", 0,
                "latency", 40,
                NULL);

  return TRUE;
}

static void
wfd_media_class_init (WfdMediaClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstRTSPMediaClass *media_class = GST_RTSP_MEDIA_CLASS (klass);

  object_class->finalize = wfd_media_finalize;

  media_class->setup_rtpbin = wfd_media_setup_rtpbin;
}

static void
wfd_media_init (WfdMedia *self)
{
  gst_rtsp_media_set_stop_on_disconnect (GST_RTSP_MEDIA (self), TRUE);
}
