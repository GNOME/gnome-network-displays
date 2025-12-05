/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <systemd/sd-varlink.h>

#include <arpa/inet.h>
#include <net/if.h> 
#include <gio/gio.h>
#include <stdint.h>

#include "nd-sd-wfd-mice-provider.h"
#include "nd-wfd-mice-sink.h"
#include "nd-provider.h"
#include "nd-sink.h"

#ifndef SD_RESOLVED_MDNS_IPV4
#define SD_RESOLVED_MDNS_IPV4  (UINT64_C(1) << 3)
#endif

#ifndef SD_RESOLVED_MDNS_IPV6
#define SD_RESOLVED_MDNS_IPV6  (UINT64_C(1) << 4)
#endif

#ifndef SD_RESOLVED_MDNS
#define SD_RESOLVED_MDNS       (SD_RESOLVED_MDNS_IPV4 | SD_RESOLVED_MDNS_IPV6)
#endif

struct _NdSdWfdMiceProvider {
  GObject parent_instance;

  GPtrArray  *sinks;

  sd_varlink *link;
  int         varlink_fd;
  GSource    *varlink_source;

  NMDevice   *nm_device;
  int         ifindex;

  gboolean    discover;

  gboolean    initial_snapshot_done;

  /* Sinks pending removal (key: sink, value: signal handler id as GUINT_TO_POINTER) */
  GHashTable *pending_removal;
};

enum {
  PROP_DEVICE = 1,
  PROP_DISCOVER,
  PROP_LAST = PROP_DISCOVER,
};

static void   nd_sd_wfd_mice_provider_provider_iface_init (NdProviderIface *iface);
static GList *nd_sd_wfd_mice_provider_get_sinks (NdProvider *provider);

G_DEFINE_TYPE_EXTENDED (NdSdWfdMiceProvider,
                        nd_sd_wfd_mice_provider,
                        G_TYPE_OBJECT,
                        0,
                        G_IMPLEMENT_INTERFACE (ND_TYPE_PROVIDER,
                                               nd_sd_wfd_mice_provider_provider_iface_init))

static GParamSpec *props[PROP_LAST] = { NULL, };
/* ---------------- ResolveService async plumbing (private) ---------------- */

typedef struct {
  gchar *name;
  gchar *type;    /* _display._tcp */
  gchar *domain;  /* local */
  gint   ifindex;
} ResolveCtx;

static void
resolve_ctx_free (ResolveCtx *ctx)
{
    if (!ctx)
        return;
    g_free (ctx->name);
    g_free (ctx->type);
    g_free (ctx->domain);
    g_free (ctx);
}

static gchar *
extract_p2p_mac_from_txt (sd_json_variant *txt_array)
{
  if (!txt_array || !sd_json_variant_is_array (txt_array))
    return NULL;

  size_t n = sd_json_variant_elements (txt_array);

  for (size_t i = 0; i < n; i++) {
    sd_json_variant *entry = sd_json_variant_by_index (txt_array, i);
    if (!entry || !sd_json_variant_is_string (entry))
      continue;

    const char *s = sd_json_variant_string (entry);
    if (!s)
      continue;

    static const char prefix[] = "p2pMAC=";

    if (g_str_has_prefix (s, prefix)) {
      const char *value = s + strlen (prefix);
      if (*value == '\0')
        continue;
      return g_strdup (value);
    }
  }

  return NULL;
}

/* services is an array of objects like:
 * {
 *   "addresses": [
 *      { "family": 2, "address": [192, 168, 1, 10] },
 *      { "family": 10, "address": [ ...16 bytes... ] }
 *   ],
 *   ...
 * }
 *
 * address is a JSON array of integers (0–255), representing raw bytes.
 */
