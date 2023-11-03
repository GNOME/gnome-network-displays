#pragma once

#include "nd-sink.h"

G_BEGIN_DECLS

#define ND_TYPE_DUMMY_WFD_SINK (nd_dummy_wfd_sink_get_type ())

G_DECLARE_FINAL_TYPE (NdDummyWFDSink, nd_dummy_wfd_sink, ND, DUMMY_WFD_SINK, GObject)

NdDummyWFDSink * nd_dummy_wfd_sink_new (void);
NdDummyWFDSink * nd_dummy_wfd_sink_from_uri (gchar *uri);

G_END_DECLS
