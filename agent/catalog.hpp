#ifndef WB_AGENT_CATALOG_HPP
#define WB_AGENT_CATALOG_HPP

/*
 * What the hub offers agents (doc/agent-tools.md: "The manifest",
 * "Naming", "Sensitivity and levels"), without I/O:
 * - a provider's manifest, read;
 * - its live tools merged with it: only tools the manifest declares, with
 *   the manifest's description and the annotations its sensitivity
 *   implies; a tool whose own hints contradict the manifest is refused;
 * - each tool's name as agents see it (`waybox__list_windows`);
 * - the level a preset gives a tool.
 * Pinning a provider's definitions at approval comes with approval
 * itself (other providers); the compositor's are taken as listed. Pure;
 * unit-tested.
 */
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "waybox/json.hpp"

namespace wb::agent {

struct ManifestTool {
	std::string sensitivity, description;
	bool untrusted = true;
	std::string reach = "local";
	std::vector<std::string> secret;  /* arguments redacted in the audit */
};

struct Manifest {
	std::string id, name, error;  /* error: why it can't be used */
	std::map<std::string, ManifestTool> tools;
};
Manifest read_manifest(std::string_view ini_text);

/* A tool as the hub offers it. */
struct OfferedTool {
	std::string provider;  /* the manifest's Id */
	std::string tool;      /* its own name */
	std::string name;      /* as agents see it */
	ManifestTool manifest;
	json::Object definition;  /* as agents get it */
	bool compositor = false;  /* the compositor's own provider */
};

/* `org.waybar.Waybar` → `waybar`: the last part of an Id, lowercased,
 * other characters as `_`, `__` collapsed, at most 16 characters. */
std::string short_name(const std::string &id);
/* `<short>__<tool>`, lowercase [a-z0-9_-], `__` only as the separator;
 * "" if longer than 48 (refused: the hashed form comes with approval). */
std::string offered_name(const std::string &short_name, const std::string &tool);

/* The live tools, as the manifest allows them; what's refused, and why, in
 * `notes`. `compositor`: the compositor's own provider (named in Hello),
 * the only one that may have its short name; `desktop` and `ui` are the
 * hub's own. */
std::vector<OfferedTool> merge_tools(const Manifest &m, const json::Array &live,
		std::vector<std::string> &notes, bool compositor);
/* Short names no other provider may take. */
bool reserved_short_name(const std::string &short_name, bool compositor);

enum class Level { Off, Ask, Allow };
/* A preset's level for a tool ("safe", "trusted", "full"); an unknown
 * sensitivity is off. */
Level preset_level(const std::string &preset, const OfferedTool &t);

}  // namespace wb::agent

#endif /* WB_AGENT_CATALOG_HPP */
