# waybox-agent-hub: the agent hub (doc/agent-tools.md), started and
# supervised by the compositor; GIO only.
agent_hub_src = files('mcp_agent.cpp')
agent_hub_exe = executable(
  'waybox-agent-hub',
  ['hub_main.cpp', 'agents.cpp', agent_hub_src, json_src],
  include_directories: [inc_dir],
  dependencies: [dependency('gio-unix-2.0')],
  cpp_args: ['-DWB_HUB_VERSION="' + meson.project_version() + '"'],
  install: true,
  install_dir: get_option('prefix') / get_option('libexecdir'),
)

# waybox-mcp: the shim an agent starts as its MCP server (stdio), passing
# MCP to the hub. Plain C++: every assistant starts one.
agent_shim_exe = executable(
  'waybox-mcp',
  ['shim_main.cpp', json_src],
  include_directories: [inc_dir],
  cpp_args: ['-DWB_SHIM_VERSION="' + meson.project_version() + '"'],
  install: true,
)
