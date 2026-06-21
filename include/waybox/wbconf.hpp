#ifndef WB_WBCONF_HPP
#define WB_WBCONF_HPP

#include <optional>
#include <string>
#include <vector>

namespace wb {

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
};

/*
 * A loaded rc.xml document. Reads/writes only the nodes wbconf manages, leaving
 * the rest of the user's configuration (keybindings, application rules, …) and
 * its comments intact. Backed by libxml2; movable, non-copyable.
 */
class RcDocument {
public:
	RcDocument(RcDocument &&) noexcept;
	RcDocument &operator=(RcDocument &&) noexcept;
	RcDocument(const RcDocument &) = delete;
	RcDocument &operator=(const RcDocument &) = delete;
	~RcDocument();

	/* Parse rc.xml from a file or an in-memory string. Returns nullopt if the
	 * document cannot be parsed or has no root element. */
	static std::optional<RcDocument> load_file(const std::string &path);
	static std::optional<RcDocument> from_string(const std::string &xml);

	/* The settings currently present in the document. */
	WayboxSettings read() const;

	/* Write the settings into the document: each engaged field is created or
	 * updated, each std::nullopt field is removed (so the default applies). */
	void apply(const WayboxSettings &settings);

	/* Serialise the whole document back to XML / to a file. */
	std::string to_string() const;
	bool save_file(const std::string &path) const;

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
