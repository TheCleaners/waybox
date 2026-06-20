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

static char *parse_xpath_expr(const char *expr, xmlXPathContextPtr ctxt) {
	xmlXPathObjectPtr object = xmlXPathEvalExpression((const xmlChar *) expr, ctxt);
	if (object == NULL) {
		wlr_log(WLR_INFO, "%s: %s", _("Unable to evaluate expression"), expr);
		xmlXPathFreeContext(ctxt);
		return(NULL);
	}
	if (!object->nodesetval) {
		wlr_log(WLR_INFO, "%s", _("No nodesetval"));
		return NULL;
	}
	xmlChar *ret = NULL;
	if (object->nodesetval->nodeNr > 0)
		ret = xmlNodeGetContent(object->nodesetval->nodeTab[0]);
	xmlXPathFreeObject(object);
	return (char *) ret;
}

static xmlChar *get_attribute(xmlNode *node, const char *attr_name) {
	static xmlChar empty[1] = {};
	xmlAttr *attr = node->properties;
	while (attr && strcmp((char *) attr->name, attr_name) != 0)
		attr = attr->next;
	return (attr && attr->children) ? attr->children->content : empty;
}

/* Return the text of an action's <command>/<execute> child, or empty. */
static std::string action_command(xmlNode *action_node) {
	for (xmlNode *child = action_node->children; child; child = child->next) {
		if (child->type != XML_ELEMENT_NODE)
			continue;
		if (strcmp((char *) child->name, "command") == 0 ||
				strcmp((char *) child->name, "execute") == 0) {
			if (child->children && child->children->content)
				return (const char *) child->children->content;
		}
	}
	return {};
}

/* Walk a keybind's child nodes and append every recognised <action> to `out`,
 * preserving document order. */
static void collect_actions(xmlNode *first, std::vector<wb::Action> &out) {
	for (xmlNode *node = first; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE &&
				strcmp((char *) node->name, "action") == 0) {
			const char *name = (const char *) get_attribute(node, "name");
			const wb::ActionSpec *spec = wb::action_spec_from_name(name);
			std::string command =
				(spec && spec->takes_command) ? action_command(node) : std::string{};
			if (auto action = wb::make_action(name, command))
				out.push_back(std::move(*action));
		} else if (node->children) {
			collect_actions(node->children, out);
		}
	}
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
			const char *name = (const char *) get_attribute(child, "name");
			const wb::ActionSpec *spec = wb::action_spec_from_name(name);
			std::string command =
				(spec && spec->takes_command) ? action_command(child) : std::string{};
			if (auto action = wb::make_action(name, command))
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
				collect_actions(mb->children, binding.actions);
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
		}
	}
	xmlXPathFreeObject(object);
}

bool init_config(struct wb_server *server) {
	struct wb_config *config = new (std::nothrow) wb_config{};
	if (config == NULL)
		return false;
	xmlDocPtr doc;
	fs::path rc_file;
	if (const char *env = getenv("WB_RC_XML"); env && env[0] != '\0') {
		rc_file = env;
	} else if (server->config_file != NULL) {
		rc_file = server->config_file;
	} else {
		rc_file = default_rc_file();
	}

	if (rc_file.empty()) {
		wlr_log(WLR_ERROR, "%s", _("Unable to determine the configuration file path."));
		delete config;
		return false;
	}

	const std::string rc_file_str = rc_file.string();
	doc = xmlReadFile(rc_file_str.c_str(), NULL, XML_PARSE_RECOVER);
	wlr_log(WLR_INFO, "Using config file %s", rc_file_str.c_str());
	setenv("WAYBOX_CONFIG_FILE", rc_file_str.c_str(), true);
	if (doc == NULL) {
		wlr_log(WLR_ERROR, "%s", _("Unable to parse the configuration file. Consult stderr for more information."));
		delete config;
		return false;
	}
	xmlXPathContextPtr ctxt = xmlXPathNewContext(doc);
	if (ctxt == NULL) {
		wlr_log(WLR_INFO, "%s", _("Couldn't create new context!"));
		xmlFreeDoc(doc);
		delete config;
		return(false);
	}
	if (xmlXPathRegisterNs(ctxt, (const xmlChar *) "ob", (const xmlChar *) "http://openbox.org/3.4/rc") != 0) {
		wlr_log(WLR_INFO, "%s", _("Couldn't register the namespace"));
	}

	config->keyboard_layout.use_config = parse_xpath_expr("//ob:keyboard//ob:xkb", ctxt) != NULL;
	if (config->keyboard_layout.use_config) {
		config->keyboard_layout.layout = parse_xpath_expr("//ob:keyboard//ob:xkb//ob:layout", ctxt);
		config->keyboard_layout.model = parse_xpath_expr("//ob:keyboard//ob:xkb//ob:model", ctxt);
		config->keyboard_layout.options = parse_xpath_expr("//ob:keyboard//ob:xkb//ob:options", ctxt);
		config->keyboard_layout.rules = parse_xpath_expr("//ob:keyboard//ob:xkb//ob:rules", ctxt);
		config->keyboard_layout.variant = parse_xpath_expr("//ob:keyboard//ob:xkb//ob:variant", ctxt);
	}

	config->libinput_config.use_config = parse_xpath_expr("//ob:mouse//ob:libinput", ctxt) != NULL;
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
		config->libinput_config.tap_drag = parse_xpath_expr("//ob:mouse//ob:libinput/ob:tapDragLock", ctxt);
	}

	if (!parse_key_bindings(config, ctxt)) {
		xmlXPathFreeContext(ctxt);
		xmlFreeDoc(doc);
		delete config;
		return false;
	}
	parse_mouse_bindings(config, ctxt);
	parse_app_rules(config, ctxt);

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

	/* Menu behaviour: waybox extension block <waybox><menu .../></waybox>, which
	 * Openbox ignores. Attributes: submenuOpen="hover|click", hoverDelay="ms",
	 * wrap="yes|no". */
	parse_menu_behavior(config, ctxt);

	server->config = config;

	xmlXPathFreeContext(ctxt);
	xmlFreeDoc(doc);
	xmlCleanupParser();

	return true;
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
