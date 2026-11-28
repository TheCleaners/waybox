# waybox-agent-hub: the agent hub (doc/agent-tools.md), started and
# supervised by the compositor; GIO only.
agent_hub_exe = executable(
  'waybox-agent-hub',
  ['hub_main.cpp'],
  dependencies: [dependency('gio-2.0')],
  cpp_args: ['-DWB_HUB_VERSION="' + meson.project_version() + '"'],
  install: true,
  install_dir: get_option('prefix') / get_option('libexecdir'),
)
