#include "nd-screencast-portal.h"
#include <gio/gunixfdlist.h>
#include <gst/base/base.h>

#define SCREEN_CAST_IFACE "org.freedesktop.portal.ScreenCast"


struct _NdScreencastPortal
{
  GObject       parent_instance;

  gchar        *session_handle;

  gint          portal_signal_id;
  GDBusProxy   *screencast;
  guint32       stream_node_id;

  GCancellable *cancellable;
};

static void      nd_screencast_portal_async_initable_iface_init (GAsyncInitableIface *iface);
static void      nd_screencast_portal_async_initable_init_async (GAsyncInitable     *initable,
                                                                 int                 io_priority,
                                                                 GCancellable       *cancellable,
                                                                 GAsyncReadyCallback callback,
                                                                 gpointer            user_data);
static gboolean nd_screencast_portal_async_initable_init_finish (GAsyncInitable *initable,
                                                                 GAsyncResult   *res,
                                                                 GError        **error);

G_DEFINE_TYPE_EXTENDED (NdScreencastPortal, nd_screencast_portal, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE,
                                               nd_screencast_portal_async_initable_iface_init);
                       )

/* Copied from GTK+ */
static char *
get_portal_path (GDBusConnection * connection,
                 const char      *kind,
                 char           **token)
{
  char *sender;
  int i;
  char *path;

  *token = g_strdup_printf ("gnd%d", g_random_int_range (0, G_MAXINT));
  sender = g_strdup (g_dbus_connection_get_unique_name (connection) + 1);
  for (i = 0; sender[i]; i++)
    if (sender[i] == '.')
      sender[i] = '_';

  path = g_strconcat ("/org/freedesktop/portal/desktop", "/", kind, "/", sender, "/", *token, NULL);

  g_free (sender);

  return path;
}

static char *
get_portal_request_path (GDBusConnection *connection,
                         char           **token)
{
  return get_portal_path (connection, "request", token);
}

static char *
get_portal_session_path (GDBusConnection *connection,
                         char           **token)
{
  return get_portal_path (connection, "session", token);
}
/* ENd: Copied from GTK */


static void
init_check_dbus_error (GObject      *source_object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  g_autoptr(GVariant) result = NULL;
  g_autoptr(GError) error = NULL;
  GTask *task = G_TASK (user_data);

  result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object),
                                     res,
                                     &error);
  if (result == NULL)
    {
      /* Do nothing if the operation was cancelled, the task was likely
       * already returned from elsewhere. */
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          NdScreencastPortal *self = ND_SCREENCAST_PORTAL (g_task_get_source_object (task));

          g_warning ("Error calling DBus method during Screencast portal initialization: %s",
                     error->message);

          g_cancellable_cancel (self->cancellable);

          g_task_return_error (task, g_steal_pointer (&error));
          g_object_unref (task);
        }
    }
}

static void
portal_start_response_received (GDBusConnection *connection,
                                const char      *sender_name,
                                const char      *object_path,
                                const char      *interface_name,
                                const char      *signal_name,
                                GVariant        *parameters,
                                gpointer         user_data)
{
  GTask *task = G_TASK (user_data);
  NdScreencastPortal *self = g_task_get_source_object (task);

  g_autoptr(GVariant) ret = NULL;
  g_autoptr(GVariant) streams = NULL;
  gsize stream_count;
  guint32 response;
  guint32 node_id;

  g_debug ("NdScreencastPortal: Received Start response");

  g_dbus_connection_signal_unsubscribe (connection, self->portal_signal_id);
  self->portal_signal_id = 0;

  g_variant_get (parameters, "(u@a{sv})", &response, &ret);

  if (response != 0)
    {
      g_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_FAILED,
                               "Failed to start streaming on portal session (response: %u)",
                               response);
      g_object_unref (task);
      return;
    }

  streams = g_variant_lookup_value (ret, "streams", G_VARIANT_TYPE ("a(ua{sv})"));

  stream_count = g_variant_n_children (streams);
  if (stream_count == 0)
    {
      g_warning ("NdScreencastPortal: Did not find any usable streams!");
      g_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_INVAL,
                               "No usable streams found!");
      g_object_unref (task);
      return;
    }

  g_variant_get_child (streams, 0, "(ua{sv})", &node_id, NULL);

  g_debug ("Got a stream with node ID: %d", node_id);
  self->stream_node_id = node_id;

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

