#ifndef WB_WBCONF_HPP
#define WB_WBCONF_HPP

#include <map>
#include <optional>

#include "waybox/action_catalog.hpp"
#include <string>
#include <vector>

namespace wb {

/* What an <output> rule matches: any of connector name, make, model and
 * serial; "" is "any". A rule with none matches nothing. */
struct OutputMatch {
	std::string name, make, model, serial;
	bool empty() const {
		return name.empty() && make.empty() && model.empty() && serial.empty();
	}
	bool operator==(const OutputMatch &) const = default;
};

/* One <waybox><outputs><output .../> rule: what it matches, and its other
 * attributes as written (mode, scale, position, transform, adaptiveSync,
 * enabled, ...); an absent one means automatic.
 *
 * `from` is the matcher it was read with, so a rule whose matcher changes
 * (connector name to make/model/serial, or back) is rewritten where it is:
 * Waybox uses the first rule that matches a monitor, so order matters.
 * Empty for a new rule, which goes before the rule `before` matches (if
 * there's one), else at the end. */
struct OutputSetting {
	std::string name;
	std::map<std::string, std::string> attrs;
	std::string make{}, model{}, serial{};
	OutputMatch from{};
	OutputMatch before{};
	OutputMatch match() const { return {name, make, model, serial}; }
	bool operator==(const OutputSetting &) const = default;
};

/* Whether `rule` matches the monitor `monitor` (its name, make, model and
 * serial), as Waybox decides: every matcher the rule has must be equal. */
bool output_rule_matches(const OutputMatch &rule, const OutputMatch &monitor);

/* The rule Waybox applies to `monitor`: the first that matches, in file
 * order; -1 if none. */
int applying_output_rule(const std::vector<OutputSetting> &rules, const OutputMatch &monitor);

/* One <waybox><startup><program> entry. `name` is what it's known by (its
 * name attribute, or the command's program); `attrs` its attributes as
 * written (enabled, restart, name). */
struct StartupSetting {
	std::string name;
	std::string command;
	std::map<std::string, std::string> attrs;
	bool operator==(const StartupSetting &) const = default;
};

/* <waybox><inputMethod command="..."><env name= value=/></inputMethod> */
struct InputMethodSetting {
	std::string command;
	std::vector<std::pair<std::string, std::string>> env;
	bool operator==(const InputMethodSetting &) const = default;
};

/*
 * The waybox settings that wbconf (our obconf-style GUI) reads from and writes
 * to rc.xml. Every field is optional: std::nullopt means "absent from rc.xml",
 * so the compositor falls back to its built-in default. Standard Openbox keys
 * (theme/name, placement/policy, margins) live as child-element text; the
 * waybox extensions (titlebar/menu/switcher) live as attributes on child
 * elements of <waybox>.
 */
struct WayboxSettings {
	/* Appearance */
	std::optional<std::string> theme_name;          /* theme/name */
	std::optional<int> titlebar_pad_y;              /* waybox/titlebar@paddingY */
	std::optional<int> titlebar_button_size;        /* waybox/titlebar@buttonSize */
	std::optional<int> titlebar_resize_grab;        /* waybox/titlebar@resizeGrab */
	std::optional<std::string> title_layout;        /* theme/titleLayout */

	/* Windows */
	std::optional<std::string> placement_policy;    /* placement/policy:
	                                                 * Smart|Center|UnderMouse */

	/* Margins (reserved screen-edge space, in px) */
	std::optional<int> margin_top;                  /* margins/top */
	std::optional<int> margin_bottom;               /* margins/bottom */
	std::optional<int> margin_left;                 /* margins/left */
	std::optional<int> margin_right;                /* margins/right */

	/* Menu (waybox extension) */
	std::optional<std::string> menu_source;         /* builtin | <command> */
	std::optional<std::string> menu_submenu_open;   /* hover | click */
	std::optional<int> menu_hover_delay;            /* ms */
	std::optional<bool> menu_wrap;
	std::optional<bool> menu_icons;

