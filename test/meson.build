# Unit tests for the pure, wlroots-free logic. Each test executable links the
# specific source under test plus the shared harness main; no wlroots/Wayland
# dependencies, so these build and run anywhere (including CI without a
# compositor or sanitizer runtime).

action_test = executable(
  'action_test',
  ['action_test.cpp', 'wb_test_main.cpp', action_src],
  include_directories: [inc_dir],
  )
test('action', action_test, suite: 'unit')

window_cycle_test = executable(
  'window_cycle_test',
  ['window_cycle_test.cpp', 'wb_test_main.cpp', window_cycle_src],
  include_directories: [inc_dir],
  )
test('window_cycle', window_cycle_test, suite: 'unit')

geometry_test = executable(
  'geometry_test',
  ['geometry_test.cpp', 'wb_test_main.cpp', geometry_src],
  include_directories: [inc_dir],
  )
test('geometry', geometry_test, suite: 'unit')

mousebind_test = executable(
  'mousebind_test',
  ['mousebind_test.cpp', 'wb_test_main.cpp', mousebind_src],
  include_directories: [inc_dir],
  )
test('mousebind', mousebind_test, suite: 'unit')

placement_test = executable(
  'placement_test',
  ['placement_test.cpp', 'wb_test_main.cpp', placement_src, geometry_src],
  include_directories: [inc_dir],
  )
test('placement', placement_test, suite: 'unit')

applications_test = executable(
  'applications_test',
  ['applications_test.cpp', 'wb_test_main.cpp', applications_src],
  include_directories: [inc_dir],
  )
test('applications', applications_test, suite: 'unit')

keychain_test = executable(
  'keychain_test',
  ['keychain_test.cpp', 'wb_test_main.cpp', keychain_src],
  include_directories: [inc_dir],
  )
test('keychain', keychain_test, suite: 'unit')

decor_mode_test = executable(
  'decor_mode_test',
  ['decor_mode_test.cpp', 'wb_test_main.cpp', decor_mode_src],
  include_directories: [inc_dir],
  )
test('decor_mode', decor_mode_test, suite: 'unit')

frame_test = executable(
  'frame_test',
  ['frame_test.cpp', 'wb_test_main.cpp', frame_src, geometry_src],
  include_directories: [inc_dir],
  )
test('frame', frame_test, suite: 'unit')

icon_test = executable(
  'icon_test',
  ['icon_test.cpp', 'wb_test_main.cpp', icon_src],
  include_directories: [inc_dir],
  )
test('icon', icon_test, suite: 'unit')

theme_test = executable(
  'theme_test',
  ['theme_test.cpp', 'wb_test_main.cpp', theme_src],
  include_directories: [inc_dir],
  )
test('theme', theme_test, suite: 'unit')

widget_test = executable(
  'widget_test',
  ['widget_test.cpp', 'wb_test_main.cpp', widget_src],
  include_directories: [inc_dir],
  )
test('widget', widget_test, suite: 'unit')

animation_test = executable(
  'animation_test',
  ['animation_test.cpp', 'wb_test_main.cpp', animation_src],
  include_directories: [inc_dir],
  )
test('animation', animation_test, suite: 'unit')

style_test = executable(
  'style_test',
  ['style_test.cpp', 'wb_test_main.cpp', style_src, theme_src, widget_src],
  include_directories: [inc_dir],
  )
test('style', style_test, suite: 'unit')

menu_test = executable(
  'menu_test',
  ['menu_test.cpp', 'wb_test_main.cpp', menu_src, action_src],
  include_directories: [inc_dir],
  )
test('menu', menu_test, suite: 'unit')

desktops_test = executable(
  'desktops_test',
  ['desktops_test.cpp', 'wb_test_main.cpp', desktops_src],
  include_directories: [inc_dir],
  )
test('desktops', desktops_test, suite: 'unit')

edge_tiling_test = executable(
  'edge_tiling_test',
  ['edge_tiling_test.cpp', 'wb_test_main.cpp', edge_tiling_src, geometry_src],
  include_directories: [inc_dir],
  )
test('edge_tiling', edge_tiling_test, suite: 'unit')

key_label_test = executable(
  'key_label_test',
  ['key_label_test.cpp', 'wb_test_main.cpp', key_label_src],
  include_directories: [inc_dir],
  )
test('key_label', key_label_test, suite: 'unit')

geo_test = executable(
  'geo_test',
  ['geo_test.cpp', 'wb_test_main.cpp', geo_src],
  include_directories: [inc_dir],
  cpp_args: ['-DWB_SOURCE_ROOT="' + meson.project_source_root() + '"'],
  )
test('geo', geo_test, suite: 'unit')
binding_clash_test = executable(
  'binding_clash_test',
  ['binding_clash_test.cpp', 'wb_test_main.cpp', binding_clash_src, key_label_src],
  include_directories: [inc_dir],
  )
test('binding_clash', binding_clash_test, suite: 'unit')

action_catalog_test = executable(
  'action_catalog_test',
  ['action_catalog_test.cpp', 'wb_test_main.cpp', action_catalog_src, action_src],
  include_directories: [inc_dir],
  )
test('action_catalog', action_catalog_test, suite: 'unit')

geometry_actions_test = executable(
  'geometry_actions_test',
  ['geometry_actions_test.cpp', 'wb_test_main.cpp', geometry_actions_src],
  include_directories: [inc_dir],
  )
test('geometry_actions', geometry_actions_test, suite: 'unit')

client_menu_test = executable(
  'client_menu_test',
  ['client_menu_test.cpp', 'wb_test_main.cpp', client_menu_src, menu_src, action_src],
  include_directories: [inc_dir],
  )
test('client_menu', client_menu_test, suite: 'unit')

security_test = executable(
  'security_test',
  ['security_test.cpp', 'wb_test_main.cpp', security_src],
  include_directories: [inc_dir],
  )
test('security', security_test, suite: 'unit')

output_rules_test = executable(
  'output_rules_test',
  ['output_rules_test.cpp', 'wb_test_main.cpp', output_rules_src],
  include_directories: [inc_dir],
  )
test('output_rules', output_rules_test, suite: 'unit')

