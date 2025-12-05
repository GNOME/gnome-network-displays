/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "gnome-network-displays-config.h"
#include "nd-sd-cc-provider.h"
#include "nd-cc-sink.h"
#include "nd-sink.h"
#include "nd-systemd-helpers.h"
#include <systemd/sd-varlink.h>
#include <systemd/sd-json.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <glib-unix.h>

struct _NdSdCCProvider
{
  GObject        parent_instance;

  GPtrArray     *sinks;
  GSocketClient *signalling_client;

  NMDevice      *nm_device;
  int            nm_ifindex;

  sd_varlink    *link;
  int            varlink_fd;
  GSource       *varlink_source;

  gboolean       discover;
};

enum {
  PROP_NM_DEVICE = 1,
  PROP_DISCOVER,
  PROP_LAST = PROP_DISCOVER,
};

static void nd_sd_cc_provider_provider_iface_init (NdProviderIface *iface);
static GList * nd_sd_cc_provider_get_sinks (NdProvider *provider);

G_DEFINE_TYPE_EXTENDED (NdSdCCProvider, nd_sd_cc_provider, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (ND_TYPE_PROVIDER,
                                               nd_sd_cc_provider_provider_iface_init))

static GParamSpec *props[PROP_LAST] = { NULL, };

/**
 * extract_friendly_name_from_txt:
 * @txt_array: Array of TXT record entries (strings like "key=value")
 *
 * Extract the friendly name (fn) and model (md) from Chromecast TXT records.
 * Returns: (transfer full): The display name or NULL if not found
 */
static gchar *
extract_friendly_name_from_txt (sd_json_variant *txt_array)
{
  g_autofree gchar *fn = NULL;
  g_autofree gchar *md = NULL;

  if (!txt_array || !sd_json_variant_is_array (txt_array))
    return NULL;

  size_t n = sd_json_variant_elements (txt_array);

  for (size_t i = 0; i < n; i++)
    {
      sd_json_variant *entry = sd_json_variant_by_index (txt_array, i);
      if (!entry || !sd_json_variant_is_string (entry))
        continue;

      const char *s = sd_json_variant_string (entry);
      if (!s)
        continue;

      if (g_str_has_prefix (s, "fn="))
        {
          g_free (fn);
          fn = g_strdup (s + 3);
        }
      else if (g_str_has_prefix (s, "md="))
        {
          g_free (md);
          md = g_strdup (s + 3);
        }
    }

  if (fn && md && *fn && *md)
    return g_strdup_printf ("%s - %s", fn, md);
  else if (fn && *fn)
    return g_steal_pointer (&fn);

  return NULL;
}

/**
 * extract_ip_from_services:
 * @services_array: JSON array of service objects with addresses
 *
 * Extract the IP address from the services array.
 * Prefers IPv4, falls back to IPv6.
 * Returns: (transfer full): The IP address string or NULL
 */
