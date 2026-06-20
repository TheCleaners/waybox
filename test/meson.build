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
