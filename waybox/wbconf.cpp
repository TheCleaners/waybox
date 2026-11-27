/*
 * rc.xml model for wbconf: a thin libxml2 wrapper that reads and writes only
 * the settings wbconf manages, preserving the rest of the user's configuration
 * (keybindings, application rules) and its comments. The GTK UI binds to the
 * plain WayboxSettings struct; this file is the XML glue and is unit-tested via
 * in-memory round-trips (test/wbconf_test.cpp).
 */
#include "waybox/font_spec.hpp"
#include "waybox/wbconf.hpp"
#include "waybox/startup.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <cerrno>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libxml/parser.h>
#include <libxml/xmlversion.h>
#include <libxml/tree.h>

#include "waybox/theme.hpp"
#include "waybox/xml_strict.hpp"

namespace wb {

namespace {

const xmlChar *xc(const char *s) {
	return reinterpret_cast<const xmlChar *>(s);
}

/* Match an element child by (namespace-agnostic) name. */
bool is_element(xmlNode *n, const char *name) {
	return n != nullptr && n->type == XML_ELEMENT_NODE &&
			xmlStrcmp(n->name, xc(name)) == 0;
}

/* The first element child of `parent` named `name`, or nullptr. */
xmlNode *child_named(xmlNode *parent, const char *name) {
	if (parent == nullptr)
		return nullptr;
	for (xmlNode *c = parent->children; c != nullptr; c = c->next) {
		if (is_element(c, name))
			return c;
	}
	return nullptr;
}

/* Append a new element child `name` to `parent`, inheriting the document's
 * default namespace so it serialises like the hand-written nodes. A little
 * leading whitespace keeps the output readable. */
xmlNode *new_child(xmlNode *parent, const char *name) {
	xmlNodePtr node = xmlNewNode(parent->ns, xc(name));
	if (node == nullptr)
		return nullptr;
	/* Indent the new node one level past its parent for legibility. */
	int depth = 0;
	for (xmlNode *p = parent; p != nullptr && p->parent != nullptr; p = p->parent)
		++depth;
	std::string indent = "\n" + std::string(static_cast<size_t>(depth) * 2, ' ');
	/* Before the whitespace that indents the parent's closing tag, so the
	 * new element gets its own line above it. */
	xmlNode *last = parent->last;
	if (last != nullptr && xmlIsBlankNode(last)) {
		xmlAddPrevSibling(last, xmlNewText(xc(indent.c_str())));
		xmlAddPrevSibling(last, node);
	} else {
		xmlAddChild(parent, xmlNewText(xc(indent.c_str())));
		xmlAddChild(parent, node);
	}
	return node;
}

/* Find or create the element child `name` under `parent`. */
xmlNode *ensure_child(xmlNode *parent, const char *name) {
	if (xmlNode *existing = child_named(parent, name))
		return existing;
	return new_child(parent, name);
}

/* Walk/create a path of nested element names from the root, returning the leaf. */
xmlNode *ensure_path(xmlNode *root, const std::vector<const char *> &path) {
	xmlNode *node = root;
	for (const char *name : path) {
		node = ensure_child(node, name);
		if (node == nullptr)
			return nullptr;
	}
	return node;
}

/* Resolve an existing path of nested elements, or nullptr if any is missing. */
xmlNode *find_path(xmlNode *root, const std::vector<const char *> &path) {
	xmlNode *node = root;
	for (const char *name : path) {
		node = child_named(node, name);
		if (node == nullptr)
			return nullptr;
	}
	return node;
}

std::string trimmed(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return {};
	return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::optional<std::string> node_text(xmlNode *node) {
	if (node == nullptr)
		return std::nullopt;
	xmlChar *content = xmlNodeGetContent(node);
	if (content == nullptr)
		return std::nullopt;
	std::string value(reinterpret_cast<const char *>(content));
	xmlFree(content);
	return value;
}

std::optional<std::string> attr_value(xmlNode *node, const char *attr) {
	if (node == nullptr)
		return std::nullopt;
	xmlChar *v = xmlGetProp(node, xc(attr));
	if (v == nullptr)
		return std::nullopt;
	std::string value(reinterpret_cast<const char *>(v));
	xmlFree(v);
	return value;
}

bool is_match_key(const std::string &key) {
	return key == "name" || key == "make" || key == "model" || key == "serial";
}

OutputMatch output_match_of(xmlNode *o) {
	return {attr_value(o, "name").value_or(""), attr_value(o, "make").value_or(""),
			attr_value(o, "model").value_or(""), attr_value(o, "serial").value_or("")};
}

std::optional<int> to_int(const std::optional<std::string> &s) {
	if (!s)
		return std::nullopt;
	try {
		size_t pos = 0;
		int v = std::stoi(*s, &pos);
		if (pos != s->size())  /* "12abc", "1.5" */
			return std::nullopt;
		return v;
	} catch (...) {
		return std::nullopt;
	}
}

std::optional<bool> to_bool(const std::optional<std::string> &s) {
	if (!s)
		return std::nullopt;
	std::string v = *s;
	std::transform(v.begin(), v.end(), v.begin(),
			[](unsigned char c) { return std::tolower(c); });
	if (v == "yes" || v == "true" || v == "1" || v == "on")
		return true;
	if (v == "no" || v == "false" || v == "0" || v == "off")
		return false;
	return std::nullopt;
}

const char *bool_str(bool b) { return b ? "yes" : "no"; }

/* Other names the compositor accepts for a font place (case-insensitively):
 * Openbox has active and inactive OSD fonts, waybox one. */
std::vector<std::string> place_aliases(const char *place) {
	if (std::strcmp(place, "ActiveOnScreenDisplay") == 0)
		return {"activeonscreendisplay", "inactiveonscreendisplay", "onscreendisplay"};
	std::string lower = place;
	std::transform(lower.begin(), lower.end(), lower.begin(),
			[](unsigned char c) { return std::tolower(c); });
	return {lower};
}

/* The <theme><font place="P"> element, or nullptr. */
xmlNode *find_font(xmlNode *root, const char *place) {
	xmlNode *theme = child_named(root, "theme");
	if (theme == nullptr)
		return nullptr;
	std::vector<std::string> names = place_aliases(place);
	for (const std::string &name : names) {
		for (xmlNode *c = theme->children; c != nullptr; c = c->next) {
			if (!is_element(c, "font"))
				continue;
			auto p = attr_value(c, "place");
			if (!p)
				continue;
			std::transform(p->begin(), p->end(), p->begin(),
					[](unsigned char ch) { return std::tolower(ch); });
			if (*p == name)
				return c;
		}
	}
	return nullptr;
}

/* Compose a <font place="P"> into a Pango-style string, "Inter Semi-Bold
 * Italic Condensed 11" (wb::font::to_pango), or nullopt if it's absent. */
std::optional<std::string> read_font(xmlNode *root, const char *place) {
	xmlNode *font = find_font(root, place);
	if (font == nullptr)
		return std::nullopt;
	wb::font::Styled st;
	st.family = node_text(child_named(font, "name")).value_or("Sans");
	if (auto size = node_text(child_named(font, "size")); size && !size->empty())
		st.size = std::atof(size->c_str());
	if (auto w = node_text(child_named(font, "weight")))
		if (int v = wb::font::weight_named(*w); v > 0)
			st.weight = v;
	if (auto sl = node_text(child_named(font, "slant"))) {
		std::string l = *sl;
		std::transform(l.begin(), l.end(), l.begin(),
				[](unsigned char c) { return std::tolower(c); });
		st.italic = l == "italic" || l == "oblique";
	}
	if (auto wd = node_text(child_named(font, "width")))
		if (int v = wb::font::width_named(*wd); v > 0)
			st.width = v;
	return wb::font::to_pango(st);
}

}  // namespace

RcDocument::RcDocument(RcDocument &&o) noexcept : doc_(o.doc_) {
	o.doc_ = nullptr;
}

RcDocument &RcDocument::operator=(RcDocument &&o) noexcept {
	if (this != &o) {
		if (doc_ != nullptr)
			xmlFreeDoc(static_cast<xmlDocPtr>(doc_));
		doc_ = o.doc_;
		o.doc_ = nullptr;
	}
	return *this;
}

RcDocument::~RcDocument() {
	if (doc_ != nullptr)
		xmlFreeDoc(static_cast<xmlDocPtr>(doc_));
}

namespace {

std::optional<xmlDocPtr> parse_strict(const char *data, int size, const char *path,
		LoadError *error) {
	XmlError err;
	xmlDocPtr doc = read_xml_strict(path, &err, data, size);
	if (doc == nullptr) {
		if (error != nullptr)
			*error = LoadError{err.message, err.line};
		return std::nullopt;
	}
	return doc;
}

}  // namespace

std::optional<RcDocument> RcDocument::load_file(const std::string &path,
		LoadError *error) {
	auto doc = parse_strict(nullptr, 0, path.c_str(), error);
	if (!doc)
		return std::nullopt;
	return RcDocument(static_cast<void *>(*doc));
}

std::optional<RcDocument> RcDocument::from_string(const std::string &xml,
		LoadError *error) {
	auto doc = parse_strict(xml.c_str(), static_cast<int>(xml.size()), "rc.xml", error);
	if (!doc)
		return std::nullopt;
	return RcDocument(static_cast<void *>(*doc));
}

bool output_rule_matches(const OutputMatch &rule, const OutputMatch &monitor) {
	if (rule.empty())
		return false;
	return (rule.name.empty() || rule.name == monitor.name) &&
			(rule.make.empty() || rule.make == monitor.make) &&
			(rule.model.empty() || rule.model == monitor.model) &&
			(rule.serial.empty() || rule.serial == monitor.serial);
}

int applying_output_rule(const std::vector<OutputSetting> &rules, const OutputMatch &monitor) {
	for (size_t i = 0; i < rules.size(); ++i)
		if (output_rule_matches(rules[i].match(), monitor))
			return static_cast<int>(i);
	return -1;
}

WayboxSettings RcDocument::read() const {
	WayboxSettings s;
	xmlNode *root = xmlDocGetRootElement(static_cast<xmlDocPtr>(doc_));
	if (root == nullptr)
		return s;

	s.theme_name = node_text(find_path(root, {"theme", "name"}));
	s.placement_policy = node_text(find_path(root, {"placement", "policy"}));
	s.margin_top = to_int(node_text(find_path(root, {"margins", "top"})));
	s.margin_bottom = to_int(node_text(find_path(root, {"margins", "bottom"})));
	s.margin_left = to_int(node_text(find_path(root, {"margins", "left"})));
	s.margin_right = to_int(node_text(find_path(root, {"margins", "right"})));

	xmlNode *menu = find_path(root, {"waybox", "menu"});
	s.menu_source = attr_value(menu, "source");
	s.menu_submenu_open = attr_value(menu, "submenuOpen");
	s.menu_hover_delay = to_int(attr_value(menu, "hoverDelay"));
	s.menu_wrap = to_bool(attr_value(menu, "wrap"));
	s.menu_icons = to_bool(attr_value(menu, "icons"));

	xmlNode *sw = find_path(root, {"waybox", "switcher"});
	s.switcher_order = attr_value(sw, "order");
	s.switcher_osd = to_bool(attr_value(sw, "osd"));
	s.switcher_wrap = to_bool(attr_value(sw, "wrap"));
	s.switcher_icons = to_bool(attr_value(sw, "icons"));

	xmlNode *tb = find_path(root, {"waybox", "titlebar"});
	s.titlebar_pad_y = to_int(attr_value(tb, "paddingY"));
	s.titlebar_button_size = to_int(attr_value(tb, "buttonSize"));
	s.titlebar_resize_grab = to_int(attr_value(tb, "resizeGrab"));
	s.title_layout = node_text(find_path(root, {"theme", "titleLayout"}));

	s.font_active_window = read_font(root, "ActiveWindow");
	s.font_inactive_window = read_font(root, "InactiveWindow");
	s.font_menu_header = read_font(root, "MenuHeader");
	s.font_menu_item = read_font(root, "MenuItem");
	s.font_osd = read_font(root, "ActiveOnScreenDisplay");

	s.desktops_number = to_int(node_text(find_path(root, {"desktops", "number"})));
	if (xmlNode *names = find_path(root, {"desktops", "names"})) {
		std::vector<std::string> list;
		for (xmlNode *c = names->children; c != nullptr; c = c->next)
			if (is_element(c, "name"))
				list.push_back(node_text(c).value_or(""));
		s.desktop_names = list;
	}
	s.desktop_popup_time = to_int(node_text(find_path(root, {"desktops", "popupTime"})));

	xmlNode *et = find_path(root, {"waybox", "edgeTiling"});
	s.edge_tiling = to_bool(attr_value(et, "enabled"));
	s.edge_tiling_range = to_int(attr_value(et, "range"));

	if (xmlNode *outs = find_path(root, {"waybox", "outputs"})) {
		for (xmlNode *o = outs->children; o != nullptr; o = o->next) {
			if (!is_element(o, "output"))
				continue;
			OutputSetting out;
			OutputMatch m = output_match_of(o);
			if (m.empty())
				continue;  /* matches nothing */
			bool seen = false;
			for (const OutputSetting &e : s.outputs)
				seen = seen || e.from == m;
			if (seen)
				continue;  /* never applies: an earlier rule has its matcher */
			out.name = m.name;
			out.make = m.make;
			out.model = m.model;
			out.serial = m.serial;
			out.from = m;
			for (xmlAttr *a = o->properties; a != nullptr; a = a->next) {
				std::string key = reinterpret_cast<const char *>(a->name);
				if (!is_match_key(key))
					out.attrs[key] = attr_value(o, key.c_str()).value_or("");
			}
			s.outputs.push_back(out);
		}
	}
	s.hdr_outputs = attr_value(find_path(root, {"waybox", "hdr"}), "outputs");
	s.cursor_theme = attr_value(find_path(root, {"waybox", "cursor"}), "theme");
	s.cursor_size = attr_value(find_path(root, {"waybox", "cursor"}), "size");

	auto read_children = [](xmlNode *parent, std::map<std::string, std::string> &into) {
		for (xmlNode *c = parent ? parent->children : nullptr; c != nullptr; c = c->next)
			if (c->type == XML_ELEMENT_NODE)
				into[reinterpret_cast<const char *>(c->name)] = node_text(c).value_or("");
	};
	read_children(find_path(root, {"keyboard", "xkb"}), s.xkb);
	read_children(find_path(root, {"mouse", "libinput"}), s.libinput);

	if (xmlNode *st = find_path(root, {"waybox", "startup"})) {
		for (xmlNode *c = st->children; c != nullptr; c = c->next) {
			if (!is_element(c, "program"))
				continue;
			StartupSetting p;
			p.command = trimmed(node_text(c).value_or(""));
			for (xmlAttr *a = c->properties; a != nullptr; a = a->next) {
				std::string key = reinterpret_cast<const char *>(a->name);
				p.attrs[key] = attr_value(c, key.c_str()).value_or("");
			}
			p.name = startup_name(p.attrs.count("name") ? p.attrs["name"] : "", p.command);
			s.startup.push_back(std::move(p));
		}
	}
	s.idle_screen_off = to_int(attr_value(find_path(root, {"waybox", "idle"}), "screenOff"));
	if (xmlNode *im = find_path(root, {"waybox", "inputMethod"})) {
		InputMethodSetting ims;
		ims.command = attr_value(im, "command").value_or("");
		for (xmlNode *c = im->children; c != nullptr; c = c->next)
			if (is_element(c, "env"))
				ims.env.emplace_back(attr_value(c, "name").value_or(""),
						attr_value(c, "value").value_or(""));
		s.input_method = ims;
	}

	s.xwayland = to_bool(attr_value(find_path(root, {"waybox", "xwayland"}), "enabled"));
	s.virtual_input = attr_value(find_path(root, {"waybox", "virtualInput"}), "allow");
	return s;
}

namespace {

/* Set or remove an element's text, creating the path on set. */
void put_text(xmlNode *root, const std::vector<const char *> &path,
		const std::optional<std::string> &value) {
	if (value) {
		xmlNode *node = ensure_path(root, path);
		if (node != nullptr) {
			/* AddContent escapes & and <; SetContent would read them as
			 * markup. */
			xmlNodeSetContent(node, nullptr);
			xmlNodeAddContent(node, xc(value->c_str()));
		}
	} else if (xmlNode *node = find_path(root, path)) {
		xmlUnlinkNode(node);
		xmlFreeNode(node);
	}
}

/* Set or remove an attribute on an element path, creating the path on set. */
void put_attr(xmlNode *root, const std::vector<const char *> &path,
		const char *attr, const std::optional<std::string> &value) {
	if (value) {
		xmlNode *node = ensure_path(root, path);
		if (node != nullptr)
			xmlSetProp(node, xc(attr), xc(value->c_str()));
	} else if (xmlNode *node = find_path(root, path)) {
		xmlUnsetProp(node, xc(attr));
	}
}

std::optional<std::string> int_str(const std::optional<int> &v) {
	if (!v)
		return std::nullopt;
	return std::to_string(*v);
}

std::optional<std::string> bool_opt_str(const std::optional<bool> &v) {
	if (!v)
		return std::nullopt;
	return std::string(bool_str(*v));
}

/* The <desktops><names> list: rewritten when it changes. */
void put_names(xmlNode *root, const std::optional<std::vector<std::string>> &names) {
	if (xmlNode *old = find_path(root, {"desktops", "names"})) {
		xmlUnlinkNode(old);
		xmlFreeNode(old);
	}
	if (!names || names->empty())
		return;
	xmlNode *list = ensure_path(root, {"desktops", "names"});
	for (const std::string &n : *names) {
		xmlNode *name = xmlNewNode(list->ns, xc("name"));
		xmlNodeSetContent(name, xc(n.c_str()));
		xmlAddChild(list, name);
	}
}

/* The <waybox><startup><program> known as `name`, or nullptr. */
xmlNode *find_program(xmlNode *root, const std::string &name) {
	xmlNode *st = find_path(root, {"waybox", "startup"});
	for (xmlNode *c = st ? st->children : nullptr; c != nullptr; c = c->next) {
		if (!is_element(c, "program"))
			continue;
		auto n = attr_value(c, "name");
		if (startup_name(n.value_or(""), trimmed(node_text(c).value_or(""))) == name)
			return c;
	}
	return nullptr;
}

/* Remove an element and the whitespace that indented it. */
void unlink_element(xmlNode *node) {
	xmlNode *prev = node->prev;
	if (prev != nullptr && xmlIsBlankNode(prev)) {
		xmlUnlinkNode(prev);
		xmlFreeNode(prev);
	}
	xmlUnlinkNode(node);
	xmlFreeNode(node);
}

/* Write <startup> program by program: a program that didn't change keeps
 * its element (and comments around it) untouched. */
void put_startup(xmlNode *root, const std::vector<StartupSetting> &now,
		const std::vector<StartupSetting> *was) {
	for (const StartupSetting &p : now) {
		const StartupSetting *old = nullptr;
		if (was != nullptr)
			for (const StartupSetting &w : *was)
				if (w.name == p.name)
					old = &w;
		if (old != nullptr && *old == p)
			continue;
		xmlNode *node = find_program(root, old != nullptr ? old->name : p.name);
		if (node == nullptr)
			node = new_child(ensure_path(root, {"waybox", "startup"}), "program");
		if (node == nullptr)
			continue;
		xmlNodeSetContent(node, nullptr);
		xmlNodeAddContent(node, xc(p.command.c_str()));  /* escapes & < > */
		std::vector<std::string> drop;
		for (xmlAttr *a = node->properties; a != nullptr; a = a->next) {
			std::string key = reinterpret_cast<const char *>(a->name);
			if (p.attrs.count(key) == 0)
				drop.push_back(key);
		}
		for (const std::string &key : drop)
			xmlUnsetProp(node, xc(key.c_str()));
		for (const auto &[key, value] : p.attrs)
			xmlSetProp(node, xc(key.c_str()), xc(value.c_str()));
	}
	/* Programs no longer listed go. */
	if (was != nullptr)
		for (const StartupSetting &w : *was) {
			bool kept = false;
			for (const StartupSetting &p : now)
				kept = kept || p.name == w.name;
			if (!kept)
				if (xmlNode *node = find_program(root, w.name))
					unlink_element(node);
		}
	/* And an empty <startup> with them. */
	if (xmlNode *st = find_path(root, {"waybox", "startup"})) {
		bool empty = true;
		for (xmlNode *c = st->children; c != nullptr; c = c->next)
			if (c->type == XML_ELEMENT_NODE || c->type == XML_COMMENT_NODE)
				empty = false;
		if (empty)
			unlink_element(st);
	}
}

/* The first <waybox><outputs><output> with exactly matcher `m`, or
 * nullptr. */
xmlNode *find_output(xmlNode *root, const OutputMatch &m) {
	if (m.empty())
		return nullptr;
	xmlNode *outs = find_path(root, {"waybox", "outputs"});
	for (xmlNode *o = outs ? outs->children : nullptr; o != nullptr; o = o->next)
		if (is_element(o, "output") && output_match_of(o) == m)
			return o;
	return nullptr;
}

/* Write one rule: its matcher (rewritten where the rule is), and of its
 * attributes only those that differ from `base` (all of them without
 * one). A rule left with no settings is removed. */
void put_output(xmlNode *root, const OutputSetting &out, const OutputSetting *base) {
	xmlNode *node = find_output(root, out.from);
	std::map<std::string, std::string> before = base ? base->attrs
			: std::map<std::string, std::string>{};
	if (out.attrs.empty() || out.match().empty()) {
		if (node != nullptr) {
			xmlUnlinkNode(node);
			xmlFreeNode(node);
		}
		return;
	}
	if (node == nullptr) {
		xmlNode *outs = ensure_path(root, {"waybox", "outputs"});
		node = new_child(outs, "output");
		/* Before the rule it must win over (the first match applies). */
		if (xmlNode *ahead = find_output(root, out.before); ahead != nullptr && ahead != node) {
			xmlUnlinkNode(node);
			xmlAddPrevSibling(ahead, node);
		}
		before.clear();  /* every attribute written */
	}
	/* Last to first, each new one put first: name, make, model, serial
	 * lead, as written by hand; one already there stays where it is. */
	const std::pair<const char *, const std::string *> match[] = {
		{"serial", &out.serial}, {"model", &out.model}, {"make", &out.make},
		{"name", &out.name}};
	for (const auto &[key, value] : match) {
		if (value->empty()) {
			if (xmlHasProp(node, xc(key)))
				xmlUnsetProp(node, xc(key));
		} else if (xmlHasProp(node, xc(key))) {
			xmlSetProp(node, xc(key), xc(value->c_str()));
		} else if (xmlAttr *a = xmlNewProp(node, xc(key), xc(value->c_str()));
				a != nullptr && a != node->properties) {
			/* Appended: moved to the front. */
			if (a->prev != nullptr)
				a->prev->next = a->next;
			if (a->next != nullptr)
				a->next->prev = a->prev;
			a->prev = nullptr;
			a->next = node->properties;
			node->properties->prev = a;
			node->properties = a;
		}
	}
	for (const auto &[key, value] : out.attrs) {
		if (is_match_key(key))
			continue;
		if (before.count(key) == 0 || before.at(key) != value)
			xmlSetProp(node, xc(key.c_str()), xc(value.c_str()));
	}
	for (const auto &[key, value] : before)
		if (out.attrs.count(key) == 0 && !is_match_key(key))
			xmlUnsetProp(node, xc(key.c_str()));
}

/* Create/update or remove the <theme><font place="P"> element from a
 * Pango-style string (wb::font::from_pango). */
void put_font(xmlNode *root, const char *place,
		const std::optional<std::string> &value) {
	xmlNode *existing = find_font(root, place);
	if (!value) {
		if (existing) {
			xmlUnlinkNode(existing);
			xmlFreeNode(existing);
		}
		return;
	}
	wb::font::Styled st = wb::font::from_pango(*value);
	if (st.family.empty())
		st.family = "Sans";

	xmlNode *theme = ensure_child(root, "theme");
	xmlNode *font = existing;
	if (font == nullptr) {
		font = xmlNewNode(theme->ns, xc("font"));
		xmlAddChild(theme, font);
		xmlSetProp(font, xc("place"), xc(place));
	}
	auto set_child = [&](const char *name, const std::string &text) {
		xmlNode *c = child_named(font, name);
		if (c == nullptr) {
			c = xmlNewNode(theme->ns, xc(name));
			xmlAddChild(font, c);
		}
		xmlNodeSetContent(c, xc(text.c_str()));
	};
	auto drop_child = [&](const char *name) {
		if (xmlNode *c = child_named(font, name)) {
			xmlUnlinkNode(c);
			xmlFreeNode(c);
		}
	};
	set_child("name", st.family);
	/* rc.xml's size is whole points. */
	if (st.size > 0)
		set_child("size", std::to_string(static_cast<int>(st.size + 0.5)));
	/* Openbox's bold and normal; the other weights by name (Waybox's). */
	std::string weight = "normal";
	if (st.weight != 400) {
		int w = wb::font::nearest_weight(st.weight);
		weight = w == 700 ? "bold" : "normal";
		for (const wb::font::Named &n : wb::font::weights())
			if (n.value == w && w != 400 && w != 700)
				weight = n.fc;
	}
	set_child("weight", weight);
	/* Openbox's <slant>: written when italic, or when it was there. */
	if (st.italic)
		set_child("slant", "italic");
	else if (child_named(font, "slant") != nullptr)
		set_child("slant", "normal");
	/* Waybox's <width> (Openbox has none): only when it isn't normal. */
	if (st.width != 100) {
		std::string name = std::to_string(st.width);
		for (const wb::font::Named &n : wb::font::widths())
			if (n.value == wb::font::nearest_width(st.width))
				name = n.fc;
		set_child("width", name);
	} else {
		drop_child("width");
	}
}

}  // namespace

namespace {

/* Write `s` into the document. With a `base`, only the fields that differ
 * from it; without, every field (std::nullopt ones are removed). */
void apply_fields(xmlNode *root, const WayboxSettings &s, const WayboxSettings *base) {
#define WB_CHANGED(field) (base == nullptr || s.field != base->field)
	if (WB_CHANGED(theme_name))
		put_text(root, {"theme", "name"}, s.theme_name);
	if (WB_CHANGED(placement_policy))
		put_text(root, {"placement", "policy"}, s.placement_policy);
	if (WB_CHANGED(margin_top))
		put_text(root, {"margins", "top"}, int_str(s.margin_top));
	if (WB_CHANGED(margin_bottom))
		put_text(root, {"margins", "bottom"}, int_str(s.margin_bottom));
	if (WB_CHANGED(margin_left))
		put_text(root, {"margins", "left"}, int_str(s.margin_left));
	if (WB_CHANGED(margin_right))
		put_text(root, {"margins", "right"}, int_str(s.margin_right));

	if (WB_CHANGED(menu_source))
		put_attr(root, {"waybox", "menu"}, "source", s.menu_source);
	if (WB_CHANGED(menu_submenu_open))
		put_attr(root, {"waybox", "menu"}, "submenuOpen", s.menu_submenu_open);
	if (WB_CHANGED(menu_hover_delay))
		put_attr(root, {"waybox", "menu"}, "hoverDelay", int_str(s.menu_hover_delay));
	if (WB_CHANGED(menu_wrap))
		put_attr(root, {"waybox", "menu"}, "wrap", bool_opt_str(s.menu_wrap));
	if (WB_CHANGED(menu_icons))
		put_attr(root, {"waybox", "menu"}, "icons", bool_opt_str(s.menu_icons));

	if (WB_CHANGED(switcher_order))
		put_attr(root, {"waybox", "switcher"}, "order", s.switcher_order);
	if (WB_CHANGED(switcher_osd))
		put_attr(root, {"waybox", "switcher"}, "osd", bool_opt_str(s.switcher_osd));
	if (WB_CHANGED(switcher_wrap))
		put_attr(root, {"waybox", "switcher"}, "wrap", bool_opt_str(s.switcher_wrap));
	if (WB_CHANGED(switcher_icons))
		put_attr(root, {"waybox", "switcher"}, "icons", bool_opt_str(s.switcher_icons));

	if (WB_CHANGED(titlebar_pad_y))
		put_attr(root, {"waybox", "titlebar"}, "paddingY", int_str(s.titlebar_pad_y));
	if (WB_CHANGED(titlebar_button_size))
		put_attr(root, {"waybox", "titlebar"}, "buttonSize",
				int_str(s.titlebar_button_size));
	if (WB_CHANGED(title_layout))
		put_text(root, {"theme", "titleLayout"}, s.title_layout);
	if (WB_CHANGED(titlebar_resize_grab))
		put_attr(root, {"waybox", "titlebar"}, "resizeGrab",
				int_str(s.titlebar_resize_grab));

	if (WB_CHANGED(font_active_window))
		put_font(root, "ActiveWindow", s.font_active_window);
	if (WB_CHANGED(font_inactive_window))
		put_font(root, "InactiveWindow", s.font_inactive_window);
	if (WB_CHANGED(font_menu_header))
		put_font(root, "MenuHeader", s.font_menu_header);
	if (WB_CHANGED(font_menu_item))
		put_font(root, "MenuItem", s.font_menu_item);
	if (WB_CHANGED(font_osd))
		put_font(root, "ActiveOnScreenDisplay", s.font_osd);

	if (WB_CHANGED(desktops_number))
		put_text(root, {"desktops", "number"}, int_str(s.desktops_number));
	if (WB_CHANGED(desktop_names))
		put_names(root, s.desktop_names);
	if (WB_CHANGED(desktop_popup_time))
		put_text(root, {"desktops", "popupTime"}, int_str(s.desktop_popup_time));

	if (WB_CHANGED(edge_tiling))
		put_attr(root, {"waybox", "edgeTiling"}, "enabled", bool_opt_str(s.edge_tiling));
	if (WB_CHANGED(edge_tiling_range))
		put_attr(root, {"waybox", "edgeTiling"}, "range", int_str(s.edge_tiling_range));

	if (WB_CHANGED(hdr_outputs))
		put_attr(root, {"waybox", "hdr"}, "outputs", s.hdr_outputs);
	/* The pointer's theme and size are one thing: a change to either writes
	 * both. (wbconf shows GTK's theme where rc.xml has none; a new size
	 * alone would otherwise leave Waybox on a theme other than the one
	 * shown.) */
	if (WB_CHANGED(cursor_theme) || WB_CHANGED(cursor_size)) {
		put_attr(root, {"waybox", "cursor"}, "theme", s.cursor_theme);
		put_attr(root, {"waybox", "cursor"}, "size", s.cursor_size);
	}
	if (WB_CHANGED(xwayland))
		put_attr(root, {"waybox", "xwayland"}, "enabled", bool_opt_str(s.xwayland));
	if (WB_CHANGED(idle_screen_off))
		put_attr(root, {"waybox", "idle"}, "screenOff", int_str(s.idle_screen_off));
	if (WB_CHANGED(startup))
		put_startup(root, s.startup, base ? &base->startup : nullptr);
	if (WB_CHANGED(input_method)) {
		/* Rewritten whole: it's one element. */
		if (xmlNode *old = find_path(root, {"waybox", "inputMethod"}))
			unlink_element(old);
		if (s.input_method) {
			xmlNode *im = new_child(ensure_path(root, {"waybox"}), "inputMethod");
			xmlSetProp(im, xc("command"), xc(s.input_method->command.c_str()));
			for (const auto &[name, value] : s.input_method->env) {
				xmlNode *e = new_child(im, "env");
				xmlSetProp(e, xc("name"), xc(name.c_str()));
				xmlSetProp(e, xc("value"), xc(value.c_str()));
			}
		}
	}
	if (WB_CHANGED(virtual_input))
		put_attr(root, {"waybox", "virtualInput"}, "allow", s.virtual_input);
	/* <keyboard><xkb> and <mouse><libinput>: one element per setting. */
	auto put_children = [&](const char *a, const char *b,
			const std::map<std::string, std::string> &now,
			const std::map<std::string, std::string> &was) {
		for (const auto &[key, value] : now) {
			auto it = was.find(key);
			if (base == nullptr || it == was.end() || it->second != value)
				put_text(root, {a, b, key.c_str()}, value);
		}
		for (const auto &[key, value] : was)
			if (now.count(key) == 0)
				put_text(root, {a, b, key.c_str()}, std::nullopt);
		/* Drop the block once it is empty. */
		xmlNode *block = find_path(root, {a, b});
		bool empty = block != nullptr;
		for (xmlNode *c = block ? block->children : nullptr; c != nullptr; c = c->next)
			if (c->type == XML_ELEMENT_NODE)
				empty = false;
		if (empty && (base == nullptr || !was.empty())) {
			xmlUnlinkNode(block);
			xmlFreeNode(block);
		}
	};
	static const std::map<std::string, std::string> none;
	if (WB_CHANGED(xkb))
		put_children("keyboard", "xkb", s.xkb, base ? base->xkb : none);
	if (WB_CHANGED(libinput))
		put_children("mouse", "libinput", s.libinput, base ? base->libinput : none);

	/* Rules are known by the matcher they were read with (`from`): the
	 * same in the base and the settings, whatever they match now. */
	auto in_base = [&](const OutputSetting &out) -> const OutputSetting * {
		if (base == nullptr || out.from.empty())
			return nullptr;
		for (const OutputSetting &b : base->outputs)
			if (b.from == out.from)
				return &b;
		return nullptr;
	};
	if (base != nullptr || !s.outputs.empty() || find_path(root, {"waybox", "outputs"})) {
		for (const OutputSetting &out : s.outputs) {
			const OutputSetting *was = in_base(out);
			if (base != nullptr && was != nullptr && *was == out)
				continue;
			put_output(root, out, was);
		}
		/* In the base but no longer in the settings (set back to all
		 * automatic): its rule goes. */
		if (base != nullptr)
			for (const OutputSetting &was : base->outputs) {
				if (was.from.empty())
					continue;
				bool kept = false;
				for (const OutputSetting &out : s.outputs)
					kept = kept || out.from == was.from;
				if (!kept) {
					OutputSetting gone = was;
					gone.attrs.clear();
					put_output(root, gone, &was);
				}
			}
	}
#undef WB_CHANGED
}

}  // namespace

void RcDocument::apply(const WayboxSettings &s) {
	if (xmlNode *root = xmlDocGetRootElement(static_cast<xmlDocPtr>(doc_)))
		apply_fields(root, s, nullptr);
}

void RcDocument::apply(const WayboxSettings &s, const WayboxSettings &unchanged) {
	if (xmlNode *root = xmlDocGetRootElement(static_cast<xmlDocPtr>(doc_)))
		apply_fields(root, s, &unchanged);
}

namespace {

/* `node` and everything in it into namespace `ns` (the document's default,
 * as the elements around it). */
void set_ns_deep(xmlNode *node, xmlNs *ns) {
	for (; node != nullptr; node = node->next)
		if (node->type == XML_ELEMENT_NODE) {
			xmlSetNs(node, ns);
			set_ns_deep(node->children, ns);
		}
}

}  // namespace

std::string RcDocument::snapshot(const std::vector<const char *> &path) const {
	xmlNode *root = xmlDocGetRootElement(static_cast<xmlDocPtr>(doc_));
	xmlNode *node = root ? find_path(root, path) : nullptr;
	if (node == nullptr)
		return {};
	xmlBufferPtr buf = xmlBufferCreate();
	xmlNodeDump(buf, static_cast<xmlDocPtr>(doc_), node, 0, 0);
	std::string out(reinterpret_cast<const char *>(xmlBufferContent(buf)),
			static_cast<size_t>(xmlBufferLength(buf)));
	xmlBufferFree(buf);
	return out;
}

void RcDocument::restore(const std::vector<const char *> &path, const std::string &xml) {
	xmlDocPtr doc = static_cast<xmlDocPtr>(doc_);
	xmlNode *root = xmlDocGetRootElement(doc);
	if (root == nullptr || path.empty())
		return;
	xmlNode *now = find_path(root, path);
	xmlNode *copy = nullptr;
	if (!xml.empty()) {
		/* No network, no entities expanded: it's our own text, but text. */
		xmlDocPtr part = xmlReadMemory(xml.data(), static_cast<int>(xml.size()), nullptr,
				nullptr, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
		xmlNode *top = part ? xmlDocGetRootElement(part) : nullptr;
		if (top != nullptr && xmlStrcmp(top->name, xc(path.back())) == 0)
			copy = xmlDocCopyNode(top, doc, 1);
		if (part != nullptr)
			xmlFreeDoc(part);
		if (copy == nullptr)
			return;  /* not what was saved: leave things as they are */
	}
	if (now != nullptr && copy != nullptr) {
		set_ns_deep(copy, now->ns);
		xmlReplaceNode(now, copy);
		xmlFreeNode(now);
	} else if (now != nullptr) {
		xmlUnlinkNode(now);
		xmlFreeNode(now);
	} else if (copy != nullptr) {
		std::vector<const char *> up(path.begin(), path.end() - 1);
		xmlNode *parent = up.empty() ? root : ensure_path(root, up);
		set_ns_deep(copy, parent->ns);
		xmlNode *placed = new_child(parent, reinterpret_cast<const char *>(copy->name));
		xmlReplaceNode(placed, copy);
		xmlFreeNode(placed);
	}
}

std::string RcDocument::to_string() const {
	xmlChar *buf = nullptr;
	int size = 0;
	xmlDocDumpFormatMemory(static_cast<xmlDocPtr>(doc_), &buf, &size, 0);
	std::string out;
	if (buf != nullptr) {
		out.assign(reinterpret_cast<const char *>(buf),
				static_cast<size_t>(size));
		xmlFree(buf);
	}
	return out;
}

bool RcDocument::save_file(const std::string &path, std::string *error) const {
	namespace fs = std::filesystem;
	auto fail = [error](const std::string &what) {
		if (error != nullptr)
			*error = what;
		return false;
	};
	std::error_code ec;
	/* Follow a symlink to the real file, so the link survives the rename. */
	fs::path target = fs::exists(path, ec) ? fs::canonical(path, ec) : fs::path(path);
	if (ec)
		target = path;
	struct stat st {};
	bool existed = ::stat(target.c_str(), &st) == 0;

	if (existed) {
		fs::path backup = target;
		backup += ".bak";
		fs::copy_file(target, backup, fs::copy_options::overwrite_existing, ec);
		if (ec)
			return fail("could not keep a backup at " + backup.string() + ": " + ec.message());
	}

	std::string text = to_string();
	fs::path tmp = target;
	tmp += ".tmp";
	int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
			existed ? (st.st_mode & 07777) : 0644);
	if (fd < 0)
		return fail("could not write " + tmp.string() + ": " + std::strerror(errno));
	const char *p = text.data();
	size_t left = text.size();
	while (left > 0) {
		ssize_t n = ::write(fd, p, left);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			std::string why = std::strerror(errno);
			::close(fd);
			::unlink(tmp.c_str());
			return fail("could not write " + tmp.string() + ": " + why);
		}
		p += n;
		left -= static_cast<size_t>(n);
	}
	if (existed)
		(void) ::fchmod(fd, st.st_mode & 07777);
	int synced = ::fsync(fd);
	int sync_errno = errno;
	int closed = ::close(fd);  /* always, so a failing disk doesn't leak fds */
	if (synced != 0 || closed != 0) {
		std::string why = std::strerror(synced != 0 ? sync_errno : errno);
		::unlink(tmp.c_str());
		return fail("could not write " + tmp.string() + ": " + why);
	}
	if (::rename(tmp.c_str(), target.c_str()) != 0) {
		std::string why = std::strerror(errno);
		::unlink(tmp.c_str());
		return fail("could not replace " + target.string() + ": " + why);
	}
	return true;
}

std::vector<std::string> installed_theme_names() {
	namespace fs = std::filesystem;
	auto env = [](const char *k) -> std::string {
		const char *v = std::getenv(k);
		return v ? v : "";
	};
	/* The themerc search bases, mirroring themerc_search_paths(): use a probe
	 * name and strip the "/<name>/openbox-3/themerc" tail to recover each base
	 * directory, then enumerate the real theme directories under it. */
	std::vector<std::string> probes = themerc_search_paths(
			"\x01", env("HOME"), env("XDG_DATA_HOME"), env("XDG_DATA_DIRS"));
	std::vector<std::string> names;
	for (const std::string &probe : probes) {
		fs::path p(probe);
		/* probe = base/<name>/openbox-3/themerc -> base is three parents up. */
		fs::path base = p.parent_path().parent_path().parent_path();
		std::error_code ec;
		if (!fs::is_directory(base, ec))
			continue;
		/* Non-throwing steps: an unreadable entry is skipped, not fatal. */
		fs::directory_iterator it(base, ec), end;
		for (; !ec && it != end; it.increment(ec)) {
			const fs::directory_entry &e = *it;
			std::error_code eec;
			if (!e.is_directory(eec))
				continue;
			fs::path themerc = e.path() / "openbox-3" / "themerc";
			if (fs::exists(themerc, ec))
				names.push_back(e.path().filename().string());
		}
	}
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	return names;
}

namespace {

std::vector<std::string> split_path(const std::string &path) {
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= path.size()) {
		size_t slash = path.find('/', start);
		std::string part = path.substr(start, slash == std::string::npos
				? std::string::npos : slash - start);
		if (!part.empty())
			out.push_back(part);
		if (slash == std::string::npos)
			break;
		start = slash + 1;
	}
	return out;
}

std::vector<const char *> c_path(const std::vector<std::string> &parts) {
	std::vector<const char *> out;
	for (const std::string &p : parts)
		out.push_back(p.c_str());
	return out;
}

/* Remove `node` and then each ancestor left without attributes or element
 * children, up to (not including) the root. */
void prune_empty(xmlNode *root, xmlNode *node) {
	while (node != nullptr && node != root) {
		bool empty = node->properties == nullptr;
		for (xmlNode *c = node->children; empty && c != nullptr; c = c->next)
			if (c->type == XML_ELEMENT_NODE || c->type == XML_COMMENT_NODE ||
					(c->type == XML_TEXT_NODE && !xmlIsBlankNode(c)))
				empty = false;
		if (!empty)
			return;
		xmlNode *parent = node->parent;
		unlink_element(node);
		node = parent;
	}
}

}  // namespace

