/* nd-sink.c
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
#include "nd-sink.h"
#include <gst/gst.h>

typedef NdSinkIface NdSinkInterface;
G_DEFINE_INTERFACE (NdSink, nd_sink, G_TYPE_OBJECT);

static void
nd_sink_default_init (NdSinkIface *iface)
{
  iface->start_stream = NULL;

  g_signal_new ("create-source", ND_TYPE_SINK, G_SIGNAL_RUN_LAST,
                0,
                g_signal_accumulator_first_wins, NULL,
                NULL,
                GST_TYPE_ELEMENT, 0);

  g_signal_new ("create-audio-source", ND_TYPE_SINK, G_SIGNAL_RUN_LAST,
                0,
                g_signal_accumulator_first_wins, NULL,
                NULL,
                GST_TYPE_ELEMENT, 0);

  g_object_interface_install_property (iface,
                                       g_param_spec_string ("display-name",
                                                            "Display Name",
                                                            "The name of the sink to display to the user",
                                                            NULL,
                                                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_interface_install_property (iface,
                                       g_param_spec_boxed ("matches",
                                                           "Match strings",
                                                           "One or more strings that uniquely identify the sink. This is used for de-duplication and should never change over the lifetime of the object.",
                                                           G_TYPE_PTR_ARRAY,
                                                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_interface_install_property (iface,
                                       g_param_spec_int ("priority",
                                                         "Sink Priority",
                                                         "The priority of this sink for de-duplication purposes (a higher priority will be the preferred method).",
                                                         G_MININT,
                                                         G_MAXINT,
                                                         0,
                                                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_interface_install_property (iface,
                                       g_param_spec_enum ("state",
                                                          "Sink State",
                                                          "The current state of the sink.",
                                                          ND_TYPE_SINK_STATE,
                                                          ND_SINK_STATE_DISCONNECTED,
                                                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_interface_install_property (iface,
                                       g_param_spec_boxed ("missing-video-codec",
                                                           "Missing Video Codec",
                                                           "One of the video codecs elements in the list is required.",
                                                           G_TYPE_STRV,
                                                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_interface_install_property (iface,
                                       g_param_spec_boxed ("missing-audio-codec",
                                                           "Missing Audio Codec",
                                                           "One of the audio codec elements in the list is required.",
                                                           G_TYPE_STRV,
                                                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_interface_install_property (iface,
                                       g_param_spec_string ("missing-firewall-zone",
                                                            "Missing Firewall Zone",
                                                            "Operation requires a specific firewall zone.",
                                                            NULL,
                                                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}

/**
 * nd_sink_start_stream
 * @sink: the #NdSink
 *
 * Start streaming to this sink. The returned sink is the one
 * that is actually streaming, and may differ from the one this
 * is called on (i.e. if this is a MetaSink grouping multiple sinks).
 *
 * Returns: (transfer full):
 *   The streaming sink owned by the caller.
 */
NdSink *
nd_sink_start_stream (NdSink *sink)
{
  NdSinkIface *iface = ND_SINK_GET_IFACE (sink);

  return iface->start_stream (sink);
}

/**
 * nd_sink_stop_stream
 * @sink: the #NdSink
 *
 * Stop any active streaming or connection attempt to this sink.
 */
void
nd_sink_stop_stream (NdSink *sink)
{
  NdSinkIface *iface = ND_SINK_GET_IFACE (sink);

  iface->stop_stream (sink);
}
