
cc_sources = [
  'cast_channel.pb-c.c',
  'cc-comm.c',
  'cc-ctrl.c',
  'cc-json-helper.c',
]

#enum_headers = files()
#
#gnome_nd_sources += gnome.mkenums_simple(
#  'nd-enum-types',
#  sources: enum_headers,
#)

cc_deps = [
  dependency('gstreamer-video-1.0', version: '>= 1.14'),
  dependency('gstreamer-rtsp-1.0', version: '>= 1.14'),
  dependency('libprotobuf-c', version: '>= 1.0.0'),
  dependency('json-glib-1.0', version: '>= 1.0'),
]

cc_cast_channel = static_library(
  'cc-cast-channel',
  cc_sources,
  dependencies: cc_deps,
)