std::optional<std::string> RcDocument::get(const RcLocation &at) const {
	xmlNode *root = xmlDocGetRootElement(static_cast<xmlDocPtr>(doc_));
	if (root == nullptr)
		return std::nullopt;
	if (!at.font.empty())
		return read_font(root, at.font.c_str());
	std::vector<std::string> parts = split_path(at.path);
	xmlNode *node = find_path(root, c_path(parts));
	if (node == nullptr)
		return std::nullopt;
	if (!at.item.empty()) {
		std::string lines;
		bool any = false;
		for (xmlNode *c = node->children; c != nullptr; c = c->next)
			if (is_element(c, at.item.c_str())) {
				lines += (any ? "\n" : "") + node_text(c).value_or("");
				any = true;
			}
		return any ? std::optional<std::string>(lines) : std::nullopt;
	}
	if (!at.attr.empty())
		return attr_value(node, at.attr.c_str());
	return node_text(node);
}

void RcDocument::set(const RcLocation &at, const std::optional<std::string> &value) {
	xmlNode *root = xmlDocGetRootElement(static_cast<xmlDocPtr>(doc_));
	if (root == nullptr)
		return;
	if (!at.font.empty()) {
		put_font(root, at.font.c_str(), value);
		return;
	}
	std::vector<std::string> parts = split_path(at.path);
	if (parts.empty())
		return;
	std::vector<const char *> path = c_path(parts);
	if (!at.item.empty()) {
		xmlNode *list = find_path(root, path);
		if (list != nullptr) {
			for (xmlNode *c = list->children; c != nullptr;) {
				xmlNode *next = c->next;
				if (is_element(c, at.item.c_str()))
					unlink_element(c);
				c = next;
			}
		}
		if (!value) {
			prune_empty(root, list);
			return;
		}
		list = ensure_path(root, path);
		size_t start = 0;
		while (list != nullptr && start <= value->size()) {
			size_t nl = value->find('\n', start);
			std::string line = value->substr(start, nl == std::string::npos
					? std::string::npos : nl - start);
			xmlNode *item = new_child(list, at.item.c_str());
			xmlNodeAddContent(item, xc(line.c_str()));
			if (nl == std::string::npos)
				break;
			start = nl + 1;
		}
		return;
	}
	if (!at.attr.empty()) {
		put_attr(root, path, at.attr.c_str(), value);
		if (!value)
			prune_empty(root, find_path(root, path));
		return;
	}
	if (value) {
		put_text(root, path, value);
	} else if (xmlNode *node = find_path(root, path)) {
		xmlNode *parent = node->parent;
		unlink_element(node);
		prune_empty(root, parent);
	}
}

