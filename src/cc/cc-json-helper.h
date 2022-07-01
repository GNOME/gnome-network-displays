/* cc-json-helper.h
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
#include <json-glib-1.0/json-glib/json-glib.h>
#include "cast_channel.pb-c.h"
#include "cc-common.h"

G_BEGIN_DECLS

typedef enum {
  CC_JSON_TYPE_STRING,
  CC_JSON_TYPE_INT,
  CC_JSON_TYPE_DOUBLE,
  CC_JSON_TYPE_BOOLEAN,
  CC_JSON_TYPE_NULL,
  CC_JSON_TYPE_OBJECT,

  CC_JSON_TYPE_ARRAY_STRING,
  CC_JSON_TYPE_ARRAY_INT,
  CC_JSON_TYPE_ARRAY_DOUBLE,
  CC_JSON_TYPE_ARRAY_BOOLEAN,
  CC_JSON_TYPE_ARRAY_NULL,
  CC_JSON_TYPE_ARRAY_OBJECT,
} CcJsonType;


/* G_GNUC_NULL_TERMINATED */

JsonNode * cc_json_helper_build_node (gchar *first_key,
                                      ...);
gchar * cc_json_helper_build_string (/* gboolean pretty_print, */ gchar *first_key,
                                     ...);
gchar * cc_json_helper_node_to_string (JsonNode *node);
CcReceivedMessageTypeEnum cc_json_helper_get_message_type (Cast__Channel__CastMessage *message,
                                                           JsonReader                 *json_reader);
void cc_json_helper_dump_message (Cast__Channel__CastMessage *message,
                                  gboolean                    borked);

G_END_DECLS
