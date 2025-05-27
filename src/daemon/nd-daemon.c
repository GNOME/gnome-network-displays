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

#include <avahi-gobject/ga-client.h>
#include <avahi-gobject/ga-service-browser.h>
#include <gst/gst.h>
#include <glib-object.h>


#include "nd-daemon.h"
#include "nd-manager.h"
#include "nd-registry.h"
#include "nd-meta-provider.h"
#include "nd-nm-device-registry.h"
#include "nd-dummy-provider.h"
#include "nd-wfd-mice-provider.h"
#include "nd-cc-provider.h"

struct _NdDaemon
{
  GApplication        parent_instance;

  NdRegistry         *registry;

  NdManager          *manager;

  GaClient           *avahi_client;
  NdMetaProvider     *meta_provider;
  NdNMDeviceRegistry *nm_device_registry;
  GPtrArray          *sink_property_bindings;
};

enum {
  PROP_REGISTRY = 1,
  PROP_MANAGER,

  PROP_LAST,
};

G_DEFINE_TYPE (NdDaemon, nd_daemon, G_TYPE_APPLICATION)

static void
nd_daemon_get_property (GObject    *object,
                        guint       prop_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
  NdDaemon *self = ND_DAEMON (object);

  switch (prop_id)
    {
    case PROP_REGISTRY:
      g_value_set_object (value, self->registry);
      break;

    case PROP_MANAGER:
      g_value_set_object (value, self->manager);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_daemon_set_property (GObject      *object,
                        guint         prop_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
  NdDaemon *self = ND_DAEMON (object);

  switch (prop_id)
    {
    case PROP_REGISTRY:
      self->registry = g_value_get_object (value);
      break;

    case PROP_MANAGER:
      self->manager = g_value_get_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
on_meta_provider_has_provider_changed_cb (NdDaemon *self)
{
  gboolean has_providers;

  g_object_get (self->meta_provider,
                "has-providers", &has_providers,
                NULL);
}

static void
nd_daemon_constructed (GObject *obj)
{
  NdDaemon *self = ND_DAEMON (obj);

  g_autoptr(GError) error = NULL;
  g_autoptr(NdWFDMiceProvider) mice_provider = NULL;
  g_autoptr(NdCCProvider) cc_provider = NULL;

  self->avahi_client = ga_client_new (GA_CLIENT_FLAG_NO_FLAGS);

  if (!ga_client_start (self->avahi_client, &error))
    {
      g_warning ("NdDaemon: Failed to start Avahi Client");
      if (error != NULL)
        g_warning ("NdDaemon: Error: %s", error->message);
      return;
    }

  g_debug ("NdDaemon: Got avahi client");

  mice_provider = nd_wfd_mice_provider_new (self->avahi_client);
  cc_provider = nd_cc_provider_new (self->avahi_client);

  if (!nd_wfd_mice_provider_browse (mice_provider, error) || !nd_cc_provider_browse (cc_provider, error))
    {
      g_warning ("NdDaemon: Avahi client failed to browse: %s", error->message);
      return;
    }

  g_debug ("NdDaemon: Got avahi browser");
  nd_meta_provider_add_provider (self->meta_provider, ND_PROVIDER (mice_provider));
  nd_meta_provider_add_provider (self->meta_provider, ND_PROVIDER (cc_provider));
}

static void
nd_daemon_finalize (GObject *obj)
{
  NdDaemon *self = ND_DAEMON (obj);

  g_clear_object (&self->meta_provider);
  g_clear_object (&self->nm_device_registry);
  g_clear_object (&self->avahi_client);

  g_clear_pointer (&self->sink_property_bindings, g_ptr_array_unref);

  G_OBJECT_CLASS (nd_daemon_parent_class)->finalize (obj);
}

static void
nd_daemon_dispose (GObject *obj)
{
  NdDaemon *self = ND_DAEMON (obj);

  g_object_run_dispose (G_OBJECT (self->avahi_client));

  G_OBJECT_CLASS (nd_daemon_parent_class)->dispose (obj);
}

static void
nd_daemon_startup (GApplication *app)
{
  /* Run indefinitely, until told to exit. */
  g_application_hold (app);

  G_APPLICATION_CLASS (nd_daemon_parent_class)->startup (app);
}

static void
nd_daemon_shutdown (GApplication *app)
{
  G_APPLICATION_CLASS (nd_daemon_parent_class)->shutdown (app);

  /* Stop running */
  g_application_release (app);
}

static void
nd_daemon_class_init (NdDaemonClass *klass)
{
  GApplicationClass *g_application_class = G_APPLICATION_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  g_application_class->startup = nd_daemon_startup;
  g_application_class->shutdown = nd_daemon_shutdown;

  object_class->get_property = nd_daemon_get_property;
  object_class->set_property = nd_daemon_set_property;

  object_class->constructed = nd_daemon_constructed;
  object_class->finalize = nd_daemon_finalize;
  object_class->dispose = nd_daemon_dispose;
}


static void
nd_daemon_init (NdDaemon *self)
{
  g_autoptr(NdDummyProvider) dummy_provider = NULL;

  self->meta_provider = nd_meta_provider_new ();
  g_signal_connect_object (self->meta_provider,
                           "notify::has-providers",
                           (GCallback) on_meta_provider_has_provider_changed_cb,
                           self,
                           G_CONNECT_SWAPPED);

  self->nm_device_registry = nd_nm_device_registry_new (self->meta_provider);

  self->registry = nd_registry_new (NULL);
  nd_registry_set_provider (self->registry, ND_PROVIDER (self->meta_provider));

  self->manager = nd_manager_new (ND_PROVIDER (self->meta_provider));

  if (g_strcmp0 (g_getenv ("NETWORK_DISPLAYS_DUMMY"), "1") == 0)
    {
      g_debug ("Adding dummy provider");
      dummy_provider = nd_dummy_provider_new ();
      nd_meta_provider_add_provider (self->meta_provider, ND_PROVIDER (dummy_provider));
    }

  self->sink_property_bindings = g_ptr_array_new_full (0, (GDestroyNotify) g_binding_unbind);
}
