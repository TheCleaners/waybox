# Copilot instructions for Waybox

Waybox is a minimalist, Openbox-inspired Wayland compositor built on **wlroots
0.20** (with 0.19 fallback). It deliberately implements only window-management
mechanism; panels, wallpaper, menus, launchers, etc. are external programs.

## Build, run, test

The build is **Meson + Ninja**, compiled as **C++20** (the wayland protocol glue
is still C):

```sh
meson setup build        # default buildtype=debug -> ASan+UBSan auto-enabled
ninja -C build           # binary at build/waybox/waybox
```

- `werror=true` is set globally — the build must stay warning-clean on **both**
  `g++` and `clang++` (CI builds both: `.github/workflows/build.yml`, `.build.yml`).
- Release build (no sanitizers): `meson setup build-release --buildtype=release`.
- Sanitizers are controlled by `-Dsanitize=` (`auto`/`enabled`/`disabled`);
  `auto` turns them on for debug buildtypes. Always develop against a sanitized
  build.

There is a small **unit-test suite** under `test/` (wired into `meson test` and
CI) that covers the **pure, wlroots-free logic** — currently the action
framework (`waybox/action.cpp`), the Alt+Tab cycle selector
(`waybox/window_cycle.cpp`), and the usable-area/strut geometry
(`waybox/geometry.cpp`). Run it with:

```sh
meson test -C build --print-errorlogs
```

Each unit-test executable links just the source under test plus the header-only
harness (`test/wb_test.hpp` + `test/wb_test_main.cpp`); it pulls in **no**
wlroots/Wayland deps, so it builds and runs anywhere (incl. CI without a
compositor or sanitizer runtime). When you add a new framework with pure logic
(geometry/struts, window-state math, parsing), factor that logic into a
wlroots-free TU and add a `test/<name>_test.cpp` in the same PR.

There is also a **headless integration test** (`test/integration/alt_tab_test.py`,
also under `meson test`) that runs the real compositor and synthesizes input via
the **virtual-keyboard protocol** (`wtype`) to drive a keybinding end to end
(Alt+Tab → action → focus) under the sanitizers. It needs `foot` + `wtype` and a
working headless backend; when those are missing it **skips** (exit 77), so CI
stays green. Run it locally against a sanitized build to exercise interactive
paths. Drive virtual input by hand with `wtype` (keyboard) and `wlrctl pointer`
(pointer); both go through the same handlers as physical devices.

The event-driven compositor glue is **not** unit-tested; validate it by running
the compositor **headless** (works over SSH, no seat needed) and exercising it
with real clients under the sanitizers:

```sh
WB_RC_XML=$PWD/data/rc.xml WLR_BACKENDS=headless WLR_RENDERER=pixman \
  ./build/waybox/waybox -s foot --debug
```

Then connect clients against its socket (e.g. `WAYLAND_DISPLAY=wayland-0 waybar`,
`wlr-randr --output HEADLESS-1 --scale 1.5`, `grim`). A clean run logs
`Display destroyed` on shutdown and produces **zero** ASan/UBSan output; treat
any sanitizer line or wlroots `Assertion ... failed` as a release blocker. Send
`SIGTERM`/`SIGINT` for a clean shutdown (it routes to `wl_display_terminate`).

Local install used during development: `meson install -C build-install` after
`meson setup build-install --buildtype=release --prefix=$HOME/.local`. The
installed `bin/waybox` is a wrapper script (`data/waybox.sh`) that execs the
real binary from `libexec/`.

## Architecture

Each subsystem owns one `wb_*` struct and a matching `init_*` function; the
central `struct wb_server` (`include/waybox/server.h`) holds pointers to all of
them plus the global wlroots managers. Startup/teardown flow lives in
`waybox/main.cpp` + `waybox/server.cpp`:

`wb_create_backend()` (display, backend, renderer, allocator, seat, cursor) →
`wb_start_server()` (`init_config`, `init_output`, scene graph, then
`init_xdg_decoration` / `init_layer_shell` / `init_xdg_shell`) → run →
`wb_terminate()`.

Subsystem map (`waybox/<name>.cpp`, headers in `include/waybox/` or `waybox/`):
- `server` — lifecycle, global protocols, `wb_spawn()`.
- `output` — output config, **HiDPI/fractional scaling**, and the
  output-management protocol (wlr-randr/kanshi). Recomputes usable area and
  reflows windows on mode/scale changes.
