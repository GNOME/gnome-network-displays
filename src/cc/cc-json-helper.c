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
      CcJsonType type;
      g_autoptr(GArray) arr = NULL;

      json_builder_set_member_name (builder, key);
      type = va_arg (var_args, CcJsonType);

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
          break;
        }

      json_builder_begin_array (builder);
      arr = va_arg (var_args, GArray *);

      for (guint i = 0; i < arr->len; i++)
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
              g_assert_not_reached ();
            }
        }

      json_builder_end_array (builder);
    }
}

JsonNode *
cc_json_helper_build_node (gchar *first_key,
                           ...)
{
  JsonBuilder *builder;
  JsonNode *node;

  va_list var_args;

  va_start (var_args, first_key);

  builder = json_builder_new ();

  json_builder_begin_object (builder);
  cc_json_helper_build_internal (builder, first_key, var_args);
  json_builder_end_object (builder);

  node = json_builder_get_root (builder);

  va_end (var_args);
  g_object_unref (builder);

  return g_steal_pointer (&node);
}

gchar *
cc_json_helper_build_string (/* gboolean pretty_print, */
  gchar *first_key,
  ...)
{
  JsonBuilder *builder;
  JsonNode *root;
  JsonGenerator *gen;
  gchar *output;

  va_list var_args;

  va_start (var_args, first_key);

  builder = json_builder_new ();

  json_builder_begin_object (builder);
  cc_json_helper_build_internal (builder, first_key, var_args);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  gen = json_generator_new ();

  /* json_generator_set_pretty (gen, pretty_print); */
  json_generator_set_root (gen, root);
  output = json_generator_to_data (gen, NULL);

  va_end (var_args);
  json_node_free (root);
  g_object_unref (gen);
  g_object_unref (builder);

  return g_steal_pointer (&output);
}

gchar *
cc_json_helper_node_to_string (JsonNode *node)
{
  gchar *output;
  JsonGenerator *gen = json_generator_new ();

  json_generator_set_root (gen, node);
  output = json_generator_to_data (gen, NULL);

  g_object_unref (gen);

  return g_steal_pointer (&output);
}

CcReceivedMessageTypeEnum
cc_json_helper_get_message_type (Cast__Channel__CastMessage *message,
                                 JsonReader                 *reader)
{
  const gchar *message_type;

  if (json_reader_read_member (reader, "type"))
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

  for (int i = CC_REC_MSG_TYPE_GET_APP_AVAILABILITY; i < CC_REC_MSG_SIZE; ++i)
    if (g_strcmp0 (message_type, CcReceivedMessageTypeStrings[i]) == 0)
      return i;

  return CC_REC_MSG_TYPE_UNKNOWN;
}

/* borked var reduces extra computation */
void
cc_json_helper_dump_message (Cast__Channel__CastMessage *message, gboolean borked)
{
  JsonParser *parser = json_parser_new ();
  JsonNode *payload_utf8_node;
  gchar *output;

  if (borked || !json_parser_load_from_data (parser, message->payload_utf8, -1, NULL))
    {
      output = cc_json_helper_build_string (
        "source_id", CC_JSON_TYPE_STRING, message->source_id,
        "destination_id", CC_JSON_TYPE_STRING, message->destination_id,
        "namespace", CC_JSON_TYPE_STRING, message->namespace_,
        "payload_utf8", CC_JSON_TYPE_STRING, g_strdup (message->payload_utf8),
        NULL);
    }
  else
    {
      payload_utf8_node = json_parser_get_root (parser);

      output = cc_json_helper_build_string (
        "source_id", CC_JSON_TYPE_STRING, message->source_id,
        "destination_id", CC_JSON_TYPE_STRING, message->destination_id,
        "namespace", CC_JSON_TYPE_STRING, message->namespace_,
        "payload_utf8", CC_JSON_TYPE_OBJECT, payload_utf8_node,
        NULL);
    }

  g_debug ("%s", output);
}
