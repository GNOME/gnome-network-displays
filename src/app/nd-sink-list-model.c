/* nd-sink-list-model.c
 *
 * Copyright 2018 Benjamin Berg <bberg@redhat.com>
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

#include "gnome-network-displays-config.h"
#include "nd-sink-list-model.h"
#include "nd-sink.h"

struct _NdSinkListModel
{
  GObject     parent_instance;

  NdProvider *provider;
  GList      *list;
};

enum {
  PROP_PROVIDER = 1,
  PROP_LAST,
};

static GParamSpec * props[PROP_LAST] = { NULL, };

static GType
nd_sink_list_model_get_item_type (GListModel *list)
{
  return ND_TYPE_SINK;
}

static guint
nd_sink_list_model_get_n_items (GListModel *list)
{
  NdSinkListModel *self = ND_SINK_LIST_MODEL (list);

  return g_list_length (self->list);
}

static gpointer
nd_sink_list_model_get_item (GListModel *list,
                             guint       position)
{
  NdSinkListModel *self = ND_SINK_LIST_MODEL (list);

  return g_list_nth_data (self->list, position);
}

static void
nd_sink_list_model_model_init (GListModelInterface *iface)
{
  iface->get_item_type = nd_sink_list_model_get_item_type;
  iface->get_n_items = nd_sink_list_model_get_n_items;
  iface->get_item = nd_sink_list_model_get_item;
}

G_DEFINE_TYPE_WITH_CODE (NdSinkListModel, nd_sink_list_model, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_LIST_MODEL, nd_sink_list_model_model_init))

static void
sink_added_cb (NdSinkListModel *sink_list_model,
               NdSink          *sink,
               NdProvider      *provider)
{
  guint n_items;

  if (!sink)
    {
      g_debug ("NdSinkList: No sink to add");
      return;
    }

  g_debug ("NdSinkList: Adding a sink");

  n_items = g_list_length (sink_list_model->list);
  sink_list_model->list = g_list_append (sink_list_model->list, sink);

  g_list_model_items_changed (G_LIST_MODEL (sink_list_model), n_items, 0, 1);
}

static void
sink_removed_cb (NdSinkListModel *sink_list_model,
                 NdSink          *sink,
                 NdProvider      *provider)
{
  guint pos;

  if (!sink)
    {
      g_debug ("NdSinkList: No sink to remove");
      return;
    }

  g_debug ("NdSinkList: Removing a sink");

  pos = g_list_index (sink_list_model->list, sink);
  g_return_if_fail (pos >= 0);
  sink_list_model->list = g_list_remove (sink_list_model->list, sink);

  g_list_model_items_changed (G_LIST_MODEL (sink_list_model), pos, 1, 0);
}

static void
nd_sink_list_model_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  NdSinkListModel *sink_list = ND_SINK_LIST_MODEL (object);

  switch (prop_id)
    {
    case PROP_PROVIDER:
      g_value_set_object (value, sink_list->provider);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_sink_list_model_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  NdSinkListModel *sink_list = ND_SINK_LIST_MODEL (object);

  switch (prop_id)
    {
    case PROP_PROVIDER:
      nd_sink_list_model_set_provider (sink_list, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_sink_list_model_finalize (GObject *object)
{
  NdSinkListModel *sink_list = ND_SINK_LIST_MODEL (object);

  nd_sink_list_model_set_provider (sink_list, NULL);

  G_OBJECT_CLASS (nd_sink_list_model_parent_class)->finalize (object);
}

static void
nd_sink_list_model_class_init (NdSinkListModelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = nd_sink_list_model_get_property;
  object_class->set_property = nd_sink_list_model_set_property;
  object_class->finalize = nd_sink_list_model_finalize;

  props[PROP_PROVIDER] =
    g_param_spec_object ("provider", "The sink provider",
                         "The sink provider (usually a MetaProvider) that finds the available sinks.",
                         ND_TYPE_PROVIDER,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, props);
}

static void
nd_sink_list_model_init (NdSinkListModel *self)
{
}

/**
 * nd_sink_list_model_get_provider
 * @sink_list: a #NdSinkListModel
 *
 * Retrieve the sink provider that is used to populate the sink list.
 *
 * Returns: (transfer none): The sink provider
 */
NdProvider *
nd_sink_list_model_get_provider (NdSinkListModel *sink_list)
{
  return sink_list->provider;
}

/**
 * nd_sink_list_model_set_provider
 * @sink_list: a #NdSinkListModel
 *
 * Set the sink provider that is used to populate the sink list.
 */
void
nd_sink_list_model_set_provider (NdSinkListModel *sink_list,
                                 NdProvider      *provider)
{
  if (sink_list->provider)
    {
      g_signal_handlers_disconnect_by_data (sink_list->provider, sink_list);
      g_clear_object (&sink_list->provider);
    }

  if (provider)
    {
      sink_list->provider = g_object_ref (provider);

      g_signal_connect_object (sink_list->provider,
                               "sink-added",
                               (GCallback) sink_added_cb,
                               sink_list,
                               G_CONNECT_SWAPPED);

      g_signal_connect_object (sink_list->provider,
                               "sink-removed",
                               (GCallback) sink_removed_cb,
                               sink_list,
                               G_CONNECT_SWAPPED);

    }
}

NdSinkListModel *
nd_sink_list_model_new (NdProvider *provider)
{
  return g_object_new (ND_TYPE_SINK_LIST_MODEL,
                       "provider", provider,
                       NULL);
}
