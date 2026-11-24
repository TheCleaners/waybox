#include <filesystem>
#include <fstream>
#include <new>
#include <cstdlib>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <pwd.h>
#include <unistd.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include "config.h"
#include "waybox/action_xml.hpp"
#include "waybox/xml_strict.hpp"

/* mousebind.cpp encodes the modifier bits itself so it can stay wlroots-free;
 * verify those values still agree with wlroots here, where both are visible. */
static_assert(WLR_MODIFIER_SHIFT == (1u << 0), "modifier bit drift");
static_assert(WLR_MODIFIER_CAPS == (1u << 1), "modifier bit drift");
static_assert(WLR_MODIFIER_CTRL == (1u << 2), "modifier bit drift");
static_assert(WLR_MODIFIER_ALT == (1u << 3), "modifier bit drift");
static_assert(WLR_MODIFIER_MOD2 == (1u << 4), "modifier bit drift");
static_assert(WLR_MODIFIER_MOD3 == (1u << 5), "modifier bit drift");
static_assert(WLR_MODIFIER_LOGO == (1u << 6), "modifier bit drift");
static_assert(WLR_MODIFIER_MOD5 == (1u << 7), "modifier bit drift");

namespace fs = std::filesystem;

/* Build the ordered list of candidate config files, most-preferred first:
 * the user's config (honoring $XDG_CONFIG_HOME, else $HOME/.config or the
 * password database) followed by the installed system default. */
static std::vector<fs::path> rc_file_candidates() {
	std::vector<fs::path> candidates;

	if (const char *xdg = getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0') {
		candidates.push_back(fs::path(xdg) / "waybox" / "rc.xml");
	} else {
		const char *home = getenv("HOME");
		if (!home || home[0] == '\0') {
			struct passwd *pw = getpwuid(getuid());
			home = pw ? pw->pw_dir : nullptr;
		}
		if (home && home[0] != '\0')
			candidates.push_back(fs::path(home) / ".config" / "waybox" / "rc.xml");
	}

#ifdef WB_SYSCONFDIR
	candidates.push_back(fs::path(WB_SYSCONFDIR) / "xdg" / "waybox" / "rc.xml");
#endif

	return candidates;
}

/* Resolve the config file to load: the first candidate that exists on disk,
 * or the most-preferred candidate so the caller can report a meaningful path
 * if none exists. Returns an empty path only when no candidate could be
 * constructed at all. */
static fs::path default_rc_file() {
	std::vector<fs::path> candidates = rc_file_candidates();

	std::error_code ec;
	for (const fs::path &path : candidates) {
		if (fs::exists(path, ec))
			return path;
	}

	return candidates.empty() ? fs::path{} : candidates.front();
}

/* Candidate menu.xml paths, mirroring rc.xml: $WB_MENU_XML wins, then the
 * user's config dir, then the installed system default. */
static std::vector<fs::path> menu_file_candidates() {
	std::vector<fs::path> candidates;

	if (const char *env = getenv("WB_MENU_XML"); env && env[0] != '\0')
		candidates.push_back(fs::path(env));

	if (const char *xdg = getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0') {
		candidates.push_back(fs::path(xdg) / "waybox" / "menu.xml");
	} else {
		const char *home = getenv("HOME");
		if (!home || home[0] == '\0') {
			struct passwd *pw = getpwuid(getuid());
			home = pw ? pw->pw_dir : nullptr;
		}
		if (home && home[0] != '\0')
			candidates.push_back(fs::path(home) / ".config" / "waybox" / "menu.xml");
	}

#ifdef WB_SYSCONFDIR
	candidates.push_back(fs::path(WB_SYSCONFDIR) / "xdg" / "waybox" / "menu.xml");
#endif

	return candidates;
}

/* Load and parse the first menu.xml that exists, or an empty MenuFile. */
static wb::MenuFile load_menu_file() {
	std::error_code ec;
	for (const fs::path &path : menu_file_candidates()) {
		if (!fs::exists(path, ec))
			continue;
		std::ifstream in(path, std::ios::binary);
		if (!in)
			continue;
		std::ostringstream buf;
		buf << in.rdbuf();
		wlr_log(WLR_INFO, "Using menu file %s", path.c_str());
		return wb::parse_menu_xml(buf.str());
	}
	return {};
}

static unsigned long strtoulong(char *s) {
	if (!s)
		return 0;
	unsigned long value = strtoul(s, (char **) NULL, 10);
	free(s);
	return value;
}

/* The text of the first node `expr` matches (to be free()d), or NULL. */
static char *parse_xpath_expr(const char *expr, xmlXPathContextPtr ctxt) {
	xmlXPathObjectPtr object = xmlXPathEvalExpression((const xmlChar *) expr, ctxt);
	if (object == NULL) {
		wlr_log(WLR_INFO, "%s: %s", _("Unable to evaluate expression"), expr);
		return NULL;
	}
	xmlChar *ret = NULL;
	if (object->nodesetval != NULL && object->nodesetval->nodeNr > 0)
		ret = xmlNodeGetContent(object->nodesetval->nodeTab[0]);
	xmlXPathFreeObject(object);
	return (char *) ret;
}

