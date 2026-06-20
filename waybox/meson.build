wb_src = [
  action_src,
  window_cycle_src,
  geometry_src,
  mousebind_src,
  placement_src,
  applications_src,
  keychain_src,
  theme_src,
  render_src,
  files(
    'config.cpp',
    'cursor.cpp',
    'decoration.cpp',
    'idle.cpp',
    'layer_shell.cpp',
    'main.cpp',
    'output.cpp',
    'scene_buffer.cpp',
    'seat.cpp',
    'server.cpp',
    'xdg_shell.cpp',
  ),
  ]

wb_dep = [
  cairo,
  libevdev,
  libinput,
  libxml2,
  pango,
  pangocairo,
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
