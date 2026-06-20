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
(`waybox/window_cycle.cpp`), the usable-area/strut + size-hint geometry
(`waybox/geometry.cpp`), the mouse-binding parsing/matching
(`waybox/mousebind.cpp`), the window-placement policies
(`waybox/placement.cpp`), and the per-application rule matching
(`waybox/applications.cpp`), the key-chain stepping
(`waybox/keychain.cpp`), the per-window decoration-mode negotiation
(`waybox/decor_mode.cpp`), the theme model + themerc parsing
(`waybox/theme.cpp`), the menu model + layout/hit-testing
(`waybox/menu.cpp`; menu.xml parsing lives in the libxml-linked
`waybox/menu_parse.cpp`, exercised by the full-build `menu_parse` test), the
shared widget-styling primitives (`waybox/widget.cpp`), and the resolved
presentation/behaviour styles + theme adapters (`waybox/style.cpp`). Run it with:

```sh
meson test -C build --print-errorlogs
```

Each unit-test executable links just the source under test plus the header-only
harness (`test/wb_test.hpp` + `test/wb_test_main.cpp`); it pulls in **no**
wlroots/Wayland deps, so it builds and runs anywhere (incl. CI without a
compositor or sanitizer runtime). A dedicated CI job builds these with
`-Dtests_only=true -Dsanitize=enabled` on stock Ubuntu (which has the
sanitizer runtimes Alpine lacks), so the pure logic is exercised under
ASan/UBSan on every push. When you add a new framework with pure logic
(geometry/struts, window-state math, parsing), factor that logic into a
wlroots-free TU and add a `test/<name>_test.cpp` in the same PR.