/* Whether `expr` matches anything. */
static bool xpath_exists(const char *expr, xmlXPathContextPtr ctxt) {
	xmlXPathObjectPtr object = xmlXPathEvalExpression((const xmlChar *) expr, ctxt);
	bool found = object != NULL && object->nodesetval != NULL &&
			object->nodesetval->nodeNr > 0;
	xmlXPathFreeObject(object);
	return found;
}

static xmlChar *get_attribute(xmlNode *node, const char *attr_name) {
	static xmlChar empty[1] = {};
	xmlAttr *attr = node->properties;
	while (attr && strcmp((char *) attr->name, attr_name) != 0)
		attr = attr->next;
	return (attr && attr->children) ? attr->children->content : empty;
}

/* Parse a "key" attribute like "W-S-M" into a keysym + modifier mask, matching
 * Openbox's syntax. strtok mutates its buffer, so work on a copy (the attribute
 * content belongs to the parsed document). */
static void parse_key_combo(const char *spec, uint32_t *sym, uint32_t *modifiers) {
	*sym = 0;
	*modifiers = 0;
	if (spec == nullptr)
		return;
	char *buf = strdup(spec);
	if (buf == nullptr)
		return;
	char *saveptr = nullptr;
	for (char *s = strtok_r(buf, "-", &saveptr); s != nullptr;
			s = strtok_r(nullptr, "-", &saveptr)) {
		if (strcmp(s, "A") == 0 || strcmp(s, "Alt") == 0)
			*modifiers |= WLR_MODIFIER_ALT;
		else if (strcmp(s, "Caps") == 0)
			*modifiers |= WLR_MODIFIER_CAPS;
		else if (strcmp(s, "C") == 0 || strcmp(s, "Ctrl") == 0)
			*modifiers |= WLR_MODIFIER_CTRL;
		else if (strcmp(s, "Mod2") == 0)
			*modifiers |= WLR_MODIFIER_MOD2;
		else if (strcmp(s, "Mod3") == 0)
			*modifiers |= WLR_MODIFIER_MOD3;
		else if (strcmp(s, "Mod5") == 0)
			*modifiers |= WLR_MODIFIER_MOD5;
		else if (strcmp(s, "S") == 0 || strcmp(s, "Shift") == 0)
			*modifiers |= WLR_MODIFIER_SHIFT;
		else if (strcmp(s, "W") == 0 || strcmp(s, "Logo") == 0)
			*modifiers |= WLR_MODIFIER_LOGO;
		/* The last token is the key itself (intermediate modifier tokens are
		 * overwritten, matching the historical behaviour). */
		*sym = xkb_keysym_from_name(s, XKB_KEYSYM_NO_FLAGS);
	}
	free(buf);
}

/* Recursively parse a <keybind>: its chord, its direct <action> children, and
 * any nested <keybind> children (a key chain). Nested keybinds are NOT walked
 * for actions of their own here -- those belong to the child node. */
static wb::KeyBinding parse_keybind(xmlNode *node) {
	wb::KeyBinding binding;
	parse_key_combo((const char *) get_attribute(node, "key"),
			&binding.sym, &binding.modifiers);

	for (xmlNode *child = node->children; child; child = child->next) {
		if (child->type != XML_ELEMENT_NODE)
			continue;
		if (strcmp((char *) child->name, "keybind") == 0) {
			binding.children.push_back(parse_keybind(child));
		} else if (strcmp((char *) child->name, "action") == 0) {
			if (auto action = wb::parse_action_xml(child))
				binding.actions.push_back(std::move(*action));
		}
	}
	return binding;
}

static bool parse_key_bindings(struct wb_config *config, xmlXPathContextPtr ctxt) {
	/* Only top-level keybinds (direct children of <keyboard>); nested ones are
	 * reached recursively by parse_keybind(). */
	xmlXPathObjectPtr object = xmlXPathEvalExpression(
			(xmlChar *) "//ob:keyboard/ob:keybind", ctxt);
	if (object == NULL) {
		wlr_log(WLR_INFO, "%s", _("Unable to evaluate expression"));
		return false;
	}
	if (!object->nodesetval) {
		wlr_log(WLR_INFO, "%s", _("No nodeset"));
		return false;
	}

	for (int i = 0; i < object->nodesetval->nodeNr; i++) {
		xmlNode *node = object->nodesetval->nodeTab[i];
		if (node != NULL)
			config->key_bindings.push_back(parse_keybind(node));
	}
	xmlXPathFreeObject(object);
	return true;
}

/* Parse the <mouse> section: <context name="..."> blocks each containing
 * <mousebind button="..." action="Press|Release|Click|...">action(s). */