static gchar *
extract_ip_from_services (sd_json_variant *services_array)
{
  gchar *ipv4 = NULL;
  gchar *ipv6 = NULL;

  if (!services_array || !sd_json_variant_is_array (services_array))
    return NULL;

  size_t sn = sd_json_variant_elements (services_array);

  for (size_t si = 0; si < sn; si++)
    {
      sd_json_variant *srv_entry = sd_json_variant_by_index (services_array, si);
      if (!srv_entry || !sd_json_variant_is_object (srv_entry))
        continue;

      sd_json_variant *addr_array = sd_json_variant_by_key (srv_entry, "addresses");
      if (!addr_array || !sd_json_variant_is_array (addr_array))
        continue;

      size_t an = sd_json_variant_elements (addr_array);

      for (size_t ai = 0; ai < an; ai++)
        {
          sd_json_variant *addr_entry = sd_json_variant_by_index (addr_array, ai);
          if (!addr_entry || !sd_json_variant_is_object (addr_entry))
            continue;

          sd_json_variant *family_v = sd_json_variant_by_key (addr_entry, "family");
          sd_json_variant *address_v = sd_json_variant_by_key (addr_entry, "address");

          if (!family_v || !sd_json_variant_is_integer (family_v))
            continue;
          if (!address_v || !sd_json_variant_is_array (address_v))
            continue;

          int64_t family = sd_json_variant_integer (family_v);

          size_t bn = sd_json_variant_elements (address_v);
          if (family == AF_INET && bn != 4)
            continue;
          if (family == AF_INET6 && bn != 16)
            continue;

          uint8_t buf[16] = {0};

          gboolean valid = TRUE;
          for (size_t bi = 0; bi < bn; bi++)
            {
              sd_json_variant *b = sd_json_variant_by_index (address_v, bi);
              if (!b || !sd_json_variant_is_unsigned (b))
                {
                  valid = FALSE;
                  break;
                }
              uint64_t v = sd_json_variant_unsigned (b);
              if (v > 255)
                {
                  valid = FALSE;
                  break;
                }
              buf[bi] = (uint8_t) v;
            }

          if (!valid)
            continue;

          char ipbuf[INET6_ADDRSTRLEN + 1] = {0};

          if (family == AF_INET)
            {
              if (inet_ntop (AF_INET, buf, ipbuf, sizeof ipbuf))
                {
                  g_clear_pointer (&ipv4, g_free);
                  ipv4 = g_strdup (ipbuf);
                }
            }
          else if (family == AF_INET6)
            {
              if (inet_ntop (AF_INET6, buf, ipbuf, sizeof ipbuf))
                {
                  g_clear_pointer (&ipv6, g_free);
                  ipv6 = g_strdup (ipbuf);
                }
            }
        }
    }

  /* Prefer IPv4 */
  if (ipv4)
    {
      g_free (ipv6);
      return ipv4;
    }

  return ipv6;
}

static NdCCSink *
find_sink_by_name (NdSdCCProvider *self,
                   const gchar    *name)
{
  for (guint i = 0; i < self->sinks->len; i++)
    {
      NdCCSink *sink = g_ptr_array_index (self->sinks, i);
      g_autofree gchar *sink_name = NULL;

      g_object_get (sink, "name", &sink_name, NULL);
      if (g_strcmp0 (sink_name, name) == 0)
        return sink;
    }
  return NULL;
}

static gboolean
compare_sinks (NdCCSink    *sink,
               const gchar *name,
               const gchar *ip)
{
  g_autofree gchar *sink_name = NULL;
  g_autofree gchar *sink_ip = NULL;

  g_object_get (sink,
                "name", &sink_name,
                "ip", &sink_ip,
                NULL);

  return g_strcmp0 (sink_name, name) == 0 &&
         g_strcmp0 (sink_ip, ip) == 0;
}

typedef struct {
  gchar *name;
  gchar *domain;
  int    ifindex;
} ResolveCtx;

static void
resolve_ctx_free (ResolveCtx *ctx)
{
  if (ctx)
    {
      g_free (ctx->name);
      g_free (ctx->domain);
      g_free (ctx);
    }
}

static void
resolve_sink_task_thread (GTask        *task,
                          gpointer      source_object,
                          gpointer      task_data,
                          GCancellable *cancellable)
{
  ResolveCtx *ctx = task_data;
  sd_varlink *vl = NULL;
  sd_json_variant *params = NULL;
  sd_json_variant *reply = NULL;
  const char *error_id = NULL;
  int r;

  r = sd_varlink_connect_address (&vl, "/run/systemd/resolve/io.systemd.Resolve");
  if (r < 0)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to connect to systemd-resolved: %s",
                               g_strerror (-r));
      goto out;
    }

  r = sd_json_buildo (&params,
                      SD_JSON_BUILD_PAIR_STRING ("name", ctx->name),
                      SD_JSON_BUILD_PAIR_STRING ("type", "_googlecast._tcp"),
                      SD_JSON_BUILD_PAIR_STRING ("domain", ctx->domain ? ctx->domain : "local"),
                      SD_JSON_BUILD_PAIR_INTEGER ("ifindex", ctx->ifindex),
                      SD_JSON_BUILD_PAIR_INTEGER ("family", AF_UNSPEC),
                      SD_JSON_BUILD_PAIR_UNSIGNED ("flags", 0));
  if (r < 0)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to build ResolveService params: %s",
                               g_strerror (-r));
      goto out;
    }

  r = sd_varlink_call (vl,
                       "io.systemd.Resolve.ResolveService",
                       params,
                       &reply,
                       &error_id);
  if (r < 0)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "ResolveService call failed: %s",
                               g_strerror (-r));
      goto out;
    }

  if (error_id)
    {
      int err = sd_varlink_error_to_errno (error_id, reply);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "ResolveService error: %s (%d)",
                               error_id, err);
      goto out;
    }

  sd_json_variant *services = sd_json_variant_by_key (reply, "services");
  sd_json_variant *txt = sd_json_variant_by_key (reply, "txt");

  gchar *ip = extract_ip_from_services (services);
  gchar *display_name = extract_friendly_name_from_txt (txt);

  GVariant *ret = g_variant_new ("(msms)",
                                 ip ? ip : NULL,
                                 display_name ? display_name : NULL);
  g_free (ip);
  g_free (display_name);

  g_task_return_pointer (task, g_variant_ref_sink (ret), (GDestroyNotify) g_variant_unref);

