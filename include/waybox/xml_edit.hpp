#ifndef WB_XML_EDIT_HPP
#define WB_XML_EDIT_HPP

/*
 * Editing Openbox XML files in place (rc.xml, menu.xml) with libxml2:
 * new elements on lines of their own, removed ones with their indentation,
 * and <action> elements read and written as the editors have them. Shared
 * by RcDocument (wbconf.cpp) and MenuDocument (menu_doc.cpp).
 */
#include <optional>
#include <string>
#include <vector>

#include <libxml/tree.h>

#include "waybox/action_catalog.hpp"

namespace wb::xml_edit {

bool is_element(const xmlNode *n, const char *name);
std::optional<std::string> attr(const xmlNode *n, const char *name);
void set_attr(xmlNode *n, const char *name, const std::string &value);  /* "" removes it */

/* A new element at the end of `parent`, on its own line, indented one
 * level past it. */
xmlNode *add_child(xmlNode *parent, const char *name);
/* `node` (detached, or new) put before `before` among its siblings, on its
 * own line; nullptr `before`: at the end of `parent`. */
void insert_before(xmlNode *parent, xmlNode *node, xmlNode *before);
/* Removed (and freed), with the whitespace that indented it. */
void remove(xmlNode *node);
/* Detached (not freed), with the whitespace that indented it. */
void detach(xmlNode *node);

/* <action>s: simple options (elements holding only text); anything more
 * (If, ForEach, nested elements) advanced. */
std::vector<BoundAction> read_actions(xmlNode *parent);
/* `parent`'s actions replaced by `actions` (its other children kept). */
void write_actions(xmlNode *parent, const std::vector<BoundAction> &actions);

}  // namespace wb::xml_edit

#endif /* WB_XML_EDIT_HPP */