static gchar *
extract_ip_from_services (sd_json_variant *services_array)
{
  if (!services_array || !sd_json_variant_is_array (services_array))
    return NULL;

  gchar *ipv4 = NULL;
  gchar *ipv6 = NULL;

  size_t sn = sd_json_variant_elements (services_array);

  for (size_t si = 0; si < sn; si++) {
    sd_json_variant *srv_entry = sd_json_variant_by_index (services_array, si);
    if (!srv_entry || !sd_json_variant_is_object (srv_entry))
      continue;

    sd_json_variant *addr_array = sd_json_variant_by_key (srv_entry, "addresses");
    if (!addr_array || !sd_json_variant_is_array (addr_array))
      continue;

    size_t an = sd_json_variant_elements (addr_array);

    for (size_t ai = 0; ai < an; ai++) {
      sd_json_variant *addr_entry = sd_json_variant_by_index (addr_array, ai);
      if (!addr_entry || !sd_json_variant_is_object (addr_entry))
        continue;

      sd_json_variant *family_v  = sd_json_variant_by_key (addr_entry, "family");
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

      for (size_t bi = 0; bi < bn; bi++) {
        sd_json_variant *b = sd_json_variant_by_index (address_v, bi);
        if (!b || !sd_json_variant_is_unsigned (b))
          goto next_addr;
        uint64_t v = sd_json_variant_unsigned (b);
        if (v > 255)
          goto next_addr;
        buf[bi] = (uint8_t) v;
      }

      {
        char ipbuf[INET6_ADDRSTRLEN + 1] = {0};

        if (family == AF_INET) {
          if (!inet_ntop (AF_INET, buf, ipbuf, sizeof ipbuf))
            goto next_addr;
          g_clear_pointer (&ipv4, g_free);
          ipv4 = g_strdup (ipbuf);
        } else if (family == AF_INET6) {
          if (!inet_ntop (AF_INET6, buf, ipbuf, sizeof ipbuf))
            goto next_addr;
          g_clear_pointer (&ipv6, g_free);
          ipv6 = g_strdup (ipbuf);
        }
      }

    next_addr:
      ;
    }
  }

  if (ipv4)
    return ipv4;
  return ipv6;
}

/* ---------------- Sink comparison (same as Avahi provider) ---------------- */

static gboolean
compare_sinks (NdWFDMiceSink *a, NdWFDMiceSink *b)
{
  gchar *a_name = NULL, *b_name = NULL;
  gchar *a_ip = NULL,   *b_ip = NULL;
  gint   a_iface = 0,    b_iface = 0;

  g_object_get (a, "name", &a_name, "ip", &a_ip, "interface", &a_iface, NULL);
  g_object_get (b, "name", &b_name, "ip", &b_ip, "interface", &b_iface, NULL);

  gboolean equal =
    g_strcmp0 (a_name, b_name) == 0 &&
    g_strcmp0 (a_ip,   b_ip)   == 0 &&
    a_iface == b_iface;

  g_free (a_name);
  g_free (b_name);
  g_free (a_ip);
  g_free (b_ip);

  return equal;
}

static void
resolve_sink_task_thread (GTask        *task,
                          gpointer      source_object,
                          gpointer      task_data,
                          GCancellable *cancellable)
{
  ResolveCtx *info = task_data;
  sd_varlink *vl = NULL;
  sd_json_variant *params = NULL;
  sd_json_variant *reply = NULL; /* Borrowed from varlink; do NOT unref */
  const char *error_id = NULL;
  int r;

  r = sd_varlink_connect_address (&vl, "/run/systemd/resolve/io.systemd.Resolve");
  if (r < 0) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "Failed to connect to systemd-resolved: %s",
                             g_strerror (-r));
    goto out;
  }

  r = sd_json_buildo (&params,
                      SD_JSON_BUILD_PAIR_STRING ("name", info->name),
                      SD_JSON_BUILD_PAIR_STRING ("type", info->type),
                      SD_JSON_BUILD_PAIR_STRING ("domain", info->domain),
                      SD_JSON_BUILD_PAIR_INTEGER ("ifindex", info->ifindex),
                      SD_JSON_BUILD_PAIR_INTEGER ("family", AF_UNSPEC),
                      SD_JSON_BUILD_PAIR_UNSIGNED ("flags", 0));
  if (r < 0) {
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
  if (r < 0) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "ResolveService call failed: %s",
                             g_strerror (-r));
    goto out;
  }

  if (error_id) {
    int err = sd_varlink_error_to_errno (error_id, reply);
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "ResolveService error: %s (%d)",
                             error_id, err);
    goto out;
  }

  sd_json_variant *services = sd_json_variant_by_key (reply, "services");
  sd_json_variant *txt      = sd_json_variant_by_key (reply, "txt");

  gchar *ip      = extract_ip_from_services (services);
  gchar *p2p_mac = extract_p2p_mac_from_txt (txt);

  GVariant *ret = g_variant_new ("(msms)",
                                 ip      ? ip      : NULL,
                                 p2p_mac ? p2p_mac : NULL);
  g_free (ip);
  g_free (p2p_mac);

  g_task_return_pointer (task, g_variant_ref_sink (ret), (GDestroyNotify) g_variant_unref);