static void parse_mouse_bindings(struct wb_config *config, xmlXPathContextPtr ctxt) {
	xmlXPathObjectPtr object = xmlXPathEvalExpression(
			(const xmlChar *) "//ob:mouse/ob:context", ctxt);
	if (object == NULL)
		return;
	if (object->nodesetval) {
		for (int i = 0; i < object->nodesetval->nodeNr; i++) {
			xmlNode *ctx_node = object->nodesetval->nodeTab[i];
			if (ctx_node == NULL)
				continue;
			std::optional<wb::MouseContext> context = wb::mouse_context_from_name(
					(const char *) get_attribute(ctx_node, "name"));
			if (!context)
				continue;

			for (xmlNode *mb = ctx_node->children; mb; mb = mb->next) {
				if (mb->type != XML_ELEMENT_NODE ||
						strcmp((char *) mb->name, "mousebind") != 0)
					continue;

				std::optional<wb::MouseButtonSpec> button = wb::parse_mouse_button(
						(const char *) get_attribute(mb, "button"));
				std::optional<wb::MouseEvent> event = wb::mouse_event_from_name(
						(const char *) get_attribute(mb, "action"));
				if (!button || !event)
					continue;

				wb::MouseBinding binding;
				binding.context = *context;
				binding.event = *event;
				binding.button = button->button;
				binding.modifiers = button->modifiers;
				wb::collect_actions_xml(mb->children, binding.actions, true);
				if (!binding.actions.empty())
					config->mouse_bindings.push_back(std::move(binding));
			}
		}
	}
	xmlXPathFreeObject(object);
}

/* The text of a named child element, or nullptr. */
static const char *child_text(xmlNode *parent, const char *name) {
	for (xmlNode *c = parent->children; c; c = c->next) {
		if (c->type == XML_ELEMENT_NODE && strcmp((char *) c->name, name) == 0 &&
				c->children && c->children->content)
			return (const char *) c->children->content;
	}
	return nullptr;
}

/* The first named child element node, or nullptr. */
static xmlNode *child_node(xmlNode *parent, const char *name) {
	for (xmlNode *c = parent->children; c; c = c->next) {
		if (c->type == XML_ELEMENT_NODE && strcmp((char *) c->name, name) == 0)
			return c;
	}
	return nullptr;
}

static std::optional<bool> parse_bool(const char *s) {
	if (s == nullptr)
		return std::nullopt;
	if (strcmp(s, "yes") == 0 || strcmp(s, "true") == 0 || strcmp(s, "1") == 0)
		return true;
	if (strcmp(s, "no") == 0 || strcmp(s, "false") == 0 || strcmp(s, "0") == 0)
		return false;
	return std::nullopt;
}

/* Parse <applications><application class= name= title=> with per-app override
 * children (maximized/fullscreen/iconic/focus/position/size). */
static void parse_app_rules(struct wb_config *config, xmlXPathContextPtr ctxt) {
	xmlXPathObjectPtr object = xmlXPathEvalExpression(
			(const xmlChar *) "//ob:applications/ob:application", ctxt);
	if (object == NULL)
		return;
	if (object->nodesetval) {
		for (int i = 0; i < object->nodesetval->nodeNr; i++) {
			xmlNode *app = object->nodesetval->nodeTab[i];
			if (app == NULL)
				continue;

			wb::AppRule rule;
			rule.class_pattern = (const char *) get_attribute(app, "class");
			rule.name_pattern = (const char *) get_attribute(app, "name");
			rule.title_pattern = (const char *) get_attribute(app, "title");
			rule.maximized = parse_bool(child_text(app, "maximized"));
			rule.fullscreen = parse_bool(child_text(app, "fullscreen"));
			rule.iconic = parse_bool(child_text(app, "iconic"));
			rule.focus = parse_bool(child_text(app, "focus"));
			rule.decor = parse_bool(child_text(app, "decor"));
			/* <desktop>N</desktop> (1-based) or "all"; anything else
			 * (including Openbox's "current") leaves the default. */
			if (const char *d = child_text(app, "desktop")) {
				std::string v = d;
				if (v == "all")
					rule.desktop = wb::AppRule::kAllDesktops;
				else if (int n = atoi(d); n >= 1)
					rule.desktop = n - 1;
			}

			if (xmlNode *pos = child_node(app, "position")) {
				if (const char *x = child_text(pos, "x"))
					rule.x = atoi(x);
				if (const char *y = child_text(pos, "y"))
					rule.y = atoi(y);
			}
			if (xmlNode *size = child_node(app, "size")) {
				if (const char *w = child_text(size, "width"))
					rule.width = atoi(w);
				if (const char *h = child_text(size, "height"))
					rule.height = atoi(h);
			}
			config->app_rules.push_back(std::move(rule));
		}
	}
	xmlXPathFreeObject(object);
}

/* Parse <desktops><number>N</number><names><name>...</name></names>
 * </desktops> (Openbox's syntax; layout options are not supported). */
