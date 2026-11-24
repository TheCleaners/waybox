/*
 * rc.xml model for wbconf: a thin libxml2 wrapper that reads and writes only
 * the settings wbconf manages, preserving the rest of the user's configuration
 * (keybindings, application rules) and its comments. The GTK UI binds to the
 * plain WayboxSettings struct; this file is the XML glue and is unit-tested via
 * in-memory round-trips (test/wbconf_test.cpp).
 */
#include "waybox/wbconf.hpp"

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

std::optional<int> to_int(const std::optional<std::string> &s) {
	if (!s)
		return std::nullopt;
	try {
		size_t pos = 0;
		int v = std::stoi(*s, &pos);
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

/* Compose a <font place="P"> into a Pango-style "Family [Bold] Size" string,
 * or nullopt if the place is absent. */
std::optional<std::string> read_font(xmlNode *root, const char *place) {
	xmlNode *font = find_font(root, place);
	if (font == nullptr)
		return std::nullopt;
	std::string family = node_text(child_named(font, "name")).value_or("Sans");
	std::optional<std::string> size = node_text(child_named(font, "size"));
	std::optional<std::string> weight = node_text(child_named(font, "weight"));
	std::string out = family;
	if (weight) {
		std::string w = *weight;
		std::transform(w.begin(), w.end(), w.begin(),
				[](unsigned char c) { return std::tolower(c); });
		if (w == "bold")
			out += " Bold";
	}
	if (size && !size->empty())
		out += " " + *size;
	return out;
}

/* Split a Pango-style "Family [Bold] Size" string into (family, size, bold).
 * A trailing integer is the size; a "Bold" token sets weight; the rest is the
 * family. */
void parse_font(const std::string &spec, std::string &family, std::string &size,
		bool &bold) {
	std::vector<std::string> tokens;
	std::string cur;
	for (char c : spec) {
		if (c == ' ') {
			if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
		} else {
			cur.push_back(c);
		}
	}
	if (!cur.empty())
		tokens.push_back(cur);

	bold = false;
	size.clear();
	/* Only a final number is the size: "3270 Nerd Font 11" is the family
	 * "3270 Nerd Font". */
	if (!tokens.empty() && tokens.back().find_first_not_of("0123456789") ==
			std::string::npos) {
		size = tokens.back();
		tokens.pop_back();
	}
	std::vector<std::string> fam;
	for (const std::string &t : tokens) {
		std::string lo = t;
		std::transform(lo.begin(), lo.end(), lo.begin(),
				[](unsigned char c) { return std::tolower(c); });
		if (lo == "bold") {
			bold = true;
		} else if (lo == "regular" || lo == "normal" || lo == "italic" ||
				lo == "oblique" || lo == "light" || lo == "medium") {
			/* style/weight words we don't model: drop from the family name */
		} else {
			fam.push_back(t);
		}
	}
	family.clear();
	for (size_t i = 0; i < fam.size(); ++i)
		family += (i ? " " : "") + fam[i];
	if (family.empty())
		family = "Sans";
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
			auto name = attr_value(o, "name");
			if (!name || name->empty() || attr_value(o, "make") ||
					attr_value(o, "model") || attr_value(o, "serial"))
				continue;  /* only plain by-name rules */
			OutputSetting out{*name, {}};
			for (xmlAttr *a = o->properties; a != nullptr; a = a->next) {
				std::string key = reinterpret_cast<const char *>(a->name);
				if (key != "name")
					out.attrs[key] = attr_value(o, key.c_str()).value_or("");
			}
			s.outputs.push_back(out);
		}
	}
	s.hdr_outputs = attr_value(find_path(root, {"waybox", "hdr"}), "outputs");

	auto read_children = [](xmlNode *parent, std::map<std::string, std::string> &into) {
		for (xmlNode *c = parent ? parent->children : nullptr; c != nullptr; c = c->next)
			if (c->type == XML_ELEMENT_NODE)
				into[reinterpret_cast<const char *>(c->name)] = node_text(c).value_or("");
	};
	read_children(find_path(root, {"keyboard", "xkb"}), s.xkb);
	read_children(find_path(root, {"mouse", "libinput"}), s.libinput);

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
		if (node != nullptr)
			xmlNodeSetContent(node, xc(value->c_str()));
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

/* The <waybox><outputs><output name="NAME"> element, or nullptr. */
xmlNode *find_output(xmlNode *root, const std::string &name) {
	xmlNode *outs = find_path(root, {"waybox", "outputs"});
	for (xmlNode *o = outs ? outs->children : nullptr; o != nullptr; o = o->next)
		if (is_element(o, "output") && attr_value(o, "name") == name &&
				!attr_value(o, "make") && !attr_value(o, "model") &&
				!attr_value(o, "serial"))
			return o;
	return nullptr;
}

/* Write one output's attributes: only those that differ from `base` (all
 * of them without one). An output left with no settings is removed. */
void put_output(xmlNode *root, const OutputSetting &out, const OutputSetting *base) {
	xmlNode *node = find_output(root, out.name);
	std::map<std::string, std::string> before = base ? base->attrs
			: std::map<std::string, std::string>{};
	if (node == nullptr) {
		if (out.attrs.empty())
			return;
		xmlNode *outs = ensure_path(root, {"waybox", "outputs"});
		node = new_child(outs, "output");
		xmlSetProp(node, xc("name"), xc(out.name.c_str()));
	}
	for (const auto &[key, value] : out.attrs)
		if (base == nullptr || before.count(key) == 0 || before.at(key) != value)
			xmlSetProp(node, xc(key.c_str()), xc(value.c_str()));
	for (const auto &[key, value] : before)
		if (out.attrs.count(key) == 0)
			xmlUnsetProp(node, xc(key.c_str()));
	bool empty = true;
	for (xmlAttr *a = node->properties; a != nullptr; a = a->next)
		if (xmlStrcmp(a->name, xc("name")) != 0)
			empty = false;
	if (empty) {
		xmlUnlinkNode(node);
		xmlFreeNode(node);
	}
}

/* Create/update or remove the <theme><font place="P"> element from a
 * Pango-style "Family [Bold] Size" string. */
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
	std::string family, size;
	bool bold = false;
	parse_font(*value, family, size, bold);

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
	set_child("name", family);
	if (!size.empty())
		set_child("size", size);
	set_child("weight", bold ? "bold" : "normal");
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
	if (WB_CHANGED(xwayland))
		put_attr(root, {"waybox", "xwayland"}, "enabled", bool_opt_str(s.xwayland));
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

	for (const OutputSetting &out : s.outputs) {
		const OutputSetting *was = nullptr;
		if (base != nullptr)
			for (const OutputSetting &b : base->outputs)
				if (b.name == out.name)
					was = &b;
		if (base != nullptr && was != nullptr && *was == out)
			continue;
		/* Without a base, every attribute is written; with one, an output
		 * the base didn't have is written whole. */
		OutputSetting empty{out.name, {}};
		put_output(root, out, base == nullptr ? nullptr : (was ? was : &empty));
	}
	/* A monitor in the base but no longer in the settings (set back to all
	 * automatic): its rule goes. */
	if (base != nullptr)
		for (const OutputSetting &was : base->outputs) {
			bool kept = false;
			for (const OutputSetting &out : s.outputs)
				kept = kept || out.name == was.name;
			if (!kept)
				put_output(root, OutputSetting{was.name, {}}, &was);
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
		for (const fs::directory_entry &e : fs::directory_iterator(base, ec)) {
			if (!e.is_directory())
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

}  // namespace wb