out:
  if (params)
    sd_json_variant_unref (params);
  if (vl)
    sd_varlink_unref (vl);
}

static gboolean
nd_sd_wfd_mice_provider_resolve_sink_async(NdSdWfdMiceProvider *provider,
                                           ResolveCtx          *ctx,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data)
{
  g_debug("NdSdWfdMiceProvider: resolve async start for name=\"%s\" ifindex=%d (ctx=%p)",
        ctx->name, ctx->ifindex, ctx);

  GTask *task = g_task_new(provider, cancellable, callback, user_data);
  g_task_set_task_data(task, ctx, (GDestroyNotify) resolve_ctx_free);
  g_task_run_in_thread(task, resolve_sink_task_thread);
  g_object_unref(task);
  return TRUE;
}

static gboolean
nd_sd_wfd_mice_provider_resolve_sink_finish(NdSdWfdMiceProvider *provider,
                                            GAsyncResult        *result,
                                            gchar              **out_ip,
                                            gchar              **out_p2p_mac,
                                            GError             **error)
{
  g_return_val_if_fail (g_task_is_valid (result, provider), FALSE);
  g_return_val_if_fail (out_ip != NULL, FALSE);
  g_return_val_if_fail (out_p2p_mac != NULL, FALSE);

  *out_ip = NULL;
  *out_p2p_mac = NULL;

  GVariant *ret = g_task_propagate_pointer (G_TASK (result), error);
  if (!ret) {
    g_debug ("NdSdWfdMiceProvider: resolve_sink_finish: no result variant");
    return FALSE;
  }

  const gchar *typestr = g_variant_get_type_string (ret);
  g_debug ("NdSdWfdMiceProvider: resolve_sink_finish: variant type=\"%s\"", typestr);

  /* Expect (msms) — two maybe strings */
  gchar *ip_local = NULL;
  gchar *p2p_local = NULL;

  g_variant_get (ret, "(msms)", &ip_local, &p2p_local);
  g_variant_unref (ret);

  g_debug ("NdSdWfdMiceProvider: resolve_sink_finish: got ip=\"%s\" p2p_mac=\"%s\"",
           ip_local ? ip_local : "(null)", p2p_local ? p2p_local : "(null)");

  if (ip_local && *ip_local)
    *out_ip = g_strdup (ip_local);
  if (p2p_local && *p2p_local)
    *out_p2p_mac = g_strdup (p2p_local);

  g_free (ip_local);
  g_free (p2p_local);

  g_debug ("NdSdWfdMiceProvider: resolve_sink_finish: out_ip=\"%s\" out_p2p_mac=\"%s\"",
           *out_ip ? *out_ip : "(null)", *out_p2p_mac ? *out_p2p_mac : "(null)");

  return TRUE;
}

