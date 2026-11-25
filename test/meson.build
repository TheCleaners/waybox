# Unit tests for the pure, wlroots-free logic. Each test executable links the
# specific source under test plus the shared harness main; no wlroots/Wayland
# dependencies, so these build and run anywhere (including CI without a
# compositor or sanitizer runtime).

action_test = executable(
  'action_test',
  ['action_test.cpp', 'wb_test_main.cpp', action_src],
  include_directories: [inc_dir],
  )
test('action', action_test)

window_cycle_test = executable(
  'window_cycle_test',
  ['window_cycle_test.cpp', 'wb_test_main.cpp', window_cycle_src],
  include_directories: [inc_dir],
  )
test('window_cycle', window_cycle_test)

geometry_test = executable(
  'geometry_test',
  ['geometry_test.cpp', 'wb_test_main.cpp', geometry_src],
  include_directories: [inc_dir],
  )
test('geometry', geometry_test)

mousebind_test = executable(
  'mousebind_test',
  ['mousebind_test.cpp', 'wb_test_main.cpp', mousebind_src],
  include_directories: [inc_dir],
  )
test('mousebind', mousebind_test)

placement_test = executable(
  'placement_test',
  ['placement_test.cpp', 'wb_test_main.cpp', placement_src, geometry_src],
  include_directories: [inc_dir],
  )
test('placement', placement_test)

applications_test = executable(
  'applications_test',
  ['applications_test.cpp', 'wb_test_main.cpp', applications_src],
  include_directories: [inc_dir],
  )
test('applications', applications_test)

keychain_test = executable(
  'keychain_test',
  ['keychain_test.cpp', 'wb_test_main.cpp', keychain_src],
  include_directories: [inc_dir],
  )
test('keychain', keychain_test)

decor_mode_test = executable(
  'decor_mode_test',
  ['decor_mode_test.cpp', 'wb_test_main.cpp', decor_mode_src],
  include_directories: [inc_dir],
  )
test('decor_mode', decor_mode_test)

frame_test = executable(
  'frame_test',
  ['frame_test.cpp', 'wb_test_main.cpp', frame_src, geometry_src],
  include_directories: [inc_dir],
  )
test('frame', frame_test)

icon_test = executable(
  'icon_test',
  ['icon_test.cpp', 'wb_test_main.cpp', icon_src],
  include_directories: [inc_dir],
  )
test('icon', icon_test)

theme_test = executable(
  'theme_test',
  ['theme_test.cpp', 'wb_test_main.cpp', theme_src],
  include_directories: [inc_dir],
  )
test('theme', theme_test)

widget_test = executable(
  'widget_test',
  ['widget_test.cpp', 'wb_test_main.cpp', widget_src],
  include_directories: [inc_dir],
  )
test('widget', widget_test)

animation_test = executable(
  'animation_test',
  ['animation_test.cpp', 'wb_test_main.cpp', animation_src],
  include_directories: [inc_dir],
  )
test('animation', animation_test)

style_test = executable(
  'style_test',
  ['style_test.cpp', 'wb_test_main.cpp', style_src, theme_src, widget_src],
  include_directories: [inc_dir],
  )
test('style', style_test)

menu_test = executable(
  'menu_test',
  ['menu_test.cpp', 'wb_test_main.cpp', menu_src, action_src],
  include_directories: [inc_dir],
  )
test('menu', menu_test)

desktops_test = executable(
  'desktops_test',
  ['desktops_test.cpp', 'wb_test_main.cpp', desktops_src],
  include_directories: [inc_dir],
  )
test('desktops', desktops_test)

edge_tiling_test = executable(
  'edge_tiling_test',
  ['edge_tiling_test.cpp', 'wb_test_main.cpp', edge_tiling_src, geometry_src],
  include_directories: [inc_dir],
  )
test('edge_tiling', edge_tiling_test)

client_menu_test = executable(
  'client_menu_test',
  ['client_menu_test.cpp', 'wb_test_main.cpp', client_menu_src, menu_src, action_src],
  include_directories: [inc_dir],
  )
