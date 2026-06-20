#include <filesystem>
#include <new>
#include <string>
#include <system_error>
#include <vector>

#include <pwd.h>
#include <unistd.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include "config.h"

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

static bool parse_key_bindings(struct wb_config *config, xmlXPathContextPtr ctxt) {
	/* Get the key bindings */
	wl_list_init(&config->key_bindings);
	xmlXPathObjectPtr object = xmlXPathEvalExpression((xmlChar *) "//ob:keyboard/ob:keybind", ctxt);
	if (object == NULL) {
		wlr_log(WLR_INFO, "%s", _("Unable to evaluate expression"));
		return(false);
	}
	if (!object->nodesetval) {
		wlr_log(WLR_INFO, "%s", _("No nodeset"));
		return false;
	}

	/* Iterate through the nodes to get the information */
	if (object->nodesetval->nodeNr > 0) {
		int i;
		for (i = 0; i < object->nodesetval->nodeNr; i++) {
			if (object->nodesetval->nodeTab[i]) {
				/* First get the key combinations */
				xmlNode *keycomb = object->nodesetval->nodeTab[i];
				char *sym;
				uint32_t modifiers = 0;
				sym = (char *) get_attribute(keycomb, "key");
				char *s;

				struct wb_key_binding *key_bind = new (std::nothrow) wb_key_binding{};
				if (key_bind == NULL) {
					continue;
				}
				key_bind->sym = 0;
				key_bind->modifiers = 0;
				while ((s = strtok(sym, "-")) != NULL) {
					if (strcmp(s, "A") == 0 || strcmp(s, "Alt") == 0)
						modifiers |= WLR_MODIFIER_ALT;
					else if (strcmp(s, "Caps") == 0)
						modifiers |= WLR_MODIFIER_CAPS;
					else if (strcmp(s, "C") == 0 || strcmp(s, "Ctrl") == 0)
						modifiers |= WLR_MODIFIER_CTRL;
					else if (strcmp(s, "Mod2") == 0)
						modifiers |= WLR_MODIFIER_MOD2;
					else if (strcmp(s, "Mod3") == 0)
						modifiers |= WLR_MODIFIER_MOD3;
					else if (strcmp(s, "Mod5") == 0)
						modifiers |= WLR_MODIFIER_MOD5;
					else if (strcmp(s, "S") == 0 || strcmp(s, "Shift") == 0)
						modifiers |= WLR_MODIFIER_SHIFT;
					else if (strcmp(s, "W") == 0 || strcmp(s, "Logo") == 0)
						modifiers |= WLR_MODIFIER_LOGO;
					key_bind->modifiers = modifiers;
					key_bind->sym = xkb_keysym_from_name(s, XKB_KEYSYM_NO_FLAGS);
					sym = NULL;
				}

				/* Now get the actions */
				xmlNode *new_node = object->nodesetval->nodeTab[i]->children;
				collect_actions(new_node, key_bind->actions);

				wl_list_insert(&config->key_bindings, &key_bind->link);
			}
		}
	}
	xmlXPathFreeObject(object);
	return true;
}

bool init_config(struct wb_server *server) {
	struct wb_config *config = static_cast<struct wb_config *>(calloc(1, sizeof(struct wb_config)));
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
		free(config);
		return false;
	}

	const std::string rc_file_str = rc_file.string();
	doc = xmlReadFile(rc_file_str.c_str(), NULL, XML_PARSE_RECOVER);
	wlr_log(WLR_INFO, "Using config file %s", rc_file_str.c_str());
	setenv("WAYBOX_CONFIG_FILE", rc_file_str.c_str(), true);
	if (doc == NULL) {
		wlr_log(WLR_ERROR, "%s", _("Unable to parse the configuration file. Consult stderr for more information."));
		free(config);
		return false;
	}
	xmlXPathContextPtr ctxt = xmlXPathNewContext(doc);
	if (ctxt == NULL) {
		wlr_log(WLR_INFO, "%s", _("Couldn't create new context!"));
		xmlFreeDoc(doc);
		free(config);
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
		free(config);
		return false;
	}

	config->margins.bottom = strtoulong(parse_xpath_expr("//ob:margins/ob:bottom", ctxt));
	config->margins.left = strtoulong(parse_xpath_expr("//ob:margins/ob:left", ctxt));
	config->margins.right = strtoulong(parse_xpath_expr("//ob:margins/ob:right", ctxt));
	config->margins.top = strtoulong(parse_xpath_expr("//ob:margins/ob:top", ctxt));

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

	struct wb_key_binding *key_binding, *tmp;
	wl_list_for_each_safe(key_binding, tmp, &config->key_bindings, link) {
		wl_list_remove(&key_binding->link);
		delete key_binding;
	}
	free(config);
}
