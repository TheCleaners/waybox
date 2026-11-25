project(
  'Waybox',
  ['c', 'cpp'],
  version: '0.2.3',
  license: 'MIT',
  meson_version: '>=0.60.0',
  default_options: [
    'c_std=c11',
    'cpp_std=c++20',
    'warning_level=2',
    'werror=true',
  ],
)

project_args = [
  '-Wno-unused-parameter',
  '-D_DEFAULT_SOURCE',
  '-DWL_HIDE_DEPRECATED', # Hide the deprecated parts of the Wayland API
  '-DWLR_USE_UNSTABLE',
  '-DPACKAGE_NAME="' + meson.project_name() + '"',
  '-DPACKAGE_VERSION="' + meson.project_version() + '"',
  '-DWB_SYSCONFDIR="' + get_option('prefix') / get_option('sysconfdir') + '"',
]
# On the BSDs everything is visible by default, and _POSIX_C_SOURCE would
# hide what's used (SOCK_CLOEXEC, O_CLOEXEC, pipe2, memfd_create, mkdtemp,
# MSG_NOSIGNAL); glibc's _DEFAULT_SOURCE keeps them with it.
if host_machine.system() not in ['freebsd', 'netbsd', 'openbsd', 'dragonfly']
  project_args += ['-D_POSIX_C_SOURCE=200112L']
endif
add_project_arguments(project_args, language: ['c', 'cpp'])

# wl_container_of() expands to offsetof(), which the C++ wlroots glue must use
# on the intrusive listener structs even though some of them are no longer
# standard-layout (they hold std::unique_ptr / wb::Listener members). offsetof
# is well defined for these layouts in practice; silence the warning rather
# than fight the C API. (clang makes this an error under -Werror; gcc does not.)
cpp = meson.get_compiler('cpp')
if cpp.has_argument('-Wno-invalid-offsetof')
  add_project_arguments('-Wno-invalid-offsetof', language: 'cpp')
endif

# GCC's -Wmaybe-uninitialized is unreliable at -O2+ and raises false positives
# from the inlined std::string machinery of std::variant<...> alternatives
# (our wb::Paint), breaking -Werror release builds. Keep it as a warning but
# not an error on GCC; clang has no such false positive and its uninitialized
# analysis still guards us under -Werror, so real bugs are not masked.
if cpp.get_id() == 'gcc' and cpp.has_argument('-Wno-error=maybe-uninitialized')
  add_project_arguments('-Wno-error=maybe-uninitialized', language: ['c', 'cpp'])
endif

cc = meson.get_compiler('c')

# Optionally build with AddressSanitizer + UndefinedBehaviorSanitizer. With the
# default 'auto' value this is enabled for debug buildtypes (the Meson default)
# so that leaks and UB are caught early during development, and disabled for
# release builds. Force with -Dsanitize=enabled / -Dsanitize=disabled.
sanitize_opt = get_option('sanitize')
build_is_debug = get_option('buildtype').startswith('debug')
if sanitize_opt.enabled() or (sanitize_opt.auto() and build_is_debug)
  sanitize_args = [
    '-fsanitize=address,undefined',
    '-fno-omit-frame-pointer',
    '-fno-sanitize-recover=all',
    # Not the alignment check: wl_list_for_each forms a pointer to an
    # "element" from the list head to know where the list ends (never
    # used), misaligned wherever elements hold a std::function (16-byte
    # aligned with libc++). Every other UB check stays.
    '-fno-sanitize=alignment',
  ]
  add_project_arguments(sanitize_args, language: ['c', 'cpp'])
  add_project_link_arguments(sanitize_args, language: ['c', 'cpp'])
  message('Building with AddressSanitizer + UndefinedBehaviorSanitizer')
endif

# Adding include directory
inc_dir = include_directories('include')

# Pure, wlroots-free sources shared by the compositor and the unit tests.
# Defined here (always processed) so the unit tests can build in tests_only mode
# without configuring the full compositor.
action_src = files('waybox/action.cpp')
window_cycle_src = files('waybox/window_cycle.cpp')
geometry_src = files('waybox/geometry.cpp')
mousebind_src = files('waybox/mousebind.cpp')
placement_src = files('waybox/placement.cpp')
applications_src = files('waybox/applications.cpp')
decor_mode_src = files('waybox/decor_mode.cpp')
frame_src = files('waybox/frame.cpp')
titlebar_draw_src = files('waybox/titlebar_draw.cpp')
icon_src = files('waybox/icon.cpp')
keychain_src = files('waybox/keychain.cpp')
theme_src = files('waybox/theme.cpp')
widget_src = files('waybox/widget.cpp')
animation_src = files('waybox/animation.cpp')
style_src = files('waybox/style.cpp')
menu_src = files('waybox/menu.cpp')
app_menu_src = files('waybox/app_menu.cpp')
desktops_src = files('waybox/desktops.cpp')
edge_tiling_src = files('waybox/edge_tiling.cpp')
client_menu_src = files('waybox/client_menu.cpp')
security_src = files('waybox/security.cpp')
output_rules_src = files('waybox/output_rules.cpp')
startup_src = files('waybox/startup.cpp')

