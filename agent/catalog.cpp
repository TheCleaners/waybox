/*
 * What the hub offers agents (catalog.hpp).
 */
#include "catalog.hpp"

#include <set>

#include "waybox/ini_doc.hpp"

namespace wb::agent {

namespace {

const std::set<std::string> kSensitivities = {"read", "content", "act", "destroy", "exec",
		"capture", "input", "ui"};

/* Lowercase [a-z0-9_-], other characters as `_`, `__` collapsed. */
std::string clean(const std::string &s) {
	std::string out;
	for (char ch : s) {
		char c = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
			c = '_';
		if (c == '_' && !out.empty() && out.back() == '_')
			continue;
		out += c;
	}
	return out;
}

bool yes(const std::string &v, bool fallback) {
	if (v == "yes" || v == "true")
		return true;
	if (v == "no" || v == "false")
		return false;
	return fallback;
}

}  // namespace

Manifest read_manifest(std::string_view ini_text) {
	Manifest m;
	auto doc = ini::Document::parse(ini_text);
	m.id = doc.get("Provider", "Id").value_or("");
	m.name = doc.get("Provider", "Name").value_or(m.id);
	std::string version = doc.get("Provider", "Version").value_or("1");
	if (m.id.empty()) {
		m.error = "no [Provider] Id";
		return m;
	}
	if (version != "1") {
		m.error = "manifest version " + version + " (this hub reads 1)";
		return m;
	}
	for (const std::string &section : doc.sections()) {
		if (section.rfind("Tool ", 0) != 0)
			continue;
		std::string name = section.substr(5);
		ManifestTool t;
		t.sensitivity = doc.get(section, "Sensitivity").value_or("");
		t.description = doc.get(section, "Description").value_or("");
		t.untrusted = yes(doc.get(section, "Untrusted").value_or(""), true);
		t.reach = doc.get(section, "Reach").value_or("local");
		m.tools[name] = t;
	}
	return m;
}

std::string short_name(const std::string &id) {
	size_t dot = id.rfind('.');
	std::string s = clean(dot == std::string::npos ? id : id.substr(dot + 1));
	if (s.size() > 16)
		s.resize(16);
	return s;
}

std::string offered_name(const std::string &short_name, const std::string &tool) {
	std::string name = short_name + "__" + clean(tool);
	return name.size() <= 48 ? name : std::string();
}

bool reserved_short_name(const std::string &shrt, bool compositor) {
	if (shrt == "desktop" || shrt == "ui")
		return true;
	return shrt == "waybox" && !compositor;
}

std::vector<OfferedTool> merge_tools(const Manifest &m, const json::Array &live,
		std::vector<std::string> &notes, bool compositor) {
	std::vector<OfferedTool> out;
	std::string shrt = short_name(m.id);
	if (reserved_short_name(shrt, compositor)) {
		notes.push_back(m.id + ": its short name (" + shrt + ") is reserved: refused");
		return out;
	}
	for (const json::Value &def : live) {
		std::string tool = def.str("name");
		auto it = m.tools.find(tool);
		if (it == m.tools.end()) {
			notes.push_back(tool + ": not in the manifest: refused");
			continue;
		}
		const ManifestTool &mt = it->second;
		bool known = kSensitivities.count(mt.sensitivity) != 0;
		bool reads = mt.sensitivity == "read" || mt.sensitivity == "content" ||
				mt.sensitivity == "capture";
		bool destroys = mt.sensitivity == "destroy" || mt.sensitivity == "exec" ||
				mt.sensitivity == "input";
		bool outside = mt.reach == "outside" || mt.sensitivity == "exec" || mt.sensitivity == "input";
		/* Its own hints may not contradict the manifest. */
		if (const json::Value *a = def.get("annotations")) {
			auto hint = [&](const char *k) -> const bool * {
				const json::Value *v = a->get(k);
				return v != nullptr ? v->boolean() : nullptr;
			};
			const bool *ro = hint("readOnlyHint");
			const bool *ds = hint("destructiveHint");
			const bool *ow = hint("openWorldHint");
			if ((ro && known && *ro != reads) || (ds && known && *ds != destroys) ||
					(ow && *ow && !outside)) {
				notes.push_back(tool + ": its hints contradict its manifest: refused");
				continue;
			}
		}
		std::string name = offered_name(shrt, tool);
		if (name.empty()) {
			notes.push_back(tool + ": its name is too long to offer: refused");
			continue;
		}
		OfferedTool t;
		t.provider = m.id;
		t.tool = tool;
		t.name = name;
		t.manifest = mt;
		t.compositor = compositor;
		/* The manifest's description, its schemas, the hints the manifest
		 * implies (all three: MCP's defaults would otherwise apply). */
		t.definition = json::Object{
			{"name", name},
			{"description", mt.description},
			{"inputSchema", def.get("inputSchema") != nullptr ? *def.get("inputSchema")
					: json::Value(json::Object{{"type", "object"}})},
			{"annotations", json::Object{{"readOnlyHint", known ? reads : false},
					{"destructiveHint", known ? destroys : true},
					{"openWorldHint", outside}}},
		};
		if (const json::Value *o = def.get("outputSchema"))
			t.definition["outputSchema"] = *o;
		out.push_back(std::move(t));
	}
	return out;
}

Level preset_level(const std::string &preset, const OfferedTool &t) {
	const std::string &s = t.manifest.sensitivity;
	if (kSensitivities.count(s) == 0)
		return Level::Off;  /* unknown: the most guarded */
	if (preset == "full")
		return Level::Allow;
	if (preset == "trusted") {
		if (s == "read" || s == "content" || s == "act" || s == "ui")
			return Level::Allow;
		if (s == "destroy" || s == "exec" || s == "capture")
			return Level::Ask;
		return Level::Off;
	}
	/* safe (and anything else: the default) */
	if (s == "read")
		return Level::Allow;
	if (s == "content")
		return t.compositor && t.tool == "window_titles" ? Level::Allow : Level::Off;
	if (s == "ui")
		return Level::Ask;
	return Level::Off;
}

}  // namespace wb::agent