out:
  if (params)
    sd_json_variant_unref (params);
  if (vl)
    sd_varlink_unref (vl);
}

static void
resolve_sink_cb (GObject      *source,
                 GAsyncResult *res,
                 gpointer      user_data);

static gboolean
nd_sd_cc_provider_resolve_sink_async (NdSdCCProvider      *provider,
                                      ResolveCtx          *ctx,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data)
{
  g_debug ("NdSdCCProvider: resolve async start for name=\"%s\" ifindex=%d (ctx=%p)",
           ctx->name, ctx->ifindex, (void *) ctx);

  GTask *task = g_task_new (provider, cancellable, callback, user_data);
  g_task_set_task_data (task, ctx, (GDestroyNotify) resolve_ctx_free);
  g_task_run_in_thread (task, resolve_sink_task_thread);
  g_object_unref (task);
  return TRUE;
}

static gboolean
nd_sd_cc_provider_resolve_sink_finish (NdSdCCProvider  *provider,
                                       GAsyncResult    *result,
                                       gchar          **out_ip,
                                       gchar          **out_display_name,
                                       GError         **error)
{
  g_return_val_if_fail (g_task_is_valid (result, provider), FALSE);
  g_return_val_if_fail (out_ip != NULL, FALSE);
  g_return_val_if_fail (out_display_name != NULL, FALSE);

  *out_ip = NULL;
  *out_display_name = NULL;

  GVariant *ret = g_task_propagate_pointer (G_TASK (result), error);
  if (!ret)
    {
      g_debug ("NdSdCCProvider: resolve_sink_finish: no result variant");
      return FALSE;
    }

  const gchar *typestr = g_variant_get_type_string (ret);
  g_debug ("NdSdCCProvider: resolve_sink_finish: variant type=\"%s\"", typestr);

  /* Expect (msms) — two maybe strings */
  gchar *ip_local = NULL;
  gchar *display_name_local = NULL;

  g_variant_get (ret, "(msms)", &ip_local, &display_name_local);
  g_variant_unref (ret);

  g_debug ("NdSdCCProvider: resolve_sink_finish: got ip=\"%s\" display_name=\"%s\"",
           ip_local ? ip_local : "(null)", display_name_local ? display_name_local : "(null)");

  if (ip_local && *ip_local)
    *out_ip = g_strdup (ip_local);
  if (display_name_local && *display_name_local)
    *out_display_name = g_strdup (display_name_local);

  g_free (ip_local);
  g_free (display_name_local);

  g_debug ("NdSdCCProvider: resolve_sink_finish: out_ip=\"%s\" out_display_name=\"%s\"",
           *out_ip ? *out_ip : "(null)", *out_display_name ? *out_display_name : "(null)");

  return TRUE;
}