startup_test = executable(
  'startup_test',
  ['startup_test.cpp', 'wb_test_main.cpp', startup_src],
  include_directories: [inc_dir],
  )
test('startup', startup_test, suite: 'unit')

integrations_test = executable(
  'integrations_test',
  ['integrations_test.cpp', 'wb_test_main.cpp', integrations_src, startup_src],
  include_directories: [inc_dir],
  )
test('integrations', integrations_test,
  env: {'WB_INTEGRATIONS_SRC': meson.project_source_root() / 'data' / 'integrations'}, suite: 'unit')
# The Lua Language Server hints describe all a page is given (none left out
# as the runner grows).
test('lua_stubs', find_program('python3'),
  args: [meson.current_source_dir() / 'integration' / 'lua_stubs_test.py',
         meson.project_source_root() / 'wbconf' / 'page_runner.cpp',
         meson.project_source_root() / 'data' / 'lua' / 'waybox-page.lua'], suite: 'unit')
# waybox-check-integration --strict over the files Waybox ships, as
# packagers and the community repository's CI will run it.
test('check_integration', find_program('sh'),
  args: ['-c', '"$0" --strict "$1"/*.integration', waybox_check_integration,
         meson.project_source_root() / 'data' / 'integrations'], suite: 'unit')
# A warning: passes, but not with --strict; a newer major format: fails.
check_files = meson.current_source_dir() / 'integration_files'
test('check_integration_warning', waybox_check_integration,
  args: [check_files / 'warning.integration'], suite: 'unit')
test('check_integration_strict', waybox_check_integration,
  args: ['--strict', check_files / 'warning.integration'], should_fail: true, suite: 'unit')
test('check_integration_newer', waybox_check_integration,
  args: [check_files / 'newer.integration'], should_fail: true, suite: 'unit')

# wbconf's color fields: their tint and text.
color_text_test = executable(
  'color_text_test',
  ['color_text_test.cpp', 'wb_test_main.cpp', color_text_src, theme_src],
  include_directories: [inc_dir],
  )
test('color_text', color_text_test, suite: 'unit')

# Cursor themes as wlroots finds them: with cursors or not, and a fallback.
xcursor_themes_test = executable(
  'xcursor_themes_test',
  ['xcursor_themes_test.cpp', 'wb_test_main.cpp', xcursor_themes_src],
  include_directories: [inc_dir],
  )
test('xcursor_themes', xcursor_themes_test, suite: 'unit')

# wbconf's list of changes not saved: one line each, kept short.
change_list_test = executable(
  'change_list_test',
  ['change_list_test.cpp', 'wb_test_main.cpp', change_list_src],
  include_directories: [inc_dir],
  )
test('change_list', change_list_test, suite: 'unit')

# Problems wbconf shows beside settings: the pointer's.
setting_problems_test = executable(
  'setting_problems_test',
  ['setting_problems_test.cpp', 'wb_test_main.cpp', setting_problems_src, xcursor_themes_src],
  include_directories: [inc_dir],
  )
test('setting_problems', setting_problems_test, suite: 'unit')

# Settings pages from another process: the page model and its JSON.
page_protocol_test = executable(
  'page_protocol_test',
  ['page_protocol_test.cpp', 'wb_test_main.cpp', page_protocol_src, json_src],
  include_directories: [inc_dir],
  )
test('page_protocol', page_protocol_test, suite: 'unit')

# Lua pages' wb.ini: INI files edited without disturbing them.
ini_doc_test = executable(
  'ini_doc_test',
  ['ini_doc_test.cpp', 'wb_test_main.cpp', ini_doc_src],
  include_directories: [inc_dir],
  )
test('ini_doc', ini_doc_test, suite: 'unit')

# Lua pages' wb.json: JSON with comments edited without disturbing it.
jsonc_doc_test = executable(
  'jsonc_doc_test',
  ['jsonc_doc_test.cpp', 'wb_test_main.cpp', jsonc_doc_src],
  include_directories: [inc_dir],
  )
test('jsonc_doc', jsonc_doc_test, suite: 'unit')

# Lua pages' wb.toml: TOML files edited without disturbing them.
toml_doc_test = executable(
  'toml_doc_test',
  ['toml_doc_test.cpp', 'wb_test_main.cpp', toml_doc_src, jsonc_doc_src],
  include_directories: [inc_dir],
  )
test('toml_doc', toml_doc_test, suite: 'unit')

# List controls: names added, moved and removed.
name_list_test = executable(
  'name_list_test',
  ['name_list_test.cpp', 'wb_test_main.cpp', name_list_src],
  include_directories: [inc_dir],
  )
test('name_list', name_list_test, suite: 'unit')

# Font controls: fontconfig names read and written.
font_spec_test = executable(
  'font_spec_test',
  ['font_spec_test.cpp', 'wb_test_main.cpp', font_spec_src],
  include_directories: [inc_dir],
  )
test('font_spec', font_spec_test, suite: 'unit')

# The bell: its style, its blinks, never strobing.
bell_style_test = executable(
  'bell_style_test',
  ['bell_style_test.cpp', 'wb_test_main.cpp', bell_style_src, theme_src],
  include_directories: [inc_dir],
  )
test('bell_style', bell_style_test, suite: 'unit')

# wbconf's monitor arranger: sizes, snapping, placing.
monitor_layout_test = executable(
  'monitor_layout_test',
  ['monitor_layout_test.cpp', 'wb_test_main.cpp', monitor_layout_src],
  include_directories: [inc_dir],
  )
test('monitor_layout', monitor_layout_test, suite: 'unit')

# Openbox's If/ForEach conditions: matching, desktops, targets.
conditions_test = executable(
  'conditions_test',
  ['conditions_test.cpp', 'wb_test_main.cpp', conditions_src, applications_src, action_src],
  include_directories: [inc_dir],
  )
test('conditions', conditions_test, suite: 'unit')

# Session restore's store: JSON, limits, expiry, where a window goes back.
session_store_test = executable(
  'session_store_test',
  ['session_store_test.cpp', 'wb_test_main.cpp', session_store_src, json_src],
  include_directories: [inc_dir],
  )
