action_src = files('action.cpp')
window_cycle_src = files('window_cycle.cpp')

wb_src = [
  action_src,
  window_cycle_src,
  files(
    'config.cpp',
    'cursor.cpp',
    'decoration.cpp',
    'idle.cpp',
    'layer_shell.cpp',
    'main.cpp',
    'output.cpp',
    'seat.cpp',
    'server.cpp',
    'xdg_shell.cpp',
  ),
  ]

wb_dep = [
  libevdev,
  libinput,
  libxml2,
  wayland_server,
  wlroots,
  xkbcommon,
  ]

waybox_exe = executable(
  'waybox',
  wb_src,
  include_directories: [inc_dir],
  dependencies: [wb_dep, wlr_protos],
  install: true,
  install_dir: get_option('prefix') / get_option('libexecdir'),
  link_args: ['-Wl,-lm'],
  )