	/* Task switcher (waybox extension) */
	std::optional<std::string> switcher_order;      /* mru | stacking | spatial */
	std::optional<bool> switcher_osd;
	std::optional<bool> switcher_wrap;
	std::optional<bool> switcher_icons;             /* waybox/switcher@icons */

	/* Per-place fonts (theme/font place="..."). Each is "Family Size" in the
	 * Pango convention (e.g. "Sans Bold 11"); std::nullopt means unset. */
	std::optional<std::string> font_active_window;   /* place="ActiveWindow" */
	std::optional<std::string> font_inactive_window; /* place="InactiveWindow" */
	std::optional<std::string> font_menu_header;     /* place="MenuHeader" */
	std::optional<std::string> font_menu_item;       /* place="MenuItem" */
	std::optional<std::string> font_osd;             /* place="ActiveOnScreenDisplay"
	                                                  * (or Inactive.../OnScreenDisplay) */

	/* Desktops */
	std::optional<int> desktops_number;              /* desktops/number */
	std::optional<std::vector<std::string>> desktop_names;  /* desktops/names/name */
	std::optional<int> desktop_popup_time;           /* desktops/popupTime (ms) */

	/* Edge tiling (waybox extension) */
	std::optional<bool> edge_tiling;                 /* waybox/edgeTiling@enabled */
	std::optional<int> edge_tiling_range;            /* waybox/edgeTiling@range */

	/* Monitors: the <waybox><outputs><output> rules that can apply, in file
	 * order (a rule with no matcher, or the same matcher as an earlier one,
	 * never does: left out, and left as it is) */
	std::vector<OutputSetting> outputs;
	std::optional<std::string> hdr_outputs;          /* waybox/hdr@outputs */
	std::optional<std::string> cursor_theme;         /* waybox/cursor@theme */
	std::optional<std::string> cursor_size;          /* waybox/cursor@size */

	/* Keyboard: <keyboard><xkb><layout|variant|model|options> */
	std::map<std::string, std::string> xkb;
	/* Mouse and touchpad: <mouse><libinput><accelProfile|...>, as written */
	std::map<std::string, std::string> libinput;

	/* Integrations: programs started with the session, and screen-off */
	std::vector<StartupSetting> startup;
	std::optional<int> idle_screen_off;              /* waybox/idle@screenOff (s) */
	std::optional<InputMethodSetting> input_method;  /* waybox/inputMethod */

	/* Advanced */
	std::optional<bool> xwayland;                    /* waybox/xwayland@enabled */
	std::optional<std::string> virtual_input;        /* waybox/virtualInput@allow */

	bool operator==(const WayboxSettings &) const = default;
};

/* Where one setting lives in rc.xml: an element's text (`path`), one of its
 * attributes (`attr`), a <theme><font place="..."> (`font`: its value is
 * "Family [Bold] Size"), or a list of child elements (`item`: the value
 * has one per line). Paths are element names from the root, as
 * "waybox/switcher". */
struct RcLocation {
	std::string path;
	std::string attr;
	std::string font;
	std::string item;
	bool operator==(const RcLocation &) const = default;
};

/* Why rc.xml couldn't be loaded: libxml2's message and the line it's on. */
struct LoadError {
	std::string message;
	int line = 0;
};

/*
 * A loaded rc.xml document. Reads/writes only the nodes wbconf manages, leaving
 * the rest of the user's configuration (keybindings, application rules, …) and
 * its comments intact. Backed by libxml2; movable, non-copyable.
 */
/* A key binding: its chain of keys ("C-a", then "x"; one key for most),
 * and what it does, in order. */
struct KeyBinding {
	std::vector<std::string> chain;
	std::vector<BoundAction> actions;
	bool operator==(const KeyBinding &) const = default;
};

/* A mouse binding: where (<context name>), which button (with modifiers:
 * "A-Left"), on what (Press, Click, DoubleClick, Release, Drag), and what
 * it does. */
struct MouseBinding {
	std::string context, button, event;
	std::vector<BoundAction> actions;
	bool operator==(const MouseBinding &) const = default;
};

class RcDocument {
public:
	RcDocument(RcDocument &&) noexcept;
	RcDocument &operator=(RcDocument &&) noexcept;
	RcDocument(const RcDocument &) = delete;
	RcDocument &operator=(const RcDocument &) = delete;
	~RcDocument();

