#include "nd-pulseaudio.h"
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC (pa_proplist, pa_proplist_free)

#define ND_PA_SINK "gnome_network_displays"

struct _NdPulseaudio
{
  GObject               parent_instance;

  GTask                *init_task;

  pa_threaded_mainloop *mainloop;
  pa_mainloop_api      *mainloop_api;
  pa_context           *context;
  guint                 null_module_idx;
  guint                 sink_idx;
  gchar                *name;
  gchar                *uuid;
  gchar                *sink_name;
  gchar                *previous_default_sink;
  gboolean              audio_routed;

  pa_operation         *operation;
};

static void      nd_pulseaudio_async_initable_iface_init (GAsyncInitableIface *iface);
static void      nd_pulseaudio_async_initable_init_async (GAsyncInitable     *initable,
                                                          int                 io_priority,
                                                          GCancellable       *cancellable,
                                                          GAsyncReadyCallback callback,
                                                          gpointer            user_data);
static gboolean nd_pulseaudio_async_initable_init_finish (GAsyncInitable *initable,
                                                          GAsyncResult   *res,
                                                          GError        **error);

static const gchar *
nd_pulseaudio_get_sink_name (NdPulseaudio *self)
{
  if (!self->sink_name)
    self->sink_name = g_strdup_printf (ND_PA_SINK "_%.8s", self->uuid);

  return self->sink_name;
}

static gboolean
nd_pulseaudio_audio_routing_enabled (void)
{
  const gchar *route = g_getenv ("GND_WFD_AUDIO_ROUTE");
  const gchar *audio = g_getenv ("GND_WFD_AUDIO");

  if (route && *route)
    return g_ascii_strcasecmp (route, "0") != 0 &&
           g_ascii_strcasecmp (route, "false") != 0 &&
           g_ascii_strcasecmp (route, "no") != 0 &&
           g_ascii_strcasecmp (route, "off") != 0;

  if (!audio || !*audio)
    return FALSE;

  return g_ascii_strcasecmp (audio, "none") != 0 &&
         g_ascii_strcasecmp (audio, "off") != 0 &&
         g_ascii_strcasecmp (audio, "disabled") != 0 &&
         g_ascii_strcasecmp (audio, "0") != 0 &&
         g_ascii_strcasecmp (audio, "false") != 0 &&
         g_ascii_strcasecmp (audio, "no") != 0;
}

static void
unref_operation (pa_operation *operation)
{
  if (operation)
    pa_operation_unref (operation);
}

static void
on_pa_move_sink_input_finished (pa_context *c, int success, void *userdata)
{
  if (!success)
    g_debug ("NdPulseaudio: Failed to move a sink input");
}

static void
on_pa_default_sink_changed (pa_context *c, int success, void *userdata)
{
  if (success)
    g_debug ("NdPulseaudio: Default sink changed for screencast audio routing");
  else
    g_debug ("NdPulseaudio: Failed to change default sink for screencast audio routing");
}

static void
on_pa_sink_input_info_for_route (pa_context                *c,
                                 const pa_sink_input_info *i,
                                 int                       eol,
                                 void                     *userdata)
{
  NdPulseaudio *self = ND_PULSEAUDIO (userdata);

  if (eol > 0)
    return;

  if (eol < 0)
    {
      g_debug ("NdPulseaudio: Error listing sink inputs for audio routing");
      return;
    }

  g_debug ("NdPulseaudio: Moving sink input %u to %s", i->index, nd_pulseaudio_get_sink_name (self));
  unref_operation (pa_context_move_sink_input_by_name (c,
                                                       i->index,
                                                       nd_pulseaudio_get_sink_name (self),
                                                       on_pa_move_sink_input_finished,
                                                       self));
}

static void
on_pa_server_info_for_route (pa_context           *c,
                             const pa_server_info *i,
                             void                 *userdata)
{
  NdPulseaudio *self = ND_PULSEAUDIO (userdata);

  if (!i)
    return;

  g_clear_pointer (&self->previous_default_sink, g_free);
  if (i->default_sink_name &&
      !g_str_equal (i->default_sink_name, nd_pulseaudio_get_sink_name (self)))
    self->previous_default_sink = g_strdup (i->default_sink_name);

  g_debug ("NdPulseaudio: Routing system audio to %s", nd_pulseaudio_get_sink_name (self));
  unref_operation (pa_context_set_default_sink (c,
                                                nd_pulseaudio_get_sink_name (self),
                                                on_pa_default_sink_changed,
                                                self));
  unref_operation (pa_context_get_sink_input_info_list (c,
                                                        on_pa_sink_input_info_for_route,
                                                        self));

  self->audio_routed = TRUE;
}

