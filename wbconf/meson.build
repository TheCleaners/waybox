# wbconf: the GTK4 settings GUI for waybox. Links the rc.xml model (wbconf.cpp)
# and the pure theme helpers; pulls in GTK4 and libxml2. Built only when GTK4 is
# present (see the top-level meson.build).

wbconf_args = [
  '-DGETTEXT_PACKAGE="' + meson.project_name().to_lower() + '"',
  '-DLOCALEDIR="' + get_option('prefix') / get_option('localedir') + '"',
  '-DUSE_NLS=1',
]

executable(
  'wbconf',
  ['main.cpp', files('../waybox/wbconf.cpp'), theme_src],
  include_directories: [inc_dir],
  dependencies: [gtk4, libxml2],
  cpp_args: wbconf_args,
  install: true,
)

# Desktop entry so wbconf shows up in application menus / launchers.
install_data(
  'wbconf.desktop',
  install_dir: get_option('datadir') / 'applications',
)
