waybox(5)

# NAME

waybox - Waybox configuration: rc.xml, menu.xml and themes

# DESCRIPTION

Waybox reads Openbox's configuration formats. *rc.xml* holds settings,
bindings and per-application rules; *menu.xml* defines the root menu; a
theme (*themerc*) styles window frames, menus and the window switcher. See
*waybox*(1) for where these files are looked up.

Waybox supports the subset of Openbox's settings described here and ignores
the rest, so an Openbox *rc.xml* can be used as it is. Settings specific to
Waybox live in a *<waybox>* block, which Openbox ignores.

Run the *Reconfigure* action, or send *SIGUSR2* to Waybox, to apply changes
without restarting.

Boolean values are written *yes* or *no* (*true*/*false* and *1*/*0* work
too).

# RC.XML

The root element is *<openbox_config xmlns="http://openbox.org/3.4/rc">*.

## Theme

```
<theme>
  <name>Onyx-Citrus</name>
  <font place="ActiveWindow">
    <name>Sans</name><size>10</size><weight>bold</weight>
  </font>
</theme>
```

*<name>* is an Openbox theme (see *THEMES*). *<font>* sets the font for a
_place_: *ActiveWindow*, *InactiveWindow*, *MenuHeader*, *MenuItem* or
*ActiveOnScreenDisplay* (the window switcher). *<weight>* is *bold* or
*normal*.

## Desktops

```
<desktops>
  <number>4</number>
  <names><name>web</name><name>code</name></names>
</desktops>
```

The number of virtual desktops (default 4) and, optionally, their names.
Unnamed desktops are called "Desktop _N_". Panels can show and switch
desktops through the ext-workspace protocol.

## Placement and margins

```
<placement><policy>Smart</policy></placement>
<margins><top>0</top><bottom>0</bottom><left>0</left><right>0</right></margins>
```

*<policy>* is where new windows go: *Smart* (avoid other windows, the
default), *Center* or *UnderMouse*. New windows open on the monitor with the
pointer (X11 windows start from the position they ask for); a dialog opens
centred over its parent window. *<margins>* keeps
space free at the screen edges, in pixels, in addition to the space panels
reserve.

## Keyboard

```
<keyboard>
  <xkb>
    <layout>us,de</layout>
    <options>grp:alt_shift_toggle</options>
  </xkb>
  <keybind key="W-Return">
    <action name="Execute"><execute>foot</execute></action>
  </keybind>
</keyboard>
```

*<xkb>* sets the keyboard with *<layout>*, *<variant>*, *<model>*,
*<options>* and *<rules>* (see *xkeyboard-config*(7)); the *XKB_DEFAULT_\**
environment variables work too.

*<keybind>* binds a key to one or more actions, run in order. The *key*
attribute is a key name (an XKB keysym such as *Return*, *F4* or *a*)
preceded by modifiers joined with *-*: *A* (Alt), *C* (Ctrl), *S* (Shift),
*W* (Super), *Caps*, *Mod2*, *Mod3* and *Mod5*. For example *C-A-Left*.

A *<keybind>* containing other *<keybind>* elements is a key chain: press
the outer key, then one of the inner ones. Any other key cancels the chain.

## Mouse

```
<mouse>
  <context name="Root">
    <mousebind button="Right" action="Press">
      <action name="ShowMenu"><menu>root-menu</menu></action>
    </mousebind>
  </context>
</mouse>
```

*<mousebind>* runs actions when a mouse button is pressed. *button* is
*Left*, *Middle* or *Right*, optionally with modifiers as for keys (e.g.
*A-Left*); *action* is *Press*. The *<context>* is *Root* (the empty desktop)
or *Client* (inside a window; the window is focused first). Clicks on panels
go to the panel.

Alt+left-drag moves a window and Alt+right-drag resizes it, without a
binding.

*<libinput>* inside *<mouse>* configures pointing devices:

[[ *Element*
:[ *Values*
|  *<accelProfile>*
:[ *adaptive*, *flat* or *none*
|  *<accelSpeed>*
:[ -1.0 to 1.0
|  *<clickMethod>*
:[ *buttonAreas*, *clickfinger* or *none*
|  *<scrollMethod>*
:[ *twofinger*, *edge*, *button* or *none*
|  *<scrollButton>*
:[ an evdev button name, e.g. *BTN_MIDDLE*
|  *<tapButtonMap>*
:[ *lrm* or *lmr*
|  *<calibrationMatrix>*
:[ six numbers
|  *<tap>*, *<tapDrag>*, *<tapDragLock>*, *<naturalScroll>*, *<leftHanded>*,
   *<middleEmulation>*, *<disableWhileTyping>*, *<disableWhileTrackpointing>*,
   *<scrollButtonLock>*
:[ *enabled* or *disabled*

## Applications

```
<applications>
  <application class="firefox">
    <desktop>2</desktop>
    <maximized>yes</maximized>
  </application>
</applications>
```

Rules for windows, applied when a window opens. The first matching rule
applies. *class* and *name* match the Wayland app ID (for X11 programs, the
WM_CLASS class); *title* matches the window title. Patterns may contain *\**
and *?*; omitted attributes match anything.

[[ *Element*
:[ *Effect*
|  *<position><x>*_N_*</x><y>*_N_*</y></position>*
:[ position, relative to the usable area
|  *<size><width>*_N_*</width><height>*_N_*</height></size>*
:[ size of the window contents
|  *<maximized>*, *<fullscreen>*
:[ open maximized or fullscreen
|  *<iconic>*
:[ open minimized
|  *<focus>*
:[ whether the window gets focus when it opens
|  *<decor>*
:[ server-side decorations on or off
|  *<desktop>*
:[ the desktop to open on (1, 2, ...), or *all*

## Waybox extensions

```
<waybox>
  <menu source="builtin" submenuOpen="hover" wrap="yes" icons="yes"/>
  <switcher osd="yes" wrap="yes"/>
  <edgeTiling enabled="yes" range="10"/>
  <titlebar paddingY="2" buttonSize="0" resizeGrab="8"/>
  <virtualInput allow="unlocked"/>
  <xwayland enabled="yes"/>
</waybox>
```

*<menu>*
	*source* is *builtin* (the native menu, default) or a command to run
	instead, such as *rofi -show drun*. *submenuOpen* is *hover* or
	*click*. *wrap*: keyboard navigation wraps at the ends. *icons*: show
	item icons from the icon theme.

*<switcher>*
	The Alt+Tab switcher lists windows on the current desktop in
	most-recently-used order. *osd*: show the list while the modifier is
	held (otherwise, switch at once). *wrap*: cycling wraps at the ends.

*<edgeTiling>*
	Dragging a window to a monitor edge tiles it: the left or right half,
	a quarter near a corner, or maximized at the top edge. A preview shows
	where it will go. Dragging a tiled window away restores its size.
	*range* is how close to the edge the pointer must be, in pixels. Only
	outer edges count, not the edges between two monitors.

*<titlebar>*
	Server-side decoration sizes in pixels: *paddingY* (above and below the
	title), *buttonSize* (0 picks one from the title height) and
	*resizeGrab* (an invisible border for resizing).

*<virtualInput>*
	Input from virtual keyboards and pointers (*wtype*, *wlrctl*, on-screen
	keyboards, remote-desktop tools): *unlocked* (default) ignores it while
	the screen is locked, *always* accepts it, *never* ignores it.

*<xwayland>*
	Run X11 programs through Xwayland, started when the first one
	connects (default *yes*).

# ACTIONS

Actions appear in key bindings, mouse bindings and menu items as
*<action name="*_Name_*">*, with options as child elements. Actions that
act on a window use the focused one.

*Execute*
	Run *<execute>* (or *<command>*) with *sh -c*.

*ShowMenu*
	Open the menu with the id in *<menu>*, at the pointer.

*NextWindow*, *PreviousWindow*
	Switch windows (Alt+Tab).

*Close*, *Iconify*, *Raise*, *Lower*
	Close, minimize, raise or lower the window.

*ToggleMaximize*, *ToggleMaximizeHorizontal*, *ToggleMaximizeVertical*
	Maximize, or maximize in one direction only.

*Fullscreen*
	Toggle fullscreen.

*Shade*, *Unshade*
	Roll the window up to its titlebar, and back.

*ToggleDecorations*
	Turn server-side decorations on or off.

*GoToDesktop*
	Go to the desktop in *<to>*: a number, *left*/*previous*,
	*right*/*next* or *last*. *<wrap>no</wrap>* stops at the first and last
	desktop.

*SendToDesktop*
	Move the window to a desktop (*<to>* and *<wrap>* as above).
	*<follow>no</follow>* stays on the current desktop.

*ToggleOmnipresent*
	Show the window on all desktops, or only the current one.

*SnapToEdge*
	Tile the window: *<direction>* is *left*, *right*, *up* (maximize),
	*up-left*, *up-right*, *down-left* or *down-right*; *down* restores it.

*Reconfigure*
	Reload the configuration, menu and theme.

*Exit*
	Exit Waybox.

# MENU.XML

```
<openbox_menu xmlns="http://openbox.org/3.4/menu">
  <menu id="root-menu" label="Waybox">
    <item label="Terminal" icon="utilities-terminal">
      <action name="Execute"><execute>foot</execute></action>
    </item>
    <separator/>
    <menu id="apps" label="Applications" execute="obamenu"/>
  </menu>
</openbox_menu>
```

A *<menu>* has an *id* and a *label*. It contains *<item>* elements (each
with a *label*, an optional *icon* and actions), *<separator>* elements (with
an optional *label*), and nested *<menu>* elements, which are submenus. A
*<menu>* with only an *id* refers to a menu defined elsewhere in the file.

A *<menu>* with an *execute* attribute is a pipe menu: when it opens, the
command runs, and its output, an *<openbox_pipe_menu>* document, becomes
the submenu. Commands that take longer than five seconds are stopped.

Icons are icon names from the icon theme, or file paths.

# THEMES

A theme is a directory containing *openbox-3/themerc*. Waybox looks for
_name_ in _$XDG_DATA_HOME/themes_ (normally _~/.local/share/themes_),
_~/.themes_, and the _themes_ directory of each entry in *XDG_DATA_DIRS*.
Openbox themes work as they are.

# SEE ALSO

*waybox*(1), *openbox*(1), *xkeyboard-config*(7), *libinput*(4)