static void
nd_pulseaudio_route_audio (NdPulseaudio *self)
{
  if (!nd_pulseaudio_audio_routing_enabled () || self->audio_routed)
    return;

  unref_operation (pa_context_get_server_info (self->context,
                                               on_pa_server_info_for_route,
                                               self));
}

static void
on_pa_default_sink_restored (pa_context *c, int success, void *userdata)
{
  if (success)
    g_debug ("NdPulseaudio: Restored previous default sink");
  else
    g_debug ("NdPulseaudio: Failed to restore previous default sink");
}

static void
on_pa_sink_input_info_for_restore (pa_context                *c,
                                   const pa_sink_input_info *i,
                                   int                       eol,
                                   void                     *userdata)
{
  NdPulseaudio *self = ND_PULSEAUDIO (userdata);

  if (eol > 0)
    return;

  if (eol < 0)
    {
      g_debug ("NdPulseaudio: Error listing sink inputs for audio restore");
      return;
    }

  if (!self->previous_default_sink)
    return;

  g_debug ("NdPulseaudio: Moving sink input %u back to %s", i->index, self->previous_default_sink);
  unref_operation (pa_context_move_sink_input_by_name (c,
                                                       i->index,
                                                       self->previous_default_sink,
                                                       on_pa_move_sink_input_finished,
                                                       self));
}

void
nd_pulseaudio_restore_audio (NdPulseaudio *self)
{
  if (!self || !self->audio_routed || !self->context ||
      !PA_CONTEXT_IS_GOOD (pa_context_get_state (self->context)))
    return;

  if (!self->previous_default_sink)
    {
      self->audio_routed = FALSE;
      return;
    }

  g_debug ("NdPulseaudio: Restoring system audio to %s", self->previous_default_sink);
  unref_operation (pa_context_set_default_sink (self->context,
                                                self->previous_default_sink,
                                                on_pa_default_sink_restored,
                                                self));
  unref_operation (pa_context_get_sink_input_info_list (self->context,
                                                        on_pa_sink_input_info_for_restore,
                                                        self));

  self->audio_routed = FALSE;
}

enum {
  PROP_NAME = 1,
  PROP_UUID,
  PROP_LAST,
};

G_DEFINE_TYPE_EXTENDED (NdPulseaudio, nd_pulseaudio, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE,
                                               nd_pulseaudio_async_initable_iface_init);
                       )

static GParamSpec * props[PROP_LAST] = { NULL, };

