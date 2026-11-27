#include "waybox/app_rule_xml.hpp"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>

namespace wb {

namespace {

const xmlChar *xc(const char *s) {
	return reinterpret_cast<const xmlChar *>(s);
}

bool is_element(const xmlNode *n, const char *name) {
	return n != nullptr && n->type == XML_ELEMENT_NODE &&
			strcmp(reinterpret_cast<const char *>(n->name), name) == 0;
}

xmlNode *child(xmlNode *parent, const char *name) {
	for (xmlNode *c = parent->children; c != nullptr; c = c->next)
		if (is_element(c, name))
			return c;
	return nullptr;
}

/* Its text, trimmed; nullopt with no such element. */
std::optional<std::string> text_of(xmlNode *parent, const char *name) {
	xmlNode *c = child(parent, name);
	if (c == nullptr)
		return std::nullopt;
	xmlChar *t = xmlNodeGetContent(c);
	std::string s = t != nullptr ? reinterpret_cast<const char *>(t) : "";
	xmlFree(t);
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string attr(xmlNode *n, const char *name) {
	xmlChar *v = xmlGetProp(n, xc(name));
	std::string s = v != nullptr ? reinterpret_cast<const char *>(v) : "";
	xmlFree(v);
	return s;
}

std::optional<bool> parse_bool(const std::optional<std::string> &s) {
	if (!s)
		return std::nullopt;
	if (*s == "yes" || *s == "true" || *s == "1")
		return true;
	if (*s == "no" || *s == "false" || *s == "0")
		return false;
	return std::nullopt;
}

/* A plain integer ("-12", "40"), as Waybox reads a coordinate. */
bool plain_int(const std::string &s) {
	if (s.empty())
		return false;
	size_t i = s[0] == '-' ? 1 : 0;
	if (i >= s.size())
		return false;
	for (; i < s.size(); ++i)
		if (s[i] < '0' || s[i] > '9')
			return false;
	return true;
}

/* Waybox's own settings in a rule (all else is kept as written). */
const char *const kRead[] = {"maximized", "fullscreen", "iconic", "focus", "decor", "layer",
		"desktop", "position", "size"};

bool read_here(std::string_view name) {
	for (const char *k : kRead)
		if (name == k)
			return true;
	return false;
}

/* A new element at the end of `parent`, on its own line, indented one
 * level past it (and the closing tag on its own line after it). */
xmlNode *add_child(xmlNode *parent, const char *name) {
	xmlNode *node = xmlNewNode(parent->ns, xc(name));
	int depth = 0;
	for (xmlNode *p = parent; p != nullptr && p->parent != nullptr; p = p->parent)
		++depth;
	std::string indent = "\n" + std::string(static_cast<size_t>(depth) * 2, ' ');
	xmlNode *last = parent->last;
	if (last != nullptr && xmlIsBlankNode(last)) {
		xmlAddPrevSibling(last, xmlNewText(xc(indent.c_str())));
		xmlAddPrevSibling(last, node);
	} else {
		xmlAddChild(parent, xmlNewText(xc(indent.c_str())));
		xmlAddChild(parent, node);
		std::string close = "\n" + std::string(static_cast<size_t>(depth > 0 ? depth - 1 : 0) * 2, ' ');
		xmlAddChild(parent, xmlNewText(xc(close.c_str())));
	}
	return node;
}

/* Removed, with the whitespace that indented it. */
void drop(xmlNode *c) {
	xmlNode *prev = c->prev;
	if (prev != nullptr && xmlIsBlankNode(prev)) {
		xmlUnlinkNode(prev);
		xmlFreeNode(prev);
	}
	xmlUnlinkNode(c);
	xmlFreeNode(c);
}

/* The element `name` with `text`, made or replaced in place (its
 * position kept); removed with nullopt. */
void set_text(xmlNode *parent, const char *name, const std::optional<std::string> &text) {
	xmlNode *c = child(parent, name);
	if (!text) {
		if (c != nullptr)
			drop(c);
		return;
	}
	if (c == nullptr)
		c = add_child(parent, name);
	xmlNodeSetContent(c, nullptr);
	xmlNodeAddContent(c, xc(text->c_str()));
}

std::optional<std::string> yes_no(const std::optional<bool> &b) {
	if (!b)
		return std::nullopt;
	return std::string(*b ? "yes" : "no");
}

std::optional<std::string> number(const std::optional<int> &n) {
	if (!n)
		return std::nullopt;
	return std::to_string(*n);
}

bool has_element_children(xmlNode *n) {
	for (xmlNode *c = n->children; c != nullptr; c = c->next)
		if (c->type == XML_ELEMENT_NODE)
			return true;
	return false;
}

void remove_if_empty(xmlNode *parent, const char *name) {
	xmlNode *c = child(parent, name);
	if (c != nullptr && !has_element_children(c))
		drop(c);
}

}  // namespace

AppRuleXml read_app_rule(xmlNode *app) {
	AppRuleXml out;
	AppRule &r = out.rule;
	r.class_pattern = attr(app, "class");
	r.name_pattern = attr(app, "name");
	r.title_pattern = attr(app, "title");
	r.tag_pattern = attr(app, "tag");
	for (xmlAttr *a = app->properties; a != nullptr; a = a->next) {
		std::string_view n = reinterpret_cast<const char *>(a->name);
		if (n != "class" && n != "name" && n != "title" && n != "tag")
			out.kept.emplace_back(n);  /* role, type, groupname... */
	}
	r.maximized = parse_bool(text_of(app, "maximized"));
	r.fullscreen = parse_bool(text_of(app, "fullscreen"));
	r.iconic = parse_bool(text_of(app, "iconic"));
	r.focus = parse_bool(text_of(app, "focus"));
	r.decor = parse_bool(text_of(app, "decor"));
	if (auto l = text_of(app, "layer")) {
		if (*l == "above" || *l == "top")
			r.layer = 1;
		else if (*l == "below" || *l == "bottom")
			r.layer = -1;
		else if (*l == "normal")
			r.layer = 0;
	}
	/* <desktop>N</desktop> (1-based) or "all"; anything else (Openbox's
	 * "current") leaves the default. */
	if (auto d = text_of(app, "desktop")) {
		if (*d == "all")
			r.desktop = AppRule::kAllDesktops;
		else if (int n = atoi(d->c_str()); n >= 1)
			r.desktop = n - 1;
	}
	if (xmlNode *pos = child(app, "position")) {
		auto x = text_of(pos, "x"), y = text_of(pos, "y");
		if (x)
			r.x = atoi(x->c_str());
		if (y)
			r.y = atoi(y->c_str());
		bool plain = (!x || plain_int(*x)) && (!y || plain_int(*y)) && pos->properties == nullptr;
		for (xmlNode *c = pos->children; c != nullptr; c = c->next)
			if (c->type == XML_ELEMENT_NODE && !is_element(c, "x") && !is_element(c, "y"))
				plain = false;  /* <monitor> */
		out.position_kept = !plain;
	}
	if (xmlNode *size = child(app, "size")) {
		/* A size below 1 means nothing: ignored. */
		if (auto w = text_of(size, "width"); w && atoi(w->c_str()) >= 1)
			r.width = atoi(w->c_str());
		if (auto h = text_of(size, "height"); h && atoi(h->c_str()) >= 1)
			r.height = atoi(h->c_str());
	}
	for (xmlNode *c = app->children; c != nullptr; c = c->next)
		if (c->type == XML_ELEMENT_NODE && !read_here(reinterpret_cast<const char *>(c->name)))
			out.kept.emplace_back(reinterpret_cast<const char *>(c->name));
	return out;
}

void write_app_rule(xmlNode *app, const AppRule &r, bool position_kept) {
	auto set_attr = [app](const char *name, const std::string &v) {
		if (v.empty())
			xmlUnsetProp(app, xc(name));
		else
			xmlSetProp(app, xc(name), xc(v.c_str()));
	};
	set_attr("class", r.class_pattern);
	set_attr("name", r.name_pattern);
	set_attr("title", r.title_pattern);
	set_attr("tag", r.tag_pattern);
	set_text(app, "desktop", r.desktop ? std::optional<std::string>(
			*r.desktop == AppRule::kAllDesktops ? "all" : std::to_string(*r.desktop + 1))
			: std::nullopt);
	set_text(app, "layer", r.layer ? std::optional<std::string>(
			*r.layer > 0 ? "above" : *r.layer < 0 ? "below" : "normal") : std::nullopt);
	set_text(app, "decor", yes_no(r.decor));
	set_text(app, "focus", yes_no(r.focus));
	set_text(app, "maximized", yes_no(r.maximized));
	set_text(app, "fullscreen", yes_no(r.fullscreen));
	set_text(app, "iconic", yes_no(r.iconic));
	if (!position_kept) {
		if (r.x || r.y) {
			xmlNode *pos = child(app, "position");
			if (pos == nullptr)
				pos = add_child(app, "position");
			set_text(pos, "x", number(r.x));
			set_text(pos, "y", number(r.y));
		} else {
			set_text(app, "position", std::nullopt);
		}
	}
	if (r.width || r.height) {
		xmlNode *size = child(app, "size");
		if (size == nullptr)
			size = add_child(app, "size");
		set_text(size, "width", number(r.width));
		set_text(size, "height", number(r.height));
		remove_if_empty(app, "size");
	} else {
		set_text(app, "size", std::nullopt);
	}
}

}  // namespace wb
