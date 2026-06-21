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

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "waybox/theme.hpp"

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

/* Find or create the element child `name` under `parent`, inheriting the
 * document's default namespace so it serialises like the hand-written nodes. A
 * little leading whitespace keeps the output readable. */
xmlNode *ensure_child(xmlNode *parent, const char *name) {
	if (xmlNode *existing = child_named(parent, name))
		return existing;
	xmlNodePtr node = xmlNewNode(parent->ns, xc(name));
	if (node == nullptr)
		return nullptr;
	/* Indent the new node one level past its parent for legibility. */
	int depth = 0;
	for (xmlNode *p = parent; p != nullptr && p->parent != nullptr; p = p->parent)
		++depth;
	std::string indent = "\n" + std::string(static_cast<size_t>(depth) * 2, ' ');
	xmlAddChild(parent, xmlNewText(xc(indent.c_str())));
	xmlAddChild(parent, node);
	return node;
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

std::optional<RcDocument> RcDocument::load_file(const std::string &path) {
	xmlDocPtr doc = xmlReadFile(path.c_str(), nullptr, XML_PARSE_RECOVER);
	if (doc == nullptr || xmlDocGetRootElement(doc) == nullptr) {
		if (doc != nullptr)
			xmlFreeDoc(doc);
		return std::nullopt;
	}
	return RcDocument(static_cast<void *>(doc));
}

std::optional<RcDocument> RcDocument::from_string(const std::string &xml) {
	xmlDocPtr doc = xmlReadMemory(xml.c_str(), static_cast<int>(xml.size()),
			"rc.xml", nullptr, XML_PARSE_RECOVER);
	if (doc == nullptr || xmlDocGetRootElement(doc) == nullptr) {
		if (doc != nullptr)
			xmlFreeDoc(doc);
		return std::nullopt;
	}
	return RcDocument(static_cast<void *>(doc));
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

	xmlNode *tb = find_path(root, {"waybox", "titlebar"});
	s.titlebar_pad_y = to_int(attr_value(tb, "paddingY"));
	s.titlebar_button_size = to_int(attr_value(tb, "buttonSize"));
	s.titlebar_resize_grab = to_int(attr_value(tb, "resizeGrab"));
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

}  // namespace

void RcDocument::apply(const WayboxSettings &s) {
	xmlNode *root = xmlDocGetRootElement(static_cast<xmlDocPtr>(doc_));
	if (root == nullptr)
		return;

	put_text(root, {"theme", "name"}, s.theme_name);
	put_text(root, {"placement", "policy"}, s.placement_policy);
	put_text(root, {"margins", "top"}, int_str(s.margin_top));
	put_text(root, {"margins", "bottom"}, int_str(s.margin_bottom));
	put_text(root, {"margins", "left"}, int_str(s.margin_left));
	put_text(root, {"margins", "right"}, int_str(s.margin_right));

	put_attr(root, {"waybox", "menu"}, "source", s.menu_source);
	put_attr(root, {"waybox", "menu"}, "submenuOpen", s.menu_submenu_open);
	put_attr(root, {"waybox", "menu"}, "hoverDelay", int_str(s.menu_hover_delay));
	put_attr(root, {"waybox", "menu"}, "wrap", bool_opt_str(s.menu_wrap));
	put_attr(root, {"waybox", "menu"}, "icons", bool_opt_str(s.menu_icons));

	put_attr(root, {"waybox", "switcher"}, "order", s.switcher_order);
	put_attr(root, {"waybox", "switcher"}, "osd", bool_opt_str(s.switcher_osd));
	put_attr(root, {"waybox", "switcher"}, "wrap", bool_opt_str(s.switcher_wrap));

	put_attr(root, {"waybox", "titlebar"}, "paddingY", int_str(s.titlebar_pad_y));
	put_attr(root, {"waybox", "titlebar"}, "buttonSize",
			int_str(s.titlebar_button_size));
	put_attr(root, {"waybox", "titlebar"}, "resizeGrab",
			int_str(s.titlebar_resize_grab));
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

bool RcDocument::save_file(const std::string &path) const {
	return xmlSaveFormatFileEnc(path.c_str(), static_cast<xmlDocPtr>(doc_),
			"UTF-8", 0) != -1;
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
