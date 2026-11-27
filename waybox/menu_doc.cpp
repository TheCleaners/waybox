#include "waybox/atomic_file.hpp"
#include "waybox/menu_doc.hpp"

#include <cstring>
#include <fstream>
#include <sstream>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "waybox/xml_edit.hpp"

namespace wb {

namespace xe = xml_edit;

struct MenuDocument::Impl {
	xmlDocPtr doc = nullptr;
	/* The file's own <?xml ...?> line and the space after it (libxml
	 * writes its own, and drops blank lines there). */
	std::string prolog;
	~Impl() {
		if (doc != nullptr)
			xmlFreeDoc(doc);
	}
};

namespace {

const xmlChar *xc(const char *s) {
	return reinterpret_cast<const xmlChar *>(s);
}

/* The entries a menu element holds, in order: items, separators, menus. */
bool is_entry(const xmlNode *n) {
	return xe::is_element(n, "item") || xe::is_element(n, "separator") ||
			xe::is_element(n, "menu");
}

std::vector<xmlNode *> entries_of(xmlNode *menu) {
	std::vector<xmlNode *> out;
	for (xmlNode *c = menu != nullptr ? menu->children : nullptr; c != nullptr; c = c->next)
		if (is_entry(c))
			out.push_back(c);
	return out;
}

bool has_entries(const xmlNode *n) {
	for (const xmlNode *c = n->children; c != nullptr; c = c->next)
		if (is_entry(c))
			return true;
	return false;
}

/* A <menu> that defines one (as menu_parse reads it), not a reference. */
bool defines(xmlNode *n) {
	return has_entries(n) || !xe::attr(n, "label").value_or("").empty() ||
			!xe::attr(n, "execute").value_or("").empty() ||
			!xe::attr(n, "type").value_or("").empty();
}

MenuEntry read_entry(xmlNode *n) {
	MenuEntry e;
	e.label = xe::attr(n, "label").value_or("");
	e.icon = xe::attr(n, "icon").value_or("");
	std::vector<const char *> known = {"label", "icon"};
	if (xe::is_element(n, "item")) {
		e.kind = MenuEntry::Kind::Item;
		e.actions = xe::read_actions(n);
		for (xmlNode *c = n->children; c != nullptr; c = c->next)
			if (c->type == XML_ELEMENT_NODE && !xe::is_element(c, "action"))
				e.kept.emplace_back(reinterpret_cast<const char *>(c->name));
	} else if (xe::is_element(n, "separator")) {
		e.kind = MenuEntry::Kind::Separator;
	} else {
		e.id = xe::attr(n, "id").value_or("");
		e.execute = xe::attr(n, "execute").value_or("");
		known = {"label", "icon", "id", "execute", "type"};
		if (!e.execute.empty())
			e.kind = MenuEntry::Kind::Pipe;
		else if (xe::attr(n, "type").value_or("") == "applications")
			e.kind = MenuEntry::Kind::Applications;
		else if (defines(n))
			e.kind = MenuEntry::Kind::Submenu;
		else
			e.kind = MenuEntry::Kind::Reference;
		if (e.kind == MenuEntry::Kind::Submenu)
			for (xmlNode *c : entries_of(n))
				e.children.push_back(read_entry(c));
		std::string type = xe::attr(n, "type").value_or("");
		if (!type.empty() && type != "applications")
			e.kept.push_back("type");
	}
	for (xmlAttr *a = n->properties; a != nullptr; a = a->next) {
		const char *name = reinterpret_cast<const char *>(a->name);
		bool k = false;
		for (const char *x : known)
			k = k || strcmp(x, name) == 0;
		if (!k)
			e.kept.emplace_back(name);
	}
	return e;
}

const char *tag_of(MenuEntry::Kind k) {
	switch (k) {
	case MenuEntry::Kind::Item:
		return "item";
	case MenuEntry::Kind::Separator:
		return "separator";
	default:
		return "menu";
	}
}

/* An entry's own settings into its element (not its children). */
void write_entry(xmlNode *n, const MenuEntry &e) {
	/* (In the order menu.xml has them: id, label, icon.) */
	if (e.kind != MenuEntry::Kind::Item && e.kind != MenuEntry::Kind::Separator)
		xe::set_attr(n, "id", e.id);
	xe::set_attr(n, "label", e.label);
	xe::set_attr(n, "icon", e.kind == MenuEntry::Kind::Separator ? "" : e.icon);
	if (e.kind == MenuEntry::Kind::Item) {
		xe::write_actions(n, e.actions);
		return;
	}
	if (e.kind == MenuEntry::Kind::Separator)
		return;
	xe::set_attr(n, "execute", e.kind == MenuEntry::Kind::Pipe ? e.execute : "");
	std::string type = xe::attr(n, "type").value_or("");
	if (e.kind == MenuEntry::Kind::Applications)
		xe::set_attr(n, "type", "applications");
	else if (type == "applications")
		xe::set_attr(n, "type", "");
	/* A reference names a menu defined elsewhere: no label of its own
	 * (one would make it a definition, an empty one). */
	if (e.kind == MenuEntry::Kind::Reference)
		xe::set_attr(n, "label", "");
}

xmlNode *make_entry(xmlNode *parent, const MenuEntry &e, xmlNode *before);

void write_children(xmlNode *n, const std::vector<MenuEntry> &children) {
	for (const MenuEntry &c : children)
		make_entry(n, c, nullptr);
}

/* A new element for `e` (and its children), before `before` in `parent`. */
xmlNode *make_entry(xmlNode *parent, const MenuEntry &e, xmlNode *before) {
	xmlNode *n = xmlNewNode(parent->ns, xc(tag_of(e.kind)));
	xe::insert_before(parent, n, before);
	write_entry(n, e);
	if (e.kind == MenuEntry::Kind::Submenu)
		write_children(n, e.children);
	return n;
}

/* The top-level <menu id=...>, else one defined inline anywhere. */
xmlNode *find_menu(xmlNode *at, const std::string &id) {
	for (xmlNode *c = at->children; c != nullptr; c = c->next)
		if (xe::is_element(c, "menu") && xe::attr(c, "id").value_or("") == id && defines(c))
			return c;
	for (xmlNode *c = at->children; c != nullptr; c = c->next)
		if (xe::is_element(c, "menu"))
			if (xmlNode *found = find_menu(c, id))
				return found;
	return nullptr;
}

/* A menu whose entries are written here (not a reference, pipe or the
 * applications menu): one an entry can go into. */
bool holds_entries(xmlNode *n) {
	return n != nullptr && xe::is_element(n, "menu") && read_entry(n).kind == MenuEntry::Kind::Submenu;
}

/* The element at `path` under `menu` (the menu itself for {}). */
xmlNode *at_path(xmlNode *menu, const MenuDocument::Path &path) {
	xmlNode *n = menu;
	for (size_t i : path) {
		std::vector<xmlNode *> es = entries_of(n);
		if (n == nullptr || i >= es.size())
			return nullptr;
		n = es[i];
	}
	return n;
}

}  // namespace

MenuDocument::MenuDocument(MenuDocument &&) noexcept = default;
MenuDocument &MenuDocument::operator=(MenuDocument &&) noexcept = default;
MenuDocument::~MenuDocument() = default;

std::optional<MenuDocument> MenuDocument::from_string(std::string_view text, std::string *error) {
	xmlDocPtr doc = xmlReadMemory(text.data(), static_cast<int>(text.size()), "menu.xml",
			nullptr, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	if (doc == nullptr || xmlDocGetRootElement(doc) == nullptr) {
		if (doc != nullptr)
			xmlFreeDoc(doc);
		if (error != nullptr) {
			const xmlError *e = xmlGetLastError();
			*error = e != nullptr && e->message != nullptr ? e->message : "not XML";
			while (!error->empty() && error->back() == '\n')
				error->pop_back();
		}
		return std::nullopt;
	}
	MenuDocument d;
	d.impl_ = std::make_unique<Impl>();
	d.impl_->doc = doc;
	if (text.rfind("<?xml", 0) == 0) {
		size_t end = text.find("?>");
		if (end != std::string_view::npos) {
			end += 2;
			while (end < text.size() && (text[end] == '\n' || text[end] == '\r' ||
					text[end] == ' ' || text[end] == '\t'))
				++end;
			d.impl_->prolog = std::string(text.substr(0, end));
		}
	}
	return d;
}

std::optional<MenuDocument> MenuDocument::load_file(const std::string &path, std::string *error) {
	std::ifstream in(path);
	if (!in) {
		if (error != nullptr)
			*error = "can't read " + path;
		return std::nullopt;
	}
	std::stringstream s;
	s << in.rdbuf();
	return from_string(s.str(), error);
}

std::string MenuDocument::to_string() const {
	xmlChar *buf = nullptr;
	int size = 0;
	xmlDocDumpFormatMemoryEnc(impl_->doc, &buf, &size, "UTF-8", 0);
	std::string out = buf != nullptr ? std::string(reinterpret_cast<const char *>(buf),
			static_cast<size_t>(size)) : std::string();
	xmlFree(buf);
	/* The file's own declaration line, as it was. */
	if (!impl_->prolog.empty() && out.rfind("<?xml", 0) == 0) {
		size_t end = out.find("?>");
		if (end != std::string::npos) {
			end += 2;
			while (end < out.size() && (out[end] == '\n' || out[end] == '\r'))
				++end;
			out = impl_->prolog + out.substr(end);
		}
	}
	return out;
}

bool MenuDocument::save_file(const std::string &path, std::string *error) const {
	/* Whole or not at all: a new file of its own beside it (mkstemp: a
	 * link or FIFO planted at a fixed name isn't followed), renamed over. */
	return wb::write_file_atomic(path, to_string(), error);
}

std::vector<std::string> MenuDocument::menu_ids() const {
	std::vector<std::string> out;
	xmlNode *root = xmlDocGetRootElement(impl_->doc);
	for (xmlNode *c = root->children; c != nullptr; c = c->next)
		if (xe::is_element(c, "menu"))
			out.push_back(xe::attr(c, "id").value_or(""));
	return out;
}

std::optional<MenuEntry> MenuDocument::menu(const std::string &id) const {
	xmlNode *m = find_menu(xmlDocGetRootElement(impl_->doc), id);
	if (m == nullptr)
		return std::nullopt;
	MenuEntry e = read_entry(m);
	if (e.kind != MenuEntry::Kind::Submenu) {
		/* A top-level pipe or applications menu: no entries to edit. */
		e.children.clear();
	}
	return e;
}

bool MenuDocument::set(const std::string &id, const Path &path, const MenuEntry &entry) {
	xmlNode *n = at_path(find_menu(xmlDocGetRootElement(impl_->doc), id), path);
	if (n == nullptr || path.empty())
		return false;
	if (strcmp(reinterpret_cast<const char *>(n->name), tag_of(entry.kind)) != 0) {
		/* Another kind of element: a new one in its place. */
		xmlNode *parent = n->parent, *next = n->next;
		while (next != nullptr && !is_entry(next))
			next = next->next;
		xe::remove(n);
		make_entry(parent, entry, next);
		return true;
	}
	write_entry(n, entry);
	/* A submenu made out of another menu kind: its entries now. */
	if (entry.kind == MenuEntry::Kind::Submenu && !has_entries(n))
		write_children(n, entry.children);
	if (entry.kind != MenuEntry::Kind::Submenu && entry.kind != MenuEntry::Kind::Item)
		for (xmlNode *c : entries_of(n))
			xe::remove(c);
	return true;
}

bool MenuDocument::insert(const std::string &id, const Path &parent, size_t index,
		const MenuEntry &entry) {
	xmlNode *p = at_path(find_menu(xmlDocGetRootElement(impl_->doc), id), parent);
	if (!holds_entries(p))
		return false;
	std::vector<xmlNode *> es = entries_of(p);
	if (index > es.size())
		return false;
	make_entry(p, entry, index < es.size() ? es[index] : nullptr);
	return true;
}

bool MenuDocument::remove(const std::string &id, const Path &path) {
	xmlNode *n = at_path(find_menu(xmlDocGetRootElement(impl_->doc), id), path);
	if (n == nullptr || path.empty())
		return false;
	xe::remove(n);
	return true;
}

bool MenuDocument::move(const std::string &id, const Path &from, const Path &to_parent,
		size_t index) {
	xmlNode *menu = find_menu(xmlDocGetRootElement(impl_->doc), id);
	xmlNode *n = at_path(menu, from);
	if (n == nullptr || from.empty())
		return false;
	/* Where it was, to put it back should `to_parent` or `index` not be. */
	xmlNode *was_parent = n->parent, *was_next = n->next;
	while (was_next != nullptr && !is_entry(was_next))
		was_next = was_next->next;
	xe::detach(n);
	xmlNode *p = at_path(menu, to_parent);
	std::vector<xmlNode *> es = p != nullptr ? entries_of(p) : std::vector<xmlNode *>{};
	/* (Not into itself: out of the menu now, it can't be reached.) */
	if (!holds_entries(p) || index > es.size()) {
		xe::insert_before(was_parent, n, was_next);
		return false;
	}
	xe::insert_before(p, n, index < es.size() ? es[index] : nullptr);
	return true;
}

}  // namespace wb
