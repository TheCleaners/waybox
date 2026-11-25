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

A file with an XML error (a missing quote, an unescaped *&*) is not read at
all. Waybox reports the file, line and error in its log and, when
*notify-send* is installed, as a notification. On *Reconfigure* it keeps the
current settings; at startup it uses the default configuration until the
file is fixed. An empty *rc.xml* simply has no settings.

Boolean values are written *yes* or *no* (*true*/*false* and *1*/*0* work
too).

# RC.XML

The root element is *<openbox_config xmlns="http://openbox.org/3.4/rc">*.

## Theme

```
<theme>
  <name>Onyx-Citrus</name>
  <titleLayout>NLIMC</titleLayout>
  <font place="ActiveWindow">
    <name>Sans</name><size>10</size><weight>bold</weight>
  </font>
</theme>
```

*<name>* is an Openbox theme (see *THEMES*). *<font>* sets the font for a
_place_: *ActiveWindow*, *InactiveWindow*, *MenuHeader*, *MenuItem* or
*ActiveOnScreenDisplay* (the window switcher). *<weight>* is *bold* or
*normal*.

*<titleLayout>* lays out the titlebar, as in Openbox: *N* the window's icon,
*L* the title, *I* iconify, *M* maximize, *C* close, *S* shade (roll up or
down), *D* on all desktops (or only this one); letters before *L* go to its
left, the rest to its right (default *NLIMC*). The icon is the one
the program sets, else its desktop file's, else its app ID as an icon name.

## Desktops

```
<desktops>
  <number>4</number>
  <names><name>web</name><name>code</name></names>
  <popupTime>875</popupTime>
</desktops>
```

The number of virtual desktops (default 4) and, optionally, their names.
Unnamed desktops are called "Desktop _N_". Panels can show and switch
desktops through the ext-workspace protocol.

*popupTime* is how long, in milliseconds, the desktop's name shows in the
middle of the monitor after switching (default 875, as in Openbox); *0*
turns it off.

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
environment variables work too. *Reconfigure* applies a changed layout to
the connected keyboards.

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
  <context name="Titlebar">
    <mousebind button="Left" action="DoubleClick">
      <action name="ToggleMaximize"/>
    </mousebind>
  </context>
</mouse>
```

*<mousebind>* runs actions on a mouse event, in a *<context>*: where the
pointer is.

*button* is *Left*, *Middle*, *Right*, or *Up* and *Down* for the scroll
wheel, optionally with modifiers as for keys (e.g. *A-Left*). Caps Lock and
Num Lock don't matter.

*action* is the event:

[[ *Event*
:[ *When*
|  *Press*
:[ the button is pressed
|  *Release*
:[ the button is released
|  *Click*
:[ released without dragging
|  *DoubleClick*
:[ a second click within half a second, on the same window
|  *Drag*
:[ moved 8 pixels with the button held

A scroll-wheel notch is a *Press* and a *Click* of *Up* or *Down*. Touchpad
scrolling is not.

The contexts are:
- *Root*: the empty desktop.
- *Desktop*: the empty desktop, and also desktop surfaces, which are the
  background and bottom layers. A desktop-icon program (pcmanfm-qt's desktop,
  xfdesktop) draws there, as do wallpaper programs that take input.
- *Client*: inside a window.
- *Titlebar*.
- *Icon*: the window icon in the titlebar.
- *Top*, *Bottom*, *Left* and *Right*: the borders of a window with a
  server-side frame.
- *Frame*: anywhere on a window.

*Desktop* bindings on a desktop program behave as in Openbox:
- **They run, and the program still gets the click**, as a window does for
  *Client* bindings. A configuration from Openbox, with *Focus*, *Raise* or
  desktop switching on the wheel in *Desktop*, leaves desktop icons working.
- **A binding that opens a menu (*ShowMenu*) is the exception:** that click
  goes to the menu only, so the program doesn't open its own under it. To
  keep the root menu on right-click with a desktop-icon program running,
  bind *ShowMenu* in *Desktop*.

A *Root* binding leaves desktop programs alone.

For a window context, the window is focused before the actions run. Clicks on
panels go to the panel, and the titlebar buttons work on their own; without an
*Icon* binding, clicking the icon opens the window menu.

Without any binding, dragging a titlebar or border moves or resizes the
window, and so do Alt+left-drag and Alt+right-drag anywhere on a window. A
*Press* binding takes the press over from these; other events add to them.

*<libinput>* inside *<mouse>* configures pointing devices (also on
*Reconfigure*; a setting left out goes back to the device's default):

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
WM_CLASS class); *title* matches the window title; *tag* matches the stable
name a program gives a window (xdg-toplevel-tag, such as "preferences" or
"player"), and never matches a window without one. Patterns may contain
*\** and *?*; omitted attributes match anything.

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
  <menu source="builtin" submenuOpen="hover" hoverDelay="100" wrap="yes" icons="yes"/>
  <switcher order="mru" osd="yes" wrap="yes" icons="yes"/>
  <edgeTiling enabled="yes" range="10"/>
  <sessionRestore enabled="yes"/>
  <titlebar paddingY="2" buttonSize="0" resizeGrab="8"/>
  <virtualInput allow="unlocked"/>
  <xwayland enabled="yes"/>
  <bell flash="yes" command="paplay /usr/share/sounds/freedesktop/stereo/bell.oga"/>
  <hdr outputs="HDMI-A-1"/>
  <outputs>
    <output name="HDMI-A-1" mode="3840x2160@60" scale="1.5" position="0,0"/>
    <output make="Dell Inc." serial="ABC123" position="2560,0"/>
  </outputs>
</waybox>
```

