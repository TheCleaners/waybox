waybox-integration(5)

# NAME

waybox-integration - companion programs described for wbconf

# SYNOPSIS

_$XDG_CONFIG_HOME/waybox/integrations/ID.integration_++
_$XDG_DATA_DIRS/waybox/integrations/ID.integration_

# DESCRIPTION

An integration file describes a program that goes with Waybox (a panel, a
notification daemon, a wallpaper setter, a night light, an emoji picker, an
input method) so that *wbconf* can offer it: start it with the session, bind
it to a key, show its options and its own settings tool. A program's package
can ship its own file, and so can anyone else.

Files are plain text: *[Section]* headers, *Key=value* lines, and comments
starting with *#*. Unknown sections and keys are ignored, so a file can carry
keys a later version of wbconf will read.

Integration files are data: wbconf shows the command it would run or save
before doing so, and a file can only suggest one.

# EXAMPLE

```
[Integration]
Name=swaybg
Category=Wallpaper
Comment=Wallpaper image or solid color
Exec=swaybg -c '#303030'
Page=wallpaper
Version=swaybg -v

[Option image]
Label=Image
Flag=-i
Hint=~/Pictures/wallpaper.png
Type=image

[Option mode]
Label=Mode
Flag=-m
Type=choice
Choices=stretch,fill,fit,center,tile,solid_color
Default=stretch
```

# [Integration]

*Name=*
	The program's name, as shown. Required.

*Exec=*
	The command, as it is started with the session (or run from the key).
	Words are split as a shell would (spaces, *'...'*, *"..."*,
	backslashes); it isn't run through one. Required.

*FormatVersion=*
	The version of this format the file is written for: _MAJOR_ or
	_MAJOR.MINOR_, *1* if left out. A file whose major version is newer
	than wbconf reads is skipped, not misread; a newer minor version is
	read, its new keys ignored.

*Category=*
	Where wbconf lists it: *Panel*, *Notifications*, *Wallpaper*,
	*NightLight*, *Idle*, *Clipboard*, *Pickers*, *Tray*, *Polkit*,
	*Displays*, in that order, then any other name alphabetically;
	*Other* if left out.

*Comment=*
	A line saying what it does.

*Binary=*
	The program file wbconf looks for to tell whether it's installed, and
	matches startup commands by. The first word of *Exec* if left out.

*Version=*
	A command printing the program's version (*swaybg -v*), shown beside
	it. Left out, none is shown: a program that doesn't know the flag
	might just start.

*Configure=*
	The program's own settings tool (*azote*, *blueman-manager*), which
	wbconf offers to open.

*Page=*
	A page wbconf has for this kind of program, shown with its options.
	Known: *wallpaper* (thumbnails, a preview). Or _NAME_*.lua*: a page
	written in Lua, beside the file (see *LUA PAGES*).

*Files=*
	For a Lua page: the files it may read and write, separated by commas;
	_~/_ is the home folder.

*Runs=*
	For a Lua page: the programs it may run, by name (found in *PATH*),
	separated by commas.

*Manages=outputs*
	It sets monitors itself (*kanshi*): while it runs, its profiles win over
	Waybox's *<outputs>* on a hotplug, and wbconf says so on its Monitors
	page.

*Kind=*
	*startup* (the default): started with the session. *binding*: run from
	a key instead (an emoji or clipboard picker). *inputmethod*: an input
	method (IBus, Fcitx5), chosen under Keyboard; one at a time.

*Key=*
	For *Kind=binding*: the key it suggests, in *rc.xml*'s syntax
	(*W-period* is Super+.).

*Env=*_NAME=VALUE_
	For *Kind=inputmethod*: a variable programs need to use it. One line
	each.

# [Option ID]

An option of the command, shown as a control. wbconf reads its value from
the command, and writes it back, by its flag: an option without *Flag* is
ignored.

*Flag=*
	The flag (*-i*, *--mode*); its value is the word after it.

*Label=*
	As shown; the id if left out.

*Type=*
	The control: *text* (the default), *file*, *folder*, *image*,
	*image-folder*, *image-or-folder*, *color*, *choice* (with *Choices*),
	*number* (with *Min*, *Max*), or *switch*: a flag with no value, there
	or not.

*Choices=*
	For *choice*: the values, separated by commas.

*Min=*, *Max=*
	For *number*: the range.

*Hint=*
	An example value, shown in an empty field.

*Default=*
	What the program does with the option left out (*stretch*), shown as
	such; never written into the command. For a *switch*, *on* or *off*.

# LUA PAGES

A page for what typed options can't do: reading and writing a program's own
config file, checking values, showing or hiding things, a button that tries
something. wbconf draws it with its own controls, so it looks, searches and
saves like any other page.