static void
portal_select_source_response_received (GDBusConnection *connection,
                                        const char      *sender_name,
                                        const char      *object_path,
                                        const char      *interface_name,
                                        const char      *signal_name,
                                        GVariant        *parameters,
                                        gpointer         user_data)
{
  GTask *task = G_TASK (user_data);
  NdScreencastPortal *self = g_task_get_source_object (task);

  g_autoptr(GVariant) ret = NULL;
  guint32 response;

  g_auto(GVariantBuilder) builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("(osa{sv})"));
  g_autofree gchar *token = NULL;
  g_autofree gchar *handle = NULL;

  g_debug ("NdScreencastPortal: Received SelectSource response");

  g_dbus_connection_signal_unsubscribe (connection, self->portal_signal_id);
  self->portal_signal_id = 0;

  g_variant_get (parameters, "(u@a{sv})", &response, &ret);

  if (response != 0)
    {
      g_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_FAILED,
                               "Failed to select source on portal session (response: %u)",
                               response);
      g_object_unref (task);
      return;
    }

  handle = get_portal_request_path (g_dbus_proxy_get_connection (self->screencast), &token);

  self->portal_signal_id = g_dbus_connection_signal_subscribe (g_dbus_proxy_get_connection (self->screencast),
                                                               "org.freedesktop.portal.Desktop",
                                                               "org.freedesktop.portal.Request",
                                                               "Response",
                                                               handle,
                                                               NULL,
                                                               G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                               portal_start_response_received,
                                                               task,
                                                               NULL);

  g_variant_builder_add_value (&builder, g_variant_new_object_path (self->session_handle));
  g_variant_builder_add_value (&builder, g_variant_new_string (""));

  g_variant_builder_open (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "handle_token", g_variant_new_string (token));
  g_variant_builder_close (&builder);

  g_dbus_proxy_call (self->screencast,
                     "Start",
                     g_variant_builder_end (&builder),
                     G_DBUS_CALL_FLAGS_NONE,
                     1000,
                     self->cancellable,
                     init_check_dbus_error,
                     task);
}

static void
portal_create_session_response_received (GDBusConnection *connection,
                                         const char      *sender_name,
                                         const char      *object_path,
                                         const char      *interface_name,
                                         const char      *signal_name,
                                         GVariant        *parameters,
                                         gpointer         user_data)
{
  GTask *task = G_TASK (user_data);
  NdScreencastPortal *self = g_task_get_source_object (task);

  g_autoptr(GVariant) ret = NULL;
  guint32 response;

  g_auto(GVariantBuilder) builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("(oa{sv})"));
  g_autofree gchar *token = NULL;
  g_autofree gchar *handle = NULL;

  g_debug ("NdScreencastPortal: Received CreateSession response");

  g_dbus_connection_signal_unsubscribe (connection, self->portal_signal_id);
  self->portal_signal_id = 0;

  g_variant_get (parameters, "(u@a{sv})", &response, &ret);

  if (response != 0)
    {
      g_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_FAILED,
                               "Failed to create portal session (response: %u)",
                               response);
      g_object_unref (task);
      return;
    }

  g_variant_lookup (ret, "session_handle", "s", &self->session_handle);
  g_debug ("simple variant lookup: %s", self->session_handle);

  handle = get_portal_request_path (g_dbus_proxy_get_connection (self->screencast), &token);

  self->portal_signal_id = g_dbus_connection_signal_subscribe (g_dbus_proxy_get_connection (self->screencast),
                                                               "org.freedesktop.portal.Desktop",
                                                               "org.freedesktop.portal.Request",
                                                               "Response",
                                                               handle,
                                                               NULL,
                                                               G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                               portal_select_source_response_received,
                                                               task,
                                                               NULL);

  g_variant_builder_add_value (&builder, g_variant_new_object_path (self->session_handle));

  g_variant_builder_open (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "handle_token", g_variant_new_string (token));
  g_variant_builder_add (&builder, "{sv}", "types", g_variant_new_uint32 (0x5));
  g_variant_builder_close (&builder);

  g_dbus_proxy_call (self->screencast,
                     "SelectSources",
                     g_variant_builder_end (&builder),
                     G_DBUS_CALL_FLAGS_NONE,
                     1000,
                     self->cancellable,
                     init_check_dbus_error,
                     task);
}

