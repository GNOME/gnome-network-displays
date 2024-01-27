subdir('icons')

desktop_file = i18n.merge_file(
  input: 'org.gnome.NetworkDisplays.desktop.in',
  output: 'org.gnome.NetworkDisplays.desktop',
  type: 'desktop',
  po_dir: '../po',
  install: true,
  install_dir: join_paths(get_option('datadir'), 'applications')
)

desktop_utils = find_program('desktop-file-validate', required: false)
if desktop_utils.found()
  test('Validate desktop file', desktop_utils,
    args: [desktop_file]
  )
endif

appstream_file = i18n.merge_file(
  input: 'org.gnome.NetworkDisplays.appdata.xml.in',
  output: 'org.gnome.NetworkDisplays.appdata.xml',
  po_dir: '../po',
  install: true,
  install_dir: join_paths(get_option('datadir'), 'metainfo')
)

appstream_util = find_program('appstream-util', required: false)
if appstream_util.found()
  test('Validate appstream file', appstream_util,
    args: ['validate', appstream_file]
  )
endif

if enable_firewalld_zone
  install_data(
    'P2P-WiFi-Display.xml',
    install_dir: join_paths(get_option('prefix'), 'lib', 'firewalld', 'zones')
  )
endif
