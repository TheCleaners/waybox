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
  '-D_POSIX_C_SOURCE=200112L',
  '-DWL_HIDE_DEPRECATED', # Hide the deprecated parts of the Wayland API
  '-DWLR_USE_UNSTABLE',
  '-DPACKAGE_NAME="' + meson.project_name() + '"',
  '-DPACKAGE_VERSION="' + meson.project_version() + '"',
  '-DWB_SYSCONFDIR="' + get_option('prefix') / get_option('sysconfdir') + '"',
]
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
keychain_src = files('waybox/keychain.cpp')
theme_src = files('waybox/theme.cpp')
menu_src = files('waybox/menu.cpp')

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

  # render.cpp needs cairo/pango (not wlroots), so it is shared by the
  # compositor and the pixel test; only defined in the full build.
  render_src = files('waybox/render.cpp')

  if libinput.found()
    add_project_arguments('-DHAS_LIBINPUT', language: 'c')
  endif

  msgfmt = find_program('msgfmt', required: false)
  if msgfmt.found()
    source_root = meson.current_source_dir()
    subdir('po')
  endif

  subdir('data')
  subdir('protocol')
  subdir('waybox')
endif

subdir('test')
