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

# Headless integration test driving real synthesized input via the
# virtual-keyboard protocol. Skips (exit 77) when foot/wtype or a headless
# display are unavailable, so CI stays green; run a sanitized local build to
# actually exercise it. Give it a generous timeout (it spawns clients + sleeps).
python = find_program('python3', required: false)
if python.found()
  test(
    'integration_alt_tab',
    python,
    args: [
      files('integration' / 'alt_tab_test.py'),
      waybox_exe,
      meson.project_source_root() / 'data' / 'rc.xml',
    ],
    timeout: 60,
    )
endif