static void
nd_pulseaudio_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  NdPulseaudio *self = ND_PULSEAUDIO (object);

  switch (prop_id)
    {
    case PROP_NAME:
      g_value_set_string (value, self->name);
      break;

    case PROP_UUID:
      g_value_set_string (value, self->uuid);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_pulseaudio_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  NdPulseaudio *self = ND_PULSEAUDIO (object);

  switch (prop_id)
    {
    case PROP_NAME:
      self->name = g_value_dup_string (value);
      break;

    case PROP_UUID:
      self->uuid = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_pulseaudio_async_initable_iface_init (GAsyncInitableIface *iface)
{
  iface->init_async = nd_pulseaudio_async_initable_init_async;
  iface->init_finish = nd_pulseaudio_async_initable_init_finish;
}

static gboolean
return_idle_cb (gpointer user_data)
{
  GTask *task = user_data;
  GError *error = g_object_steal_data (G_OBJECT (task), "result");

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);

  g_object_unref (task);

  return G_SOURCE_REMOVE;
}

/* Helper function to return an error from idle */
static void
return_idle_error (GTask *task, GError *error)
{
  g_assert (error);
  g_object_set_data_full (G_OBJECT (task), "result", error, (GDestroyNotify) g_error_free);
  g_idle_add (return_idle_cb, g_object_ref (task));
}

static void
return_idle_success (GTask *task)
{
  g_idle_add (return_idle_cb, g_object_ref (task));
}

static void
on_pa_null_module_loaded (pa_context *c,
                          uint32_t    idx,
                          void       *userdata)
{
  NdPulseaudio *self = ND_PULSEAUDIO (userdata);

  if (idx == PA_INVALID_INDEX)
    {
      g_debug ("NdPulseaudio: Module load failed!");
      return_idle_error (self->init_task,
                         g_error_new (G_IO_ERROR,
                                      G_IO_ERROR_FAILED,
                                      "Failed to load NULL module for screencast PA sink"));
      g_clear_object (&self->init_task);
      return;
    }

  g_debug ("NdPulseaudio: Module loaded, we are ready to grab audio! ");
  self->null_module_idx = idx;
  return_idle_success (self->init_task);
  g_clear_object (&self->init_task);
}

static void
on_pa_nd_sink_got_info (pa_context         *c,
                        const pa_sink_info *i,
                        int                 eol,
                        void               *userdata)
{
  NdPulseaudio *self = ND_PULSEAUDIO (userdata);

  /* eol is negative for error, and 1 for end of list (with NULL sink info then) */

  /* We are only ever called once, as we cancel the operation
   * if we get what we want. */
  if (eol < 0 && pa_context_errno (self->context) != PA_ERR_NOENTITY)
    {
      g_debug ("NdPulseaudio: Error querying sink info");
      return_idle_error (self->init_task,
                         g_error_new (G_IO_ERROR,
                                      G_IO_ERROR_FAILED,
                                      "Error querying sink from PA: %s",
                                      pa_strerror (pa_context_errno (self->context))));
      g_clear_object (&self->init_task);
      return;
    }

  if (eol == 0)
    {
      /* This is the case that we could query the sink, so it seems
       * like it exists already. Just double check things, and
       * return successful initialization.
       */

      /* Cancel the operation as we would be called a second time for
       * the list end which would cause an immediate successful return. */
      pa_operation_cancel (self->operation);
      g_clear_pointer (&self->operation, pa_operation_unref);

      g_debug ("NdPulseaudio: Error querying sink info");
      g_debug ("NdPulseaudio: Got a sink info for the expected name");
      self->null_module_idx = i->owner_module;
      self->sink_idx = i->index;
      return_idle_success (self->init_task);
      g_clear_object (&self->init_task);
      return;
    }

  g_debug ("NdPulseaudio: Creating sink for: %s", self->name);
  /* We have reached the list end without being cancelled first.
   * This means no screencast sink exist, and we need to create it. */
  g_autofree gchar *module_args = g_strdup_printf ("sink_name=" ND_PA_SINK "_%.8s"
                                                   " "
                                                   "rate=48000 "
                                                   "sink_properties=device.class=\"sound\" "
                                                   "device.icon_name=\"network-wireless\" "
                                                   "device.description=\"%s\"",
                                                   self->uuid,
                                                   self->name);
  self->operation = pa_context_load_module (self->context,
                                            "module-null-sink",
                                            module_args,
                                            on_pa_null_module_loaded,
                                            self);
}

static void
nd_pulseaudio_unload_module_cb (pa_context *c, int success, void *userdata)
{
  if (success)
    g_info ("NdPulseaudio: Module unloaded");
  else
    g_warning ("NdPulseaudio: Error unloading module");
}

void
nd_pulseaudio_unload (NdPulseaudio *self)
{
  g_debug ("NdPulseaudio: Unloading module");

  if (!PA_CONTEXT_IS_GOOD (pa_context_get_state (self->context)))
    return;
  while (pa_context_get_state (self->context) != PA_CONTEXT_READY)
    pa_threaded_mainloop_wait (self->mainloop);

  nd_pulseaudio_restore_audio (self);

  pa_operation *operation = pa_context_unload_module (self->context,
                                                      self->null_module_idx,
                                                      nd_pulseaudio_unload_module_cb,
                                                      NULL);
  if (!operation)
    {
      g_warning ("NdPulseaudio: Error unloading module operation");
      return;
    }
  while (pa_operation_get_state (operation) == PA_OPERATION_RUNNING)
    pa_threaded_mainloop_wait (self->mainloop);
  pa_operation_unref (operation);
}

static void
nd_pulseaudio_state_cb (pa_context *context,
                        void       *user_data)
{
  NdPulseaudio *self = ND_PULSEAUDIO (user_data);

  switch (pa_context_get_state (context))
    {
    case PA_CONTEXT_UNCONNECTED:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
      break;

    case PA_CONTEXT_READY:
      if (!self->init_task)
        return;

      g_debug ("NdPulseaudio: Querying sink info by name");
      g_autofree gchar *sink_name = g_strdup_printf (ND_PA_SINK "_%.8s", self->uuid);
      self->operation = pa_context_get_sink_info_by_name (self->context,
                                                          sink_name,
                                                          on_pa_nd_sink_got_info,
                                                          self);
      break;

    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
      if (!self->init_task)
        return;

      g_debug ("NdPulseaudio: PA context went into failed state during init");
      return_idle_error (self->init_task,
                         g_error_new (G_IO_ERROR,
                                      G_IO_ERROR_FAILED,
                                      "PA failed"));
      g_clear_object (&self->init_task);
      break;

    default:
      /* FIXME: */
      break;
    }
}

static void
nd_pulseaudio_async_initable_init_async (GAsyncInitable     *initable,
                                         int                 io_priority,
                                         GCancellable       *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer            user_data)
{
  NdPulseaudio *self = ND_PULSEAUDIO (initable);

  g_autoptr(pa_proplist) proplist = NULL;
  gint res;

  self->mainloop = pa_threaded_mainloop_new ();
  pa_threaded_mainloop_start (self->mainloop);
  self->mainloop_api = pa_threaded_mainloop_get_api (self->mainloop);

  proplist = pa_proplist_new ();
  g_autofree gchar *app_name = g_strdup_printf ("GNOME Network Displays PulseAudio Client for %s", self->name);
  g_autofree gchar *app_id = g_strdup_printf ("org.gnome.NetworkDisplays.PulseAudio_%.8s", self->uuid);
  pa_proplist_sets (proplist, PA_PROP_APPLICATION_NAME, app_name);
  pa_proplist_sets (proplist, PA_PROP_APPLICATION_ID, app_id);
  /* pa_proplist_sets (proplist, PA_PROP_APPLICATION_ICON_NAME, ); */

  self->context = pa_context_new_with_proplist (self->mainloop_api, NULL, proplist);

  /* Create our task; we currently don't handle cancellation internally */
  self->init_task = g_task_new (initable, cancellable, callback, user_data);

  pa_context_set_state_callback (self->context,
                                 nd_pulseaudio_state_cb,
                                 self);

  res = pa_context_connect (self->context, NULL, (pa_context_flags_t) PA_CONTEXT_NOFLAGS, NULL);
  if (res < 0)
    {
      g_debug ("NdPulseaudio: Error querying sink info");
      if (self->init_task)
        {
          g_task_return_new_error (self->init_task,
                                   G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                   "Error connecting to PA: %s",
                                   pa_strerror (pa_context_errno (self->context)));
        }
      g_clear_object (&self->init_task);
      return;
    }

  /* Wait for us to be connected. */
}

static gboolean
nd_pulseaudio_async_initable_init_finish (GAsyncInitable *initable,
                                          GAsyncResult   *res,
                                          GError        **error)
{
  return g_task_propagate_boolean (G_TASK (res), error);
}


NdPulseaudio *
nd_pulseaudio_new (gchar *name, gchar *uuid)
{
  return g_object_new (ND_TYPE_PULSEAUDIO,
                       "name", name,
                       "uuid", uuid,
                       NULL);
}

static void
nd_pulseaudio_finalize (GObject *object)
{
  NdPulseaudio *self = (NdPulseaudio *) object;

  if (self->init_task)
    {
      g_task_return_new_error (self->init_task,
                               G_IO_ERROR,
                               G_IO_ERROR_CANCELLED,
                               "Object finalized, async init was cancelled.");
      g_clear_object (&self->init_task);
    }

  if (self->operation)
    pa_operation_cancel (self->operation);
  g_clear_pointer (&self->operation, pa_operation_unref);
  if (self->context)
    pa_context_disconnect (self->context);
  g_clear_pointer (&self->context, pa_context_unref);
  self->mainloop_api = NULL;
  pa_threaded_mainloop_stop (self->mainloop);
  g_clear_pointer (&self->mainloop, pa_threaded_mainloop_free);
  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->uuid, g_free);
  g_clear_pointer (&self->sink_name, g_free);
  g_clear_pointer (&self->previous_default_sink, g_free);

  G_OBJECT_CLASS (nd_pulseaudio_parent_class)->finalize (object);
}

