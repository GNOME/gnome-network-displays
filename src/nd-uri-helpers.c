/* nd-uri-helpers.c
 *
 * Copyright 2023 Pedro Sader Azevedo <pedro.saderazevedo@proton.me>
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

#include <glib-object.h>
#include "nd-uri-helpers.h"

/**
 * nd_uri_helpers_generate_uri
 * @params: a #GHashTable of URI parameters
 *
 * Create a URI string for a sink, given the URI parameters
 *
 * Returns: (transfer full): a URI string or NULL if unable to generate it
 */
gchar * nd_uri_helpers_generate_uri (GHashTable *params) {
  g_autoptr (GError) error = NULL;

  g_autoptr (GStrvBuilder) strv_builder = g_strv_builder_new ();
  GHashTableIter iter;
  gpointer key, value;
  gpointer key_parsed, value_parsed;
  g_hash_table_iter_init (&iter, params);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      key_parsed = g_uri_escape_string (key, G_URI_RESERVED_CHARS_GENERIC_DELIMITERS, FALSE);
      value_parsed = g_uri_escape_string (value, G_URI_RESERVED_CHARS_GENERIC_DELIMITERS, FALSE);

      g_strv_builder_add (strv_builder,
                          g_strdup_printf ("%s=%s", (gchar *) key_parsed, (gchar *) value_parsed));
    }

  g_auto (GStrv) params_array = g_strv_builder_end (strv_builder);
  g_autofree gchar *query = g_strjoinv ("&", params_array);

  GUri *guri = g_uri_build (G_URI_FLAGS_ENCODED,
                           "gnome-network-displays",
                           NULL,
                           "sink",
                           -1,
                           "",
                           query,
                           NULL);
  if (!guri) {
      g_warning ("Failed to build GUri: %s\n", error->message);
      return NULL;
  }

  gchar *uri;
  uri = g_uri_to_string (guri);
  if (!g_uri_is_valid (uri, G_URI_FLAGS_NONE, &error)) {
      g_warning ("Generated URI is not valid: %s\n", error->message);
      return NULL;
  }

  return uri;
}

/**
 * nd_uri_helpers_parse_uri
 * @params: a URI string
 *
 * Create a #GHashTable with the parameters of the query in the given URI string
 *
 * Returns: (transfer container): a #GHashTable of parameters or NULL if unable to generate it
 */
GHashTable * nd_uri_helpers_parse_uri (gchar *uri) {
  g_autoptr (GError) error = NULL;

  if (!g_uri_is_valid (uri, G_URI_FLAGS_NONE, &error)) {
      g_warning ("Generated URI is not valid: %s\n", error->message);
      return NULL;
  }

  g_autoptr (GUri) guri = NULL;
  guri = g_uri_parse (uri, G_URI_FLAGS_ENCODED, &error);
  if (!guri) {
      g_warning ("Failed to parse URI: %s\n", error->message);
      return NULL;
  }

  GHashTable *params = NULL;
  params = g_uri_parse_params (g_uri_get_query (guri),
                               -1,
                               "&",
                               G_URI_PARAMS_NONE,
                               &error);

  if (!params) {
      g_warning ("Failed to parse params: %s\n", error->message);
      return NULL;
  }

  return params;
}