static void
resolve_sink_cb (GObject      *source,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  NdSdCCProvider *self = ND_SD_CC_PROVIDER (source);
  ResolveCtx *ctx = (ResolveCtx *) user_data;

  /* Copy what we need from ctx; GTask will free ctx later. */
  g_autofree gchar *name_copy = g_strdup (ctx->name);
  int ifindex_copy = ctx->ifindex;

  g_autofree gchar *ip = NULL;
  g_autofree gchar *display_name = NULL;
  g_autoptr (GError) error = NULL;

  if (!nd_sd_cc_provider_resolve_sink_finish (self, res, &ip, &display_name, &error))
    {
      g_debug ("NdSdCCProvider: ResolveService failed for \"%s\": %s",
               name_copy ? name_copy : "(null)",
               error ? error->message : "unknown error");
      /* DO NOT free ctx here; GTask owns it. */
      return;
    }

  if (!ip || !*ip)
    {
      g_warning ("NdSdCCProvider: No IP address found for %s", name_copy);
      return;
    }

  /* Check if we already have this exact sink */
  NdCCSink *sink = find_sink_by_name (self, name_copy);
  if (sink && compare_sinks (sink, name_copy, ip))
    {
      g_debug ("NdSdCCProvider: Sink %s already exists with same info", name_copy);
      return;
    }

  if (sink)
    {
      /* Remove the old sink, we'll add a new one with updated info */
      g_debug ("NdSdCCProvider: Updating sink %s", name_copy);
      g_ptr_array_remove (self->sinks, sink);
      g_signal_emit_by_name (self, "sink-removed", sink);
    }

  g_debug ("NdSdCCProvider: Adding new Chromecast sink: %s (%s) at %s",
           name_copy, display_name ? display_name : name_copy, ip);

  sink = nd_cc_sink_new (self->signalling_client,
                         g_strdup (name_copy),
                         g_strdup (ip),
                         g_strdup (display_name ? display_name : name_copy),
                         ifindex_copy);

  g_ptr_array_add (self->sinks, sink);
  g_signal_emit_by_name (self, "sink-added", sink);
}

typedef struct {
  NdSdCCProvider *provider;
  NdCCSink       *sink;
} DeferredRemovalData;

static gboolean
sink_remove_idle_cb (gpointer user_data)
{
  DeferredRemovalData *data = user_data;
  NdSdCCProvider *self = data->provider;
  NdCCSink *sink = data->sink;

  if (g_ptr_array_find (self->sinks, sink, NULL))
    {
      g_debug ("NdSdCCProvider: Deferred removal of sink");
      g_ptr_array_remove (self->sinks, sink);
      g_signal_emit_by_name (self, "sink-removed", sink);
    }

  g_object_unref (data->provider);
  g_object_unref (data->sink);
  g_free (data);

  return G_SOURCE_REMOVE;
}

static void
sink_notify_state_cb (NdCCSink       *sink,
                      GParamSpec     *pspec,
                      NdSdCCProvider *self)
{
  NdSinkState state;
  DeferredRemovalData *data;

  g_object_get (sink, "state", &state, NULL);

  if (state == ND_SINK_STATE_DISCONNECTED)
    {
      data = g_new0 (DeferredRemovalData, 1);
      data->provider = g_object_ref (self);
      data->sink = g_object_ref (sink);

      g_idle_add (sink_remove_idle_cb, data);
    }
}

static void
handle_service_added (NdSdCCProvider *self,
                      sd_json_variant *service)
{
  sd_json_variant *name_v, *domain_v, *ifindex_v;
  const char *name, *domain;
  int ifindex;

  name_v = sd_json_variant_by_key (service, "name");
  domain_v = sd_json_variant_by_key (service, "domain");
  ifindex_v = sd_json_variant_by_key (service, "ifindex");

  if (!name_v || !sd_json_variant_is_string (name_v))
    return;

  name = sd_json_variant_string (name_v);
  domain = domain_v && sd_json_variant_is_string (domain_v) ?
           sd_json_variant_string (domain_v) : "local";
  ifindex = ifindex_v ? sd_json_variant_integer (ifindex_v) : 0;

  /* Filter by interface if we have a specific one */
  if (self->nm_ifindex > 0 && ifindex > 0 && ifindex != self->nm_ifindex)
    return;

  g_debug ("NdSdCCProvider: handle_service_added name=\"%s\" domain=\"%s\" ifindex=%d",
           name ? name : "(null)", domain ? domain : "(null)", ifindex);

  ResolveCtx *ctx = g_new0 (ResolveCtx, 1);
  ctx->name = g_strdup (name);
  ctx->domain = g_strdup (domain ? domain : "local");
  ctx->ifindex = ifindex;

  g_debug ("NdSdCCProvider: starting async resolve for \"%s\" on ifindex %d",
           ctx->name, ctx->ifindex);

  nd_sd_cc_provider_resolve_sink_async (self,
                                        ctx,
                                        NULL,
                                        resolve_sink_cb,
                                        ctx);
}