- `xdg_shell` — application windows (`wb_toplevel`), focus, move/resize, maximize.
- `layer_shell` — panels/launchers (`wb_layer_surface`); `arrange_layers()`
  computes each output's `usable_area` from exclusive zones. Usable-area
  consumers (constrain-to-usable, maximize insets) run through the pure,
  unit-tested `wb::Rect`/`wb::Strut` helpers in `waybox/geometry.cpp`.
- `seat` — keyboards, keybindings (`handle_keybinding`), libinput config, and
  virtual keyboard/pointer (`zwp_virtual_keyboard_v1` / `wlr_virtual_pointer_v1`,
  routed through the same handlers as physical devices).
- `cursor` — pointer handling, interactive move/resize, Alt+drag.
- `config` — parses Openbox-style `rc.xml` via libxml2 + XPath.
- `decoration` / `idle` — xdg-decoration and idle-inhibit glue.

Rendering is the wlroots **scene graph**; per output there are layer scene-trees
(`background`/`bottom`/`top`/`overlay`). Window **stacking order** is
`server->toplevels` (`wb_toplevel::link`, head = top; use `raise_toplevel()` /
`lower_toplevel()`) and is kept independent of **focus order**, which is the MRU
list `server->focus_order` (`wb_toplevel::focus_link`, head = active; use
`first_toplevel()` for the active window). Focusing conventionally raises, so the
two heads usually coincide, but they are tracked separately. Alt+Tab selection
goes through the pure, unit-tested `wb::cycle_next()` (`window_cycle.cpp`).

## Conventions

- **Never include raw `<wlr/...>` headers.** Include `"waybox/wlroots.hpp"`,
  which pre-includes the C++ stdlib then includes the wlroots/wayland headers
  inside `extern "C"` with `static`/`namespace`/`class` keyword guards. Add any
  new wlroots header there. (wlroots 0.20 headers are not directly C++-includable.)
- **Prefer `wb::Listener` (`include/waybox/listener.hpp`) over raw
  `wl_listener`.** It connects to a `wl_signal` with a lambda (capture the
  context you need) and disconnects in its destructor, replacing the
  `wl_container_of` + manual `wl_list_remove()` teardown. All listeners use it.
- **Destructor ordering with `wb::Listener`:** if a destructor frees a wlroots
  object whose signals its listeners are attached to, call `.disconnect()` on
  those listeners **at the top of the destructor body** — member destructors run
  *after* the body, and `wlr_*_destroy` asserts no listeners remain.
- **Never index `server->toplevels.next` / `link.next` without an empty/sentinel
  check** — use `first_toplevel()` / `wl_list_empty()`. The list head is a
  sentinel, not a `wb_toplevel`.
- **Scene-node `data` is polymorphic** (a `wb_toplevel*` for toplevels, a
  `wb_scene_descriptor*` for layer surfaces). Validate before reinterpreting —
  e.g. `get_toplevel_at()` confirms the candidate is in `server->toplevels`.
- **Defer `wlr_xdg_popup_unconstrain_from_box()` / size/maximize calls to the
  surface's initial commit**, not creation: `wlr_xdg_surface_schedule_configure`
  asserts `surface->initialized`.
- Spawn external processes through `wb_spawn()` (double-fork, `setsid`,
  zombie-free), not bare `fork()`/`execl()`.
- Wrap user-facing strings in `_()` for gettext; new strings go in `po/`.
- The config path is resolved from `WB_RC_XML`, then `--config-file`, then
  `$XDG_CONFIG_HOME`/`$HOME`. `data/rc.xml` is the shipped default and the
  reference for which `rc.xml` elements/actions are supported.

## Adding a Wayland protocol

Add the protocol XML to the `protocols` list in `protocol/meson.build` (it
generates the `*-protocol.h`/`.c`); then create its manager in the relevant
`init_*` and wire events with `wb::Listener`.

## Adding a key/menu action

Actions use a registry (`waybox/action.cpp`) + a parsed `std::vector<wb::Action>`
per binding. To add one: add a row to `kRegistry` in `waybox/action.cpp` (name,
`ActionType`, whether it takes a `<command>`/`<execute>`), add the enum value in
`include/waybox/action.hpp`, add a `case` to `wb::run_action()` in `waybox/seat.cpp`
(the live half), and extend `test/action_test.cpp`. rc.xml parsing
(`collect_actions` in `config.cpp`) and dispatch are name-driven, so no other
wiring is needed.