static void parse_desktops(struct wb_config *config, xmlXPathContextPtr ctxt) {
	xmlXPathObjectPtr object = xmlXPathEvalExpression(
			(const xmlChar *) "//ob:desktops", ctxt);
	if (object == NULL)
		return;
	if (object->nodesetval && object->nodesetval->nodeNr > 0) {
		xmlNode *node = object->nodesetval->nodeTab[0];
		if (const char *n = child_text(node, "number"))
			config->desktops.number =
					wb::parse_desktop_count(n, config->desktops.number);
		if (const char *t = child_text(node, "popupTime"))
			config->desktops.popup_time_ms =
					wb::parse_popup_time(t, config->desktops.popup_time_ms);
		if (xmlNode *names = child_node(node, "names")) {
			for (xmlNode *c = names->children; c; c = c->next) {
				if (c->type != XML_ELEMENT_NODE ||
						strcmp((const char *) c->name, "name") != 0)
					continue;
				xmlChar *content = xmlNodeGetContent(c);
				config->desktops.names.emplace_back(
						content ? (const char *) content : "");
				xmlFree(content);
			}
		}
	}
	xmlXPathFreeObject(object);
}

/* Parse the waybox extension block <waybox><menu submenuOpen="hover|click"
 * hoverDelay="ms" wrap="yes|no"/></waybox>. Openbox ignores unknown elements,
 * so this is forward/backward compatible. */
static void parse_menu_behavior(struct wb_config *config,
		xmlXPathContextPtr ctxt) {
	xmlXPathObjectPtr object = xmlXPathEvalExpression(
			(const xmlChar *) "//ob:waybox/ob:menu", ctxt);
	if (object == NULL)
		return;
	if (object->nodesetval && object->nodesetval->nodeNr > 0) {
		xmlNode *node = object->nodesetval->nodeTab[0];
		if (node != NULL) {
			if (const char *v = (const char *) get_attribute(node, "submenuOpen"))
				if (auto m = wb::submenu_open_from_name(v))
					config->menu_behavior.submenu_open = *m;
			if (const char *v = (const char *) get_attribute(node, "hoverDelay"))
				if (v[0] != '\0')
					config->menu_behavior.submenu_delay_ms = atoi(v);
			if (auto w = parse_bool((const char *) get_attribute(node, "wrap")))
				config->menu_behavior.wrap = *w;
			if (auto si = parse_bool((const char *) get_attribute(node, "icons")))
				config->menu_behavior.show_icons = *si;
			config->menu_source = wb::menu_source_from_config(
					(const char *) get_attribute(node, "source"));
		}
	}
	xmlXPathFreeObject(object);
}

/* Parse <waybox><switcher order="mru|stacking|spatial" osd="yes|no"
 * wrap="yes|no"/></waybox>: the interactive Alt+Tab task switcher options. */
static void parse_switcher_behavior(struct wb_config *config,
		xmlXPathContextPtr ctxt) {
	xmlXPathObjectPtr object = xmlXPathEvalExpression(
			(const xmlChar *) "//ob:waybox/ob:switcher", ctxt);
	if (object == NULL)
		return;
	if (object->nodesetval && object->nodesetval->nodeNr > 0) {
		xmlNode *node = object->nodesetval->nodeTab[0];
		if (node != NULL) {
			if (const char *v = (const char *) get_attribute(node, "order"))
				if (auto o = wb::switcher_order_from_name(v))
					config->switcher_behavior.order = *o;
			if (auto osd = parse_bool((const char *) get_attribute(node, "osd")))
				config->switcher_behavior.show_on_hold = *osd;
			if (auto w = parse_bool((const char *) get_attribute(node, "wrap")))
				config->switcher_behavior.wrap = *w;
			if (auto i = parse_bool((const char *) get_attribute(node, "icons")))
				config->switcher_behavior.show_icons = *i;
		}
	}
	xmlXPathFreeObject(object);
}

/* Parse <waybox><virtualInput allow="unlocked|always|never"/></waybox>. */
static void parse_virtual_input(struct wb_config *config,
		xmlXPathContextPtr ctxt) {
	xmlXPathObjectPtr object = xmlXPathEvalExpression(
			(const xmlChar *) "//ob:waybox/ob:virtualInput", ctxt);
	if (object == NULL)
		return;
	if (object->nodesetval && object->nodesetval->nodeNr > 0) {
		xmlNode *node = object->nodesetval->nodeTab[0];
		const char *v = node ? (const char *) get_attribute(node, "allow") : nullptr;
		if (v == nullptr || strcmp(v, "unlocked") == 0)
			config->virtual_input = wb_config::VirtualInput::Unlocked;
		else if (strcmp(v, "always") == 0)
			config->virtual_input = wb_config::VirtualInput::Always;
		else if (strcmp(v, "never") == 0)
			config->virtual_input = wb_config::VirtualInput::Never;
		else
			wlr_log(WLR_ERROR, "%s: %s", _("Unknown virtualInput allow value"), v);
	}
	xmlXPathFreeObject(object);
}