static void
on_portal_nd_proxy_acquired (GObject      *source_object,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  GTask *task = G_TASK (user_data);
  NdScreencastPortal *self = g_task_get_source_object (task);

  g_autoptr(GError) error = NULL;
  GDBusProxy *screencast = NULL;

  g_auto(GVariantBuilder) builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("(a{sv})"));
  g_autofree gchar *session_token = NULL;
  g_autofree gchar *session_handle = NULL;
  g_autofree gchar *token = NULL;
  g_autofree gchar *handle = NULL;

  g_debug ("NdScreencastPortal: Acquired Portal proxy");

  screencast = g_dbus_proxy_new_for_bus_finish (res, &error);
  if (screencast == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Could not create screencast portal proxy: %s", error->message);

      g_task_return_error (task, g_steal_pointer (&error));
      g_object_unref (&task);
      return;
    }

  self->screencast = screencast;

  session_handle = get_portal_session_path (g_dbus_proxy_get_connection (self->screencast), &session_token);
  handle = get_portal_request_path (g_dbus_proxy_get_connection (self->screencast), &token);

  g_debug ("task is: %p", task);

  self->portal_signal_id = g_dbus_connection_signal_subscribe (g_dbus_proxy_get_connection (self->screencast),
                                                               "org.freedesktop.portal.Desktop",
                                                               "org.freedesktop.portal.Request",
                                                               "Response",
                                                               handle,
                                                               NULL,
                                                               G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                               portal_create_session_response_received,
                                                               task,
                                                               NULL);


  g_variant_builder_open (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&builder, "{sv}", "session_handle_token", g_variant_new_string (session_token));
  g_variant_builder_add (&builder, "{sv}", "handle_token", g_variant_new_string (token));
  g_variant_builder_close (&builder);

  g_dbus_proxy_call (self->screencast,
                     "CreateSession",
                     g_variant_builder_end (&builder),
                     G_DBUS_CALL_FLAGS_NONE,
                     1000,
                     self->cancellable,
                     init_check_dbus_error,
                     task);
}

static void
nd_screencast_portal_async_initable_iface_init (GAsyncInitableIface *iface)
{
  iface->init_async = nd_screencast_portal_async_initable_init_async;
  iface->init_finish = nd_screencast_portal_async_initable_init_finish;
}

typedef struct
{
  GCancellable *cancellable;
  gulong        cancel_handler_id;
} AsyncInitData;

static void
async_init_data_free (gpointer user_data)
{
  AsyncInitData *data = user_data;

  g_cancellable_disconnect (data->cancellable, data->cancel_handler_id);
  g_clear_object (&data->cancellable);
  g_free (data);
}

static void
init_cancelable_cancelled_cb (GCancellable *external, GTask *task)
{
  NdScreencastPortal *self = ND_SCREENCAST_PORTAL (g_task_get_source_object (task));

  /* Ensure no further callbacks are called. */
  if (self->portal_signal_id)
    {
      g_dbus_connection_signal_unsubscribe (g_dbus_proxy_get_connection (self->screencast),
                                            self->portal_signal_id);
      self->portal_signal_id = 0;
    }

  /* Cancel the underlying action and return the task immediately. */
  g_cancellable_cancel (self->cancellable);
  g_task_return_error_if_cancelled (task);
  g_object_unref (task);
}

