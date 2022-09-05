/* cc-common.h
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

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define CC_MAX_MSG_SIZE (64 * 1024) /* 64KB */
#define CC_MAX_MESSAGE_TIMEOUT (20) /* 20 seconds */
/* this might pose a problem when there are two gnd applications around */
#define CC_DEFAULT_SENDER_ID "sender-gnd"
#define CC_DEFAULT_RECEIVER_ID "receiver-0"
#define CC_MIRRORING_APP_ID "0F5096E8"

#define CC_NAMESPACE_AUTH "urn:x-cast:com.google.cast.tp.deviceauth"
#define CC_NAMESPACE_CONNECTION "urn:x-cast:com.google.cast.tp.connection"
#define CC_NAMESPACE_HEARTBEAT "urn:x-cast:com.google.cast.tp.heartbeat"
#define CC_NAMESPACE_RECEIVER "urn:x-cast:com.google.cast.receiver"
#define CC_NAMESPACE_MEDIA "urn:x-cast:com.google.cast.media"
#define CC_NAMESPACE_WEBRTC "urn:x-cast:com.google.cast.webrtc"

/* string switch case */
#define cc_switch(x) \
  const gchar *to_cmp = x; \
  do \

#define cc_case(x) if (g_strcmp0 (to_cmp, x) == 0)

#define cc_end while (0);


typedef enum {
  CC_RWAIT_TYPE_NONE                 = 0,
  CC_RWAIT_TYPE_GET_APP_AVAILABILITY = 1 << 0, /* key is `responseType` */
  CC_RWAIT_TYPE_LAUNCH_ERROR         = 1 << 1, /* all other keys are `type` */
  CC_RWAIT_TYPE_ANSWER               = 1 << 2,
  CC_RWAIT_TYPE_RECEIVER_STATUS      = 1 << 3,
  CC_RWAIT_TYPE_MEDIA_STATUS         = 1 << 4,
  CC_RWAIT_TYPE_PING                 = 1 << 5,
  CC_RWAIT_TYPE_PONG                 = 1 << 6,
  CC_RWAIT_TYPE_CLOSE                = 1 << 7,
  CC_RWAIT_TYPE_UNKNOWN              = 1 << 8,
} CcReceivedMessageType;

typedef CcReceivedMessageType CcWaitingFor;

typedef struct _Stream
{
  gint index;

  /* Default channel count is 1, e.g. for video. */
  gint channels;
  gint rtp_payload_type;
  gint ssrc;
  gint target_delay;

  /*
   * AES Key and IV mask format is very strict: a 32 digit hex string that
   * must be converted to a 16 digit byte array.
   * uint8_t aes_key[16];
   * uint8_t aes_iv_mask[16];
   */
  gchar   *aes_iv_mask;
  gchar   *aes_key;
  gboolean receiver_rtcp_event_log;
  gchar   *receiver_rtcp_dscp;
  /* gint rtp_timebase; */
  gchar   *rtp_timebase;

  /* The codec parameter field honors the format laid out in RFC 6381:
   * https://datatracker.ietf.org/doc/html/rfc6381.
   */
  gchar *codec_parameter;
} Stream;

typedef struct _AudioStream
{
  Stream stream;

  gchar *codec;
  gint   bit_rate;
  gint   sample_rate;
  gchar *profile;
} AudioStream;

typedef struct _VideoStream
{
  Stream stream;

  gchar *codec;
  gchar *max_frame_rate;
  gint   max_bit_rate;
  gchar *protection;
  gchar *profile;
  gchar *level;
  gint **resolutions;
  gchar *error_recovery_mode;
} VideoStream;

typedef struct _Offer
{
  gchar      *cast_mode;
  guint       seq_num;
  gboolean    receiver_get_status;

  AudioStream audio_stream;
  VideoStream video_stream;
} Offer;

G_END_DECLS
