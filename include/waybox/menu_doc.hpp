#ifndef WB_MENU_DOC_HPP
#define WB_MENU_DOC_HPP

/*
 * menu.xml as wbconf's menu editor edits it (#238): read into a tree of
 * entries, and changed in place (comments, and what the editor doesn't
 * model, kept), as RcDocument does rc.xml. Read as Waybox's parser reads
 * it (menu_parse.cpp): an <item> runs actions, a <separator> divides
 * (labelled: a title), a <menu> with a label, children, execute= or type=
 * is a submenu defined there (a pipe menu with execute=, the applications
 * menu with type="applications"), a bare <menu id="..."/> one defined
 * elsewhere. (test/menu_doc_test.cpp.)
 */
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "waybox/action_catalog.hpp"

namespace wb {

struct MenuEntry {
	enum class Kind {
		Item,          /* runs actions */
		Separator,     /* a divider; labelled, a title */
		Submenu,       /* a menu defined here, its entries as children */
		Reference,     /* <menu id="..."/>: a menu defined elsewhere */
		Pipe,          /* execute=: its entries from a command's output */
		Applications,  /* type="applications": the installed programs */
	};
	Kind kind = Kind::Item;
	std::string label, icon;
	std::string id;       /* Submenu, Reference, Pipe, Applications */
	std::string execute;  /* Pipe */
	std::vector<BoundAction> actions;  /* Item */
	std::vector<MenuEntry> children;   /* Submenu */
	/* What's there the editor doesn't model (attributes, elements), by
	 * name: kept as written. */
	std::vector<std::string> kept;
	bool operator==(const MenuEntry &) const = default;
};

class MenuDocument {
public:
	MenuDocument(MenuDocument &&) noexcept;
	MenuDocument &operator=(MenuDocument &&) noexcept;
	~MenuDocument();

	/* nullopt (with why) when it isn't well-formed XML. */
	static std::optional<MenuDocument> from_string(std::string_view text,
			std::string *error = nullptr);
	static std::optional<MenuDocument> load_file(const std::string &path,
			std::string *error = nullptr);
	std::string to_string() const;
	bool save_file(const std::string &path, std::string *error = nullptr) const;

	/* The menus defined at the top of the file, by id, in order. */
	std::vector<std::string> menu_ids() const;
	/* A menu defined anywhere (at the top or inline), as a Submenu entry
	 * holding its entries; nullopt when there's none. */
	std::optional<MenuEntry> menu(const std::string &id) const;

	/* An entry's place: indices into each level's entries, from the menu
	 * `id`'s ("{2, 0}": the third entry's first). */
	using Path = std::vector<size_t>;
	/* The entry at `path` changed in place: its kind, label, icon, id,
	 * command and actions (a submenu's entries are left as they are; what
	 * the editor doesn't model kept). */
	bool set(const std::string &id, const Path &path, const MenuEntry &entry);
	/* `entry` (with its children) put at `index` among the entries of the
	 * submenu at `parent` ({} the menu itself). */
	bool insert(const std::string &id, const Path &parent, size_t index, const MenuEntry &entry);
	bool remove(const std::string &id, const Path &path);
	/* The entry at `from` moved to `index` among `to_parent`'s entries
	 * (indices as they are after it's taken out). */
	bool move(const std::string &id, const Path &from, const Path &to_parent, size_t index);

private:
	MenuDocument() = default;
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace wb

#endif /* WB_MENU_DOC_HPP */