/* Parse <waybox><xwayland enabled="yes|no"/>. */
static void parse_xwayland(struct wb_config *config, xmlXPathContextPtr ctxt) {
	xmlXPathObjectPtr object = xmlXPathEvalExpression(
			(const xmlChar *) "//ob:waybox/ob:xwayland", ctxt);
	if (object == NULL)
		return;
	if (object->nodesetval && object->nodesetval->nodeNr > 0) {
		xmlNode *node = object->nodesetval->nodeTab[0];
		if (node != nullptr) {
			if (auto v = parse_bool((const char *) get_attribute(node, "enabled")))
				config->xwayland = *v;
		}
	}
	xmlXPathFreeObject(object);
}

/* Parse <waybox><hdr outputs="NAME NAME..."/> (spaces or commas). */
static void parse_hdr(struct wb_config *config, xmlXPathContextPtr ctxt) {
	xmlXPathObjectPtr object = xmlXPathEvalExpression(
			(const xmlChar *) "//ob:waybox/ob:hdr", ctxt);
	if (object == NULL)
		return;
	if (object->nodesetval && object->nodesetval->nodeNr > 0) {
		xmlNode *node = object->nodesetval->nodeTab[0];
		std::string list = node ? (const char *) get_attribute(node, "outputs") : "";
		for (char &c : list)
			if (c == ',')
				c = ' ';
		std::istringstream in(list);
		std::string name;
		while (in >> name)
			config->hdr_outputs.push_back(name);
	}
	xmlXPathFreeObject(object);
}

/* Parse <waybox><outputs><output .../>...</outputs>. Bad values are logged
 * and ignored; the rest of the rule still applies. */
static void parse_outputs(struct wb_config *config, xmlXPathContextPtr ctxt) {
	xmlXPathObjectPtr object = xmlXPathEvalExpression(
			(const xmlChar *) "//ob:waybox/ob:outputs/ob:output", ctxt);
	if (object == NULL)
		return;
	for (int i = 0; object->nodesetval && i < object->nodesetval->nodeNr; i++) {
		xmlNode *node = object->nodesetval->nodeTab[i];
		auto attr = [node](const char *name) -> std::string {
			xmlChar *v = xmlGetProp(node, (const xmlChar *) name);
			std::string s = v ? (const char *) v : "";
			xmlFree(v);
			return s;
		};
		auto bad = [](const char *what, const std::string &value) {
			wlr_log(WLR_ERROR, "%s %s=\"%s\"", _("Ignoring the output setting"),
					what, value.c_str());
		};
		wb::OutputRule rule;
		rule.match = {attr("name"), attr("make"), attr("model"), attr("serial")};
		if (std::string v = attr("enabled"); !v.empty()) {
			if (auto b = parse_bool(v.c_str())) rule.enabled = *b; else bad("enabled", v);
		}
		if (std::string v = attr("mode"); !v.empty() &&
				!wb::parse_mode(v, rule.width, rule.height, rule.refresh))
			bad("mode", v);
		if (std::string v = attr("scale"); !v.empty()) {
			char *end = nullptr;
			double s = strtod(v.c_str(), &end);
			if (end != v.c_str() + v.size() || !(s >= 0.25 && s <= 10)) bad("scale", v);
			else rule.scale = s;
		}
		if (std::string v = attr("position"); !v.empty()) {
			int x, y;
			if (wb::parse_position(v, x, y)) { rule.x = x; rule.y = y; } else bad("position", v);
		}
		if (std::string v = attr("transform"); !v.empty()) {
			if (auto t = wb::parse_transform(v)) rule.transform = *t; else bad("transform", v);
		}
		if (std::string v = attr("adaptiveSync"); !v.empty()) {
			if (auto b = parse_bool(v.c_str())) rule.adaptive_sync = *b; else bad("adaptiveSync", v);
		}
		const wb::OutputIdentity &m = rule.match;
		if (m.name.empty() && m.make.empty() && m.model.empty() && m.serial.empty())
			wlr_log(WLR_ERROR, "%s", _("An <output> needs name, make, model or serial; ignored"));
		else
			config->output_rules.push_back(rule);
	}
	xmlXPathFreeObject(object);
}

/* Parse <waybox><edgeTiling enabled="yes|no" range="N"/>. */
static void parse_edge_tiling(struct wb_config *config, xmlXPathContextPtr ctxt) {
	xmlXPathObjectPtr object = xmlXPathEvalExpression(
			(const xmlChar *) "//ob:waybox/ob:edgeTiling", ctxt);
	if (object == NULL)
		return;
	if (object->nodesetval && object->nodesetval->nodeNr > 0) {
		xmlNode *node = object->nodesetval->nodeTab[0];
		if (node != nullptr) {
			if (auto v = parse_bool((const char *) get_attribute(node, "enabled")))
				config->edge_tiling.enabled = *v;
			const char *range = (const char *) get_attribute(node, "range");
			if (range[0] != '\0' && atoi(range) >= 0)
				config->edge_tiling.range = atoi(range);
		}
	}
	xmlXPathFreeObject(object);
}

/* Parse <waybox><titlebar paddingY="N" buttonSize="N" resizeGrab="N"/></waybox>:
 * server-side-decoration sizing. Each attribute is optional; an absent value
 * leaves the field at -1 ("unset"), and frame_metrics() picks a default. */