static void
handle_service_removed (NdSdCCProvider *self,
                        sd_json_variant *service)
{
  sd_json_variant *name_v;
  const char *name;
  NdCCSink *sink;
  NdSinkState state;

  name_v = sd_json_variant_by_key (service, "name");
  if (!name_v || !sd_json_variant_is_string (name_v))
    return;

  name = sd_json_variant_string (name_v);
  sink = find_sink_by_name (self, name);

  if (!sink)
    return;

  g_debug ("NdSdCCProvider: Service removed: %s", name);

  g_object_get (sink, "state", &state, NULL);

  /* If currently streaming, defer removal */
  if (state != ND_SINK_STATE_DISCONNECTED)
    {
      g_debug ("NdSdCCProvider: Sink is streaming, deferring removal");
      g_signal_connect (sink, "notify::state",
                        G_CALLBACK (sink_notify_state_cb), self);
      return;
    }

  g_ptr_array_remove (self->sinks, sink);
  g_signal_emit_by_name (self, "sink-removed", sink);
}

static void
disconnect_from_resolved (NdSdCCProvider *self)
{
  if (self->varlink_source)
    {
      g_source_destroy (self->varlink_source);
      self->varlink_source = NULL;
    }

  if (self->link)
    {
      sd_varlink_unref (self->link);
      self->link = NULL;
    }

  self->varlink_fd = -1;
}

static gboolean
reconnect_browse_idle_cb (gpointer user_data)
{
  NdSdCCProvider *self = ND_SD_CC_PROVIDER (user_data);
  g_autoptr(GError) error = NULL;

  g_debug ("NdSdCCProvider: reconnect_browse_idle_cb: reconnecting to systemd-resolved");

  /* Clean up old connection */
  disconnect_from_resolved (self);

  /* Reconnect and re-subscribe */
  if (!nd_sd_cc_provider_browse (self, &error))
    {
      g_warning ("NdSdCCProvider: Failed to reconnect to systemd-resolved: %s",
                 error ? error->message : "unknown error");
    }
  else
    {
      g_debug ("NdSdCCProvider: Successfully reconnected to BrowseServices");
    }

  return G_SOURCE_REMOVE;
}

static int
varlink_notify_cb (sd_varlink           *link,
                   sd_json_variant      *params,
                   const char           *error_id,
                   sd_varlink_reply_flags_t flags,
                   void                 *userdata)
{
  NdSdCCProvider *self = ND_SD_CC_PROVIDER (userdata);
  sd_json_variant *array;

  g_debug ("NdSdCCProvider: varlink_notify_cb: called, error_id=%s, flags=0x%lx",
           error_id ? error_id : "(null)", (unsigned long) flags);

  if (error_id)
    {
      g_debug ("NdSdCCProvider: Browse notification error: %s", error_id);

      /* If the subscription timed out or errored, we need to reconnect.
       * Schedule reconnection in an idle callback to avoid re-entrancy. */
      if (g_strcmp0 (error_id, "io.systemd.TimedOut") == 0 ||
          g_strcmp0 (error_id, "io.systemd.Disconnected") == 0)
        {
          g_debug ("NdSdCCProvider: BrowseServices subscription ended (%s), scheduling reconnect",
                   error_id);
          g_idle_add (reconnect_browse_idle_cb, self);
        }
      return 0;
    }

  if (!params)
    {
      g_debug ("NdSdCCProvider: varlink_notify_cb: no params");
      return 0;
    }

  array = sd_json_variant_by_key (params, "browserServiceData");

  if (!array || !sd_json_variant_is_array (array))
    {
      g_debug ("NdSdCCProvider: varlink_notify_cb: no browserServiceData array present");
      return 0;
    }

  size_t n = sd_json_variant_elements (array);
  g_debug ("NdSdCCProvider: varlink_notify_cb: browserServiceData elements=%zu", n);

  for (size_t i = 0; i < n; i++)
    {
      sd_json_variant *entry = sd_json_variant_by_index (array, i);
      if (!entry || !sd_json_variant_is_object (entry))
        continue;

      sd_json_variant *update_flag_v = sd_json_variant_by_key (entry, "updateFlag");
      sd_json_variant *name_v        = sd_json_variant_by_key (entry, "name");
      sd_json_variant *type_v        = sd_json_variant_by_key (entry, "type");
      sd_json_variant *domain_v      = sd_json_variant_by_key (entry, "domain");
      sd_json_variant *ifindex_v     = sd_json_variant_by_key (entry, "ifindex");

      const char *update_flag = update_flag_v && sd_json_variant_is_string (update_flag_v)
                              ? sd_json_variant_string (update_flag_v) : NULL;
      const char *name = name_v && sd_json_variant_is_string (name_v)
                       ? sd_json_variant_string (name_v) : NULL;
      const char *type = type_v && sd_json_variant_is_string (type_v)
                       ? sd_json_variant_string (type_v) : NULL;
      const char *domain = domain_v && sd_json_variant_is_string (domain_v)
                         ? sd_json_variant_string (domain_v) : NULL;
      int64_t ifindex = ifindex_v && sd_json_variant_is_integer (ifindex_v)
                      ? sd_json_variant_integer (ifindex_v) : 0;

      g_debug ("NdSdCCProvider: varlink_notify_cb: entry[%zu] update=%s name=%s type=%s domain=%s ifindex=%" G_GINT64_FORMAT,
               i,
               update_flag ? update_flag : "(null)",
               name ? name : "(null)",
               type ? type : "(null)",
               domain ? domain : "(null)",
               ifindex);

      if (g_strcmp0 (update_flag, "added") == 0)
        handle_service_added (self, entry);
      else if (g_strcmp0 (update_flag, "removed") == 0)
        handle_service_removed (self, entry);
    }

  return 0;
}

