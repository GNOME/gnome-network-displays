#pragma once

#include "nd-sink.h"

G_BEGIN_DECLS

#define ND_TYPE_DUMMY_CC_SINK (nd_dummy_cc_sink_get_type ())

G_DECLARE_FINAL_TYPE (NdDummyCCSink, nd_dummy_cc_sink, ND, DUMMY_CC_SINK, GObject)

NdDummyCCSink * nd_dummy_cc_sink_new (void);
NdDummyCCSink * nd_dummy_cc_sink_from_uri (gchar *uri);

G_END_DECLS
