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

#include <gio/gio.h>

G_BEGIN_DECLS

struct _CcComm
{
  /*< public >*/
  GIOStream *con;
};

typedef struct _CcComm CcComm;

#define MAX_MSG_SIZE 64 * 1024

enum MessageType {
    MESSAGE_TYPE_CONNECT,
    MESSAGE_TYPE_DISCONNECT,
    MESSAGE_TYPE_PING,
    MESSAGE_TYPE_PONG,
    MESSAGE_TYPE_RECEIVER,
};

gboolean cc_comm_make_connection (CcComm *comm, gchar *remote_address, GError **error);
gboolean cc_comm_send_request (CcComm *sink, enum MessageType message_type, char *utf8_payload, GError **error);
gboolean cc_comm_send_ping (CcComm *sink);

G_END_DECLS
