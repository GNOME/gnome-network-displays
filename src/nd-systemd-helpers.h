#include <gio/gio.h>
#include <glib-object.h>

GVariant * build_properties (const gchar *uuid,
                             const gchar *uri,
                             const gchar *display_name);
GVariant * build_aux ();
