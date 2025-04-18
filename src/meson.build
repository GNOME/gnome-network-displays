
subdir('wfd')
subdir('cc')

gnome_nd_sources = [
  'app/main.c',
  'app/nd-codec-install.c',
  'app/nd-sink-list-model.c',
  'app/nd-sink-row.c',
  'app/nd-window.c',
  'nd-firewalld.c',
  'nd-sink.c',
  'nd-provider.c',
  'nd-meta-sink.c',
  'nd-meta-provider.c',
  'nd-wfd-mice-sink.c',
  'nd-wfd-mice-provider.c',
  'nd-cc-sink.c',
  'nd-cc-provider.c',
  'nd-wfd-p2p-sink.c',
  'nd-wfd-p2p-provider.c',
  'nd-nm-device-registry.c',
  'nd-dummy-provider.c',
  'nd-dummy-wfd-sink.c',
  'nd-dummy-cc-sink.c',
  'nd-pulseaudio.c',
  'nd-uri-helpers.c',
]

enum_headers = files('nd-sink.h')

gnome_nd_sources += gnome.mkenums_simple(
  'nd-enum-types',
  sources: enum_headers,
)

gnome_nd_deps = [
  dependency('avahi-client'),
  dependency('avahi-gobject'),
  dependency('gio-2.0', version: '>= 2.50'),
  dependency('gstreamer-1.0', version: '>= 1.14'),
  dependency('gstreamer-pbutils-1.0', version: '>= 1.14'),
  dependency('gstreamer-plugins-base-1.0'),
  dependency('gstreamer-rtsp-server-1.0'),
  dependency('gtk4'),
  dependency('libnm', version: '>= 1.15'),
  dependency('libportal-gtk4', version: '>= 0.7'),
  dependency('libpulse-mainloop-glib'),
  dependency('libadwaita-1', version: '>= 1.0.0'),
]

gnome_nd_deps += wfd_server_deps
gnome_nd_deps += cc_deps

gnome_nd_sources += gnome.compile_resources('gnome-nd-resources',
  'gnome-network-displays.gresource.xml',
  c_name: 'gnome_screencast'
)

executable('gnome-network-displays',
  gnome_nd_sources,
  dependencies: gnome_nd_deps,
  install: true,
  link_with: [wfd_server, cc_cast_channel],
)
