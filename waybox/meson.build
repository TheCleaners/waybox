action_src = files('action.cpp')
window_cycle_src = files('window_cycle.cpp')
geometry_src = files('geometry.cpp')
mousebind_src = files('mousebind.cpp')
placement_src = files('placement.cpp')
applications_src = files('applications.cpp')

wb_src = [
  action_src,
  window_cycle_src,
  geometry_src,
  mousebind_src,
  placement_src,
  applications_src,
  keychain_src,
  theme_src,
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