	/* Parse rc.xml from a file or an in-memory string. Strict: a document
	 * with any XML error is refused (nullopt, with the error in `error`),
	 * never repaired, since saving a repaired document would silently drop
	 * whatever the parser skipped. */
	static std::optional<RcDocument> load_file(const std::string &path,
			LoadError *error = nullptr);
	static std::optional<RcDocument> from_string(const std::string &xml,
			LoadError *error = nullptr);

	/* The settings currently present in the document. */
	WayboxSettings read() const;

	/* Write the settings into the document: each engaged field is created or
	 * updated, each std::nullopt field is removed (so the default applies). */
	void apply(const WayboxSettings &settings);
	/* The same, but only for the fields that differ from `unchanged` (what
	 * the editor started from): settings the user didn't touch are left as
	 * the file has them, absent ones stay absent. */
	void apply(const WayboxSettings &settings, const WayboxSettings &unchanged);

	/* One setting, by where it lives: its value as written, or nullopt when
	 * absent. set() with nullopt removes it (and an element left empty). */
	std::optional<std::string> get(const RcLocation &at) const;
	void set(const RcLocation &at, const std::optional<std::string> &value);

	/* <keyboard><keybind key="K"> bindings whose one action is Execute, as
	 * (key, command), in file order. */
	std::vector<std::pair<std::string, std::string>> execute_keybinds() const;
	/* Every key with a <keybind>, whatever it does. */
	std::vector<std::string> keybind_keys() const;

	/* Every key binding (chains flattened) and mouse binding, in file
	 * order. Actions the editor doesn't model (If, ForEach, nested
	 * elements) come back as `advanced`, their XML left as it is. */
	std::vector<KeyBinding> keybindings() const;
	std::vector<MouseBinding> mousebindings() const;
	/* Edited one at a time, in place: `was` (the chain, or the context,
	 * button and event, as read) replaced by `now`, or added when `was` is
	 * empty; removed with nullopt. One with advanced actions is refused
	 * (false): only rc.xml edits those. A chain's keys left empty go too. */
	bool set_keybinding(const std::vector<std::string> &was, const std::optional<KeyBinding> &now);
	bool set_mousebinding(const std::optional<MouseBinding> &was,
			const std::optional<MouseBinding> &now);
	/* Bind `key` to run `command` (an Execute keybind, replacing what the
	 * key had), or with nullopt remove `key`'s binding if it's an Execute
	 * one. Other bindings and their comments stay as they are. */
	void set_keybind(const std::string &key, const std::optional<std::string> &command);

	/* Serialise the whole document back to XML / to a file. */
	std::string to_string() const;
	/* The element at `path` (from the root: {"waybox", "outputs"}) as
	 * written, or "" if there's none; and that element put back as it was
	 * (or removed, for ""), where it is or at the end of its parent. For
	 * going back to earlier monitor settings exactly. */
	std::string snapshot(const std::vector<const char *> &path) const;
	void restore(const std::vector<const char *> &path, const std::string &xml);
	/* Save safely: the previous file is kept as PATH.bak, and the new one is
	 * written to a temporary file and renamed over the old (so a crash or a
	 * full disk can't leave half a file). A symlinked rc.xml (dotfile
	 * managers) is followed: its target is replaced, the link kept. */
	bool save_file(const std::string &path, std::string *error = nullptr) const;

private:
	explicit RcDocument(void *doc) : doc_(doc) {}
	void *doc_ = nullptr;  /* xmlDocPtr, owned */
};

/*
 * The names of installed Openbox/waybox themes — every directory under the
 * theme search paths that contains an openbox-3/themerc. Sorted, de-duplicated.
 * Reads the real environment ($XDG_DATA_HOME, $HOME, $XDG_DATA_DIRS).
 */
std::vector<std::string> installed_theme_names();

}  // namespace wb

#endif /* WB_WBCONF_HPP */