static void parse_titlebar(struct wb_config *config, xmlXPathContextPtr ctxt) {
	xmlXPathObjectPtr object = xmlXPathEvalExpression(
			(const xmlChar *) "//ob:waybox/ob:titlebar", ctxt);
	if (object == NULL)
		return;
	if (object->nodesetval && object->nodesetval->nodeNr > 0) {
		xmlNode *node = object->nodesetval->nodeTab[0];
		if (node != NULL) {
			if (const char *v = (const char *) get_attribute(node, "paddingY"))
				if (v[0] != '\0')
					config->titlebar.pad_y = atoi(v);
			if (const char *v = (const char *) get_attribute(node, "buttonSize"))
				if (v[0] != '\0')
					config->titlebar.button_size = atoi(v);
			if (const char *v = (const char *) get_attribute(node, "resizeGrab"))
				if (v[0] != '\0')
					config->titlebar.resize_grab = atoi(v);
		}
	}
	xmlXPathFreeObject(object);
}

/* Parse Openbox font configuration: <theme><font place="...">[<name>, <size>,
 * <weight>bold</weight>]</font></theme>. Each place maps to a Theme font field;
 * fonts live in rc.xml (not themerc), so this runs after the theme is loaded. */
static void parse_fonts(struct wb_config *config, xmlXPathContextPtr ctxt) {
	xmlXPathObjectPtr object = xmlXPathEvalExpression(
			(const xmlChar *) "//ob:theme/ob:font", ctxt);
	if (object == NULL)
		return;
	if (object->nodesetval) {
		for (int i = 0; i < object->nodesetval->nodeNr; i++) {
			xmlNode *node = object->nodesetval->nodeTab[i];
			if (node == NULL)
				continue;
			const char *place = (const char *) get_attribute(node, "place");
			if (place == NULL)
				continue;
			auto fp = wb::font_place_from_name(place);
			if (!fp)
				continue;
			wb::FontSpec *target = nullptr;
			switch (*fp) {
			case wb::FontPlace::ActiveWindow:
				target = &config->theme.font_active_title; break;
			case wb::FontPlace::InactiveWindow:
				target = &config->theme.font_inactive_title; break;
			case wb::FontPlace::MenuHeader:
				target = &config->theme.font_menu_header; break;
			case wb::FontPlace::MenuItem:
				target = &config->theme.font_menu_item; break;
			case wb::FontPlace::OnScreenDisplay:
				target = &config->theme.font_osd; break;
			}
			if (target == nullptr)
				continue;
			for (xmlNode *c = node->children; c != NULL; c = c->next) {
				if (c->type != XML_ELEMENT_NODE)
					continue;
				xmlChar *content = xmlNodeGetContent(c);
				const char *v = content ? (const char *) content : "";
				if (xmlStrcmp(c->name, (const xmlChar *) "name") == 0 &&
						v[0] != '\0') {
					target->family = v;
				} else if (xmlStrcmp(c->name, (const xmlChar *) "size") == 0 &&
						v[0] != '\0') {
					int s = atoi(v);
					if (s > 0)
						target->size_pt = s;
				} else if (xmlStrcmp(c->name, (const xmlChar *) "weight") == 0) {
					target->bold = (xmlStrcasecmp((const xmlChar *) v,
							(const xmlChar *) "bold") == 0);
				}
				if (content)
					xmlFree(content);
			}
		}
	}
	xmlXPathFreeObject(object);
}

/* The rc.xml to read: --config-file, else $WB_RC_XML (which the session
 * script sets), else the first that exists of the user's and the system's. */
static fs::path chosen_rc_file(struct wb_server *server) {
	if (server->config_file != NULL)
		return server->config_file;
	if (const char *env = getenv("WB_RC_XML"); env && env[0] != '\0')
		return env;
	return default_rc_file();
}

/* Read one rc.xml into a new config. A file with any XML error is refused
 * (nullptr, with `problem` saying where), never half-read. */
static struct wb_config *load_config_file(const fs::path &rc_file, std::string *problem) {
	struct wb_config *config = new (std::nothrow) wb_config{};
	if (config == NULL)
		return nullptr;
	const std::string rc_file_str = rc_file.string();
	wb::XmlError err;
	xmlDocPtr doc = wb::read_xml_strict(rc_file_str.c_str(), &err);
	if (doc == NULL) {
		if (problem != nullptr) {
			char line[32] = "";
			if (err.line > 0)
				snprintf(line, sizeof(line), ", %s %d", _("line"), err.line);
			*problem = rc_file_str + line + ": " + err.message;
		}
		delete config;
		return nullptr;
	}
	wlr_log(WLR_INFO, "Using config file %s", rc_file_str.c_str());
	setenv("WAYBOX_CONFIG_FILE", rc_file_str.c_str(), true);
	xmlXPathContextPtr ctxt = xmlXPathNewContext(doc);
	if (ctxt == NULL) {
		xmlFreeDoc(doc);
		delete config;
		if (problem != nullptr)
			*problem = _("Couldn't create new context!");
		return nullptr;
	}
	if (xmlXPathRegisterNs(ctxt, (const xmlChar *) "ob", (const xmlChar *) "http://openbox.org/3.4/rc") != 0) {
		wlr_log(WLR_INFO, "%s", _("Couldn't register the namespace"));
	}