static void
resolve_sink_cb (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  NdSdWfdMiceProvider *provider = ND_SD_WFD_MICE_PROVIDER (source);
  ResolveCtx *ctx = (ResolveCtx *) user_data;

  /* Copy what we need from ctx; GTask will free ctx later. */
  g_autofree gchar *name_copy = g_strdup (ctx->name);
  int ifindex_copy = ctx->ifindex;

  g_autofree gchar *ip = NULL;
  g_autofree gchar *p2p_mac = NULL;
  g_autoptr (GError) error = NULL;

  if (!nd_sd_wfd_mice_provider_resolve_sink_finish (provider, result, &ip, &p2p_mac, &error)) {
    g_debug ("NdSdWfdMiceProvider: ResolveService failed for \"%s\": %s",
             name_copy ? name_copy : "(null)",
             error ? error->message : "unknown error");
    /* DO NOT free ctx here; GTask owns it. */
    return;
  }

  NdWFDMiceSink *sink =
    nd_wfd_mice_sink_new (g_strdup (name_copy),
                          g_strdup (ip ? ip : ""),
                          g_strdup (p2p_mac ? p2p_mac : ""),
                          ifindex_copy);

  if (g_ptr_array_find_with_equal_func (provider->sinks, sink,
                                        (GEqualFunc) compare_sinks, NULL)) {
    g_debug ("NdSdWfdMiceProvider: duplicate sink \"%s\" on ifindex %d — not adding",
             name_copy, ifindex_copy);
    g_object_unref (sink);
    return;
  }

  g_ptr_array_add (provider->sinks, sink);
  g_debug ("NdSdWfdMiceProvider: emitting sink-added for \"%s\" (ip=%s, ifindex=%d)",
           name_copy, ip ? ip : "(null)", ifindex_copy);
  g_signal_emit_by_name (provider, "sink-added", sink);
}

/* ---------------- Notification handling ---------------- */

static void
sink_state_changed_cb (GObject    *object,
                       GParamSpec *pspec,
                       gpointer    user_data);

static void
handle_added (NdSdWfdMiceProvider *provider,
              const char          *name,
              const char          *domain,
              int                  ifindex)
{
  g_debug ("NdSdWfdMiceProvider: handle_added name=\"%s\" domain=\"%s\" ifindex=%d",
            name ? name : "(null)", domain ? domain : "(null)", ifindex);

  ResolveCtx *ctx = g_new0 (ResolveCtx, 1);
  ctx->name   = g_strdup (name);
  ctx->type   = g_strdup ("_display._tcp");
  ctx->domain = g_strdup (domain ? domain : "local");
  ctx->ifindex = ifindex;

  g_debug ("NdSdWfdMiceProvider: starting async resolve for \"%s\" on ifindex %d",
            ctx->name, ctx->ifindex);

  nd_sd_wfd_mice_provider_resolve_sink_async (provider,
                                              ctx,
                                              NULL,
                                              resolve_sink_cb,
                                              ctx);
}

static void
handle_removed (NdSdWfdMiceProvider *provider,
                const char          *name,
                const char          *domain,
                int                  ifindex)
{
  g_debug ("NdSdWfdMiceProvider: handle_removed name=\"%s\" domain=\"%s\" ifindex=%d",
           name ? name : "(null)", domain ? domain : "(null)", ifindex);

  for (guint i = 0; i < provider->sinks->len; i++) {
    g_autoptr (NdWFDMiceSink) sink = g_object_ref (g_ptr_array_index (provider->sinks, i));

    gchar *remote_name = NULL;
    gint   sink_ifindex = 0;
    gint   sink_state = 0;
    g_object_get (sink,
                  "name", &remote_name,
                  "interface", &sink_ifindex,
                  "state", &sink_state,
                  NULL);

    g_debug ("NdSdWfdMiceProvider: compare sink[%u] name=\"%s\" iface=%d",
             i, remote_name ? remote_name : "(null)", sink_ifindex);

    if (g_str_equal (remote_name, name) && (sink_ifindex == ifindex))
      {
        /* Do not remove sinks that are actively trying to stream or streaming.
         * Instead, mark them for deferred removal when streaming ends. */
        if (sink_state == ND_SINK_STATE_WAIT_STREAMING || sink_state == ND_SINK_STATE_STREAMING)
          {
            /* Check if already pending removal */
            if (g_hash_table_contains (provider->pending_removal, sink))
              {
                g_debug ("NdSdWfdMiceProvider: Sink '%s' already pending removal",
                         remote_name ? remote_name : "(null)");
                g_free (remote_name);
                return;
              }

            g_debug ("NdSdWfdMiceProvider: Found sink '%s' (%p) in active state (%d); deferring removal",
                     remote_name ? remote_name : "(null)", (void *)sink, sink_state);

            /* Connect to state changes so we can remove when streaming ends */
            gulong handler_id = g_signal_connect (sink,
                                                  "notify::state",
                                                  G_CALLBACK (sink_state_changed_cb),
                                                  provider);
            g_debug ("NdSdWfdMiceProvider: Connected notify::state handler %lu to sink %p",
                     handler_id, (void *)sink);
            g_hash_table_insert (provider->pending_removal,
                                 g_object_ref (sink),
                                 GUINT_TO_POINTER (handler_id));

            g_free (remote_name);
            return;
          }
        g_debug ("NdSdWfdMiceProvider: Removing sink \"%s\" from interface %i", remote_name, sink_ifindex);

        g_ptr_array_remove_index (provider->sinks, i);
        g_signal_emit_by_name (provider, "sink-removed", sink);

        g_free (remote_name);

        return;
      } 
    else
      g_debug ("NdSdWfdMiceProvider: Keeping sink \"%s\" on interface %i", remote_name, sink_ifindex);
    
    g_free (remote_name);
  }

  g_debug ("NdSdWfdMiceProvider: no matching sink found for removal name=\"%s\" ifindex=%d",
           name ? name : "(null)", ifindex);
}