/* ---------------- Varlink → GLib main loop integration ---------------- */

static gboolean
varlink_io_cb (GIOChannel   *source,
               GIOCondition  condition,
               gpointer      user_data)
{
  NdSdCCProvider *self = ND_SD_CC_PROVIDER (user_data);
  int r;

  g_debug ("NdSdCCProvider: varlink_io_cb: condition=0x%x", condition);

  if (condition & (G_IO_HUP | G_IO_ERR))
    {
      g_warning ("NdSdCCProvider: varlink_io_cb: HUP or ERR");
      return G_SOURCE_REMOVE;
    }

  while ((r = sd_varlink_process (self->link)) > 0)
    g_debug ("NdSdCCProvider: varlink_io_cb: processed, r=%d", r);

  if (r < 0)
    {
      g_warning ("NdSdCCProvider: varlink_process failed: %s", g_strerror (-r));
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

static void
nd_sd_cc_provider_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  NdSdCCProvider *self = ND_SD_CC_PROVIDER (object);

  switch (prop_id)
    {
    case PROP_NM_DEVICE:
      g_value_set_object (value, self->nm_device);
      break;

    case PROP_DISCOVER:
      g_value_set_boolean (value, self->discover);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_sd_cc_provider_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  NdSdCCProvider *self = ND_SD_CC_PROVIDER (object);

  switch (prop_id)
    {
    case PROP_NM_DEVICE:
      self->nm_device = g_value_dup_object (value);
      if (self->nm_device)
        {
          const char *ifname = nm_device_get_iface (self->nm_device);
          if (ifname)
            self->nm_ifindex = if_nametoindex (ifname);
        }
      break;

    case PROP_DISCOVER:
      self->discover = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_sd_cc_provider_finalize (GObject *object)
{
  NdSdCCProvider *self = ND_SD_CC_PROVIDER (object);

  g_debug ("NdSdCCProvider: finalize");

  if (self->varlink_source)
    {
      g_source_destroy (self->varlink_source);
      g_source_unref (self->varlink_source);
      self->varlink_source = NULL;
    }

  g_clear_pointer (&self->link, sd_varlink_unref);
  g_clear_pointer (&self->sinks, g_ptr_array_unref);
  g_clear_object (&self->signalling_client);
  g_clear_object (&self->nm_device);

  G_OBJECT_CLASS (nd_sd_cc_provider_parent_class)->finalize (object);
}

static void
nd_sd_cc_provider_class_init (NdSdCCProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = nd_sd_cc_provider_get_property;
  object_class->set_property = nd_sd_cc_provider_set_property;
  object_class->finalize = nd_sd_cc_provider_finalize;

  props[PROP_NM_DEVICE] =
    g_param_spec_object ("nm-device", "NM Device",
                         "The NetworkManager device to use for discovery",
                         NM_TYPE_DEVICE,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, props);

  g_object_class_override_property (object_class, PROP_DISCOVER, "discover");
}

static void
nd_sd_cc_provider_init (NdSdCCProvider *self)
{
  self->sinks = g_ptr_array_new_with_free_func (g_object_unref);
  self->signalling_client = g_socket_client_new ();
  self->discover = TRUE;
  self->varlink_fd = -1;
}

static GList *
nd_sd_cc_provider_get_sinks (NdProvider *provider)
{
  NdSdCCProvider *self = ND_SD_CC_PROVIDER (provider);
  GList *res = NULL;

  for (guint i = 0; i < self->sinks->len; i++)
    res = g_list_prepend (res, self->sinks->pdata[i]);

  return res;
}

static void
nd_sd_cc_provider_provider_iface_init (NdProviderIface *iface)
{
  iface->get_sinks = nd_sd_cc_provider_get_sinks;
}

NdSdCCProvider *
nd_sd_cc_provider_new (NMDevice *nm_device)
{
  return g_object_new (ND_TYPE_SD_CC_PROVIDER,
                       "nm-device", nm_device,
                       NULL);
}

gboolean
nd_sd_cc_provider_browse (NdSdCCProvider *provider,
                          GError        **error)
{
  int r;

  g_return_val_if_fail (ND_IS_SD_CC_PROVIDER (provider), FALSE);

  g_debug ("NdSdCCProvider: browse: connecting to systemd-resolved");

  /* Connect to systemd-resolved */
  r = sd_varlink_connect_address (&provider->link,
                                  "/run/systemd/resolve/io.systemd.Resolve");
  if (r < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to connect to systemd-resolved: %s", g_strerror (-r));
      provider->link = NULL;
      return FALSE;
    }

  g_debug ("NdSdCCProvider: browse: connected to systemd-resolved");

  /* Get the file descriptor for the varlink connection */
  provider->varlink_fd = sd_varlink_get_fd (provider->link);
  if (provider->varlink_fd < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "sd_varlink_get_fd failed");
      sd_varlink_unref (provider->link);
      provider->link = NULL;
      return FALSE;
    }

  g_debug ("NdSdCCProvider: browse: got varlink fd=%d", provider->varlink_fd);

  /* Set up GSource for varlink events using GIOChannel like MICE provider */
  GIOChannel *channel = g_io_channel_unix_new (provider->varlink_fd);
  provider->varlink_source = g_io_create_watch (channel, G_IO_IN | G_IO_HUP | G_IO_ERR);
  g_io_channel_unref (channel);

  g_source_set_callback (provider->varlink_source,
                         (GSourceFunc) varlink_io_cb,
                         provider,
                         NULL);
  g_source_attach (provider->varlink_source, NULL);

  g_debug ("NdSdCCProvider: browse: attached varlink source to main loop");

  /* Set userdata and bind reply handler */
  sd_varlink_set_userdata (provider->link, provider);
  sd_varlink_bind_reply (provider->link, varlink_notify_cb);

  g_debug ("NdSdCCProvider: browse: bound reply handler");

  /* Start observing for Chromecast services */
  r = sd_varlink_observebo (provider->link,
                            "io.systemd.Resolve.BrowseServices",
                            SD_JSON_BUILD_PAIR_STRING ("domain", "local"),
                            SD_JSON_BUILD_PAIR_STRING ("type", "_googlecast._tcp"),
                            SD_JSON_BUILD_PAIR_INTEGER ("ifindex", provider->nm_ifindex),
                            SD_JSON_BUILD_PAIR_UNSIGNED ("flags", 0));
  if (r < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "BrowseServices failed: %s", g_strerror (-r));
      return FALSE;
    }

  g_debug ("NdSdCCProvider: browse: called sd_varlink_observebo");

  /* Flush to ensure the request is sent */
  sd_varlink_flush (provider->link);

  g_debug ("NdSdCCProvider: browse: Started browsing for _googlecast._tcp services on ifindex=%d",
           provider->nm_ifindex);

  return TRUE;
}

GSocketClient *
nd_sd_cc_provider_get_signalling_client (NdSdCCProvider *provider)
{
  g_return_val_if_fail (ND_IS_SD_CC_PROVIDER (provider), NULL);
  return provider->signalling_client;
}
