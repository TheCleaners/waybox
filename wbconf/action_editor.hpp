#ifndef WBCONF_ACTION_EDITOR_HPP
#define WBCONF_ACTION_EDITOR_HPP

/*
 * One action, edited: a searchable list of the catalog's actions and the
 * chosen one's options (only those it has), as the Shortcuts and Menu
 * pages show them. Labels go in the caller's size group, so they line up
 * with the caller's own fields.
 */
#include <functional>

#include <gtk/gtk.h>

#include "waybox/action_catalog.hpp"

namespace wbconf {

/* A box holding the action's field and its options' fields; `changed`
 * gets the action each time it's edited. The editor lives as long as the
 * box. */
GtkWidget *action_editor_new(GtkSizeGroup *labels, const wb::BoundAction &action,
		std::function<void(const wb::BoundAction &)> changed);

/* For tests: the action chosen by name, an option set by name (as a
 * person would, through the widgets). */
void action_editor_select(GtkWidget *editor, const std::string &name);
void action_editor_set_option(GtkWidget *editor, const std::string &option,
		const std::string &value);

}  // namespace wbconf

#endif /* WBCONF_ACTION_EDITOR_HPP */
