/* main.c
 *
 * Copyright 2018 Benjamin Berg
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

#include <adwaita.h>
#include <glib/gi18n.h>
#include <gst/gst.h>
#include "gnome-network-displays-config.h"
#include "nd-window.h"

static void
on_activate (AdwApplication *app)
{
  GtkWindow *window;

  /* It's good practice to check your parameters at the beginning of the
   * function. It helps catch errors early and in development instead of
   * by your users.
   */
  g_assert (GTK_IS_APPLICATION (app));

  window = g_object_new (ND_TYPE_WINDOW,
                         "application", app,
                         NULL);

  /* Ask the window manager/compositor to present the window. */
  gtk_window_present (window);
}

int
main (int   argc,
      char *argv[])
{
  g_autoptr(AdwApplication) app = NULL;

  /* Set up gettext translations */
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  gst_init (&argc, &argv);

  /*
   * Create a new GtkApplication. The application manages our main loop,
   * application windows, integration with the window manager/compositor, and
   * desktop features such as file opening and single-instance applications.
   */
#if GLIB_CHECK_VERSION (2, 74, 0)
  app = adw_application_new ("org.gnome.NetworkDisplays", G_APPLICATION_DEFAULT_FLAGS);
#else
  app = adw_application_new ("org.gnome.NetworkDisplays", G_APPLICATION_FLAGS_NONE);
#endif

  g_set_application_name (_("GNOME Network Displays"));

  /*
   * We connect to the activate signal to create a window when the application
   * has been launched. Additionally, this signal notifies us when the user
   * tries to launch a "second instance" of the application. When they try
   * to do that, we'll just present any existing window.
   *
   * Because we can't pass a pointer to any function type, we have to cast
   * our "on_activate" function to a GCallback.
   */
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);

  /*
   * Run the application. This function will block until the application
   * exits. Upon return, we have our exit code to return to the shell. (This
   * is the code you see when you do `echo $?` after running a command in a
   * terminal.
   *
   * Since GtkApplication inherits from GApplication, we use the parent class
   * method "run". But we need to cast, which is what the "G_APPLICATION()"
   * macro does.
   */
  return g_application_run (G_APPLICATION (app), argc, argv);
}
