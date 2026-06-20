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
]
add_project_arguments(project_args, language: ['c', 'cpp'])

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