test('client_menu', client_menu_test)

security_test = executable(
  'security_test',
  ['security_test.cpp', 'wb_test_main.cpp', security_src],
  include_directories: [inc_dir],
  )
test('security', security_test)

output_rules_test = executable(
  'output_rules_test',
  ['output_rules_test.cpp', 'wb_test_main.cpp', output_rules_src],
  include_directories: [inc_dir],
  )
test('output_rules', output_rules_test)

startup_test = executable(
  'startup_test',
  ['startup_test.cpp', 'wb_test_main.cpp', startup_src],
  include_directories: [inc_dir],
  )
test('startup', startup_test)

integrations_test = executable(
  'integrations_test',
  ['integrations_test.cpp', 'wb_test_main.cpp', integrations_src, startup_src],
  include_directories: [inc_dir],
  )
test('integrations', integrations_test)

json_test = executable(
  'json_test',
  ['json_test.cpp', 'wb_test_main.cpp', json_src],
  include_directories: [inc_dir],
  )
test('json', json_test)

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
    )
endif

# Parser test for menu.xml; links libxml2, so full-build only (like render).
if not get_option('tests_only')
  menu_parse_test = executable(
    'menu_parse_test',
    ['menu_parse_test.cpp', 'wb_test_main.cpp', menu_src,
     action_src, files('../waybox/menu_parse.cpp', '../waybox/action_xml.cpp')],
    include_directories: [inc_dir],
    dependencies: [libxml2],
    )
  test('menu_parse', menu_parse_test)
endif