static void
nd_screencast_portal_async_initable_init_async (GAsyncInitable     *initable,
                                                int                 io_priority,
                                                GCancellable       *cancellable,
                                                GAsyncReadyCallback callback,
                                                gpointer            user_data)
{
  NdScreencastPortal *self = ND_SCREENCAST_PORTAL (initable);
  AsyncInitData *data = NULL;
  GTask *task = NULL;

  self->cancellable = g_cancellable_new ();

  /* NOTE: We use the external cancellable here, because we may
   *       otherwise return G_IO_ERROR_CANCELLED for some error
   *       conditions. */
  task = g_task_new (initable, cancellable, callback, user_data);
  if (cancellable)
    {
      data = g_new0 (AsyncInitData, 1);
      data->cancellable = g_object_ref (cancellable);

      data->cancel_handler_id = g_cancellable_connect (cancellable,
                                                       (GCallback) init_cancelable_cancelled_cb,
                                                       task,
                                                       NULL);

      g_task_set_task_data (task, data, async_init_data_free);
    }

  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                            NULL,
                            "org.freedesktop.portal.Desktop",
                            "/org/freedesktop/portal/desktop",
                            SCREEN_CAST_IFACE,
                            self->cancellable,
                            on_portal_nd_proxy_acquired,
                            task);
}

static gboolean
nd_screencast_portal_async_initable_init_finish (GAsyncInitable *initable,
                                                 GAsyncResult   *res,
                                                 GError        **error)
{
  return g_task_propagate_boolean (G_TASK (res), error);
}

NdScreencastPortal *
nd_screencast_portal_new (void)
{
  return g_object_new (ND_TYPE_SCREENCAST_PORTAL, NULL);
}

static void
nd_screencast_portal_finalize (GObject *object)
{
  NdScreencastPortal *self = (NdScreencastPortal *) object;

  G_OBJECT_CLASS (nd_screencast_portal_parent_class)->finalize (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_clear_pointer (&self->session_handle, g_free);
  if (self->portal_signal_id)
    {
      g_assert (self->screencast);

      g_dbus_connection_signal_unsubscribe (g_dbus_proxy_get_connection (self->screencast),
                                            self->portal_signal_id);
      self->portal_signal_id = 0;
    }

  g_clear_object (&self->screencast);
}

static void
nd_screencast_portal_class_init (NdScreencastPortalClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = nd_screencast_portal_finalize;
}

static void
nd_screencast_portal_init (NdScreencastPortal *self)
{
}

GstElement *
nd_screencast_portal_get_source (NdScreencastPortal *self)
{
  g_auto(GVariantBuilder) builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("(oa{sv})"));
  g_autoptr(GVariant) res = NULL;
  GUnixFDList *out_fd_list = NULL;

  g_autoptr(GError) error = NULL;
  g_autoptr(GstElement) src = NULL;
  g_autofree gint *fds = NULL;
  g_autofree gchar *path = NULL;

  g_assert (self->screencast);
  g_assert (self->session_handle);
  g_assert (self->stream_node_id != 0);

  g_variant_builder_add_value (&builder, g_variant_new_object_path (self->session_handle));
  g_variant_builder_open (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_close (&builder);

  /* The sync call here is not nice, but not a big deal either */
  res = g_dbus_proxy_call_with_unix_fd_list_sync (self->screencast,
                                                  "OpenPipeWireRemote",
                                                  g_variant_builder_end (&builder),
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  500,
                                                  NULL,
                                                  &out_fd_list,
                                                  NULL,
                                                  &error);

  if (!res || !out_fd_list || g_unix_fd_list_get_length (out_fd_list) != 1)
    {
      g_warning ("Error opening pipewire remote: %s", error->message);
      return NULL;
    }

  fds = g_unix_fd_list_steal_fds (out_fd_list, NULL);
  path = g_strdup_printf ("%u", self->stream_node_id);

  src = gst_element_factory_make ("pipewiresrc", "portal-pipewire-source");
  if (src == NULL)
    g_error ("GStreamer element \"pipewiresrc\" could not be created!");

  gst_base_src_set_live (GST_BASE_SRC (src), TRUE);
  g_object_set (src,
                "fd", fds[0],
                "path", path,
                "do-timestamp", TRUE,
                NULL);

  g_object_unref (out_fd_list);

  return g_steal_pointer (&src);
}
