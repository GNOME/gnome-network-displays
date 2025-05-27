#pragma once

#include <gio/gio.h>
#include <glib-object.h>
#include <gst/gst.h>

G_BEGIN_DECLS

#define ND_TYPE_PULSEAUDIO (nd_pulseaudio_get_type ())

G_DECLARE_FINAL_TYPE (NdPulseaudio, nd_pulseaudio, ND, PULSEAUDIO, GObject)

NdPulseaudio *nd_pulseaudio_new (gchar * name, gchar * uuid);

GstElement           *nd_pulseaudio_get_source (NdPulseaudio *self);

void  nd_pulseaudio_unload (NdPulseaudio *self);

G_END_DECLS
