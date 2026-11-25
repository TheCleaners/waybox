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
