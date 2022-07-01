/* cc-ctrl.h
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
#include "cc-comm.h"
#include "cc-json-helper.h"
#include "cc-common.h"
#include "cc-http-server.h"

G_BEGIN_DECLS

typedef enum {
  CC_CTRL_STATE_DISCONNECTED,
  CC_CTRL_STATE_CONNECTED,
  CC_CTRL_STATE_LAUNCH_SENT,
  CC_CTRL_STATE_APP_OPEN,
  CC_CTRL_STATE_READY,
  CC_CTRL_STATE_STREAMING,
  CC_CTRL_STATE_ERROR,
} CcCtrlState;

struct _CcCtrlClosure
{
  gpointer userdata;
  void     (*end_stream) (gpointer userdata,
                          GError  *error);
};

typedef struct _CcCtrlClosure CcCtrlClosure;

struct _CcCtrl
{
  /*< public >*/
  CcComm         comm;
  CcHttpServer  *http_server;
  GCancellable  *cancellable;
  CcCtrlClosure *closure;

  /*< private >*/
  CcCtrlState state;
  gchar      *session_id;
  guint       request_id;
  guint       ping_timeout_handle;
};

typedef struct _CcCtrl CcCtrl;

/* public functions */
gboolean cc_ctrl_connection_init (CcCtrl *ctrl,
                                  gchar  *remote_address);
void cc_ctrl_send_media_request (CcCtrl *ctrl);
void cc_ctrl_finish (CcCtrl *ctrl);

G_END_DECLS