There is also a **headless integration suite** (`test/integration/`, also under
`meson test`) that runs the real compositor and synthesizes input via the
**virtual-keyboard protocol** (`wtype`) to drive keybindings end to end under
the sanitizers: `alt_tab_test.py` (Alt+Tab → action → focus),
`window_state_test.py` (maximize/fullscreen restore-rect correctness, verified
from the compositor's `wb-geom` geometry log), `mouse_binding_test.py`
(virtual-pointer right-click on Root → mouse binding → Execute, via `wlrctl`),
and `menu_test.py` (right-click → `ShowMenu` → render → pointer-grab hover →
select entry → Execute, exercising the whole menu widget under ASan; uses
`test/lsan.supp` to suppress Pango/fontconfig global caches). They need
`foot`/`wtype`/`wlrctl` and a
working headless backend; when those are missing they **skip** (exit 77), so CI
stays green. Run them locally against a sanitized build to exercise interactive
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
- `server` — lifecycle, global protocols (incl. xdg-activation focus requests),
  `wb_spawn()`.
- `output` — output config, **HiDPI/fractional scaling**, and the
  output-management protocol (wlr-randr/kanshi). Recomputes usable area and
  reflows windows on mode/scale changes.
- `xdg_shell` — application windows (`wb_toplevel`), focus, move/resize. Window
  state lives on `wb_toplevel`: independent `max_horz`/`max_vert`, plus separate
  `restore_*` rects per state (maximize/fullscreen/shade/minimize) so
  interleaving them restores correctly. `set_toplevel_maximized()` /
  `set_toplevel_fullscreen()` are the canonical state setters. New windows are
  positioned by the pure policies in `waybox/placement.cpp` (Smart/Center/
  UnderMouse, `<placement><policy>`). Each mapped toplevel also exposes a
  zwlr-foreign-toplevel-management handle (taskbar list/activate/close/
  maximize/minimize/fullscreen), kept in sync with focus/state changes.
- `layer_shell` — panels/launchers (`wb_layer_surface`); `arrange_layers()`
  computes each output's `usable_area` from exclusive zones. Usable-area
  consumers (constrain-to-usable, maximize insets) run through the pure,
  unit-tested `wb::Rect`/`wb::Strut` helpers in `waybox/geometry.cpp`.
- `seat` — keyboards, keybindings (`handle_keybinding`; bindings are a tree of
  `wb::KeyBinding` supporting Openbox key chains, stepped via the pure
  `waybox/keychain.cpp`), libinput config, and
  virtual keyboard/pointer (`zwp_virtual_keyboard_v1` / `wlr_virtual_pointer_v1`,
  routed through the same handlers as physical devices).
- `cursor` — pointer handling, interactive move/resize (with edge/window
  snapping via `waybox/placement.cpp`'s `snap_move`), Alt+drag, and mouse
  bindings (`waybox/mousebind.cpp` parses `<mouse><context>`; `on_button`
  resolves the context — Root/Client today, Titlebar/Frame once SSD lands — and
  dispatches matching bindings through `run_action`).
- `config` — parses Openbox-style `rc.xml` via libxml2 + XPath.
- `decoration` / `idle` — xdg-decoration and idle-inhibit glue.

**Portability:** the compositor avoids OS-specific headers in its own logic —
pointer button codes use `wb::MOUSE_BUTTON_*` (the evdev values wlroots reports
on every platform) instead of `<linux/input-event-codes.h>`. The remaining
non-portable-looking dependency is libevdev (`seat.cpp`, only to resolve a
scroll-button name), which is itself available on the BSDs. Keep new code free
of `<linux/...>` includes so a BSD port stays within reach.

Rendering is the wlroots **scene graph**; per output there are layer scene-trees
(`background`/`bottom`/`top`/`overlay`). Window **stacking order** is
`server->toplevels` (`wb_toplevel::link`, head = top; use `raise_toplevel()` /
`lower_toplevel()`) and is kept independent of **focus order**, which is the MRU
list `server->focus_order` (`wb_toplevel::focus_link`, head = active; use
`first_toplevel()` for the active window). Focusing conventionally raises, so the
two heads usually coincide, but they are tracked separately. Alt+Tab selection
goes through the pure, unit-tested `wb::cycle_next()` (`window_cycle.cpp`).

Compositor-drawn chrome is rasterised on the CPU with **Cairo/Pango** and cached
as a `wlr_scene_buffer` that the GPU then composites: `waybox/render.cpp` has the
painting primitives (`paint_rect`/`paint_texture`/`paint_text`/`paint_fill`,
pixel-tested in `render_test`, no wlroots), and `waybox/scene_buffer.cpp`'s
`wb::SceneCanvas` wraps a Cairo surface as a `wlr_buffer` and attaches it to the
scene (HiDPI via dest-size). `SceneCanvas` is **re-committable** — repaint and
`commit()` again to update a surface (e.g. a menu hover); it does not tear down
its context on commit.

The first consumer is the **interactive root menu** (`waybox/menu_widget.cpp`,
`wb::MenuWidget`): the `ShowMenu` action opens a named menu (from `menu.xml`,
loaded in `config.cpp` into `wb_config::menu`) at the pointer, rendering one
`SceneCanvas` panel per open level themed from a `MenuStyle`. While open it is an
input grab — `cursor.cpp`/`seat.cpp` route pointer motion/buttons and Escape to
it before clients. Selecting an entry yields its actions via `take_actions()`,
which the caller runs **after** destroying the widget (no reentrancy). The pure
geometry (`layout_menu`/`menu_item_at`/`place_root_menu`/`place_submenu`) lives
in `menu.cpp` and is unit-tested; the widget is covered by `integration_menu`.

Compositor-drawn chrome (titlebars, OSD, SSD) is otherwise not built yet.

Presentation is layered so widgets never re-derive look-and-feel: `wb::Theme`
(`waybox/theme.cpp`) is the **raw themerc file model** (Openbox keys verbatim,
plus `waybox.*` extension keys Openbox ignores); `waybox/widget.cpp` defines a
shared **styling vocabulary** (`Insets`, `Border`, `TextStyle`, `Surface`,
`ControlStyle`/`StateStyle` per `WidgetState`); and `waybox/style.cpp` resolves
a Theme into render-ready, per-widget styles (`MenuStyle`, `FrameStyle`,
`SwitcherStyle`) plus behaviour structs (`MenuBehavior`, `SwitcherBehavior`) via
`*_from_theme()` adapters. **Renderers consume the resolved `*Style` structs,
never `wb::Theme` directly** — so richer themes/settings can populate fields
themerc never had, and the menu/switcher/frame all share one substrate.

Fills are a `wb::Paint` `std::variant<SolidPaint, GradientPaint, ImagePaint,
ShaderPaint>` and surfaces carry an `Effects` struct (shadow/backdrop blur) —
the seams for future GPU/shader chrome (animated borders, shader backgrounds via
SceneFX or a GPU painter). Everything visual is **opt-in and capability-gated**:
`effective_paint()`/`effective_effects()` resolve a `Paint`/`Effects` against
`RenderCaps` and **degrade to a safe solid fallback** when shaders/images are
disabled or unaccelerated — effects must never crash or stall a session.
Animation has no timeline in wlroots/SceneFX, so `waybox/animation.cpp` provides
the pure easing/progress maths and an `Animation` interface a backend ticks per
output frame. Rasterise static chrome once and cache; reserve per-frame GPU work
for genuine animations/effects.

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