	config->keyboard_layout.use_config = xpath_exists("//ob:keyboard//ob:xkb", ctxt);
	if (config->keyboard_layout.use_config) {
		config->keyboard_layout.layout = parse_xpath_expr("//ob:keyboard//ob:xkb//ob:layout", ctxt);
		config->keyboard_layout.model = parse_xpath_expr("//ob:keyboard//ob:xkb//ob:model", ctxt);
		config->keyboard_layout.options = parse_xpath_expr("//ob:keyboard//ob:xkb//ob:options", ctxt);
		config->keyboard_layout.rules = parse_xpath_expr("//ob:keyboard//ob:xkb//ob:rules", ctxt);
		config->keyboard_layout.variant = parse_xpath_expr("//ob:keyboard//ob:xkb//ob:variant", ctxt);
	}

	config->libinput_config.use_config = xpath_exists("//ob:mouse//ob:libinput", ctxt);
	if (config->libinput_config.use_config) {
		config->libinput_config.accel_profile = parse_xpath_expr("//ob:mouse//ob:libinput/ob:accelProfile", ctxt);
		config->libinput_config.accel_speed = parse_xpath_expr("//ob:mouse//ob:libinput/ob:accelSpeed", ctxt);
		config->libinput_config.calibration_matrix = parse_xpath_expr("//ob:mouse//ob:libinput/ob:calibrationMatrix", ctxt);
		config->libinput_config.click_method = parse_xpath_expr("//ob:mouse//ob:libinput/ob:clickMethod", ctxt);
		config->libinput_config.dwt = parse_xpath_expr("//ob:mouse//ob:libinput/ob:disableWhileTyping", ctxt);
		config->libinput_config.dwtp = parse_xpath_expr("//ob:mouse//ob:libinput/ob:disableWhileTrackpointing", ctxt);
		config->libinput_config.left_handed = parse_xpath_expr("//ob:mouse//ob:libinput/ob:leftHanded", ctxt);
		config->libinput_config.middle_emulation = parse_xpath_expr("//ob:mouse//ob:libinput/ob:middleEmulation", ctxt);
		config->libinput_config.natural_scroll = parse_xpath_expr("//ob:mouse//ob:libinput/ob:naturalScroll", ctxt);
		config->libinput_config.scroll_button = parse_xpath_expr("//ob:mouse//ob:libinput/ob:scrollButton", ctxt);
		config->libinput_config.scroll_button_lock = parse_xpath_expr("//ob:mouse//ob:libinput/ob:scrollButtonLock", ctxt);
		config->libinput_config.scroll_method = parse_xpath_expr("//ob:mouse//ob:libinput/ob:scrollMethod", ctxt);
		config->libinput_config.tap = parse_xpath_expr("//ob:mouse//ob:libinput/ob:tap", ctxt);
		config->libinput_config.tap_button_map = parse_xpath_expr("//ob:mouse//ob:libinput/ob:tapButtonMap", ctxt);
		config->libinput_config.tap_drag = parse_xpath_expr("//ob:mouse//ob:libinput/ob:tapDrag", ctxt);
		config->libinput_config.tap_drag_lock = parse_xpath_expr("//ob:mouse//ob:libinput/ob:tapDragLock", ctxt);
	}

	if (!parse_key_bindings(config, ctxt)) {
		xmlXPathFreeContext(ctxt);
		xmlFreeDoc(doc);
		deinit_config(config);
		if (problem != nullptr)
			*problem = rc_file_str + ": " + _("could not read the key bindings");
		return nullptr;
	}
	parse_mouse_bindings(config, ctxt);
	parse_app_rules(config, ctxt);
	parse_desktops(config, ctxt);

	if (char *policy = parse_xpath_expr("//ob:placement/ob:policy", ctxt)) {
		if (auto parsed = wb::placement_policy_from_name(policy))
			config->placement_policy = *parsed;
		free(policy);
	}

	config->margins.bottom = strtoulong(parse_xpath_expr("//ob:margins/ob:bottom", ctxt));
	config->margins.left = strtoulong(parse_xpath_expr("//ob:margins/ob:left", ctxt));
	config->margins.right = strtoulong(parse_xpath_expr("//ob:margins/ob:right", ctxt));
	config->margins.top = strtoulong(parse_xpath_expr("//ob:margins/ob:top", ctxt));

	config->menu = load_menu_file();

	/* Theme: <theme><name>...</name></theme> (Openbox standard). load_theme
	 * falls back to the built-in default when the name is absent or not found. */
	if (char *name = parse_xpath_expr("//ob:theme/ob:name", ctxt)) {
		config->theme = wb::load_theme(name);
		free(name);
	} else {
		config->theme = wb::default_theme();
	}

