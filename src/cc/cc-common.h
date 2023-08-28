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

/* TODO: reimplement the timeout for when no message is received from the other
 * device. Now that read is truly async, this should work without any hiccups
 */
/* #define CC_MAX_MESSAGE_TIMEOUT (20) */ /* 20 seconds */

#define CC_MAX_MSG_SIZE (64 * 1024) /* 64KB */
/* all G-N-D applications have this prefix for sender id */
#define CC_DEFAULT_SENDER_ID "sender-gnd"
#define CC_DEFAULT_RECEIVER_ID "receiver-0"
#define CC_APP_ID "CC1AD845"

#define CC_NAMESPACE_AUTH "urn:x-cast:com.google.cast.tp.deviceauth"
#define CC_NAMESPACE_CONNECTION "urn:x-cast:com.google.cast.tp.connection"
#define CC_NAMESPACE_HEARTBEAT "urn:x-cast:com.google.cast.tp.heartbeat"
#define CC_NAMESPACE_RECEIVER "urn:x-cast:com.google.cast.receiver"
#define CC_NAMESPACE_MEDIA "urn:x-cast:com.google.cast.media"

#define CC_ERROR 1

typedef enum {
  CC_ERROR_TLS_READ_FAILED = 2,
  CC_ERROR_HANDSHAKE_FAILED,
  CC_ERROR_NO_TLS_CONN,
  CC_ERROR_TLS_WRITE_FAILED,

  CC_ERROR_GST_PIPELINE_FAULT,
  CC_ERROR_GST_PIPELINE_CREATION_FAILED,
  CC_ERROR_GST_PIPELINE_SET_STATE_FAILED,

  CC_ERROR_MESSAGE_SEND_FAILED,
  CC_ERROR_HTTP_SERVER_LISTEN_FAILED,
  CC_ERROR_APP_UNAVAILABLE,
  CC_ERROR_APP_COMMUNICATION_FAILED,

  CC_ERROR_LAUNCH_ERROR_MSG_RECEIVED,
  CC_ERROR_ERROR_MSG_RECEIVED,
  CC_ERROR_LOAD_FAILED_MSG_RECEIVED,
} CcError;

typedef enum {
  CC_REC_MSG_TYPE_GET_APP_AVAILABILITY, /* key is `responseType` */
  CC_REC_MSG_TYPE_LAUNCH_ERROR, /* all other keys are `type` */
  CC_REC_MSG_TYPE_ERROR,
  CC_REC_MSG_TYPE_LOAD_FAILED,
  CC_REC_MSG_TYPE_RECEIVER_STATUS,
  CC_REC_MSG_TYPE_MEDIA_STATUS,
  CC_REC_MSG_TYPE_PING,
  CC_REC_MSG_TYPE_PONG,
  CC_REC_MSG_TYPE_CLOSE,
  CC_REC_MSG_TYPE_UNKNOWN,

  CC_REC_MSG_SIZE = CC_REC_MSG_TYPE_UNKNOWN,
} CcReceivedMessageTypeEnum;

static const char * const CcReceivedMessageTypeStrings[] =
{
  [CC_REC_MSG_TYPE_GET_APP_AVAILABILITY] = "GET_APP_AVAILABILITY",
  [CC_REC_MSG_TYPE_LAUNCH_ERROR] = "LAUNCH_ERROR",
  [CC_REC_MSG_TYPE_ERROR] = "ERROR",
  [CC_REC_MSG_TYPE_LOAD_FAILED] = "LOAD_FAILED",
  [CC_REC_MSG_TYPE_RECEIVER_STATUS] = "RECEIVER_STATUS",
  [CC_REC_MSG_TYPE_MEDIA_STATUS] = "MEDIA_STATUS",
  [CC_REC_MSG_TYPE_PING] = "PING",
  [CC_REC_MSG_TYPE_PONG] = "PONG",
  [CC_REC_MSG_TYPE_CLOSE] = "CLOSE",
};

G_END_DECLS
