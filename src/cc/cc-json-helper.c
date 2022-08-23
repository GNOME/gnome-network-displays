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

// static void
// cc_json_helper_add_type_value (JsonBuilder *builder,
//                                CcJsonType   type,
//                                gpointer     value)
// {
//   switch (type)
//     {
//     case CC_JSON_TYPE_STRING:
//       json_builder_add_string_value (builder, (gchar *) *value);
//       break;
//     case CC_JSON_TYPE_INT:
//       json_builder_add_int_value (builder, (gint) *value);
//       break;
//     case CC_JSON_TYPE_DOUBLE:
//       json_builder_add_double_value (builder, (gdouble) *value);
//       break;
//     case CC_JSON_TYPE_BOOLEAN:
//       json_builder_add_boolean_value (builder, (gboolean) *value);
//       break;
//     case CC_JSON_TYPE_NULL: /* no additional arg is required here */
//       json_builder_add_null_value (builder);
//       break;
//     case CC_JSON_TYPE_OBJECT:
//       json_builder_begin_object (builder);
//       json_builder_add_value (builder, (JsonNode *) value);
//       json_builder_end_object (builder);
//       break;
//     /* only 1D arrays supported */
//     }
// }

// void
// cc_json_helper_build_root (JsonBuilder *builder,
//                            const gchar *first_key,
//                            va_list	    var_args)
// {
//   gchar *key = first_key;

//   while (key)
//     {
//       json_builder_set_member_name (builder, key);
//       CcJsonType type = va_arg (var_args, CcJsonType);

//       if (type == CC_JSON_TYPE_ARRAY)
//         {
//           json_builder_begin_array (builder);
//           gint length = va_arg (var_args, gint);
//           for (gint i = 0; i < length; i++)
//             {
//               cc_json_helper_add_type_value (builder, type, va_arg (var_args, gpointer));
//             }
//           json_builder_end_array (builder);
//         }
//       switch (type)
//         {
//         case CC_JSON_TYPE_STRING:
//           json_builder_add_string_value (builder, va_arg (var_args, gchar *));
//           break;
//         case CC_JSON_TYPE_INT:
//           json_builder_add_int_value (builder, va_arg (var_args, gint));
//           break;
//         case CC_JSON_TYPE_DOUBLE:
//           json_builder_add_double_value (builder, va_arg (var_args, gdouble));
//           break;
//         case CC_JSON_TYPE_BOOLEAN:
//           json_builder_add_boolean_value (builder, va_arg (var_args, gboolean));
//           break;
//         case CC_JSON_TYPE_NULL: /* no additional arg is required here */
//           json_builder_add_null_value (builder);
//           break;
//         case CC_JSON_TYPE_OBJECT:
//           json_builder_begin_object (builder);
//           json_builder_add_value (builder, va_arg (var_args, JsonNode *));
//           json_builder_end_object (builder);
//           break;
//         case CC_JSON_TYPE_ARRAY: /* type for array elements is also required here */
//           json_builder_begin_array (builder);
//           CcJsonType array_type = va_arg (var_args, CcJsonType);
//           /* GArray */
//           json_builder_end_array (builder);
//           break;
//         default:
//           output = NULL;
//           return;
//         }

//       key = va_arg (var_args, gchar *);
//     }
// }

// void
// cc_json_helper_build_string (gchar       *output,
//                              const gchar *first_key,
//                              ...)
// {
//   va_list var_args;
//   va_start (var_args, first_key);

//   JsonBuilder *builder = json_builder_new ();

//   json_builder_begin_object (builder);
//   cc_json_helper_build_root (builder, first_key, var_args);
//   json_builder_end_object (builder);

//   JsonGenerator *gen = json_generator_new ();
//   JsonNode *root = json_builder_get_root (builder);
//   json_generator_set_root (gen, root);

//   output = json_generator_to_data (gen, NULL);

//   va_end (var_args);
//   json_node_free (root);
//   g_object_unref (gen);
//   g_object_unref (builder);
// }

// void
// cc_json_helper_build_string (gchar       *output,
//                              const gchar *first_key,
//                              ...)
// {
//   va_list var_args;
//   va_start (var_args, first_key);

//   JsonBuilder *builder = json_builder_new ();

//   json_builder_begin_object (builder);
//   cc_json_helper_build_root (builder, first_key, var_args);
//   json_builder_end_object (builder);

//   JsonGenerator *gen = json_generator_new ();
//   JsonNode *root = json_builder_get_root (builder);
//   json_generator_set_root (gen, root);

//   output = json_generator_to_data (gen, NULL);

//   va_end (var_args);
//   json_node_free (root);
//   g_object_unref (gen);
//   g_object_unref (builder);
// }



CcReceivedMessageType
cc_json_helper_get_message_type (Cast__Channel__CastMessage *message,
                                 JsonReader *reader)
{
  const gchar *message_type;
  g_autoptr (GError) error = NULL;

  if (reader == NULL)
    {
      g_autoptr(JsonParser) parser = NULL;

      parser = json_parser_new ();
      if (!json_parser_load_from_data (parser, message->payload_utf8, -1, &error))
        {
          cc_json_helper_dump_message (message);
          g_warning ("CcJsonHelper: Error parsing received message JSON: %s", error->message);
          return -1;
        }

      reader = json_reader_new (json_parser_get_root (parser));
    }

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
          cc_json_helper_dump_message (message);
          g_warning ("CcJsonHelper: Error parsing received message JSON: no type or responseType keys");
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
      // default
        return CC_RWAIT_TYPE_UNKNOWN;
    } cc_end
}

void
cc_json_helper_dump_message (Cast__Channel__CastMessage *message)
{
  // TODO: pretty print json object
  g_debug ("{ source_id: %s, destination_id: %s, namespace_: %s, payload_type: %d, payload_utf8: %s }",
    message->source_id,
    message->destination_id,
    message->namespace_,
    message->payload_type,
    message->payload_utf8);
}