static void
nd_pulseaudio_class_init (NdPulseaudioClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = nd_pulseaudio_finalize;
  object_class->set_property = nd_pulseaudio_set_property;
  object_class->get_property = nd_pulseaudio_get_property;

  props[PROP_NAME] =
    g_param_spec_string ("name", "Sink Name",
                         "The sink name.",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  props[PROP_UUID] =
    g_param_spec_string ("uuid", "Sink UUID",
                         "The sink UUID.",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, props);
}

static void
nd_pulseaudio_init (NdPulseaudio *self)
{
  self->null_module_idx = PA_INVALID_INDEX;
  self->sink_idx = PA_INVALID_INDEX;
}

GstElement *
nd_pulseaudio_get_source (NdPulseaudio *self)
{
  g_autoptr(GstElement) src = NULL;

  g_debug ("NdPulseaudio: Get source");

  g_assert (self->init_task == NULL);
  g_assert (self->context != NULL);

  nd_pulseaudio_route_audio (self);

  g_autofree gchar *element_name = g_strdup_printf ("pulseaudio-source-%s", self->uuid);
  src = gst_element_factory_make ("pulsesrc", element_name);

  g_autofree gchar *device_name = g_strdup_printf ("%s.monitor", nd_pulseaudio_get_sink_name (self));
  g_autofree gchar *client_name = g_strdup_printf ("GNOME Network Displays Audio Grabber for %s", self->name);
  g_object_set (src,
                "device", device_name,
                "client-name", client_name,
                "do-timestamp", TRUE,
                "server", pa_context_get_server (self->context),
                NULL);

  return g_steal_pointer (&src);
}
