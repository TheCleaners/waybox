waybox(1)

# NAME

waybox - an Openbox-style Wayland compositor

# SYNOPSIS

*waybox* [_options_...]

# DESCRIPTION

Waybox is a Wayland compositor modelled on Openbox. It reads Openbox's
configuration formats (*rc.xml*, *menu.xml* and themes) and leaves panels,
wallpapers, launchers and other desktop functions to separate programs.

The *waybox* command is the session script: it loads the session
environment, picks the configuration files, and starts the compositor with
the autostart scripts as its startup command (see *SESSION*). Start it from
a virtual terminal, or choose Waybox in a display manager.

# OPTIONS

*-h*, *--help*
	Show a summary of the options and exit.

*-V*, *--version*
	Show the version and exit.

*-v*, *--debug*
	Log informational messages to standard error, not only errors.

*-s*, *--startup* _command_
	Run _command_ with _sh -c_ once the compositor is running. The session
	script uses this to run the autostart scripts.

*--config-file* _file_
	Read _file_ instead of the usual *rc.xml*.

*--sm-disable*
	Accepted for compatibility with Openbox; has no effect.

# SESSION

The session script does the following, in order:

. Sources the first *environment* file found, so the variables it exports
  reach Waybox and every program it starts.
. Picks the first *rc.xml* and *menu.xml* found and exports their paths as
  *WB_RC_XML* and *WB_MENU_XML*.
. Starts a D-Bus session bus with *dbus-launch* if none is running.
. Starts the compositor, which runs the first *autostart* script found and
  then the XDG autostart entries.

Each file is looked up in _$XDG_CONFIG_HOME/waybox/_ (normally
_~/.config/waybox/_), then in the system configuration directory (usually
_/etc/xdg/waybox/_). If neither has one, Openbox's files are used, with a
warning.

The default *autostart* starts a dock, a panel, a notification daemon, a
wallpaper and a polkit authentication agent, and passes *WAYLAND_DISPLAY*, *DISPLAY*,
*XDG_CURRENT_DESKTOP* and *XDG_SESSION_TYPE* to the D-Bus activation
environment. XDG autostart entries are started for the *Waybox* desktop
only; entries limited to other desktops (_OnlyShowIn_) are skipped.

# SIGNALS

*SIGINT*, *SIGTERM*
	Exit.

*SIGUSR1*, *SIGUSR2*
	Reload *rc.xml*, *menu.xml* and the theme, and restyle open windows.
	The *Reconfigure* action does the same.

# ENVIRONMENT

*WB_RC_XML*, *WB_MENU_XML*
	The configuration and menu files to read. *--config-file* takes
	precedence over *WB_RC_XML*.

*WB_ICON_THEME*
	The icon theme for menu icons. By default, the GTK icon theme setting
	is used, then _hicolor_.

*XCURSOR_THEME*, *XCURSOR_SIZE*
	The cursor theme and size.

*XDG_CONFIG_HOME*, *XDG_DATA_HOME*, *XDG_DATA_DIRS*
	Where configuration files, themes and icons are looked up.

*WLR_DRM_DEVICES*
	Which GPUs to use, colon-separated (e.g. _/dev/dri/card0:/dev/dri/card1_).
	The desktop is drawn on the first. Waybox is built on wlroots, and
	wlroots' other variables (such as *WLR_RENDERER*) apply as well.

Waybox sets *WAYLAND_DISPLAY* for the programs it starts, and *DISPLAY* when
Xwayland is enabled (see *waybox*(5)).

# FILES

_~/.config/waybox/rc.xml_
	Configuration; see *waybox*(5).

_~/.config/waybox/menu.xml_
	The root menu; see *waybox*(5).

_~/.config/waybox/environment_
	A shell script sourced at session start.

_~/.config/waybox/autostart_
	A shell script run when the session starts (it must be executable).

_/etc/xdg/waybox/_
	The system defaults for all of the above (under the installation's
	configuration directory).

# SEE ALSO

*waybox*(5), *openbox*(1), *swaylock*(1), *kanshi*(1)