test('session_store', session_store_test, suite: 'unit')

# Files written whole, through a temporary file of their own.
atomic_file_test = executable(
  'atomic_file_test',
  ['atomic_file_test.cpp', 'wb_test_main.cpp', atomic_file_src],
  include_directories: [inc_dir],
  )
test('atomic_file', atomic_file_test, suite: 'unit')

# The location agent's decisions (accuracy names, kept answers).
location_agent_test = executable(
  'location_agent_test',
  ['location_agent_test.cpp', 'wb_test_main.cpp', location_agent_src],
  include_directories: [inc_dir],
  )
test('location_agent', location_agent_test, suite: 'unit')

# The main thread's mailbox and the worker pool (run under ThreadSanitizer
# with -Dtsan=true).
threads_test = executable(
  'threads_test',
  ['threads_test.cpp', 'wb_test_main.cpp', threads_src],
  include_directories: [inc_dir],
  dependencies: [threads_dep],
  )
test('threads', threads_test, suite: 'unit')

# Whether only the system could have changed a file (pages run unasked).
trust_test = executable(
  'trust_test',
  ['trust_test.cpp', 'wb_test_main.cpp', trust_src],
  include_directories: [inc_dir],
  )
test('trust', trust_test, suite: 'unit')

# The applications menu: .desktop files, categories, the scan and its cache.
app_menu_test = executable(
  'app_menu_test',
  ['app_menu_test.cpp', 'wb_test_main.cpp', app_menu_src, menu_src, action_src, atomic_file_src],
  include_directories: [inc_dir],
  )
test('app_menu', app_menu_test, suite: 'unit')

# The autostart script's start lines and XDG autostart entries (wbconf).
startup_sources_test = executable(
  'startup_sources_test',
  ['startup_sources_test.cpp', 'wb_test_main.cpp', startup_sources_src, startup_src],
  include_directories: [inc_dir],
  )
test('startup_sources', startup_sources_test, suite: 'unit')

# The same parsers under random input (fixed seeds; sanitized in debug builds).
startup_sources_fuzz_test = executable(
  'startup_sources_fuzz_test',
  ['startup_sources_fuzz_test.cpp', 'wb_test_main.cpp', startup_sources_src, startup_src,
   integrations_src],
  include_directories: [inc_dir],
  )
test('startup_sources_fuzz', startup_sources_fuzz_test, timeout: 120, suite: 'unit')

# waybox-wallpaper: the image and the wallpaper program it picks.
python3 = find_program('python3')
test('wallpaper_helper', python3,
  args: [files('wallpaper_test.py'), meson.project_source_root() / 'data' / 'wallpaper'], suite: 'unit')

# Measuring what blocks the event loop.
stall_test = executable(
  'stall_test',
  ['stall_test.cpp', 'wb_test_main.cpp', stall_src],
  include_directories: [inc_dir],
  )
test('stall', stall_test, suite: 'unit')

# Icon/cursor/GTK themes and broken icon-theme fallbacks (wbconf).
gtk_appearance_test = executable(
  'gtk_appearance_test',
  ['gtk_appearance_test.cpp', 'wb_test_main.cpp', gtk_appearance_src],
  include_directories: [inc_dir],
  )
test('gtk_appearance', gtk_appearance_test, suite: 'unit')

json_test = executable(
  'json_test',
  ['json_test.cpp', 'wb_test_main.cpp', json_src],
  include_directories: [inc_dir],
  )
test('json', json_test, suite: 'unit')

# The waybox Python module, against a fake socket (no compositor needed).
test('python_module', find_program('python3'),
  args: [files('python/waybox_module_test.py'), python_src_dir], suite: 'unit')

# Pixel test for the Cairo/Pango painting primitives. Needs cairo/pango, so it
# is only built in the full build (not tests_only); it links no wlroots.
if not get_option('tests_only')
  render_test = executable(
    'render_test',
    ['render_test.cpp', 'wb_test_main.cpp', render_src],
    include_directories: [inc_dir],
    dependencies: [cairo, pango, pangocairo],
    )
  test('render', render_test,
    env: {
      'LSAN_OPTIONS': 'suppressions=' + meson.current_source_dir() / 'lsan.supp',
    },
    suite: 'unit',
    )
endif

# Drawing titlebars (Waybox's frames and wbconf's previews); Cairo and Pango.
if not get_option('tests_only')
  titlebar_draw_test = executable(
    'titlebar_draw_test',
    ['titlebar_draw_test.cpp', 'wb_test_main.cpp', titlebar_draw_src, frame_src,
     geometry_src, render_src, theme_src, style_src, widget_src],
    include_directories: [inc_dir],
    dependencies: [cairo, pango, pangocairo],
    )
  test('titlebar_draw', titlebar_draw_test,
    env: {
      # Pango and fontconfig caches live for the whole process.
      'LSAN_OPTIONS': 'suppressions=' + meson.current_source_dir() / 'lsan.supp',
    },
    suite: 'unit',
    )
endif

# Parser test for menu.xml; links libxml2, so full-build only (like render).
if not get_option('tests_only')
  menu_parse_test = executable(
    'menu_parse_test',
    ['menu_parse_test.cpp', 'wb_test_main.cpp', menu_src,
     action_src, conditions_src, applications_src,
     files('../waybox/menu_parse.cpp', '../waybox/action_xml.cpp')],
    include_directories: [inc_dir],
    dependencies: [libxml2],
    )
  test('menu_parse', menu_parse_test, suite: 'unit')
endif

