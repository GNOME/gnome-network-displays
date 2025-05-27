/* nd-daemon.c
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

#include <gst/gst.h>
#include <glib-object.h>
#include <gio/gio.h>
#include "gnome-network-displays-config.h"
#include "nd-daemon.h"

static void
terminate_received_cb (GAction  *action,
                       GVariant *parameter,
                       NdDaemon *daemon)
{
  g_application_release (G_APPLICATION (daemon));
}

static void
add_actions (GApplication *app)
{
  g_autoptr(GSimpleAction) action = NULL;

  action = g_simple_action_new ("terminate", NULL);
  g_signal_connect (action, "activate", G_CALLBACK (terminate_received_cb), app);
  g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (action));
}

int
main (int   argc,
      char *argv[])
{
  g_autoptr(NdDaemon) daemon = NULL;

  gst_init (&argc, &argv);

  daemon = g_object_new (ND_TYPE_DAEMON,
                         "application-id", "org.gnome.NetworkDisplays.Daemon",
                         "flags", G_APPLICATION_IS_SERVICE,
                         NULL);

  add_actions (G_APPLICATION (daemon));

  g_set_application_name ("GNOME Network Displays daemon");

  return g_application_run (G_APPLICATION (daemon), argc, argv);
}