typedef struct {
  NdSdWfdMiceProvider *provider;
  NdWFDMiceSink       *sink;
} DeferredRemovalData;

static gboolean
deferred_removal_idle_cb (gpointer user_data)
{
  DeferredRemovalData *data = user_data;
  NdSdWfdMiceProvider *provider = data->provider;
  NdWFDMiceSink *sink = data->sink;
  guint idx;

  g_debug ("NdSdWfdMiceProvider: deferred_removal_idle_cb for sink %p", (void *)sink);

  /* Perform the actual removal */
  if (g_ptr_array_find (provider->sinks, sink, &idx))
    {
      gchar *sink_name = NULL;
      g_object_get (sink, "name", &sink_name, NULL);

      g_debug ("NdSdWfdMiceProvider: Removing sink '%s' in idle callback",
               sink_name ? sink_name : "(null)");

      g_ptr_array_remove_index (provider->sinks, idx);
      g_signal_emit_by_name (provider, "sink-removed", sink);

      g_free (sink_name);
    }
  else
    {
      g_debug ("NdSdWfdMiceProvider: Sink %p not found in sinks array during idle removal",
               (void *)sink);
    }

  g_object_unref (sink);
  g_object_unref (provider);
  g_free (data);

  return G_SOURCE_REMOVE;
}

static void
sink_state_changed_cb (GObject    *object,
                       GParamSpec *pspec,
                       gpointer    user_data)
{
  NdWFDMiceSink *sink = ND_WFD_MICE_SINK (object);
  NdSdWfdMiceProvider *provider = ND_SD_WFD_MICE_PROVIDER (user_data);
  gint sink_state = 0;
  gchar *sink_name = NULL;

  g_object_get (sink, "state", &sink_state, "name", &sink_name, NULL);

  g_debug ("NdSdWfdMiceProvider: sink_state_changed_cb CALLED for sink %p '%s', new state=%d (0x%x)",
           (void *)sink, sink_name ? sink_name : "(null)", sink_state, sink_state);

  /* Only proceed if no longer in streaming states */
  if (sink_state == ND_SINK_STATE_WAIT_STREAMING || sink_state == ND_SINK_STATE_STREAMING)
    {
      g_free (sink_name);
      return;
    }

  /* Disconnect the signal handler and remove from pending table */
  gpointer handler_ptr = g_hash_table_lookup (provider->pending_removal, sink);
  if (handler_ptr)
    {
      gulong handler_id = GPOINTER_TO_UINT (handler_ptr);
      g_signal_handler_disconnect (sink, handler_id);
      g_hash_table_remove (provider->pending_removal, sink);
    }

  g_debug ("NdSdWfdMiceProvider: Scheduling deferred removal of sink '%s' (state=%d)",
           sink_name ? sink_name : "(null)", sink_state);

  /* Schedule removal in an idle callback to avoid re-entrancy issues.
   * The sink might still be in use by the caller that triggered the state change.
   * Use G_PRIORITY_LOW to ensure GTK has finished its frame processing before
   * we remove the sink (which triggers UI updates). */
  DeferredRemovalData *data = g_new0 (DeferredRemovalData, 1);
  data->provider = g_object_ref (provider);
  data->sink = g_object_ref (sink);
  g_idle_add_full (G_PRIORITY_LOW, deferred_removal_idle_cb, data, NULL);

  g_free (sink_name);
}