# wbconf rc.xml model round-trip test; links libxml2 + theme, full-build only.
if not get_option('tests_only')
  wbconf_test = executable(
    'wbconf_test',
    ['wbconf_test.cpp', 'wb_test_main.cpp', files('../waybox/wbconf.cpp'), startup_src,
     atomic_file_src,
     font_spec_src, app_rule_xml_src, xml_edit_src, applications_src,
     output_rules_src,
     xml_strict_src,
     theme_src],
    include_directories: [inc_dir],
    dependencies: [libxml2],
    )
  test('wbconf', wbconf_test, suite: 'unit')

  menu_doc_test = executable(
    'menu_doc_test',
    ['menu_doc_test.cpp', 'wb_test_main.cpp', menu_doc_src, xml_edit_src, atomic_file_src],
    include_directories: [inc_dir],
    dependencies: [libxml2],
    )
  test('menu_doc', menu_doc_test, suite: 'unit')

  if json_glib.found()
  settings_schema_test = executable(
    'settings_schema_test',
    ['settings_schema_test.cpp', 'wb_test_main.cpp', files('../waybox/settings_schema.cpp',
     '../waybox/wbconf.cpp'), startup_src, xml_strict_src, theme_src, font_spec_src,
     atomic_file_src,
     app_rule_xml_src, xml_edit_src, applications_src],
    include_directories: [inc_dir],
    dependencies: [libxml2, json_glib],
    cpp_args: ['-DWB_SOURCE_ROOT="' + meson.project_source_root() + '"'],
    )
  test('settings_schema', settings_schema_test, suite: 'unit')
  endif
endif