```
-- mako.lua (Page=mako.lua, Files=~/.config/mako/config, Runs=makoctl)
local text = wb.read("~/.config/mako/config") or ""

page {
  group "Where" {
    choice { "anchor", label = "Corner", value = "top-right",
             choices = { "top-left", "top-right", "bottom-left", "bottom-right" } },
  },
  number { "timeout", label = "Disappear after", unit = "ms", min = 0,
           value = text:match("default%-timeout=(%d+)") or 5000 },
  button { "Send a test", on_click = function()
    wb.run { "makoctl", "reload" }
  end },
}

function on_change(key, value)
  wb.problem("timeout", key == "timeout" and tonumber(value) < 1000
      and "Under a second" or "")
end

function on_save(values)
  wb.write("~/.config/mako/config", "default-timeout=" .. values.timeout .. "\n")
  wb.run { "makoctl", "reload" }
end
```

*page {* ... *}*
	The page: the controls, in order. Given once.

*text*, *number*, *switch*, *choice*, *color*, *file*, *folder*, *image* *{* _"key"_, ... *}*
	A setting: its key first, then *label*, *value*, *description*,
	*hint*; *unit*, *min*, *max* for a *number*; *choices* for a *choice*;
	*on_change* (a function of the new value). A value reaches the page as
	text; a switch's is *"on"* or *""*.

*group* _"Title"_ *{* ... *}*, *label* _"text"_
	A titled group of controls; a line of text.

*button {* _"Label"_*, on_click =* _function_ *}*
	A button. *key* names it for *wb.set*.

*on_change(*_key_*,* _value_*)*, *on_save(*_values_*)*
	Called when a setting changes, and on Save & Apply with every value.

*wb.read(*_path_*)*
	A file's text, if it's in *Files*; nil if it can't be read.

*wb.write(*_path_*,* _text_*)*, *wb.run {* _program_*,* _arg_*, ... }*
	A file to write and a program to run. wbconf does both on Save & Apply
	(a button's runs at once), only for *Files* and *Runs*, and keeps the
	file as it was as _path_*.bak*.

*wb.get(*_key_*)*, *wb.set(*_key_*, {* value, visible, enabled, problem, label *})*, *wb.problem(*_key_*,* _text_*)*
	A setting's value; changing a control; a problem shown under it (*""*
	clears it).

*wb.log(*...*)*, *print(*...*)*
	To wbconf's log.

A page in the user's own folder (_$XDG_CONFIG_HOME/waybox/integrations_,
where files from anywhere land) runs only once allowed: wbconf first shows
where it came from, what it may read, change and run, and its script, and
asks. Pages installed with Waybox or by packages (the data folders) aren't
asked about. An approval is kept in _$XDG_CONFIG_HOME/waybox/approved-pages_
for the page as it is: changing its integration file or script asks again,
and deleting its line takes the approval back.

A Lua page runs in *wbconf-lua*, a process of its own, without *io*, *os*,
*require*, *load* or *debug*: it can only reach what its integration file
declares, and wbconf checks every write and run again. Each call has a
quarter of a second; a page that doesn't answer within 3 seconds is stopped
and shown as unavailable. Globals are strict: assigning or reading one not
declared with *local* is an error. Errors are shown on the page with their
line (_mako.lua:14: 'timout' is not a setting of this page (timeout?)_).

# WHERE FILES ARE READ

In this order, the first file for each _ID_ (the file's name without
*.integration*) winning:

. _$XDG_CONFIG_HOME/waybox/integrations_ (normally
  _~/.config/waybox/integrations_): the user's own, over any other.
. _waybox/integrations_ under each directory of *$XDG_DATA_DIRS*
  (normally _/usr/local/share_ and _/usr/share_): programs' packages.
. Waybox's own, installed with it.

To change a file Waybox or a package ships, copy it into the user's
directory and edit the copy.

# PACKAGING

A program's package installs its file as
_$datadir/waybox/integrations/ID.integration_, _ID_ being the program's
name (letters, digits, *-*, *\_*, *.* and *+*). Where Waybox ships a file
for the same program, the package's is read first; Waybox's stays as a
fallback.

Check a file before shipping it:

	*waybox-check-integration* [*--strict*] _FILE.integration_...

It reads each file as wbconf does and prints its problems as
_FILE:LINE: error|warning: message_. Errors (no *Name* or *Exec*, a newer
major *FormatVersion*) make a file unusable; warnings say what's ignored or
guessed (an unknown key, an option without a *Flag*). It exits 1 on an error,
or on a warning with *--strict*, and 2 if a file can't be read. It needs
neither a display nor GTK, so it can run in a package's or repository's CI.

# SEE ALSO

*waybox*(1), *waybox*(5), *waybox-lua*(5)