*<menu>*
	*source* is *builtin* (the native menu, default) or a command to run
	instead, such as *rofi -show drun*. *submenuOpen* is *hover* or
	*click*. *hoverDelay*: with *hover*, how long the pointer rests on a
	submenu's entry before it opens, in milliseconds (default 100; *0*
	opens it at once). *wrap*: keyboard navigation wraps at the ends.
	*icons*: show item icons from the icon theme.

*<switcher>*
	The Alt+Tab switcher lists windows on the current desktop. *order*:
	*mru* (most recently used first, the default), *stacking* (top to
	bottom), or *spatial* (monitor by monitor, then by position, left to
	right and top to bottom); Alt+Tab goes to the next window in that order.
	*osd*: show the list while the modifier is held (otherwise, switch at
	once). *wrap*: cycling wraps at the ends. *icons*: each window's icon
	beside its title (default *yes*).

*<edgeTiling>*
	Dragging a window to a monitor edge tiles it: the left or right half,
	a quarter near a corner, or maximized at the top edge. A preview shows
	where it will go. Dragging a tiled window away restores its size.
	*range* is how close to the edge the pointer must be, in pixels. Only
	outer edges count, not the edges between two monitors.

*<sessionRestore>*
	Programs that ask (xdg-session-management) get their windows back at
	the size and place they had, on the same monitor if it's there, and
	maximized or fullscreen if they were. Recovering from a crash or
	restoring a session also brings back each window's desktop; a program
	simply started again opens on the desktop shown. A rule in
	*<applications>* still wins. The state is kept in
	_$XDG_STATE_HOME/waybox/sessions.json_ (normally
	_~/.local/state/waybox_); sessions unused for 90 days are forgotten.
	*enabled="no"*: nothing is kept or restored.
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

*<bell>*
	What a program ringing the bell (a terminal's \\a, a toolkit's error
	beep) does; nothing unless set. *flash*: briefly flash the window that
	rang (or the monitor under the pointer). *command*: run it (through
	*sh*), at most once per quick burst of rings.

*<hdr>*
	*outputs* lists outputs (by name, separated by spaces) to drive in
	HDR10: BT.2020 primaries and the PQ transfer function, 10 bits per
	channel. It needs the Vulkan renderer (the default on real displays;
	see *WLR_RENDERER* in *waybox*(1)) and an output that supports HDR10; otherwise, or if the output refuses it,
	the output stays in SDR and the log says why. Colour-managed programs
	(see *waybox*(1)) then show HDR content on it.