# Headless integration test driving real synthesized input via the
# virtual-keyboard protocol. Skips (exit 77) when foot/wtype or a headless
# display are unavailable, so CI stays green; run a sanitized local build to
# actually exercise it. Give it a generous timeout (it spawns clients + sleeps).
python = find_program('python3', required: false)
# Integration tests now exercise server-side decorations, so the compositor
# renders titlebars via Pango/fontconfig, whose process-global font caches
# LeakSanitizer would flag at exit. Suppress those library caches (as the
# render test does) for every integration test.
integration_env = {
  'LSAN_OPTIONS': 'suppressions=' + meson.current_source_dir() / 'lsan.supp',
  # When the tests run inside a Waybox session, the session script's exported
  # config paths would override each test's own rc.xml/menu.xml (waybox
  # ignores them when empty).
  'WB_RC_XML': '',
  'WB_MENU_XML': '',
  # Nor may a test's waybox-msg or wbconf reach that session's Waybox (its
  # socket and pid, exported to programs it starts): empty is unset to both,
  # so they find the test's own through its WAYLAND_DISPLAY.
  'WAYBOX_SOCK': '',
  'WAYBOX_PID': '',
  # Nor the session's D-Bus: a test's Waybox reporting a broken rc.xml (as
  # tests make on purpose) would notify the user's desktop, and anything a
  # test starts could reach that session's services. Tests that need a
  # bus start a private one.
  'DBUS_SESSION_BUS_ADDRESS': 'disabled:',
  # GTK programs under test (wbconf) abort on any critical: a test fails
  # on one rather than letting it scroll by.
  'G_DEBUG': 'fatal-criticals',
}
if python.found() and not get_option('tests_only')
  # A minimal ext-session-lock-v1 client for the session-lock test.
  session_lock_xml = wl_protocol_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml'
  lock_client = executable(
    'lock_client',
    [
      'integration' / 'lock_client.c',
      wayland_scanner_client.process(session_lock_xml,
          wl_protocol_dir / 'unstable/text-input/text-input-unstable-v3.xml'),
      wayland_scanner_code.process(session_lock_xml,
          wl_protocol_dir / 'unstable/text-input/text-input-unstable-v3.xml'),
    ],
    dependencies: [dependency('wayland-client')],
    )
  # A minimal ext-workspace-v1 client (a panel stand-in) for the workspace test.
  workspace_xml = wl_protocol_dir / 'staging/ext-workspace/ext-workspace-v1.xml'
  workspace_client = executable(
    'workspace_client',
    [
      'integration' / 'workspace_client.c',
      wayland_scanner_client.process(workspace_xml),
      wayland_scanner_code.process(workspace_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )
  # A pointer-constraints / relative-pointer client for the pointer lock test.
  constraints_xml = wl_protocol_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml'
  relative_xml = wl_protocol_dir / 'unstable/relative-pointer/relative-pointer-unstable-v1.xml'
  xdg_shell_client_xml = wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml'
  constraint_client = executable(
    'constraint_client',
    [
      'integration' / 'constraint_client.c',
      wayland_scanner_client.process(constraints_xml),
      wayland_scanner_code.process(constraints_xml),
      wayland_scanner_client.process(relative_xml),
      wayland_scanner_code.process(relative_xml),
      wayland_scanner_client.process(xdg_shell_client_xml),
      wayland_scanner_code.process(xdg_shell_client_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )
  # An input method and a text-input application for the input method test.
  text_input_xml = wl_protocol_dir / 'unstable/text-input/text-input-unstable-v3.xml'
  input_method_xml = meson.project_source_root() / 'protocol' / 'input-method-unstable-v2.xml'
  virtual_keyboard_xml = meson.project_source_root() / 'protocol' / 'virtual-keyboard-unstable-v1.xml'
  ime_client = executable(
    'ime_client',
    [
      'integration' / 'ime_client.c',
      wayland_scanner_client.process(input_method_xml, text_input_xml, virtual_keyboard_xml),
      wayland_scanner_code.process(input_method_xml, text_input_xml, virtual_keyboard_xml),
    ],
    dependencies: [dependency('wayland-client'), dependency('xkbcommon')],
    )
  text_input_client = executable(
    'text_input_client',
    [
      'integration' / 'text_input_client.c',
      wayland_scanner_client.process(text_input_xml, xdg_shell_client_xml),
      wayland_scanner_code.process(text_input_xml, xdg_shell_client_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )
  # Two programs, one exporting a window and one attaching a dialog to it,
  # for the dialogs test (xdg-foreign, xdg-dialog).
  foreign_xml = wl_protocol_dir / 'unstable/xdg-foreign/xdg-foreign-unstable-v2.xml'
  dialog_xml = wl_protocol_dir / 'staging/xdg-dialog/xdg-dialog-v1.xml'
  dialog_client = executable(
    'dialog_client',
    [
      'integration' / 'dialog_client.c',
      wayland_scanner_client.process(xdg_shell_client_xml, foreign_xml, dialog_xml),
      wayland_scanner_code.process(xdg_shell_client_xml, foreign_xml, dialog_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # Switches outputs off and on (like wlopm), for the output power test.
  output_power_xml = meson.project_source_root() / 'protocol' / 'wlr-output-power-management-unstable-v1.xml'
  power_client = executable(
    'power_client',
    [
      'integration' / 'power_client.c',
      wayland_scanner_client.process(output_power_xml),
      wayland_scanner_code.process(output_power_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # A virtual pointer that can hold a button (wlrctl only clicks).
  vpointer_xml = meson.project_source_root() / 'protocol' / 'wlr-virtual-pointer-unstable-v1.xml'
  vpointer_client = executable(
    'vpointer_client',
    [
      'integration' / 'vpointer_client.c',
      wayland_scanner_client.process(vpointer_xml),
      wayland_scanner_code.process(vpointer_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # A fullscreen "game" asking for tearing, for the tearing test.
  tearing_xml = wl_protocol_dir / 'staging/tearing-control/tearing-control-v1.xml'
  tearing_client = executable(
    'tearing_client',
    [
      'integration' / 'tearing_client.c',
      wayland_scanner_client.process(xdg_shell_client_xml),
      wayland_scanner_code.process(xdg_shell_client_xml),
      wayland_scanner_client.process(tearing_xml),
      wayland_scanner_code.process(tearing_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # A VM viewer taking the keyboard shortcuts, for the shortcuts test.
  kbinhibit_xml = wl_protocol_dir / 'unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml'
  kbinhibit_client = executable(
    'kbinhibit_client',
    [
      'integration' / 'kbinhibit_client.c',
      wayland_scanner_client.process(xdg_shell_client_xml),
      wayland_scanner_code.process(xdg_shell_client_xml),
      wayland_scanner_client.process(kbinhibit_xml),
      wayland_scanner_code.process(kbinhibit_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # A clipboard manager on the older wlr protocol (as cliphist).
  wlr_dc_xml = meson.project_source_root() / 'protocol' / 'wlr-data-control-unstable-v1.xml'
  wlr_dc_client = executable(
    'wlr_dc_client',
    [
      'integration' / 'wlr_dc_client.c',
      wayland_scanner_client.process(wlr_dc_xml),
      wayland_scanner_code.process(wlr_dc_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # wl_fixes (core protocol): destroy_registry, ack_global_remove.
  fixes_client = executable(
    'fixes_client',
    ['integration' / 'fixes_client.c'],
    dependencies: [dependency('wayland-client')],
    )
  # pointer-gestures: subscribing to swipe, pinch and hold.
  gestures_client = executable(
    'gestures_client',
    [
      'integration' / 'gestures_client.c',
      wayland_scanner_client.process(wl_protocol_dir / 'unstable/pointer-gestures/pointer-gestures-unstable-v1.xml'),
      wayland_scanner_code.process(wl_protocol_dir / 'unstable/pointer-gestures/pointer-gestures-unstable-v1.xml'),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # Makes and drops idle inhibitors, for the idle test.
  idle_inhibit_xml = wl_protocol_dir / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml'
  inhibit_client = executable(
    'inhibit_client',
    [
      'integration' / 'inhibit_client.c',
      wayland_scanner_client.process(idle_inhibit_xml),
      wayland_scanner_code.process(idle_inhibit_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # A program with a popup grab, for the lock-screen test.
  # Rings the system bell (xdg-system-bell).
  bell_client = executable(
    'bell_client',
    [
      'integration' / 'bell_client.c',
      wayland_scanner_client.process(xdg_shell_client_xml),
      wayland_scanner_code.process(xdg_shell_client_xml),
      wayland_scanner_client.process(wl_protocol_dir / 'staging/xdg-system-bell/xdg-system-bell-v1.xml'),
      wayland_scanner_code.process(wl_protocol_dir / 'staging/xdg-system-bell/xdg-system-bell-v1.xml'),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # A window that tags itself (xdg-toplevel-tag).
  # A window whose icon is pixels (xdg-toplevel-icon's buffer path).
  icon_xml = wl_protocol_dir / 'staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml'
  icon_buffer_client = executable(
    'icon_buffer_client',
    [
      'integration' / 'icon_buffer_client.c',
      wayland_scanner_client.process(xdg_shell_client_xml),
      wayland_scanner_code.process(xdg_shell_client_xml),
      wayland_scanner_client.process(icon_xml),
      wayland_scanner_code.process(icon_xml),
      wayland_scanner_client.process(wl_protocol_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'),
      wayland_scanner_code.process(wl_protocol_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'),
    ],
    dependencies: [dependency('wayland-client')],
  )
  activate_client = executable(
    'activate_client',
    [
      'integration' / 'activate_client.c',
      wayland_scanner_client.process(xdg_shell_client_xml),
      wayland_scanner_code.process(xdg_shell_client_xml),
      wayland_scanner_client.process(wl_protocol_dir / 'staging/xdg-activation/xdg-activation-v1.xml'),
      wayland_scanner_code.process(wl_protocol_dir / 'staging/xdg-activation/xdg-activation-v1.xml'),
    ],
    dependencies: [dependency('wayland-client')],
  )
  tag_client = executable(
    'tag_client',
    [
      'integration' / 'tag_client.c',
      wayland_scanner_client.process(xdg_shell_client_xml),
      wayland_scanner_code.process(xdg_shell_client_xml),
      wayland_scanner_client.process(wl_protocol_dir / 'staging/xdg-toplevel-tag/xdg-toplevel-tag-v1.xml'),
      wayland_scanner_code.process(wl_protocol_dir / 'staging/xdg-toplevel-tag/xdg-toplevel-tag-v1.xml'),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # Prints its preferred fractional scale and output enters.
  scale_client = executable(
    'scale_client',
    [
      'integration' / 'scale_client.c',
      wayland_scanner_client.process(xdg_shell_client_xml),
      wayland_scanner_code.process(xdg_shell_client_xml),
      wayland_scanner_client.process(wl_protocol_dir / 'staging/fractional-scale/fractional-scale-v1.xml'),
      wayland_scanner_code.process(wl_protocol_dir / 'staging/fractional-scale/fractional-scale-v1.xml'),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # pointer-warp and alpha-modifier.
  warp_xml = meson.project_source_root() / 'protocol' / 'pointer-warp-v1.xml'
  alpha_xml = wl_protocol_dir / 'staging/alpha-modifier/alpha-modifier-v1.xml'
  warp_alpha_client = executable(
    'warp_alpha_client',
    [
      'integration' / 'warp_alpha_client.c',
      wayland_scanner_client.process(xdg_shell_client_xml),
      wayland_scanner_code.process(xdg_shell_client_xml),
      wayland_scanner_client.process(warp_xml),
      wayland_scanner_code.process(warp_xml),
      wayland_scanner_client.process(alpha_xml),
      wayland_scanner_code.process(alpha_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # wlr-screencopy: one capture of the output.
  screencopy_xml = meson.project_source_root() / 'protocol' / 'wlr-screencopy-unstable-v1.xml'
  screencopy_client = executable(
    'screencopy_client',
    [
      'integration' / 'screencopy_client.c',
      wayland_scanner_client.process(screencopy_xml),
      wayland_scanner_code.process(screencopy_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # Session restore: a window that gets a session and names itself in it.
  session_xml = meson.project_source_root() / 'protocol' / 'xdg-session-management-v1.xml'
  session_client = executable(
    'session_client',
    [
      'integration' / 'session_client.c',
      wayland_scanner_client.process(xdg_shell_client_xml),
      wayland_scanner_code.process(xdg_shell_client_xml),
      wayland_scanner_client.process(session_xml),
      wayland_scanner_code.process(session_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # Touchscreen test windows: with and without wl_touch.
  fake_daemon = executable('fake_daemon', 'integration' / 'fake_daemon.c')

  touch_client = executable(
    'touch_client',
    [
      'integration' / 'touch_client.c',
      wayland_scanner_client.process(xdg_shell_client_xml),
      wayland_scanner_code.process(xdg_shell_client_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # Drag-and-drop between two windows, with an icon.
  dnd_client = executable(
    'dnd_client',
    [
      'integration' / 'dnd_client.c',
      wayland_scanner_client.process(xdg_shell_client_xml),
      wayland_scanner_code.process(xdg_shell_client_xml),
      wayland_scanner_client.process(wl_protocol_dir / 'staging/xdg-toplevel-drag/xdg-toplevel-drag-v1.xml'),
      wayland_scanner_code.process(wl_protocol_dir / 'staging/xdg-toplevel-drag/xdg-toplevel-drag-v1.xml'),
    ],
    dependencies: [dependency('wayland-client')],
    )

  popup_client = executable(
    'popup_client',
    [
      'integration' / 'popup_client.c',
      wayland_scanner_client.process(xdg_shell_client_xml),
      wayland_scanner_code.process(xdg_shell_client_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # A security-context-v1 "sandbox engine" for the sandbox test.
  security_context_xml = wl_protocol_dir / 'staging/security-context/security-context-v1.xml'
  sandbox_client = executable(
    'sandbox_client',
    [
      'integration' / 'sandbox_client.c',
      wayland_scanner_client.process(security_context_xml),
      wayland_scanner_code.process(security_context_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )
  # A minimal wlr-layer-shell panel client for the output-hotplug test.
  layer_shell_xml = meson.project_source_root() / 'protocol' / 'wlr-layer-shell-unstable-v1.xml'
  xdg_shell_xml = wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml'
  # A stand-in for a desktop-icon program: a full-output layer surface.
  desktop_client = executable(
    'desktop_client',
    [
      'integration' / 'desktop_client.c',
      wayland_scanner_client.process(layer_shell_xml),
      wayland_scanner_code.process(layer_shell_xml),
      wayland_scanner_code.process(xdg_shell_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )

  layer_client = executable(
    'layer_client',
    [
      'integration' / 'layer_client.c',
      wayland_scanner_client.process(layer_shell_xml),
      wayland_scanner_code.process(layer_shell_xml),
      wayland_scanner_code.process(xdg_shell_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # A panel's popup with a popup of its own (a tray submenu).
  nested_popup_client = executable(
    'nested_popup_client',
    [
      'integration' / 'nested_popup_client.c',
      wayland_scanner_client.process(xdg_shell_client_xml),
      wayland_scanner_code.process(xdg_shell_client_xml),
      wayland_scanner_client.process(layer_shell_xml),
      wayland_scanner_code.process(layer_shell_xml),
    ],
    dependencies: [dependency('wayland-client')],
    )

  # The integration tests: [name, script in integration/, arguments after the
  # Waybox binary, timeout in seconds]. Each runs with integration_env and
  # skips (exit 77) without its tools. One line per test, kept sorted, so
  # branches that each add a test don't conflict here.
  default_rc = meson.project_source_root() / 'data' / 'rc.xml'
  integration_tests = [
    ['alt_tab', 'alt_tab_test.py', [default_rc], 60],
    ['app_rule', 'app_rule_test.py', [], 60],
    ['app_rule_regex', 'app_rule_test.py', ['regex'], 60],
    ['app_rule_openbox', 'app_rule_test.py', ['openbox'], 60],
    ['bell', 'bell_test.py', [bell_client, vpointer_client], 60],
    ['bell_styles', 'bell_styles_test.py', [bell_client, waybox_msg], 60],
    ['cursor_theme', 'cursor_theme_test.py', [], 60],
    ['color_pick', 'color_pick_test.py', [waybox_msg, desktop_client], 60],
    ['color_management', 'color_management_test.py', [], 90],
    ['config_errors', 'config_errors_test.py', [], 60],
    ['desktop_popup', 'desktop_popup_test.py', [], 60],
    ['dialogs', 'dialogs_test.py', [dialog_client], 60],
    ['desktops', 'desktops_test.py', [], 90],
    ['dnd', 'dnd_test.py', [dnd_client, vpointer_client], 60],
    ['edge_tiling', 'edge_tiling_test.py', [vpointer_client], 90],
    ['foreign_toplevel', 'foreign_toplevel_test.py', [default_rc], 60],
    ['fullscreen_panel', 'fullscreen_panel_test.py', [layer_client], 60],
    ['window_layers', 'window_layers_test.py', [], 60],
    ['about_card', 'about_card_test.py', [], 60],
    ['ibus_real', 'ibus_real_test.py', [text_input_client], 60],
    ['idle_inhibit', 'idle_inhibit_test.py', [inhibit_client], 60],
    ['idle_screen_off', 'idle_screen_off_test.py', [inhibit_client], 60],
    ['input_focus', 'input_focus_test.py', [], 60],
    ['input_method', 'input_method_test.py', [ime_client, text_input_client, lock_client], 90],
    ['input_method_setting', 'input_method_setting_test.py', [], 60],
    ['ipc', 'ipc_test.py', [waybox_msg, lock_client], 60],
    ['lock_control', 'lock_control_test.py', [lock_client, workspace_client], 60],
    ['cycle_outline', 'cycle_outline_test.py', [], 60],
    ['switcher_close', 'switcher_close_test.py', [], 60],
    ['exit_prompt', 'exit_prompt_test.py', [waybox_msg], 60],
    ['setup_failure', 'setup_failure_test.py', [], 60],
    ['render_reset_output', 'render_reset_output_test.py', [], 60],
    ['osd_look', 'osd_look_test.py', [waybox_msg], 60],
    ['python_ipc', 'python_ipc_test.py', [touch_client, python_src_dir], 60],
    ['desktop_context', 'desktop_context_test.py', [desktop_client, vpointer_client], 60],
    ['openbox_actions', 'openbox_actions_test.py', [touch_client], 60],
    ['if_foreach', 'if_foreach_test.py', [touch_client], 60],
    ['key_chain', 'key_chain_test.py', [], 60],
    ['key_repeat', 'key_repeat_test.py', [], 60],
    ['lock_popup', 'lock_popup_test.py', [popup_client, lock_client], 60],
    ['popup_reposition', 'popup_reposition_test.py', [popup_client], 40],
    ['keymap_reconfigure', 'keymap_reconfigure_test.py', [], 60],
    ['lua', 'lua_test.py', [touch_client, meson.project_source_root() / 'data' / 'scripts'], 60],
    ['lua_policy', 'lua_policy_test.py', [touch_client, meson.project_source_root() / 'data' / 'scripts'], 60],
    ['lua_spawn', 'lua_spawn_test.py', [], 60],
    ['lua_menu', 'lua_menu_test.py', [touch_client, meson.project_source_root() / 'data' / 'scripts'], 60],
    ['menu', 'menu_test.py', [], 60],
    ['menu_hover_delay', 'menu_hover_delay_test.py', [], 60],
    ['minimize', 'minimize_test.py', [touch_client, layer_client], 60],
    ['mouse_binding', 'mouse_binding_test.py', [], 60],
    ['grouped_binds', 'grouped_binds_test.py', [], 60],
    ['menu_position', 'menu_position_test.py', [], 60],
    ['desktops_per_monitor', 'desktops_per_monitor_test.py', [waybox_msg, workspace_client], 90],
    ['protocol_inventory', 'protocol_inventory_test.py', [], 60],
    ['mouse_contexts', 'mouse_contexts_test.py', [], 60],
    ['multi_output', 'multi_output_test.py', [layer_client], 60],
    ['output_hotplug', 'output_hotplug_test.py', [layer_client], 60],
    ['output_power', 'output_power_test.py', [power_client], 60],
    ['output_rules', 'output_rules_test.py', [], 60],
    ['panel_reflow', 'panel_reflow_test.py', [layer_client], 60],
    ['layer_order', 'layer_order_test.py', [layer_client, desktop_client], 60],
    ['pipe_menu', 'pipe_menu_test.py', [], 60],
    ['apps_menu', 'apps_menu_test.py', [], 60],
    ['session_restore', 'session_restore_test.py', [session_client], 90],
    ['warp_alpha', 'warp_alpha_test.py', [warp_alpha_client, screencopy_client], 60],
    ['placement', 'placement_test.py', [default_rc], 60],
    ['pointer_grab', 'pointer_grab_test.py', [constraint_client, layer_client, vpointer_client], 60],
    ['pointer_lock', 'pointer_lock_test.py', [constraint_client], 60],
    ['reconfigure', 'reconfigure_test.py', [], 60],
    ['render_reset', 'render_reset_test.py', [scale_client, touch_client], 60],
    ['robustness', 'robustness_test.py', [layer_client, nested_popup_client], 90],
    ['sandbox', 'sandbox_test.py', [sandbox_client], 60],
    ['agent_compositor', 'agent_compositor_test.py', [lock_client], 60],
    ['session_lock', 'session_lock_test.py', [lock_client], 60],
    ['session_lock_virtual_always', 'session_lock_test.py', [lock_client, 'always'], 60],
    ['shortcuts_inhibit', 'shortcuts_inhibit_test.py', [kbinhibit_client], 60],
    ['small_protocols', 'small_protocols_test.py', [fixes_client, gestures_client], 60],
    ['ssd', 'ssd_test.py', [], 60],
    ['stalls', 'stalls_test.py', [], 60],
    ['startup_programs', 'startup_programs_test.py', [], 60],
    ['stuck_button', 'stuck_button_test.py', [touch_client, vpointer_client], 60],
    ['switcher_icons', 'switcher_icons_test.py', [], 60],
    ['switcher_order', 'switcher_order_test.py', [], 60],
    ['tags', 'tags_test.py', [tag_client], 60],
    ['tearing', 'tearing_test.py', [tearing_client], 60],
    ['titlebar_buttons', 'titlebar_buttons_test.py', [], 60],
    ['titlebar_icon', 'titlebar_icon_test.py', [], 60],
    ['pixel_icon', 'pixel_icon_test.py', [icon_buffer_client], 60],
    ['touch', 'touch_test.py', [touch_client, dnd_client, lock_client], 60],
    ['window_capture', 'window_capture_test.py', [lock_client], 60],
    ['window_menu', 'window_menu_test.py', [], 90],
    ['window_state', 'window_state_test.py', [], 60],
    ['wlr_data_control', 'wlr_data_control_test.py', [wlr_dc_client, lock_client], 60],
    ['workspace', 'workspace_test.py', [workspace_client], 60],
    ['urgency', 'urgency_test.py', [activate_client, workspace_client], 60],
    ['switcher_keys', 'switcher_keys_test.py', [activate_client], 60],
    ['geometry_actions', 'geometry_actions_test.py', [activate_client], 60],
    ['xwayland', 'xwayland_test.py', [vpointer_client], 90],
  ]
  # Those above test Waybox (suite "compositor"); those below, wbconf and
  # its pages (suite "wbconf").
  compositor_count = integration_tests.length()
  # wbconf is built only with GTK 4.
  if is_variable('wbconf_exe')
    integration_tests += [['wbconf_save', 'wbconf_save_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_gtk', 'wbconf_gtk_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_versions', 'wbconf_versions_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_move', 'wbconf_move_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf', fake_daemon], 60]]
    integration_tests += [['wbconf_xdg_move', 'wbconf_xdg_move_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf', fake_daemon], 60]]
    integration_tests += [['wbconf_startup', 'wbconf_startup_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_autostart_watch', 'wbconf_autostart_watch_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_options', 'wbconf_options_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_wallpaper', 'wbconf_wallpaper_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_monitors', 'wbconf_monitors_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_outputs', 'wbconf_outputs_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    if is_variable('wbconf_lua_exe')
      # Integrations' pages in Lua: the runner over its protocol, and in wbconf.
      integration_tests += [['page_runner', 'page_runner_test.py', [wbconf_lua_exe], 60]]
      integration_tests += [['json_page', 'json_page_test.py', [wbconf_lua_exe], 60]]
      # The checker running pages (wbconf-lua, an empty home).
      test('check_pages', python3,
        args: [meson.current_source_dir() / 'integration' / 'check_pages_test.py',
               waybox_check_integration, wbconf_lua_exe,
               meson.project_source_root() / 'data' / 'integrations'],
        timeout: 120, suite: 'unit')
      integration_tests += [['toml_page', 'toml_page_test.py', [wbconf_lua_exe], 60]]
      integration_tests += [['mako_page', 'mako_page_test.py',
          [wbconf_lua_exe, meson.project_source_root() / 'data' / 'integrations'], 60]]
      integration_tests += [['waybar_page', 'waybar_page_test.py',
          [wbconf_lua_exe, meson.project_source_root() / 'data' / 'integrations'], 60]]
      integration_tests += [['fuzzel_page', 'fuzzel_page_test.py',
          [wbconf_lua_exe, meson.project_source_root() / 'data' / 'integrations'], 60]]
      integration_tests += [['wbconf_lua_page', 'wbconf_lua_page_test.py',
          [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf', wbconf_lua_exe], 150]]
      integration_tests += [['wbconf_page_reload', 'wbconf_page_reload_test.py',
          [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf', wbconf_lua_exe], 90]]
      integration_tests += [['wbconf_page_option', 'wbconf_page_option_test.py',
          [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf', wbconf_lua_exe], 90]]
      integration_tests += [['wbconf_page_list', 'wbconf_page_list_test.py',
          [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf', wbconf_lua_exe], 90]]
    endif
    integration_tests += [['wbconf_provides', 'wbconf_provides_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_shortcut_clash', 'wbconf_shortcut_clash_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf',
         meson.project_source_root() / 'data' / 'rc.xml'], 60]]
    integration_tests += [['wbconf_geo', 'wbconf_geo_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf',
         meson.project_source_root()], 60]]
    integration_tests += [['wbconf_location', 'wbconf_location_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf',
         meson.project_source_root()], 60]]
    integration_tests += [['wbconf_menu', 'wbconf_menu_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 90]]
    integration_tests += [['wbconf_rules', 'wbconf_rules_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_shortcuts', 'wbconf_shortcuts_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_problems', 'wbconf_problems_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_changes', 'wbconf_changes_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 90]]
    integration_tests += [['wbconf_reload', 'wbconf_reload_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_save_status', 'wbconf_save_status_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 90]]
    integration_tests += [['location_agent', 'location_agent_test.py',
        [location_agent_exe], 90]]
    integration_tests += [['wbconf_color_burst', 'wbconf_color_burst_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_toc', 'wbconf_toc_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_color', 'wbconf_color_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf', desktop_client], 60]]
    integration_tests += [['wbconf_output_manager', 'wbconf_output_manager_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf', fake_daemon], 60]]
    integration_tests += [['bell_wbconf', 'bell_styles_test.py',
        [bell_client, waybox_msg, wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['wbconf_osd', 'wbconf_osd_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
    integration_tests += [['identify', 'identify_test.py',
        [waybox_msg, wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
  endif
  # Run alone (Xwayland, and edge_tiling, which starts an X11 window):
  # every Waybox instance reserves an X display (wlroots tries
  # 33), so with many tests in parallel the one that needs Xwayland could
  # find none left.
  run_alone = ['xwayland', 'edge_tiling']
  i = 0
  foreach t : integration_tests
    test(
      'integration_' + t[0],
      python,
      args: [files('integration' / t[1]), waybox_exe] + t[2],
      timeout: t[3],
      env: integration_env,
      is_parallel: not run_alone.contains(t[0]),
      suite: i < compositor_count ? 'compositor' : 'wbconf',
      )
    i += 1
  endforeach

endif

# data/xdg-autostart against fixture autostart directories (needs only sh).
sh = find_program('sh', required: false)
if sh.found()
  test(
    'xdg_autostart',
    sh,
    args: [
      files('xdg_autostart_test.sh'),
      meson.project_source_root() / 'data' / 'xdg-autostart',
    ],
    timeout: 30,
    suite: 'unit',
    )
endif

# wbconf's process list (procs.cpp): only what runs, zombies left out.
if not get_option('tests_only') and is_variable('gtk4') and gtk4.found()
  procs_test = executable(
    'procs_test',
    ['procs_test.cpp', 'wb_test_main.cpp', files('../wbconf/procs.cpp')],
    include_directories: [inc_dir],
    dependencies: [gtk4, json_glib],
    )
  test('procs', procs_test, suite: 'unit')
endif