# wbconf rc.xml model round-trip test; links libxml2 + theme, full-build only.
if not get_option('tests_only')
  wbconf_test = executable(
    'wbconf_test',
    ['wbconf_test.cpp', 'wb_test_main.cpp', files('../waybox/wbconf.cpp'), startup_src,
     xml_strict_src,
     theme_src],
    include_directories: [inc_dir],
    dependencies: [libxml2],
    )
  test('wbconf', wbconf_test)

  if json_glib.found()
  settings_schema_test = executable(
    'settings_schema_test',
    ['settings_schema_test.cpp', 'wb_test_main.cpp', files('../waybox/settings_schema.cpp',
     '../waybox/wbconf.cpp'), startup_src, xml_strict_src, theme_src],
    include_directories: [inc_dir],
    dependencies: [libxml2, json_glib],
    cpp_args: ['-DWB_SOURCE_ROOT="' + meson.project_source_root() + '"'],
    )
  test('settings_schema', settings_schema_test)
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
}
if python.found() and not get_option('tests_only')
  # A minimal ext-session-lock-v1 client for the session-lock test.
  session_lock_xml = wl_protocol_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml'
  lock_client = executable(
    'lock_client',
    [
      'integration' / 'lock_client.c',
      wayland_scanner_client.process(session_lock_xml),
      wayland_scanner_code.process(session_lock_xml),
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

  # The integration tests: [name, script in integration/, arguments after the
  # Waybox binary, timeout in seconds]. Each runs with integration_env and
  # skips (exit 77) without its tools. One line per test, kept sorted, so
  # branches that each add a test don't conflict here.
  default_rc = meson.project_source_root() / 'data' / 'rc.xml'
  integration_tests = [
    ['alt_tab', 'alt_tab_test.py', [default_rc], 60],
    ['app_rule', 'app_rule_test.py', [], 60],
    ['color_management', 'color_management_test.py', [], 90],
    ['config_errors', 'config_errors_test.py', [], 60],
    ['desktop_popup', 'desktop_popup_test.py', [], 60],
    ['dialogs', 'dialogs_test.py', [dialog_client], 60],
    ['desktops', 'desktops_test.py', [], 90],
    ['edge_tiling', 'edge_tiling_test.py', [], 90],
    ['foreign_toplevel', 'foreign_toplevel_test.py', [default_rc], 60],
    ['fullscreen_panel', 'fullscreen_panel_test.py', [layer_client], 60],
    ['idle_inhibit', 'idle_inhibit_test.py', [inhibit_client], 60],
    ['idle_screen_off', 'idle_screen_off_test.py', [inhibit_client], 60],
    ['input_focus', 'input_focus_test.py', [], 60],
    ['input_method', 'input_method_test.py', [ime_client, text_input_client], 90],
    ['ipc', 'ipc_test.py', [waybox_msg], 60],
    ['key_chain', 'key_chain_test.py', [], 60],
    ['lock_popup', 'lock_popup_test.py', [popup_client, lock_client], 60],
    ['keymap_reconfigure', 'keymap_reconfigure_test.py', [], 60],
    ['menu', 'menu_test.py', [], 60],
    ['menu_hover_delay', 'menu_hover_delay_test.py', [], 60],
    ['mouse_binding', 'mouse_binding_test.py', [], 60],
    ['mouse_contexts', 'mouse_contexts_test.py', [], 60],
    ['multi_output', 'multi_output_test.py', [layer_client], 60],
    ['output_hotplug', 'output_hotplug_test.py', [layer_client], 60],
    ['output_power', 'output_power_test.py', [power_client], 60],
    ['output_rules', 'output_rules_test.py', [], 60],
    ['panel_reflow', 'panel_reflow_test.py', [layer_client], 60],
    ['pipe_menu', 'pipe_menu_test.py', [], 60],
    ['placement', 'placement_test.py', [default_rc], 60],
    ['pointer_grab', 'pointer_grab_test.py', [constraint_client, layer_client, vpointer_client], 60],
    ['pointer_lock', 'pointer_lock_test.py', [constraint_client], 60],
    ['reconfigure', 'reconfigure_test.py', [], 60],
    ['robustness', 'robustness_test.py', [layer_client], 90],
    ['sandbox', 'sandbox_test.py', [sandbox_client], 60],
    ['session_lock', 'session_lock_test.py', [lock_client], 60],
    ['session_lock_virtual_always', 'session_lock_test.py', [lock_client, 'always'], 60],
    ['shortcuts_inhibit', 'shortcuts_inhibit_test.py', [kbinhibit_client], 60],
    ['ssd', 'ssd_test.py', [], 60],
    ['startup_programs', 'startup_programs_test.py', [], 60],
    ['switcher_icons', 'switcher_icons_test.py', [], 60],
    ['switcher_order', 'switcher_order_test.py', [], 60],
    ['tearing', 'tearing_test.py', [tearing_client], 60],
    ['titlebar_buttons', 'titlebar_buttons_test.py', [], 60],
    ['titlebar_icon', 'titlebar_icon_test.py', [], 60],
    ['window_capture', 'window_capture_test.py', [], 60],
    ['window_menu', 'window_menu_test.py', [], 90],
    ['window_state', 'window_state_test.py', [], 60],
    ['wlr_data_control', 'wlr_data_control_test.py', [wlr_dc_client], 60],
    ['workspace', 'workspace_test.py', [workspace_client], 60],
    ['xwayland', 'xwayland_test.py', [], 90],
  ]
  # wbconf is built only with GTK 4.
  if is_variable('wbconf_exe')
    integration_tests += [['wbconf_save', 'wbconf_save_test.py',
        [wbconf_exe, meson.project_source_root() / 'data' / 'wbconf'], 60]]
  endif
  # Run alone (Xwayland, and edge_tiling, which starts an X11 window):
  # every Waybox instance reserves an X display (wlroots tries
  # 33), so with many tests in parallel the one that needs Xwayland could
  # find none left.
  run_alone = ['xwayland', 'edge_tiling']
  foreach t : integration_tests
    test(
      'integration_' + t[0],
      python,
      args: [files('integration' / t[1]), waybox_exe] + t[2],
      timeout: t[3],
      env: integration_env,
      is_parallel: not run_alone.contains(t[0]),
      )
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
    )
endif
