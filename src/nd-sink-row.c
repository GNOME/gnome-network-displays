/* nd-sink-row.c
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

#include "nd-sink-row.h"
#include "gnome-network-displays-config.h"

struct _NdSinkRow
{
  AdwActionRow parent_instance;

  NdSink      *sink;
};

enum {
  PROP_SINK = 1,
  PROP_LAST,
};

G_DEFINE_TYPE (NdSinkRow, nd_sink_row, ADW_TYPE_ACTION_ROW)

static GParamSpec * props[PROP_LAST] = { NULL, };


static void
nd_sink_row_sync (NdSinkRow *self)
{
  g_object_bind_property (self->sink, "display-name", self, "title", G_BINDING_SYNC_CREATE);
}

static void
nd_sink_row_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  NdSinkRow *sink_row = ND_SINK_ROW (object);

  switch (prop_id)
    {
    case PROP_SINK:
      g_value_set_object (value, sink_row->sink);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_sink_row_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  NdSinkRow *sink_row = ND_SINK_ROW (object);

  switch (prop_id)
    {
    case PROP_SINK:
      g_assert (sink_row->sink == NULL);
      sink_row->sink = g_value_dup_object (value);

      g_assert (sink_row->sink != NULL);

      g_signal_connect_object (sink_row->sink,
                               "notify",
                               (GCallback) nd_sink_row_sync,
                               object,
                               G_CONNECT_SWAPPED);
      nd_sink_row_sync (sink_row);

      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
nd_sink_row_finalize (GObject *object)
{
  NdSinkRow *sink_row = ND_SINK_ROW (object);

  g_signal_handlers_disconnect_by_data (sink_row->sink, object);
  g_clear_object (&sink_row->sink);

  G_OBJECT_CLASS (nd_sink_row_parent_class)->finalize (object);
}

static void
nd_sink_row_class_init (NdSinkRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = nd_sink_row_get_property;
  object_class->set_property = nd_sink_row_set_property;
  object_class->finalize = nd_sink_row_finalize;

  props[PROP_SINK] =
    g_param_spec_object ("sink", "Sink",
                         "The sink that the row is representing.",
                         ND_TYPE_SINK,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, props);
}

static void
nd_sink_row_init (NdSinkRow *self)
{
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (self), TRUE);
}

/**
 * nd_sink_row_new
 * @sink: a #NdSink
 *
 * Creates a new #NdSinkRow for the given #NdSink.
 *
 * Returns:
 *   a newly created #NdSinkRow
 */
GtkWidget *
nd_sink_row_new (NdSink *sink)
{
  return g_object_new (ND_TYPE_SINK_ROW,
                       "sink", sink,
                       NULL);
}

/**
 * nd_sink_row_get_sink
 * @sink_row: a #NdSinkRow
 *
 * Retrieve the #NdSink for this #NdSinkRow.
 *
 * Returns: (transfer none):
 *   the sink for this row
 */
NdSink *
nd_sink_row_get_sink (NdSinkRow *sink_row)
{
  return sink_row->sink;
}
