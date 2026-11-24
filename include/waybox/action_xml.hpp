#ifndef WB_ACTION_XML_HPP
#define WB_ACTION_XML_HPP

#include <optional>
#include <vector>

#include <libxml/tree.h>

#include "waybox/action.hpp"

namespace wb {

/* Parse one <action name="..."> element (rc.xml key/mouse bindings and
 * menu.xml items share this): the action name plus its options --
 * <command>/<execute>/<menu> for actions that take a command, and
 * <to>/<wrap>/<follow> for the desktop actions. Returns std::nullopt for an
 * unknown action name. */
std::optional<Action> parse_action_xml(xmlNode *action_node);

/* Append every <action> among `first` and its siblings to `out`, in document
 * order. With `recurse`, also look inside other elements (rc.xml mousebinds
 * wrap their actions loosely); nested <keybind>s are never entered. */
void collect_actions_xml(xmlNode *first, std::vector<Action> &out,
		bool recurse = false);

}  // namespace wb

#endif /* WB_ACTION_XML_HPP */
