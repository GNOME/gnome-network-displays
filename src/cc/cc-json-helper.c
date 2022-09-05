/* cc-json-helper.c
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

#include "cc-json-helper.h"

static void
cc_json_helper_build_internal (JsonBuilder *builder,
                               gchar       *first_key,
                               va_list      var_args)
{
  for (gchar *key = first_key; key != NULL; key = va_arg (var_args, gchar *))
    {
      json_builder_set_member_name (builder, key);
      CcJsonType type = va_arg (var_args, CcJsonType);

      g_assert (type >= CC_JSON_TYPE_STRING && type <= CC_JSON_TYPE_ARRAY_OBJECT);

      switch (type)
        {
        case CC_JSON_TYPE_STRING:
          json_builder_add_string_value (builder, va_arg (var_args, gchar *));
          continue;

        case CC_JSON_TYPE_INT:
          json_builder_add_int_value (builder, va_arg (var_args, gint));
          continue;

        case CC_JSON_TYPE_DOUBLE:
          json_builder_add_double_value (builder, va_arg (var_args, gdouble));
          continue;

        case CC_JSON_TYPE_BOOLEAN:
          json_builder_add_boolean_value (builder, va_arg (var_args, gboolean));
          continue;

        case CC_JSON_TYPE_NULL: /* no additional arg is required here */
          json_builder_add_null_value (builder);
          continue;

        case CC_JSON_TYPE_OBJECT:
          json_builder_add_value (builder, va_arg (var_args, JsonNode *));
          continue;

        default:
          return;
        }

      json_builder_begin_array (builder);
      g_autoptr (GArray) arr = va_arg (var_args, GArray *);
      guint i;

      for (i = 0; i < arr->len; i++)
        {
          switch (type)
            {
            case CC_JSON_TYPE_ARRAY_STRING:
              json_builder_add_string_value (builder, g_array_index (arr, gchar *, i));
              break;

            case CC_JSON_TYPE_ARRAY_INT:
              json_builder_add_int_value (builder, g_array_index (arr, gint, i));
              break;

            case CC_JSON_TYPE_ARRAY_DOUBLE:
              json_builder_add_double_value (builder, g_array_index (arr, gdouble, i));
              break;

            case CC_JSON_TYPE_ARRAY_BOOLEAN:
              json_builder_add_boolean_value (builder, g_array_index (arr, gboolean, i));
              break;

            case CC_JSON_TYPE_ARRAY_NULL:
              json_builder_add_null_value (builder);
              break;

            case CC_JSON_TYPE_ARRAY_OBJECT:
              json_builder_add_value (builder, g_array_index (arr, JsonNode *, i));
              break;

            default:
              return;
            }
        }

      json_builder_end_array (builder);
    }
}

JsonNode *
cc_json_helper_build_node (gchar *first_key,
                           ...)
{
  va_list var_args;

  va_start (var_args, first_key);

  JsonBuilder *builder = json_builder_new ();

  json_builder_begin_object (builder);
  cc_json_helper_build_internal (builder, first_key, var_args);
  json_builder_end_object (builder);

  JsonNode *node = json_builder_get_root (builder);

  va_end (var_args);
  g_object_unref (builder);

  return g_steal_pointer (&node);
}

gchar *
cc_json_helper_build_string (/* gboolean pretty_print, */
  gchar *first_key,
  ...)
{
  va_list var_args;

  va_start (var_args, first_key);

  JsonBuilder *builder = json_builder_new ();

  json_builder_begin_object (builder);
  cc_json_helper_build_internal (builder, first_key, var_args);
  json_builder_end_object (builder);

  JsonNode *root = json_builder_get_root (builder);
  JsonGenerator *gen = json_generator_new ();

  /* json_generator_set_pretty (gen, pretty_print); */
  json_generator_set_root (gen, root);
  gchar *output = json_generator_to_data (gen, NULL);

  va_end (var_args);
  json_node_free (root);
  g_object_unref (gen);
  g_object_unref (builder);

  return g_steal_pointer (&output);
}

gchar *
cc_json_helper_node_to_string (JsonNode *node)
{
  JsonGenerator *gen = json_generator_new ();

  json_generator_set_root (gen, node);
  gchar *output = json_generator_to_data (gen, NULL);

  g_object_unref (gen);

  return g_steal_pointer (&output);
}

CcReceivedMessageType
cc_json_helper_get_message_type (Cast__Channel__CastMessage *message,
                                 JsonReader                 *reader)
{
  const gchar *message_type;

  gboolean typeExists = json_reader_read_member (reader, "type");

  if (typeExists)
    message_type = json_reader_get_string_value (reader);
  else
    {
      json_reader_end_member (reader);
      if (json_reader_read_member (reader, "responseType"))
        message_type = json_reader_get_string_value (reader);
      else
        {
          g_warning ("CcJsonHelper: Error parsing received message JSON: no type or responseType keys");
          cc_json_helper_dump_message (message, TRUE);
          return -1;
        }
    }
  json_reader_end_member (reader);

  cc_switch (message_type)
  {
    cc_case ("RECEIVER_STATUS")
    return CC_RWAIT_TYPE_RECEIVER_STATUS;
    cc_case ("GET_APP_AVAILABILITY")
    return CC_RWAIT_TYPE_GET_APP_AVAILABILITY;
    cc_case ("LAUNCH_ERROR")
    return CC_RWAIT_TYPE_LAUNCH_ERROR;
    cc_case ("ANSWER")
    return CC_RWAIT_TYPE_ANSWER;
    cc_case ("MEDIA_STATUS")
    return CC_RWAIT_TYPE_MEDIA_STATUS;
    cc_case ("PING")
    return CC_RWAIT_TYPE_PING;
    cc_case ("PONG")
    return CC_RWAIT_TYPE_PONG;
    cc_case ("CLOSE")
    return CC_RWAIT_TYPE_CLOSE;
    /* default */
    return CC_RWAIT_TYPE_UNKNOWN;
  } cc_end
}

/* borked var reduces extra computation */
void
cc_json_helper_dump_message (Cast__Channel__CastMessage *message, gboolean borked)
{
  JsonNode *payload_utf8_node;
  JsonParser *parser = json_parser_new ();

  g_autoptr(GError) error = NULL;

  if (borked || !json_parser_load_from_data (parser, message->payload_utf8, -1, &error))
    {
      if (error)
        g_warning ("CcJsonHelper: Error parsing received JSON payload: %s", error->message);
      else
        g_warning ("CcJsonHelper: Error parsing received JSON payload");

      g_debug ("{ source_id: %s, destination_id: %s, namespace_: %s, payload_utf8: %s }",
               message->source_id,
               message->destination_id,
               message->namespace_,
               message->payload_utf8);
      return;
    }

  payload_utf8_node = json_parser_get_root (parser);

  gchar *output = cc_json_helper_build_string (
    "source_id", CC_JSON_TYPE_STRING, message->source_id,
    "destination_id", CC_JSON_TYPE_STRING, message->destination_id,
    "namespace", CC_JSON_TYPE_STRING, message->namespace_,
    "payload_utf8", CC_JSON_TYPE_OBJECT, payload_utf8_node,
    NULL);

  g_debug ("%s", output);
}
