
subdir('wfd')
subdir('cc')

gnome_nd_common_sources = [
  'nd-firewalld.c',
  'nd-sink.c',
  'nd-provider.c',
  'nd-meta-sink.c',
  'nd-meta-provider.c',
  'nd-wfd-mice-sink.c',
  'nd-cc-sink.c',
  'nd-wfd-p2p-sink.c',
  'nd-wfd-p2p-provider.c',
  'nd-nm-device-registry.c',
  'nd-dummy-provider.c',
  'nd-dummy-wfd-sink.c',
  'nd-dummy-cc-sink.c',
  'nd-pulseaudio.c',
  'nd-uri-helpers.c',
]

systemd_sources = [
  'nd-sd-wfd-mice-provider.c',
  'nd-sd-cc-provider.c',
]

avahi_sources = [
  'nd-wfd-mice-provider.c',
  'nd-cc-provider.c',
]

if get_option('systemd_resolved')
  gnome_nd_common_sources += systemd_sources
else
  gnome_nd_common_sources += avahi_sources
endif

enum_headers = files('nd-sink.h')

gnome_nd_common_sources += gnome.mkenums_simple(
  'nd-enum-types',
  sources: enum_headers,
)

gnome_nd_common_deps = [
  dependency('gio-2.0', version: '>= 2.50'),
  dependency('gstreamer-1.0', version: '>= 1.14'),
  dependency('gstreamer-pbutils-1.0', version: '>= 1.14'),
  dependency('gstreamer-plugins-base-1.0'),
  dependency('gstreamer-rtsp-server-1.0'),
  dependency('gtk4', version: '>= 4.13.0'),
  dependency('libnm', version: '>= 1.15'),
  dependency('libportal-gtk4', version: '>= 0.7'),
  dependency('libpulse-mainloop-glib'),
  dependency('libadwaita-1', version: '>= 1.6.0'),
]

if get_option('systemd_resolved')
  gnome_nd_common_deps += [
    dependency('libsystemd', version: '>=259')
  ]
else
  gnome_nd_common_deps += [
    dependency('avahi-client'),
    dependency('avahi-gobject'),
  ]
endif

gnome_nd_common_deps += wfd_server_deps
gnome_nd_common_deps += cc_deps

# Daemon sources and dependencies
gnome_nd_daemon_sources = [
  'daemon/main.c',
  'daemon/nd-daemon.c',
  'nd-manager.c',
  'nd-systemd-helpers.c',
]

gnome_nd_daemon_sources += gnome_nd_common_sources

gnome_nd_daemon_sources += gnome.gdbus_codegen(
  'nd-dbus-manager',
  'org.gnome.NetworkDisplays.Manager.xml',
  interface_prefix: 'org.gnome.NetworkDisplays.',
  namespace: 'NdDBus'
)

gnome_nd_daemon_sources += gnome.gdbus_codegen(
  'nd-dbus-systemd',
  'org.freedesktop.systemd1.Manager.xml',
  interface_prefix: 'org.freedesktop.systemd1.Manager.',
  namespace: 'NdDBus'
)

gnome_nd_daemon_deps = []
gnome_nd_daemon_deps += gnome_nd_common_deps

# Stream sources and dependencies
gnome_nd_stream_sources = [
  'stream/main.c',
  'stream/nd-stream.c',
]

gnome_nd_stream_sources += gnome_nd_common_sources

gnome_nd_stream_deps = []
gnome_nd_stream_deps += gnome_nd_common_deps

# App sources and dependencies
gnome_nd_app_sources = [
  'app/main.c',
  'app/nd-codec-install.c',
  'app/nd-sink-list-model.c',
  'app/nd-sink-row.c',
  'app/nd-window.c',
]

gnome_nd_app_sources += gnome.compile_resources(
  'gnome-nd-resources',
  'gnome-network-displays.gresource.xml',
  c_name: 'gnome_screencast'
)

gnome_nd_app_sources += gnome_nd_common_sources

gnome_nd_app_deps = []
gnome_nd_app_deps += gnome_nd_common_deps

if build_daemon
  executable('gnome-network-displays-daemon',
    gnome_nd_daemon_sources,
    dependencies: gnome_nd_daemon_deps,
    install: true,
    link_with: [wfd_server, cc_cast_channel],
  )
  executable('gnome-network-displays-stream',
    gnome_nd_stream_sources,
    dependencies: gnome_nd_stream_deps,
    install: true,
    link_with: [wfd_server, cc_cast_channel],
    install_dir: join_paths(get_option('prefix'), 'libexec')
  )
endif

if build_app
  executable('gnome-network-displays',
    gnome_nd_app_sources,
    dependencies: gnome_nd_app_deps,
    install: true,
    link_with: [wfd_server, cc_cast_channel],
  )
endif

