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

*Reload=*
	How the program takes its settings again once its page saved them
	(the page's *wb.reload()*): *command* _program_ _args_ (run as
	written, *command makoctl reload*); *signal USR1*, *USR2* or *HUP*,
	sent to the program (_Binary_) only where it's the user's and runs in
	this Wayland session, never to Waybox (*signal USR2* for Waybar);
	*none*, when it notices by itself. Without it, wbconf says to restart
	the program to see its settings. The approval card shows it.

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
	*hint*, *unit*; *min*, *max* for a *number*; *choices* for a *choice*,
	each a value or *{* _value_*,* _label_ *}* (*{ "top-right", "Top right" }*);
	*wide = true* for a *text* to take the row's width; *on_change* (a
	function of the new value). A value reaches the page as text; a
	switch's is *"on"* or *""*.

*list {* _"key"_, ... *}*
	An ordered list of names, each moved up or down or removed, and more
	added: typed, or picked from *choices* (names, or *{* _name_*,*
	_description_ *}*), offered while not in the list. Its *value* is a
	table of names, and so is what *on_change*, *on_save* and *wb.get*
	give.

*font {* _"key"_, *format =* _"pango"_ | _"fontconfig"_, ... *}*
	A font: its family (or the program's default, with *hint* saying
	which) and size, then a button saying its style that opens its
	weight, width and italic with a larger preview; and a line of text in
	it. Its
	value is written as the program takes it: as Pango writes it
	(*"Inter Bold 11"*, the default format; mako) or as fontconfig names
	(*"Iosevka,Noto Color Emoji:size=12:weight=bold"*; foot, fuzzel).
	What it doesn't edit is kept: fallback families and other properties
	(*antialias=true*, *fontfeatures=...*); a Pango font's variations.
	Letter spacing and line height are the program's own settings: a
	*number* each. Its family is picked from a searchable list, each name
	in its own font; *monospace = true* has it start with monospace
	families only (a terminal's font), a toggle showing the rest.

*sections {* _"key"_, *fields = {* ... *}*, ... *}*
	Repeated sections (mako's criteria, *[urgency=high]*): each named,
	moved, removed or opened to change its name and its *fields* (made
	with *text*, *number*, *choice*, *color*...: the section's, not the
	page's settings, so a key may be both); a field left empty is left
	out. More are added as a list's are; a suggestion ending with *=*
	(*app-name=*) is a start, the name finished in the section. Its
	*value* is a table of *{ name =* _name_*, values = {* _field_ *=*
	_text_*, ... } }*, as *on_change*, *on_save* and *wb.get* give it.

*group* _"Title"_ *{* ... *}*, *label* _"text"_
	A titled group of controls; a line of text.

*button {* _"Label"_*, on_click =* _function_ *}*
	A button. *key* names it for *wb.set*.

*on_change(*_key_*,* _value_*)*, *on_save(*_values_*)*
	Called when a setting changes, and on Save & Apply with every value.

*wb.read(*_path_*)*
	A file's text, if it's in *Files*; nil if it can't be read.

*wb.reload()*
	The program takes its settings again, as its *Reload=* says; done
	after the save's writes.

*wb.write(*_path_*,* _text_*)*, *wb.run {* _program_*,* _arg_*, ... }*
	A file to write and a program to run. wbconf does both on Save & Apply
	(a button's runs at once), only for *Files* and *Runs*, and keeps the
	file as it was as _path_*.bak*.

*wb.get(*_key_*)*, *wb.set(*_key_*, {* value, visible, enabled, problem, label *})*, *wb.problem(*_key_*,* _text_*)*
	A setting's value; changing a control; a problem shown under it (*""*
	clears it).

*wb.log(*...*)*, *print(*...*)*
	To wbconf's log.

*wb.ini.read(*_path_*)*, *wb.ini.parse(*_text_*)*
	An INI file to edit in place: what isn't changed stays as it was,
	comments, blank lines, order and spacing included; a new key goes at
	the end of its section, a new section at the end. Keys before any
	*[section]* are the file's own: *doc:get(*_key_*,* _default_*)*,
	*doc:set(*_key_*,* _value_*)* (nil removes it), *doc:set_all(*_table_*)*,
	*doc:entries()*; a section's through *doc:section(*_name_*)*, with the
	same calls. *doc:sections()* lists them, *doc:text()* is the file to
	write. A value is everything after the first *=*, a *#* in it included
	(colors); a section that repeats (mako's criteria) reads as one, a key
	changed where it last appears. *doc:remove_section(*_name_*)* takes a
	section out (each time it appears, with the comments above it);
	*doc:order({* _names_ *})* puts sections in that order.

*wb.json.read(*_path_*)*, *wb.json.parse(*_text_*)*
	A JSON file to edit in place, comments (*//* and */\* \*/*) and
	trailing commas allowed, as in Waybar's config: what isn't changed
	stays as it was; a changed value is replaced where it is; a new key
	goes after the last of its object, in that object's indentation and
	style (a trailing comma kept); a removed one takes its line. A value is
	reached by a path: a key, an index (from 1) or a table of them, as
	*{ 1, "clock", "format" }* for the first bar's clock format.
	*doc:get(*_path_*,* _default_*)* gives strings, numbers, booleans and
	tables (*wb.json.null* for null); *doc:set(*_path_*,* _value_*)* (nil
	removes it) makes the objects on the way that aren't there;
	*doc:set_all(*_table_*,* _path_*)* sets an object's keys one by one,
	keeping the rest; *doc:keys(*_path_*)* lists an object's keys in order;
	*doc:text()* is the file to write. A table with keys 1..n is written as
	an array, any other as an object; *wb.json.array {}* is an empty array.
	A file that isn't JSON is an error saying where.

*wb.toml.read(*_path_*)*, *wb.toml.parse(*_text_*)*
	A TOML file to edit in place, as Alacritty's and wpaperd's, with the
	same calls as *wb.json*. A path is the value's full one however the
	file spells it: *[font]* *size = 11*, *font.size = 11* and *font = {
	size = 11 }* are all *{ "font", "size" }*; an array of tables'
	element by its index, as *{ "keyboard", "bindings", 1, "key" }*. A
	changed value is replaced where it is (a float stays one, 12 written
	as 12.0); a new key goes after the last of its table, in its
	indentation, or into its inline table; a new table goes at the end
	under its own *[header]*, unless it's already made of dotted keys
	where it is. Dates and times read as their text. TOML has no null,
	and a page doesn't add elements to an array of tables.

A page runs only once allowed unless no one but the system could have
changed it: its folder, its integration file and its script all unwritable
by the user, as a package's files under _/usr_ are. Any other page (in
_$XDG_CONFIG_HOME/waybox/integrations_, where files from anywhere land, or
in a Waybox installed into the home folder) is asked about first: wbconf
shows where it came from, what it may read, change and run, and its
script. An approval is kept in _$XDG_CONFIG_HOME/waybox/approved-pages_ for
the page as it is: changing its integration file or script asks again, and
deleting its line takes the approval back.

What it may touch is shown as routine only when it's plainly the
program's own: the program itself or a companion named after it, its name
followed by _ctl_, _msg_, _client_ or _cli_ (*makoctl* for *mako*), and files in its own folder in _~/.config_,
_~/.local/share_, _~/.local/state_ or _~/.cache_, or _~/.mako_, and its
system-wide default in _/etc/xdg/mako_. Anything else is "another program" or "not its own
file": a program runs with the user's access, and many can start others.
Some are known to be full access to the account, and said so: shells and
interpreters, programs that run other commands (*env*, *sudo*, *find*,
terminals, launchers, D-Bus tools, tools typing keys), and files that run
something later (a shell's startup files, Waybox's own files, autostart,
_~/.local/bin_). Allowing a page lets it run its programs as the user.
*waybox-check-integration* warns about full access in *Files* and *Runs*.

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

Editors complete and check a page with the Lua Language Server's hints
Waybox installs to _/usr/share/waybox/lua_ (_waybox-page.lua_): every
function and control a page is given, with its arguments. In the page's
folder, a _.luarc.json_ naming that folder:

```
{ "workspace.library": ["/usr/share/waybox/lua"] }
```

Check a file before shipping it:

	*waybox-check-integration* [*--strict*] [*--runner* _PATH_] _FILE.integration_...

It reads each file as wbconf does and prints its problems as
_FILE:LINE: error|warning: message_. Errors (no *Name* or *Exec*, a newer
major *FormatVersion*) make a file unusable; warnings say what's ignored or
guessed (an unknown key, an option without a *Flag*). It exits 1 on an error,
or on a warning with *--strict*, and 2 if a file can't be read. It needs
neither a display nor GTK, so it can run in a package's or repository's CI.

A file's Lua page is run too, as wbconf runs it, through *wbconf-lua*
(*--runner*, else _$WBCONF_LUA_, else where Waybox installs it): with an
empty home folder, as for someone whose program has no config file yet,
then saved with its own values, nothing written. Its errors are said as
_NAME.lua:LINE: error: message_, and make the file fail; a page that ran
and saved says _ok_ with how many controls it has. Without *wbconf-lua*
(Waybox built without Lua) the page isn't run, and a note says so.

# SEE ALSO

*waybox*(1), *waybox*(5), *waybox-lua*(5)