# The build's version from git (see version.h.in): regenerated each build.
wb_version_cmd = ['git', '-C', meson.project_source_root(), 'describe', '--tags',
  '--dirty', '--always', '--abbrev=12']
version_in = files('version.h.in')
integrations_src = files('waybox/integrations.cpp')
startup_sources_src = files('waybox/startup_sources.cpp')
gtk_appearance_src = files('waybox/gtk_appearance.cpp')
stall_src = files('waybox/stall.cpp')
json_src = files('waybox/json.cpp')
session_store_src = files('waybox/session_store.cpp')
xml_strict_src = files('waybox/xml_strict.cpp')

# tests_only builds just the dependency-free unit tests (no wlroots, no
# compositor), so they can run sanitized in CI on a stock distro that lacks a
# matching wlroots package. The full build is unaffected.
tests_only = get_option('tests_only')

if not tests_only
  if get_option('wlroots-version') != ''
    wlroots_version = get_option('wlroots-version')
  else
    wlroots_version = ['wlroots-0.20', 'wlroots-0.19']
  endif

  libevdev = dependency('libevdev')
  libinput = dependency('libinput', version: '>=1.21.0', required: false)
  libxml2 = dependency('libxml-2.0')
  wayland_protos = dependency('wayland-protocols', version: '>=1.39')
  wayland_server = dependency('wayland-server', version: '>=1.15')
  wlroots = dependency(wlroots_version, version: '>=0.17.0')
  xkbcommon = dependency('xkbcommon')
  # Cairo + Pango: CPU rasterisation of compositor-drawn chrome (menus,
  # titlebars, OSD) into cached buffers that the GPU then composites. Only
  # linked into the compositor; the pure unit tests stay dependency-free.
  cairo = dependency('cairo')
  pango = dependency('pango')
  pangocairo = dependency('pangocairo')
  # librsvg is optional: when present, menu icons can be loaded from SVG
  # (most modern icon themes are SVG-only); otherwise icons are PNG-only and
  # SVG icons are skipped safely.
  rsvg = dependency('librsvg-2.0', required: get_option('svg'))
  if rsvg.found()
    add_project_arguments('-DHAVE_RSVG', language: 'cpp')
  endif
  # Lua scripting (optional): LuaJIT, else Lua 5.5 or 5.4.
  lua = dependency('luajit', 'lua5.5', 'lua-5.5', 'lua55', 'lua5.4', 'lua-5.4', 'lua54',
                   required: get_option('lua'))
  if lua.found()
    add_project_arguments('-DHAVE_LUA', language: 'cpp')
  endif

  # render.cpp needs cairo/pango (not wlroots), so it is shared by the
  # compositor and the pixel test; only defined in the full build.
  render_src = files('waybox/render.cpp')

  if libinput.found()
    add_project_arguments('-DHAS_LIBINPUT', language: ['c', 'cpp'])
  endif

  msgfmt = find_program('msgfmt', required: false)
  if msgfmt.found()
    source_root = meson.current_source_dir()
    subdir('po')
  endif

  # wbconf: an optional GTK4 settings GUI (obconf-style). Built only when GTK4
  # is available, so the compositor still builds on hosts without it.
  gtk4 = dependency('gtk4', required: get_option('wbconf'))
  json_glib = dependency('json-glib-1.0', required: get_option('wbconf'))

  subdir('data')
  subdir('doc')
  subdir('protocol')
  subdir('waybox')
  if gtk4.found() and json_glib.found()
    subdir('wbconf')
  endif
endif

# waybox-msg: the scripting socket from the shell.
if not get_option('tests_only')
  msg_version_h = vcs_tag(
    command: wb_version_cmd,
    fallback: meson.project_version(),
    input: version_in,
    output: 'version.h',
    replace_string: '@WB_VERSION@',
  )
  waybox_msg = executable('waybox-msg', ['msg/waybox-msg.cpp', json_src, msg_version_h],
    include_directories: [inc_dir],
    install: true,
  )
endif

subdir('test')