	if (char *layout = parse_xpath_expr("//ob:theme/ob:titleLayout", ctxt)) {
		config->title_layout = layout;
		free(layout);
	}

	/* Per-place fonts: <theme><font place="...">. Applied onto the loaded
	 * theme so they override its defaults. */
	parse_fonts(config, ctxt);

	/* Menu behaviour: waybox extension block <waybox><menu .../></waybox>, which
	 * Openbox ignores. Attributes: submenuOpen="hover|click", hoverDelay="ms",
	 * wrap="yes|no". */
	parse_menu_behavior(config, ctxt);
	parse_switcher_behavior(config, ctxt);
	parse_virtual_input(config, ctxt);
	parse_xwayland(config, ctxt);
	parse_edge_tiling(config, ctxt);
	parse_hdr(config, ctxt);
	parse_outputs(config, ctxt);
	parse_titlebar(config, ctxt);

	xmlXPathFreeContext(ctxt);
	xmlFreeDoc(doc);
	xmlCleanupParser();

	return config;
}

/* Say that rc.xml couldn't be read, and what Waybox does instead: in the
 * log, and as a desktop notification when notify-send is installed (after
 * a moment at startup, so the notification daemon is up). */
static void report_config_problem(const std::string &problem, const char *instead,
		bool startup) {
	wlr_log(WLR_ERROR, "%s %s. %s", _("Configuration error:"), problem.c_str(), instead);
	auto quote = [](const std::string &text) {
		std::string q = "'";
		for (char c : text) {
			if (c == '\'')
				q += "'\\''";
			else
				q += c;
		}
		return q + "'";
	};
	std::string cmd = std::string("command -v notify-send >/dev/null && { ") +
			(startup ? "sleep 5; " : "") + "notify-send -a Waybox -u critical " +
			quote(_("Waybox: rc.xml has an error")) + " " +
			quote(problem + "\n" + instead) + "; }";
	wb_spawn(cmd.c_str());
}

/* A config file that is missing or empty just means "no settings". */
static bool has_settings(const fs::path &path) {
	std::error_code ec;
	return fs::exists(path, ec) && fs::file_size(path, ec) > 0 && !ec;
}

struct wb_config *load_config(struct wb_server *server, std::string *problem) {
	fs::path rc_file = chosen_rc_file(server);
	if (rc_file.empty()) {
		if (problem != nullptr)
			*problem = _("Unable to determine the configuration file path.");
		return nullptr;
	}
	return load_config_file(rc_file, problem);
}

bool init_config(struct wb_server *server) {
	fs::path rc_file = chosen_rc_file(server);
	std::string problem;
	struct wb_config *config = nullptr;
	if (has_settings(rc_file)) {
		config = load_config_file(rc_file, &problem);
	} else {
		wlr_log(WLR_INFO, "%s %s", _("No settings in"), rc_file.c_str());
	}
#ifdef WB_SYSCONFDIR
	/* A broken user rc.xml: start with the system's instead, rather than
	 * half of the user's. */
	fs::path system_rc = fs::path(WB_SYSCONFDIR) / "xdg" / "waybox" / "rc.xml";
	if (config == nullptr && !problem.empty() && rc_file != system_rc &&
			has_settings(system_rc)) {
		std::string ignored;
		config = load_config_file(system_rc, &ignored);
	}
#endif
	if (!problem.empty())
		report_config_problem(problem, config != nullptr
				? _("Using the default configuration until it is fixed.")
				: _("Using built-in defaults until it is fixed."), true);
	server->config = config;
	return config != nullptr;
}

/* Reconfigure: read rc.xml again. A broken file leaves the current
 * configuration in place. Returns the new config, or nullptr. */
struct wb_config *reload_config(struct wb_server *server) {
	std::string problem;
	struct wb_config *config = load_config(server, &problem);
	if (config == nullptr)
		report_config_problem(problem, _("Keeping the current settings."), false);
	return config;
}

void deinit_config(struct wb_config *config) {
	if (!config)
		return;

	/* Free everything allocated in init_config */
	free(config->keyboard_layout.layout);
	free(config->keyboard_layout.model);
	free(config->keyboard_layout.options);
	free(config->keyboard_layout.rules);
	free(config->keyboard_layout.variant);

	free(config->libinput_config.accel_profile);
	free(config->libinput_config.accel_speed);
	free(config->libinput_config.calibration_matrix);
	free(config->libinput_config.click_method);
	free(config->libinput_config.dwt);
	free(config->libinput_config.dwtp);
	free(config->libinput_config.left_handed);
	free(config->libinput_config.middle_emulation);
	free(config->libinput_config.natural_scroll);
	free(config->libinput_config.scroll_button);
	free(config->libinput_config.scroll_button_lock);
	free(config->libinput_config.scroll_method);
	free(config->libinput_config.tap);
	free(config->libinput_config.tap_button_map);
	free(config->libinput_config.tap_drag);
	free(config->libinput_config.tap_drag_lock);

	/* key_bindings/mouse_bindings/app_rules are std::vectors and free
	 * themselves when `config` is deleted. */
	delete config;
}
