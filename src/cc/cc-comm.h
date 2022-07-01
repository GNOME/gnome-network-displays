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

struct _CcCommClosure
{
  gpointer userdata;
  void     (*message_received_cb) (gpointer                    userdata,
                                   Cast__Channel__CastMessage *message);
  void     (*handshake_completed) (gpointer userdata);
  void     (*error_close_connection_cb) (gpointer userdata,
                                         GError  *error);
};

typedef struct _CcCommClosure CcCommClosure;

struct _CcComm
{
  /*< public >*/
  GIOStream     *con;
  GCancellable  *cancellable;
  CcCommClosure *closure;

  /*< private >*/
  gchar   *sender_id;
  gchar   *local_address;
  GSource *input_source;
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
} CcMessageType;

gboolean cc_comm_make_connection (CcComm  *comm,
                                  gchar   *remote_address,
                                  GError **error);

void cc_comm_finish (CcComm *comm);

gboolean cc_comm_send_request (CcComm       *comm,
                               gchar        *destination_id,
                               CcMessageType message_type,
                               gchar        *utf8_payload);

G_END_DECLS