static void
disconnect_from_resolved (NdSdWfdMiceProvider *provider)
{
  if (provider->varlink_source)
    {
      g_source_destroy (provider->varlink_source);
      provider->varlink_source = NULL;
    }

  if (provider->link)
    {
      sd_varlink_unref (provider->link);
      provider->link = NULL;
    }

  provider->varlink_fd = -1;
}

static gboolean
reconnect_browse_idle_cb (gpointer user_data)
{
  NdSdWfdMiceProvider *provider = ND_SD_WFD_MICE_PROVIDER (user_data);
  g_autoptr (GError) error = NULL;

  g_debug ("NdSdWfdMiceProvider: reconnect_browse_idle_cb: reconnecting to systemd-resolved");

  /* Clean up old connection */
  disconnect_from_resolved (provider);

  /* Reconnect and re-subscribe */
  if (!nd_sd_wfd_mice_provider_browse (provider, &error))
    {
      g_warning ("NdSdWfdMiceProvider: Failed to reconnect to systemd-resolved: %s",
                 error ? error->message : "unknown error");
      /* XXX: Could schedule a retry here with exponential backoff if needed */
    }
  else
    {
      g_debug ("NdSdWfdMiceProvider: Successfully reconnected to BrowseServices");
    }

  return G_SOURCE_REMOVE;
}

static int
notify_cb (sd_varlink            *link,
           sd_json_variant       *parameters,
           const char            *error_id,
           sd_varlink_reply_flags_t flags,
           void                  *userdata)
{
  NdSdWfdMiceProvider *provider = userdata;

  if (error_id) {
    g_debug("NdSdWfdMiceProvider: notify_cb() got error_id=\"%s\"", error_id);

    /* If the subscription timed out or errored, we need to reconnect.
     * Schedule reconnection in an idle callback to avoid re-entrancy. */
    if (g_strcmp0 (error_id, "io.systemd.TimedOut") == 0 ||
        g_strcmp0 (error_id, "io.systemd.Disconnected") == 0)
      {
        g_debug ("NdSdWfdMiceProvider: BrowseServices subscription ended (%s), scheduling reconnect",
                 error_id);
        g_idle_add (reconnect_browse_idle_cb, provider);
      }
    return 0;
  }

  sd_json_variant *array = sd_json_variant_by_key (parameters, "browserServiceData");

  if (!array || !sd_json_variant_is_array (array)) {
    g_debug("NdSdWfdMiceProvider: notify_cb() no browserServiceData array present");
    return 0;
  }

  size_t n = sd_json_variant_elements (array);
  g_debug("NdSdWfdMiceProvider: notify_cb() browserServiceData elements=%zu", n);

  for (size_t i = 0; i < n; i++) {
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

    g_debug("NdSdWfdMiceProvider: notify_cb() entry[%zu] update=%s name=%s type=%s domain=%s ifindex=%"PRId64,
            i,
            update_flag ? update_flag : "(null)",
            name ? name : "(null)",
            type ? type : "(null)",
            domain ? domain : "(null)",
            ifindex);

    if (!type || g_strcmp0 (type, "_display._tcp") != 0)
      continue;

    if (g_strcmp0 (update_flag, "added") == 0) {
      /* Mark that we’ve got our initial snapshot */
      provider->initial_snapshot_done = TRUE;

      handle_added (provider, name, domain, (int)ifindex);
    } else if (g_strcmp0 (update_flag, "removed") == 0) {
      handle_removed (provider, name, domain, (int)ifindex);
    }
  }

  return 0;
}


