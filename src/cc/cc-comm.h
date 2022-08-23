/* cc-comm.h
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

#include <json-glib-1.0/json-glib/json-glib.h>
#include <glib-object.h>
#include "cast_channel.pb-c.h"
#include "cc-json-helper.h"
#include "cc-common.h"

G_BEGIN_DECLS

// #define CC_MAX_MSG_SIZE (64 * 1024) // 64KB
// #define CC_MAX_MESSAGE_TIMEOUT (20) // 20 seconds
// // this might pose a problem when there are two gnd applications around
// #define CC_DEFAULT_SENDER_ID "sender-gnd"
// #define CC_DEFAULT_RECEIVER_ID "receiver-0"
// #define CC_MIRRORING_APP_ID "0F5096E8"

// #define CC_NAMESPACE_AUTH "urn:x-cast:com.google.cast.tp.deviceauth"
// #define CC_NAMESPACE_CONNECTION "urn:x-cast:com.google.cast.tp.connection"
// #define CC_NAMESPACE_HEARTBEAT "urn:x-cast:com.google.cast.tp.heartbeat"
// #define CC_NAMESPACE_RECEIVER "urn:x-cast:com.google.cast.receiver"
// #define CC_NAMESPACE_MEDIA "urn:x-cast:com.google.cast.media"
// #define CC_NAMESPACE_WEBRTC "urn:x-cast:com.google.cast.webrtc"

struct _CcCommClosure
{
  gpointer userdata;
  void (*message_received_cb) (struct _CcCommClosure *closure,
                               Cast__Channel__CastMessage *message);
  void (*fatal_error_cb) (struct _CcCommClosure *closure, GError **error);
};

typedef struct _CcCommClosure CcCommClosure;

struct _CcComm
{
  /*< public >*/
  GIOStream    *con;

  guint8       *header_buffer;
  guint8       *message_buffer;

  GCancellable *cancellable;

  CcCommClosure    *closure;
};

typedef struct _CcComm CcComm;

typedef enum {
  CC_MESSAGE_TYPE_AUTH,
  CC_MESSAGE_TYPE_CONNECT,
  CC_MESSAGE_TYPE_DISCONNECT,
  CC_MESSAGE_TYPE_PING,
  CC_MESSAGE_TYPE_PONG,
  CC_MESSAGE_TYPE_RECEIVER,
  CC_MESSAGE_TYPE_MEDIA,
  CC_MESSAGE_TYPE_WEBRTC,
} CcMessageType;

// typedef enum {
//   CC_RWAIT_TYPE_NONE                 = 0b0,
//   CC_RWAIT_TYPE_GET_APP_AVAILABILITY = 0b1 << 0, /* key is `responseType` */
//   CC_RWAIT_TYPE_LAUNCH_ERROR         = 0b1 << 1, /* all other keys are `type` */
//   CC_RWAIT_TYPE_ANSWER               = 0b1 << 2,
//   CC_RWAIT_TYPE_RECEIVER_STATUS      = 0b1 << 3,
//   CC_RWAIT_TYPE_MEDIA_STATUS         = 0b1 << 4,
//   CC_RWAIT_TYPE_PING                 = 0b1 << 5,
//   CC_RWAIT_TYPE_PONG                 = 0b1 << 6,
//   CC_RWAIT_TYPE_CLOSE                = 0b1 << 7,
//   CC_RWAIT_TYPE_UNKNOWN              = 0b1 << 8,
// } CcReceivedMessageType;

// // typedef CcReceivedMessageType CcWaitingFor;
// #define CcWaitingFor CcReceivedMessageType

// typedef enum {
//   CC_WAITING_FOR_NOTHING         =  0b0,
//   CC_WAITING_FOR_APP_AVAILABLE   =  0b1 << 0,
//   CC_WAITING_FOR_BROADCAST       =  0b1 << 1,
//   CC_WAITING_FOR_STATUS          =  0b1 << 2,
//   CC_WAITING_FOR_ANSWER          =  0b1 << 3,
//   CC_WAITING_FOR_PONG            =  0b1 << 4,
// } CcWaitingFor;

gboolean cc_comm_make_connection (CcComm  *comm,
                                  gchar   *remote_address,
                                  GError **error);

void cc_comm_close_connection (CcComm *comm);

gboolean cc_comm_send_request (CcComm       *comm,
                               gchar        *destination_id,
                               CcMessageType message_type,
                               gchar        *utf8_payload,
                               GError      **error);

G_END_DECLS // TODO: separate out cc stuff in a different header file
