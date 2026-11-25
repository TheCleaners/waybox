waybox-lua(5)

# NAME

waybox-lua - scripting Waybox with Lua

# DESCRIPTION

Waybox runs Lua scripts, when built with Lua (LuaJIT, Lua 5.5 or 5.4):
_$XDG_CONFIG_HOME/waybox/init.lua_ if it exists, then each file listed in
rc.xml:

```
<waybox>
  <scripts budget="10">
    <script>tiling.lua</script>
    <script>~/code/waybox/rules.lua</script>
  </scripts>
</waybox>
```

Relative paths are taken from _$XDG_CONFIG_HOME/waybox_. Without any script,
none of this costs anything. Reconfigure loads the scripts afresh.

Everything is in the global table *wb*. Output of *print* and *wb.log* goes
to Waybox's log, prefixed _wb-script:_.

# WHAT A SCRIPT CAN DO

Scripts run as you, inside Waybox. Like your autostart script, they can do
whatever your account can (*wb.action("Execute", ...)* runs any command).
Only run scripts you would run as shell scripts.

Scripts have the whole standard library (*io*, *os*, *require*, *debug*).

Every call into a script has a time budget (*budget*, in milliseconds,
default 10): a call that runs longer is stopped and logged, and a callback
stopped three times in a row is turned off until the next Reconfigure.
Errors are logged with their file and line, and skipped. The budget stops
Lua code, not a call that waits: *os.execute*, *io.popen* reads, or reading
a slow file hold Waybox up for as long as they take. To run a program, use
*wb.spawn* (or *wb.action("Execute", {command = ...})*), which don't wait.

# EVENTS

*wb.on(*_event_*,* _function_*)*

[[ *Event*
:[ *Arguments*
|  _window.new_, _window.close_, _window.focus_, _window.title_
:[ the window, and a table with its *id*, *app_id* and *title* (as they
   were: after _window.close_ the window itself reads *nil*)
|  _desktop.switch_
:[ the new desktop's number
|  _output.added_, _output.removed_
:[ the output's name

Events are delivered just after the change that caused them is over, never in
the middle of it.

# POLICIES

*wb.policy(*_name_*,* _function_*)* lets a script decide, where Waybox would.
It returns a decision, or *nil* to leave it to Waybox; on an error, or past
the time budget, Waybox decides. A policy only decides: actions asked for
during one are refused (*wb.action* returns *false*). Calling *wb.policy*
again replaces the function; *nil* removes it.

*rules*: _function(w)_
	For each new window, before it is shown. _w_ is a table: *app_id*,
	*title*, *tag*, *width*, *height*, *dialog*, *xwayland*. Return a
	table of rules, over rc.xml's *<applications>* key by key: *desktop*
	(a number, or "all"), *maximized*, *fullscreen*, *minimized*,
	*focus*, *decor*, *x*, *y* (relative to the usable area), *width*,
	*height*.

*place*: _function(w, area, proposed)_
	Where a new window goes, before its first frame, after rules and
	Waybox's placement (_proposed_). _area_ is the usable area. Return
	*{x=, y=, width=, height=}* (any of them; absolute coordinates).

*layout*: _function(windows, area, output, desktop)_
	Lays out the windows on each output, for the desktop shown: those
	that float (not maximized, fullscreen, tiled to an edge, minimized,
	or dialogs), as window handles, top of the stack first. Return
	*{[w.id] = {x=, y=, width=, height=}}*; windows left out stay where
	they are. Called after windows open, close, are minimized or sent
	to a desktop, after a desktop switch and when the usable area
	changes, once for several changes in a row; *wb.relayout()* asks
	for it too.

# WINDOWS

A window is a handle: it reads the window's state when asked, and once the
window has closed its fields are *nil* and its methods do nothing (they
return *false*).

Fields: *id* (the same as over IPC), *app_id*, *title*, *tag*, *x*, *y*,
*width*, *height*, *desktop*, *omnipresent*, *minimized*, *maximized*,
*fullscreen*, *focused*, *xwayland*.

Methods: *w:focus()* (which also brings back a minimized window),
*w:close()*, *w:raise()*, *w:lower()*, *w:minimize()*,
*w:toggle_maximize()*, *w:toggle_fullscreen()*, *w:toggle_decorations()*,
*w:toggle_omnipresent()*, *w:shade()*, *w:unshade()*,
*w:move_to_desktop(*_n_*)*, *w:set_geometry{*_x_*,* _y_*,* _width_*,*
_height_*}* (any of them; sizes are of the window's contents, and the window
leaves maximized or tiled first), and *w:action(*_name_*,* _params_*)* for
any action.

# FUNCTIONS

*wb.windows()*
	The shown windows, most recently used first.

*wb.focused()*
	The focused window, or *nil*.

*wb.desktop()*
	A table: *current* and *count*.

*wb.outputs()*
	A list of tables: *name*, *x*, *y*, *width*, *height*, *scale*.

*wb.action(*_name_*,* _params_*,* _window_*)*
	Runs an action by its rc.xml name (see *waybox*(5)) on _window_, or the
	focused window. _params_ is an optional table: *command*, *to*,
	*direction*, *wrap*, *follow*. Returns whether it ran. Nothing runs
	while the screen is locked.

*wb.bind(*_key_*,* _function_*)*
	Binds a key, in rc.xml's syntax (_W-Return_, _C-A-t_). The function
	gets the focused window (or *nil*). Script bindings come before
	rc.xml's.

*wb.relayout()*
	Asks for the *layout* policy to run (see POLICIES).

*wb.go_to_desktop(*_n_*)*
	Shows desktop _n_.

*wb.spawn(*_command_*,* _function_*)*
	Runs a program without waiting for it: _command_ is a string (run by
	_sh_) or a list (the program and its arguments). When it ends, the
	function (optional) gets its output (up to 1 MiB), its exit code
	(negative: the signal that ended it) and whether the output was cut.
	At most 16 run at once (then it returns *nil* and a reason); otherwise
	it returns the program's process id.

*wb.timer(*_ms_*,* _function_*)*, *wb.every(*_ms_*,* _function_*)*
	Runs the function once after _ms_ milliseconds, or every _ms_ (at least
	10). Returns an id for *wb.cancel(*_id_*)*.

*wb.log(...)*
	Writes to Waybox's log.

*wb.api_version*
	1. Additions keep it; changes or removals raise it, with the old
	behaviour kept for one release.

# SAMPLE SCRIPTS

Waybox ships some in _/usr/share/waybox/scripts_ (under its install
prefix); each is used only when listed in *<scripts>*:

*scratchpad.lua*
	Super+grave shows or hides a drop-down terminal.

*tiling.lua*
	Super+Alt+T tiles the desktop shown (one main window, the others
	stacked), or lets it float again.

*pip.lua*
	Picture-in-picture video windows on every desktop.

*app-desktops.lua*
	Programs open on the desktop of your choice (copy it and edit its
	table).

# EXAMPLE

```
-- Picture-in-picture windows on every desktop.
wb.on("window.new", function(w)
  if w.title:match("Picture%-in%-Picture") and not w.omnipresent then
    w:toggle_omnipresent()
  end
end)

-- Super+M: maximize the focused window, or restore it.
wb.bind("W-m", function(w)
  if w then w:toggle_maximize() end
end)
```

# SEE ALSO

*waybox*(1), *waybox*(5)
