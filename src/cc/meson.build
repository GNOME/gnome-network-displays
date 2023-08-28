
cc_sources = [
  'cast_channel.pb-c.c',
  'cc-comm.c',
  'cc-ctrl.c',
  'cc-json-helper.c',
  'cc-http-server.c',
  'cc-media-factory.c',
]

#enum_headers = files()
#
#gnome_nd_sources += gnome.mkenums_simple(
#  'nd-enum-types',
#  sources: enum_headers,
#)

cc_deps = [
  dependency('libprotobuf-c', version: '>= 1.0.0'),
  dependency('json-glib-1.0', version: '>= 1.0'),
  dependency('libsoup-3.0', version: '>= 3.0'),
  dependency('gstreamer-rtsp-server-1.0'),
]

cc_cast_channel = static_library(
  'cc-cast-channel',
  cc_sources,
  dependencies: cc_deps,
)