/* ---------------- Varlink → GLib main loop integration ---------------- */

static gboolean
varlink_io_cb (GIOChannel   *source,
               GIOCondition  condition,
               gpointer      user_data)
{
  NdSdWfdMiceProvider *provider = user_data;

  if (condition & (G_IO_HUP | G_IO_ERR))
    return G_SOURCE_REMOVE;

  int r;
  while ((r = sd_varlink_process (provider->link)) > 0);
  if (r < 0)
    return G_SOURCE_REMOVE;

  return G_SOURCE_CONTINUE;
}

static gboolean
connect_to_resolved (NdSdWfdMiceProvider *provider,
                     GError             **error)
{
  int r;

  provider->link = NULL;
  provider->varlink_fd = -1;

  r = sd_varlink_connect_address (&provider->link,
                                  "/run/systemd/resolve/io.systemd.Resolve");
  if (r < 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Failed to connect to systemd-resolved: %s", g_strerror (-r));
    provider->link = NULL;
    return FALSE;
  }

  provider->varlink_fd = sd_varlink_get_fd (provider->link);
  if (provider->varlink_fd < 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "sd_varlink_get_fd failed");
    sd_varlink_unref (provider->link);
    provider->link = NULL;
    return FALSE;
  }

  GIOChannel *channel = g_io_channel_unix_new (provider->varlink_fd);
  provider->varlink_source =
    g_io_create_watch (channel, G_IO_IN | G_IO_HUP | G_IO_ERR);
  g_io_channel_unref (channel);

  g_source_set_callback (provider->varlink_source,
                         (GSourceFunc) varlink_io_cb,
                         provider,
                         NULL);
  g_source_attach (provider->varlink_source, NULL);

  sd_varlink_set_userdata (provider->link, provider);
  sd_varlink_bind_reply (provider->link, notify_cb);

  return TRUE;
}

/* ---------------- Public browse() ---------------- */

gboolean
nd_sd_wfd_mice_provider_browse (NdSdWfdMiceProvider *provider,
                                GError             **error)
{
  g_return_val_if_fail (ND_IS_SD_WFD_MICE_PROVIDER (provider), FALSE);

  if (!connect_to_resolved (provider, error))
    return FALSE;

  provider->initial_snapshot_done = FALSE;

  int r = sd_varlink_observebo (provider->link,
                                "io.systemd.Resolve.BrowseServices",
                                SD_JSON_BUILD_PAIR_STRING ("domain", "local"),
                                SD_JSON_BUILD_PAIR_STRING ("type", "_display._tcp"),
                                SD_JSON_BUILD_PAIR_INTEGER ("ifindex", provider->ifindex),
                                SD_JSON_BUILD_PAIR_UNSIGNED ("flags", 0));
  if (r < 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "BrowseServices failed: %s", g_strerror (-r));
    return FALSE;
  }

  sd_varlink_flush (provider->link);

  /* Bounded wait for the initial batch via varlink (≤1s) */
  {
    uint64_t deadline = g_get_monotonic_time () + G_TIME_SPAN_SECOND; /* 1s */
    while (!provider->initial_snapshot_done && g_get_monotonic_time () < deadline) {
      /* Wait for readability/events up to 100ms, then process */
      (void) sd_varlink_wait (provider->link, 100 * 1000); /* 100ms in µs */
      int pr = sd_varlink_process (provider->link);
      if (pr < 0) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Varlink process failed: %s", g_strerror (-pr));
        return FALSE;
      }
      /* If nothing was processed, loop until deadline or initial snapshot */
    }
  }

  g_debug ("NdSdWfdMiceProvider: BrowseServices subscription active%s",
           provider->initial_snapshot_done ? " (initial batch received)" : " (no initial batch yet)");

  return TRUE;
}



/* ---------------- NdProvider interface ---------------- */

