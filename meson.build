project('gnome-network-displays', 'c',
  version: '0.97.0',
  meson_version: '>= 0.59.0',
)

gnome = import('gnome')
i18n = import('i18n')

datadir = get_option('datadir')
icondir = join_paths(datadir, 'icons')

config_h = configuration_data()
config_h.set_quoted('PACKAGE_VERSION', meson.project_version())
config_h.set_quoted('GETTEXT_PACKAGE', 'gnome-network-displays')
config_h.set_quoted('LOCALEDIR', join_paths(get_option('prefix'), get_option('localedir')))
configure_file(
  output: 'gnome-network-displays-config.h',
  configuration: config_h,
)
add_project_arguments([
  '-I' + meson.current_build_dir(),
  '-DG_LOG_DOMAIN="Gnd"',
], language: 'c')

enable_firewalld_zone = get_option('firewalld_zone')
build_daemon = get_option('build_daemon')
build_app = get_option('build_app')

subdir('data')
subdir('src')
if build_app
  subdir('po')
endif

gnome.post_install(
  gtk_update_icon_cache: true,
  update_desktop_database: true,
)
