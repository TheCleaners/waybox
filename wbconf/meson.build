# wbconf: the GTK4 settings GUI for waybox. Links the rc.xml model (wbconf.cpp)
# and the pure theme helpers; pulls in GTK4 and libxml2. Built only when GTK4 is
# present (see the top-level meson.build).

wbconf_args = [
  '-DGETTEXT_PACKAGE="' + meson.project_name().to_lower() + '"',
  '-DLOCALEDIR="' + get_option('prefix') / get_option('localedir') + '"',
  '-DUSE_NLS=1',
  '-DWB_SCHEMA_DIR="' + get_option('prefix') / get_option('datadir') / 'waybox' / 'wbconf' + '"',
  '-DWB_INTEGRATIONS_DIR="' + get_option('prefix') / get_option('datadir') / 'waybox' / 'integrations' + '"',
  '-DWB_ICONS_DIR="' + get_option('prefix') / get_option('datadir') / 'icons' + '"',
  '-DWB_GEO_DIR="' + get_option('prefix') / get_option('datadir') / 'waybox' / 'geo' + '"',
]

wbconf_version_h = vcs_tag(
  command: wb_version_cmd,
  fallback: meson.project_version(),
  input: version_in,
  output: 'version.h',
  replace_string: '@WB_VERSION@',
)

wbconf_exe = executable(
  'wbconf',
  [wbconf_version_h, 'main.cpp', 'session.cpp', 'monitors.cpp', 'widgets.cpp', 'option_widgets.cpp', 'monitor_arranger.cpp', 'color_control.cpp', 'list_control.cpp', 'sections_control.cpp', 'font_control.cpp', 'unsaved_changes.cpp', 'problems.cpp', 'lua_page.cpp', 'pages/wallpaper.cpp', 'fonts.cpp',
   'theme_page.cpp', 'integrations_page.cpp', 'startup_sources_page.cpp', 'procs.cpp', 'gtk_page.cpp', 'settings.cpp', 'settings_view.cpp', 'shortcuts_page.cpp', 'rules_page.cpp', 'menu_page.cpp', 'action_editor.cpp', 'geo_picker.cpp',
   files('../waybox/wbconf.cpp', '../waybox/settings_schema.cpp'), action_catalog_src, action_src, key_label_src, binding_clash_src, app_rule_xml_src, xml_edit_src, menu_doc_src, geo_src, app_menu_src, menu_src, applications_src, xml_strict_src, theme_src, style_src, widget_src,
   frame_src, geometry_src, titlebar_draw_src,
   render_src, startup_src, integrations_src, startup_sources_src, gtk_appearance_src,
   monitor_layout_src, color_text_src, xcursor_themes_src, change_list_src, setting_problems_src,
   page_protocol_src, json_src, name_list_src, font_spec_src, atomic_file_src, trust_src],
  include_directories: [inc_dir],
  dependencies: [gtk4, json_glib, libxml2, cairo, pango, pangocairo],
  cpp_args: wbconf_args + ['-DWB_PAGE_RUNNER="' + get_option('prefix') / get_option('libexecdir') / 'wbconf-lua' + '"'],
  install: true,
)

# wbconf-lua: integrations' settings pages written in Lua, in a sandboxed
# process of its own (Page=NAME.lua).
if lua.found()
  wbconf_lua_exe = executable(
    'wbconf-lua',
    ['page_runner.cpp', json_src, page_protocol_src, ini_doc_src, jsonc_doc_src, toml_doc_src],
    include_directories: [inc_dir],
    dependencies: [lua],
    install: true,
    install_dir: get_option('prefix') / get_option('libexecdir'),
  )
endif

# Desktop entry so wbconf shows up in application menus / launchers.
install_data(
  'org.waybox.wbconf.desktop',
  install_dir: get_option('datadir') / 'applications',
)