static GList *
nd_sd_wfd_mice_provider_get_sinks (NdProvider *iface)
{
  NdSdWfdMiceProvider *provider = ND_SD_WFD_MICE_PROVIDER (iface);
  GList *list = NULL;

  for (guint i = 0; i < provider->sinks->len; i++)
    list = g_list_prepend (list, g_ptr_array_index (provider->sinks, i));

  return list;
}

/* ---------------- GObject boilerplate ---------------- */

static void
nd_sd_wfd_mice_provider_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
    NdSdWfdMiceProvider *self = ND_SD_WFD_MICE_PROVIDER (object);
    int ifindex = 0;
    const char *ifname;

    switch (prop_id) {
    case PROP_DEVICE:
      /* Construct only */
      self->nm_device = g_value_dup_object (value);
      
      ifname = nm_device_get_iface (self->nm_device);  // returns interface name
      if (!ifname)
        {
          g_warning ("NdNMDeviceRegistry: systemd-resolved provider failed get ifname for device \"%p\"", self->nm_device);
          return;
        }

      ifindex = if_nametoindex (ifname);
      if (ifindex == 0)
        {
          g_warning ("NdNMDeviceRegistry: systemd-resolved provider failed to get ifindex for ifname \"%s\"", ifname);
          return;
        }
      self->ifindex = ifindex;

      break;

    case PROP_DISCOVER:
      self->discover = g_value_get_boolean (value);
      break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
    
}

static void
nd_sd_wfd_mice_provider_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
    NdSdWfdMiceProvider *self = ND_SD_WFD_MICE_PROVIDER (object);

    switch (prop_id) {
    case PROP_DEVICE:
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
nd_sd_wfd_mice_provider_init (NdSdWfdMiceProvider *provider)
{
  provider->discover = TRUE;
  provider->sinks = g_ptr_array_new_with_free_func (g_object_unref);
  provider->link = NULL;
  provider->varlink_fd = -1;
  provider->varlink_source = NULL;
  provider->initial_snapshot_done = FALSE;
  provider->pending_removal = g_hash_table_new_full (g_direct_hash,
                                                     g_direct_equal,
                                                     g_object_unref,
                                                     NULL);
}

static void
nd_sd_wfd_mice_provider_finalize (GObject *object)
{
  NdSdWfdMiceProvider *provider = ND_SD_WFD_MICE_PROVIDER (object);

  disconnect_from_resolved (provider);

  /* Disconnect any pending state change handlers before freeing */
  if (provider->pending_removal)
    {
      GHashTableIter iter;
      gpointer key, value;
      g_hash_table_iter_init (&iter, provider->pending_removal);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          NdWFDMiceSink *sink = ND_WFD_MICE_SINK (key);
          gulong handler_id = GPOINTER_TO_UINT (value);
          g_signal_handler_disconnect (sink, handler_id);
        }
      g_hash_table_destroy (provider->pending_removal);
    }

  g_clear_pointer (&provider->sinks, g_ptr_array_unref);

  G_OBJECT_CLASS (nd_sd_wfd_mice_provider_parent_class)->finalize (object);
}

static void
nd_sd_wfd_mice_provider_class_init (NdSdWfdMiceProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = nd_sd_wfd_mice_provider_set_property;
  object_class->get_property = nd_sd_wfd_mice_provider_get_property;
  object_class->finalize = nd_sd_wfd_mice_provider_finalize;

  props[PROP_DEVICE] =
    g_param_spec_object ("device", "Device",
                         "The NMDevice the sink was found on.",
                         NM_TYPE_DEVICE,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, props);

  g_object_class_override_property (object_class, PROP_DISCOVER, "discover");
}


static void
nd_sd_wfd_mice_provider_provider_iface_init (NdProviderIface *iface)
{
  iface->get_sinks = nd_sd_wfd_mice_provider_get_sinks;
}

/* ---------------- Constructor ---------------- */

NdSdWfdMiceProvider *
nd_sd_wfd_mice_provider_new (NMDevice *nm_device)
{
  return g_object_new (ND_TYPE_SD_WFD_MICE_PROVIDER,
                       "device", nm_device,
                       NULL);
}
