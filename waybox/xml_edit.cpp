#include "waybox/xml_edit.hpp"

#include <cstring>

namespace wb::xml_edit {

namespace {

const xmlChar *xc(const char *s) {
	return reinterpret_cast<const xmlChar *>(s);
}

int depth_of(const xmlNode *n) {
	int depth = 0;
	for (const xmlNode *p = n; p != nullptr && p->parent != nullptr &&
			p->parent->type == XML_ELEMENT_NODE; p = p->parent)
		++depth;
	return depth;
}

std::string trimmed(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return {};
	return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

}  // namespace

bool is_element(const xmlNode *n, const char *name) {
	return n != nullptr && n->type == XML_ELEMENT_NODE &&
			strcmp(reinterpret_cast<const char *>(n->name), name) == 0;
}

std::optional<std::string> attr(const xmlNode *n, const char *name) {
	xmlChar *v = xmlGetProp(n, xc(name));
	if (v == nullptr)
		return std::nullopt;
	std::string s = reinterpret_cast<const char *>(v);
	xmlFree(v);
	return s;
}

void set_attr(xmlNode *n, const char *name, const std::string &value) {
	if (value.empty())
		xmlUnsetProp(n, xc(name));
	else
		xmlSetProp(n, xc(name), xc(value.c_str()));
}

/* The whitespace before `n` on its line ("\n    "), if it starts a line. */
std::optional<std::string> line_start(const xmlNode *n) {
	if (n == nullptr || n->prev == nullptr || !xmlIsBlankNode(n->prev))
		return std::nullopt;
	xmlChar *t = xmlNodeGetContent(n->prev);
	std::string s = t != nullptr ? reinterpret_cast<const char *>(t) : "";
	xmlFree(t);
	size_t nl = s.rfind('\n');
	if (nl == std::string::npos)
		return std::nullopt;
	return s.substr(nl);
}

void insert_before(xmlNode *parent, xmlNode *node, xmlNode *before) {
	/* Its indentation: as its siblings have it (files differ: menu.xml's
	 * top-level menus start at the margin, rc.xml's sections don't); with
	 * none, one level past the parent. */
	std::optional<std::string> sib;
	for (xmlNode *c = parent->children; c != nullptr && !sib; c = c->next)
		if (c->type == XML_ELEMENT_NODE)
			sib = line_start(c);
	/* With none: two spaces past the parent's own line, or past its depth. */
	std::optional<std::string> own_line = line_start(parent);
	std::string indent = sib ? *sib : own_line ? *own_line + "  "
			: "\n" + std::string(static_cast<size_t>(depth_of(parent) + 1) * 2, ' ');
	if (before != nullptr) {
		/* Before `before`'s own indentation, so each keeps its line. */
		xmlNode *at = before->prev != nullptr && xmlIsBlankNode(before->prev) ? before->prev
				: before;
		xmlAddPrevSibling(at, xmlNewText(xc(indent.c_str())));
		xmlAddPrevSibling(at, node);
		return;
	}
	xmlNode *last = parent->last;
	if (last != nullptr && xmlIsBlankNode(last)) {
		xmlAddPrevSibling(last, xmlNewText(xc(indent.c_str())));
		xmlAddPrevSibling(last, node);
	} else {
		xmlAddChild(parent, xmlNewText(xc(indent.c_str())));
		xmlAddChild(parent, node);
		/* The closing tag where the opening one is. */
		std::optional<std::string> own = line_start(parent);
		std::string close = own ? *own
				: "\n" + std::string(static_cast<size_t>(depth_of(parent)) * 2, ' ');
		xmlAddChild(parent, xmlNewText(xc(close.c_str())));
	}
}

xmlNode *add_child(xmlNode *parent, const char *name) {
	xmlNode *node = xmlNewNode(parent->ns, xc(name));
	if (node != nullptr)
		insert_before(parent, node, nullptr);
	return node;
}

void detach(xmlNode *node) {
	xmlNode *prev = node->prev;
	if (prev != nullptr && xmlIsBlankNode(prev)) {
		xmlUnlinkNode(prev);
		xmlFreeNode(prev);
	}
	xmlUnlinkNode(node);
}

void remove(xmlNode *node) {
	detach(node);
	xmlFreeNode(node);
}

std::vector<BoundAction> read_actions(xmlNode *parent) {
	std::vector<BoundAction> out;
	for (xmlNode *action = parent->children; action != nullptr; action = action->next) {
		if (!is_element(action, "action"))
			continue;
		BoundAction a;
		a.name = attr(action, "name").value_or("");
		if (a.name == "If" || a.name == "ForEach")
			a.advanced = true;
		for (xmlNode *c = action->children; c != nullptr; c = c->next) {
			if (c->type != XML_ELEMENT_NODE)
				continue;
			for (xmlNode *g = c->children; g != nullptr; g = g->next)
				if (g->type == XML_ELEMENT_NODE)
					a.advanced = true;
			xmlChar *t = xmlNodeGetContent(c);
			std::string text = t != nullptr ? reinterpret_cast<const char *>(t) : "";
			xmlFree(t);
			a.options.emplace_back(reinterpret_cast<const char *>(c->name), trimmed(text));
		}
		out.push_back(std::move(a));
	}
	return out;
}

void write_actions(xmlNode *parent, const std::vector<BoundAction> &actions) {
	for (xmlNode *c = parent->children; c != nullptr;) {
		xmlNode *next = c->next;
		if (is_element(c, "action"))
			remove(c);
		c = next;
	}
	for (const BoundAction &a : actions) {
		xmlNode *node = add_child(parent, "action");
		xmlSetProp(node, xc("name"), xc(a.name.c_str()));
		for (const auto &[k, v] : a.options) {
			xmlNode *o = xmlNewNode(node->ns, xc(k.c_str()));
			xmlNodeAddContent(o, xc(v.c_str()));
			xmlAddChild(node, o);
		}
	}
}

}  // namespace wb::xml_edit
