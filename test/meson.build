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
     action_src, files('../waybox/menu_parse.cpp')],
    include_directories: [inc_dir],
    dependencies: [libxml2],
    )
  test('menu_parse', menu_parse_test)
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
}
if python.found() and not get_option('tests_only')
  test(
    'integration_alt_tab',
    python,
    args: [
      files('integration' / 'alt_tab_test.py'),
      waybox_exe,
      meson.project_source_root() / 'data' / 'rc.xml',
    ],
    timeout: 60,
    env: integration_env,
    )
  test(
    'integration_window_state',
    python,
    args: [
      files('integration' / 'window_state_test.py'),
      waybox_exe,
    ],
    timeout: 60,
    env: integration_env,
    )
  test(
    'integration_mouse_binding',
    python,
    args: [
      files('integration' / 'mouse_binding_test.py'),
      waybox_exe,
    ],
    timeout: 60,
    env: integration_env,
    )
  test(
    'integration_placement',
    python,
    args: [
      files('integration' / 'placement_test.py'),
      waybox_exe,
      meson.project_source_root() / 'data' / 'rc.xml',
    ],
    timeout: 60,
    env: integration_env,
    )
  test(
    'integration_app_rule',
    python,
    args: [
      files('integration' / 'app_rule_test.py'),
      waybox_exe,
    ],
    timeout: 60,
    env: integration_env,
    )
  test(
    'integration_key_chain',
    python,
    args: [
      files('integration' / 'key_chain_test.py'),
      waybox_exe,
    ],
    timeout: 60,
    env: integration_env,
    )
  test(
    'integration_foreign_toplevel',
    python,
    args: [
      files('integration' / 'foreign_toplevel_test.py'),
      waybox_exe,
      meson.project_source_root() / 'data' / 'rc.xml',
    ],
    timeout: 60,
    env: integration_env,
    )
  test(
    'integration_menu',
    python,
    args: [
      files('integration' / 'menu_test.py'),
      waybox_exe,
    ],
    timeout: 60,
    env: integration_env,
    )
  test(
    'integration_ssd',
    python,
    args: [
      files('integration' / 'ssd_test.py'),
      waybox_exe,
    ],
    timeout: 60,
    env: integration_env,
    )
endif
