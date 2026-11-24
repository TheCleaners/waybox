# wbconf: the GTK4 settings GUI for waybox. Links the rc.xml model (wbconf.cpp)
# and the pure theme helpers; pulls in GTK4 and libxml2. Built only when GTK4 is
# present (see the top-level meson.build).

wbconf_args = [
  '-DGETTEXT_PACKAGE="' + meson.project_name().to_lower() + '"',
  '-DLOCALEDIR="' + get_option('prefix') / get_option('localedir') + '"',
  '-DUSE_NLS=1',
  '-DWB_SCHEMA_DIR="' + get_option('prefix') / get_option('datadir') / 'waybox' / 'wbconf' + '"',
  '-DWB_INTEGRATIONS_DIR="' + get_option('prefix') / get_option('datadir') / 'waybox' / 'integrations' + '"',
]

executable(
  'wbconf',
  ['main.cpp', 'session.cpp', 'monitors.cpp', 'widgets.cpp', 'fonts.cpp',
   'theme_page.cpp', 'integrations_page.cpp', 'settings.cpp', 'settings_view.cpp',
   files('../waybox/wbconf.cpp', '../waybox/settings_schema.cpp'), xml_strict_src, theme_src, style_src, widget_src,
   render_src, startup_src, integrations_src],
  include_directories: [inc_dir],
  dependencies: [gtk4, json_glib, libxml2, cairo, pango, pangocairo],
  cpp_args: wbconf_args,
  install: true,
)

# Desktop entry so wbconf shows up in application menus / launchers.
install_data(
  'org.waybox.wbconf.desktop',
  install_dir: get_option('datadir') / 'applications',
)
