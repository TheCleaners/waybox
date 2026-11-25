cfdata = configuration_data()
cfdata.set('bindir', get_option('prefix') / get_option('bindir'))
cfdata.set('libexecdir', get_option('prefix') / get_option('libexecdir'))
cfdata.set('localedir', get_option('prefix') / get_option('localedir'))
cfdata.set('sysconfdir', get_option('prefix') / get_option('sysconfdir'))
cfdata.set('package', meson.project_name().to_lower())

configure_file(
  configuration: cfdata,
  input: 'waybox.sh',
  output: 'waybox',
  install: true,
  install_dir: get_option('prefix') / get_option('bindir'),
  install_mode: 'rwxr-xr-x',
)

scripts = files(
  'autostart',
  'environment',
  'xdg-autostart',
  )

wb_xdg_dir = get_option('prefix') / get_option('sysconfdir') + '/xdg/waybox'

install_data(
  scripts,
  install_dir: wb_xdg_dir,
  install_mode: 'rwxr-xr-x',
  )

install_data(
  'rc.xml',
  install_dir: wb_xdg_dir,
  install_mode: 'rw-r--r--',
  )

install_data(
  'menu.xml',
  install_dir: wb_xdg_dir,
  install_mode: 'rw-r--r--',
  )

install_data(
  'waybox.svg',
  install_dir: get_option('prefix') / get_option('datadir') / 'icons',
  install_mode: 'rw-r--r--',
  )

install_data(
  'waybox.desktop',
  install_dir: get_option('prefix') / get_option('datadir') + '/wayland-sessions',
  install_mode: 'rw-r--r--',
  )

# Portal backend selection for XDG_CURRENT_DESKTOP=Waybox; without it
# xdg-desktop-portal finds no ScreenCast/Screenshot backend for this desktop.
install_data(
  'waybox-portals.conf',
  install_dir: get_option('prefix') / get_option('datadir') / 'xdg-desktop-portal',
  install_mode: 'rw-r--r--',
  )

install_data(
  'menu',
  install_dir: get_option('prefix') / get_option('bindir'),
  install_mode: 'rwxr-xr-x',
  rename: ['waybox-menu']
  )

install_data(
  'wallpaper',
  install_dir: get_option('prefix') / get_option('bindir'),
  install_mode: 'rwxr-xr-x',
  rename: ['waybox-wallpaper']
  )

# Companion programs wbconf can set up (see #153); users can add their own in
# $XDG_CONFIG_HOME/waybox/integrations.
install_subdir('integrations',
  install_dir: get_option('prefix') / get_option('datadir') / 'waybox',
)

# wbconf's settings schema; more *.json files beside it (or in
# $XDG_CONFIG_HOME/waybox/wbconf) can add settings.
install_subdir('wbconf',
  install_dir: get_option('prefix') / get_option('datadir') / 'waybox',
)