namespace {

/* A keybind's command if its only action is Execute (a <command> or
 * <execute> child), else nullopt. */
std::optional<std::string> execute_command(xmlNode *keybind) {
	xmlNode *only = nullptr;
	for (xmlNode *c = keybind->children; c != nullptr; c = c->next) {
		if (c->type != XML_ELEMENT_NODE)
			continue;
		if (only != nullptr || !is_element(c, "action"))
			return std::nullopt;
		only = c;
	}
	if (only == nullptr || attr_value(only, "name").value_or("") != "Execute")
		return std::nullopt;
	xmlNode *cmd = child_named(only, "command");
	if (cmd == nullptr)
		cmd = child_named(only, "execute");
	if (cmd == nullptr)
		return std::nullopt;
	return trimmed(node_text(cmd).value_or(""));
}

xmlNode *find_keybind(xmlNode *root, const std::string &key) {
	xmlNode *kb = find_path(root, {"keyboard"});
	for (xmlNode *c = kb ? kb->children : nullptr; c != nullptr; c = c->next)
		if (is_element(c, "keybind") && attr_value(c, "key").value_or("") == key)
			return c;
	return nullptr;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> RcDocument::execute_keybinds() const {
	std::vector<std::pair<std::string, std::string>> out;
	xmlNode *root = xmlDocGetRootElement(static_cast<xmlDocPtr>(doc_));
	xmlNode *kb = root ? find_path(root, {"keyboard"}) : nullptr;
	for (xmlNode *c = kb ? kb->children : nullptr; c != nullptr; c = c->next) {
		if (!is_element(c, "keybind"))
			continue;
		if (auto cmd = execute_command(c))
			out.emplace_back(attr_value(c, "key").value_or(""), *cmd);
	}
	return out;
}

std::vector<std::string> RcDocument::keybind_keys() const {
	std::vector<std::string> out;
	xmlNode *root = xmlDocGetRootElement(static_cast<xmlDocPtr>(doc_));
	xmlNode *kb = root ? find_path(root, {"keyboard"}) : nullptr;
	for (xmlNode *c = kb ? kb->children : nullptr; c != nullptr; c = c->next)
		if (is_element(c, "keybind"))
			out.push_back(attr_value(c, "key").value_or(""));
	return out;
}

void RcDocument::set_keybind(const std::string &key, const std::optional<std::string> &command) {
	xmlNode *root = xmlDocGetRootElement(static_cast<xmlDocPtr>(doc_));
	if (root == nullptr || key.empty())
		return;
	xmlNode *existing = find_keybind(root, key);
	if (!command) {
		if (existing != nullptr && execute_command(existing))
			unlink_element(existing);
		return;
	}
	xmlNode *bind = existing;
	if (bind == nullptr) {
		bind = new_child(ensure_path(root, {"keyboard"}), "keybind");
		xmlSetProp(bind, xc("key"), xc(key.c_str()));
	} else {
		/* What the key did before goes. */
		for (xmlNode *c = bind->children; c != nullptr;) {
			xmlNode *next = c->next;
			xmlUnlinkNode(c);
			xmlFreeNode(c);
			c = next;
		}
	}
	xmlNode *action = new_child(bind, "action");
	xmlSetProp(action, xc("name"), xc("Execute"));
	xmlNode *cmd = xmlNewNode(action->ns, xc("command"));
	xmlNodeAddContent(cmd, xc(command->c_str()));
	xmlAddChild(action, cmd);
}

namespace {

/* An <action> as the editor sees it: simple options (elements holding
 * only text); anything more (If, ForEach, nested elements) advanced. */
BoundAction read_action(xmlNode *action) {
	BoundAction a;
	a.name = attr_value(action, "name").value_or("");
	if (a.name == "If" || a.name == "ForEach")
		a.advanced = true;
	for (xmlNode *c = action->children; c != nullptr; c = c->next) {
		if (c->type != XML_ELEMENT_NODE)
			continue;
		for (xmlNode *g = c->children; g != nullptr; g = g->next)
			if (g->type == XML_ELEMENT_NODE)
				a.advanced = true;
		a.options.emplace_back(reinterpret_cast<const char *>(c->name),
				trimmed(node_text(c).value_or("")));
	}
	return a;
}

std::vector<BoundAction> read_actions(xmlNode *parent) {
	std::vector<BoundAction> out;
	for (xmlNode *c = parent->children; c != nullptr; c = c->next)
		if (is_element(c, "action"))
			out.push_back(read_action(c));
	return out;
}

bool any_advanced(const std::vector<BoundAction> &actions) {
	for (const BoundAction &a : actions)
		if (a.advanced)
			return true;
	return false;
}

/* `parent`'s actions replaced by `actions` (its other children kept). */
void write_actions(xmlNode *parent, const std::vector<BoundAction> &actions) {
	for (xmlNode *c = parent->children; c != nullptr;) {
		xmlNode *next = c->next;
		if (is_element(c, "action"))
			unlink_element(c);
		c = next;
	}
	for (const BoundAction &a : actions) {
		xmlNode *node = new_child(parent, "action");
		xmlSetProp(node, xc("name"), xc(a.name.c_str()));
		for (const auto &[k, v] : a.options) {
			xmlNode *o = xmlNewNode(node->ns, xc(k.c_str()));
			xmlNodeAddContent(o, xc(v.c_str()));
			xmlAddChild(node, o);
		}
	}
}

void collect_keys(xmlNode *parent, std::vector<std::string> &chain, std::vector<KeyBinding> &out) {
	for (xmlNode *c = parent->children; c != nullptr; c = c->next) {
		if (!is_element(c, "keybind"))
			continue;
		chain.push_back(attr_value(c, "key").value_or(""));
		bool nested = false;
		for (xmlNode *g = c->children; g != nullptr; g = g->next)
			if (is_element(g, "keybind"))
				nested = true;
		if (nested)
			collect_keys(c, chain, out);
		else
			out.push_back({chain, read_actions(c)});
		chain.pop_back();
	}
}

/* The <keybind> at the end of `chain` under `kb`, or nullptr. */
xmlNode *find_chain(xmlNode *kb, const std::vector<std::string> &chain) {
	xmlNode *at = kb;
	for (const std::string &key : chain) {
		xmlNode *next = nullptr;
		for (xmlNode *c = at ? at->children : nullptr; c != nullptr; c = c->next)
			if (is_element(c, "keybind") && attr_value(c, "key").value_or("") == key)
				next = c;
		if (next == nullptr)
			return nullptr;
		at = next;
	}
	return at == kb ? nullptr : at;
}

/* Remove `node`, then any chain keys left with nothing in them. */
void remove_with_empty_parents(xmlNode *node) {
	xmlNode *parent = node->parent;
	unlink_element(node);
	while (parent != nullptr && is_element(parent, "keybind")) {
		bool empty = true;
		for (xmlNode *c = parent->children; c != nullptr; c = c->next)
			if (c->type == XML_ELEMENT_NODE)
				empty = false;
		if (!empty)
			break;
		xmlNode *up = parent->parent;
		unlink_element(parent);
		parent = up;
	}
}

xmlNode *find_mousebind(xmlNode *root, const MouseBinding &m) {
	xmlNode *mouse = find_path(root, {"mouse"});
	for (xmlNode *ctx = mouse ? mouse->children : nullptr; ctx != nullptr; ctx = ctx->next) {
		if (!is_element(ctx, "context") || attr_value(ctx, "name").value_or("") != m.context)
			continue;
		for (xmlNode *b = ctx->children; b != nullptr; b = b->next)
			if (is_element(b, "mousebind") &&
					attr_value(b, "button").value_or("") == m.button &&
					attr_value(b, "action").value_or("") == m.event)
				return b;
	}
	return nullptr;
}

}  // namespace

std::vector<KeyBinding> RcDocument::keybindings() const {
	std::vector<KeyBinding> out;
	xmlNode *root = xmlDocGetRootElement(static_cast<xmlDocPtr>(doc_));
	xmlNode *kb = root ? find_path(root, {"keyboard"}) : nullptr;
	if (kb != nullptr) {
		std::vector<std::string> chain;
		collect_keys(kb, chain, out);
	}
	return out;
}

std::vector<MouseBinding> RcDocument::mousebindings() const {
	std::vector<MouseBinding> out;
	xmlNode *root = xmlDocGetRootElement(static_cast<xmlDocPtr>(doc_));
	xmlNode *mouse = root ? find_path(root, {"mouse"}) : nullptr;
	for (xmlNode *ctx = mouse ? mouse->children : nullptr; ctx != nullptr; ctx = ctx->next) {
		if (!is_element(ctx, "context"))
			continue;
		std::string name = attr_value(ctx, "name").value_or("");
		for (xmlNode *b = ctx->children; b != nullptr; b = b->next)
			if (is_element(b, "mousebind"))
				out.push_back({name, attr_value(b, "button").value_or(""),
						attr_value(b, "action").value_or(""), read_actions(b)});
	}
	return out;
}

bool RcDocument::set_keybinding(const std::vector<std::string> &was,
		const std::optional<KeyBinding> &now) {
	xmlNode *root = xmlDocGetRootElement(static_cast<xmlDocPtr>(doc_));
	if (root == nullptr || (now && (now->chain.empty() || any_advanced(now->actions))))
		return false;
	xmlNode *kb = find_path(root, {"keyboard"});
	xmlNode *old = was.empty() || kb == nullptr ? nullptr : find_chain(kb, was);
	if (old != nullptr && any_advanced(read_actions(old)))
		return false;  /* only rc.xml edits those */
	if (!was.empty() && old == nullptr)
		return false;
	/* The same keys: actions replaced where it stands (comments kept). */
	if (old != nullptr && now && now->chain == was) {
		write_actions(old, now->actions);
		return true;
	}
	if (old != nullptr)
		remove_with_empty_parents(old);
	if (!now)
		return true;
	/* Added: down the chain, its keys reused where they're there. */
	xmlNode *at = ensure_path(root, {"keyboard"});
	for (const std::string &key : now->chain) {
		xmlNode *next = nullptr;
		for (xmlNode *c = at->children; c != nullptr; c = c->next)
			if (is_element(c, "keybind") && attr_value(c, "key").value_or("") == key)
				next = c;
		if (next == nullptr) {
			next = new_child(at, "keybind");
			xmlSetProp(next, xc("key"), xc(key.c_str()));
		}
		at = next;
	}
	write_actions(at, now->actions);
	return true;
}

bool RcDocument::set_mousebinding(const std::optional<MouseBinding> &was,
		const std::optional<MouseBinding> &now) {
	xmlNode *root = xmlDocGetRootElement(static_cast<xmlDocPtr>(doc_));
	if (root == nullptr || (now && any_advanced(now->actions)))
		return false;
	xmlNode *old = was ? find_mousebind(root, *was) : nullptr;
	if (was && old == nullptr)
		return false;
	if (old != nullptr && any_advanced(read_actions(old)))
		return false;
	bool same_place = was && now && was->context == now->context &&
			was->button == now->button && was->event == now->event;
	if (old != nullptr && same_place) {
		write_actions(old, now->actions);
		return true;
	}
	if (old != nullptr) {
		xmlNode *ctx = old->parent;
		unlink_element(old);
		bool empty = true;
		for (xmlNode *c = ctx->children; c != nullptr; c = c->next)
			if (c->type == XML_ELEMENT_NODE)
				empty = false;
		if (empty)
			unlink_element(ctx);
	}
	if (!now)
		return true;
	xmlNode *mouse = ensure_path(root, {"mouse"});
	xmlNode *ctx = nullptr;
	for (xmlNode *c = mouse->children; c != nullptr; c = c->next)
		if (is_element(c, "context") && attr_value(c, "name").value_or("") == now->context)
			ctx = c;
	if (ctx == nullptr) {
		ctx = new_child(mouse, "context");
		xmlSetProp(ctx, xc("name"), xc(now->context.c_str()));
	}
	xmlNode *b = new_child(ctx, "mousebind");
	xmlSetProp(b, xc("button"), xc(now->button.c_str()));
	xmlSetProp(b, xc("action"), xc(now->event.c_str()));
	write_actions(b, now->actions);
	return true;
}

}  // namespace wb