*<outputs>*
	Monitor settings, one *<output>* per monitor. An *<output>* matches
	by *name* (the connector, as *wlr-randr* shows it), or by *make*,
	*model* and *serial*: every one given must match. The first matching
	*<output>* applies. Its settings, all optional:

	*mode* _WIDTHxHEIGHT_[*@*_HZ_], e.g. *3840x2160@60*. Without a rate,
	the highest at that size; otherwise the closest.

	*scale* (e.g. *1.5*), *position* _X_*,*_Y_ (in the layout, after
	scaling), *transform* (*normal*, *90*, *180*, *270*, *flipped*,
	*flipped-90*, *flipped-180*, *flipped-270*), *adaptiveSync*
	(*yes*|*no*), *enabled* (*yes*|*no*).

	Without an *<output>*, a monitor gets its preferred resolution at the
	highest refresh rate it offers there, a scale for its pixel density,
	and a place to the right of the others. A mode the monitor refuses
	falls back to its preferred mode.

	The settings apply when a monitor is connected and on *Reconfigure*,
	except that a monitor an output-management program (kanshi,
	wlr-randr) has changed keeps those changes for the session.

# ACTIONS

Actions appear in key bindings, mouse bindings and menu items as
*<action name="*_Name_*">*, with options as child elements. Actions that
act on a window use the focused one.

*Execute*
	Run *<execute>* (or *<command>*) with *sh -c*.

*ShowMenu*
	Open the menu with the id in *<menu>*, at the pointer. *client-menu* is
	the window menu, built in: send to desktop, iconify, maximize, move,
	resize, roll up, decorate and close, for the window it was opened on.

*NextWindow*, *PreviousWindow*
	Switch windows (Alt+Tab).

*Close*, *Iconify*, *Raise*, *Lower*
	Close, minimize, raise or lower the window.

*Focus*
	Give the window keyboard focus: it's raised, brought back if minimized,
	and its desktop is shown if it's on another.

*ToggleMaximize*, *ToggleMaximizeHorizontal*, *ToggleMaximizeVertical*
	Maximize, or maximize in one direction only.

*Fullscreen*
	Toggle fullscreen.

*Shade*, *Unshade*
	Roll the window up to its titlebar, hiding its contents, and back down.
	Only windows with a server-side titlebar can be rolled up.

*Move*, *Resize*
	Start moving or resizing the window with the pointer (resizing from
	the nearest corner), as dragging its titlebar or border does. For
	*Drag* mouse bindings.

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
    <menu id="applications" label="Applications" type="applications"/>
    <menu id="places" label="Places" execute="~/.config/waybox/places.sh"/>
  </menu>
</openbox_menu>
```

A *<menu>* has an *id* and a *label*. It contains *<item>* elements (each
with a *label*, an optional *icon* and actions), *<separator>* elements (with
an optional *label*), and nested *<menu>* elements, which are submenus. A
*<menu>* with only an *id* refers to a menu defined elsewhere in the file.

A *<menu>* with *type="applications"* lists the installed programs, by
category, from their _.desktop_ files in _$XDG_DATA_HOME/applications_ and
the _applications_ directory of each entry in *XDG_DATA_DIRS*. Programs
marked *NoDisplay* or *Hidden*, meant only for other desktops (*OnlyShowIn*,
*NotShowIn*, against *XDG_CURRENT_DESKTOP*) or whose *TryExec* isn't
installed are left out. Programs that run in a terminal start in
*$TERMINAL* (or *xdg-terminal-exec*). The menu is read again when a
program is installed or removed.

A *<menu>* with an *execute* attribute is a pipe menu: when it opens, the
command runs, and its output, an *<openbox_pipe_menu>* document, becomes
the submenu. Commands that take longer than five seconds are stopped.
Commands run through _/bin/sh_.

Icons are icon names from the icon theme, or file paths.

# THEMES

A theme is a directory containing *openbox-3/themerc*. Waybox looks for
_name_ in _$XDG_DATA_HOME/themes_ (normally _~/.local/share/themes_),
_~/.themes_, and the _themes_ directory of each entry in *XDG_DATA_DIRS*.
Openbox themes work as they are.

# SEE ALSO

*waybox*(1), *openbox*(1), *xkeyboard-config*(7), *libinput*(4)
