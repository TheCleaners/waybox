/*
 * The settings schema (see settings_schema.hpp).
 */
#include "waybox/settings_schema.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <json-glib/json-glib.h>

namespace fs = std::filesystem;

namespace wb {

namespace {

std::string str(JsonObject *o, const char *key) {
	if (o == nullptr || !json_object_has_member(o, key))
		return {};
	JsonNode *n = json_object_get_member(o, key);
	if (!JSON_NODE_HOLDS_VALUE(n))
		return {};
	if (json_node_get_value_type(n) == G_TYPE_STRING)
		return json_node_get_string(n);
	if (json_node_get_value_type(n) == G_TYPE_INT64)
		return std::to_string(json_node_get_int(n));
	if (json_node_get_value_type(n) == G_TYPE_BOOLEAN)
		return json_node_get_boolean(n) ? "yes" : "no";
	return {};
}

int num(JsonObject *o, const char *key, int fallback) {
	if (o == nullptr || !json_object_has_member(o, key))
		return fallback;
	JsonNode *n = json_object_get_member(o, key);
	if (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_INT64)
		return static_cast<int>(json_node_get_int(n));
	return fallback;
}

SettingType type_of(const std::string &t) {
	if (t == "bool") return SettingType::Bool;
	if (t == "int") return SettingType::Int;
	if (t == "enum") return SettingType::Enum;
	if (t == "font") return SettingType::Font;
	if (t == "lines") return SettingType::Lines;
	if (t == "custom") return SettingType::Custom;
	return SettingType::String;
}

std::string lower(std::string_view s) {
	std::string out(s);
	std::transform(out.begin(), out.end(), out.begin(),
			[](unsigned char c) { return std::tolower(c); });
	return out;
}

}  // namespace

bool merge_settings_schema(std::vector<SettingCategory> &into, std::string_view json,
		std::string *error) {
	JsonParser *parser = json_parser_new();
	GError *err = nullptr;
	if (!json_parser_load_from_data(parser, json.data(), static_cast<gssize>(json.size()), &err)) {
		if (error)
			*error = err ? err->message : "invalid JSON";
		g_clear_error(&err);
		g_object_unref(parser);
		return false;
	}
	JsonNode *root = json_parser_get_root(parser);
	JsonObject *top = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : nullptr;
	JsonArray *cats = top && json_object_has_member(top, "categories")
		? json_object_get_array_member(top, "categories") : nullptr;
	if (cats == nullptr) {
		if (error)
			*error = "no \"categories\" list";
		g_object_unref(parser);
		return false;
	}
	auto defined = [&into](const std::string &id) {
		for (const SettingCategory &c : into)
			for (const SettingDef &s : c.settings)
				if (s.id == id)
					return true;
		return false;
	};
	for (guint i = 0; i < json_array_get_length(cats); i++) {
		JsonObject *co = json_array_get_object_element(cats, i);
		std::string cid = str(co, "id");
		if (cid.empty())
			continue;
		SettingCategory *cat = nullptr;
		for (SettingCategory &c : into)
			if (c.id == cid)
				cat = &c;
		if (cat == nullptr) {
			into.push_back({cid, str(co, "title"), {}});
			cat = &into.back();
			if (cat->title.empty())
				cat->title = cid;
		}
		JsonArray *settings = json_object_has_member(co, "settings")
			? json_object_get_array_member(co, "settings") : nullptr;
		for (guint j = 0; settings && j < json_array_get_length(settings); j++) {
			JsonObject *so = json_array_get_object_element(settings, j);
			SettingDef s;
			s.id = str(so, "id");
			if (s.id.empty() || defined(s.id))
				continue;
			s.section = str(so, "section");
			s.title = str(so, "title");
			s.description = str(so, "description");
			s.keywords = str(so, "keywords");
			s.type = type_of(str(so, "type"));
			s.def = str(so, "default");
			s.min = num(so, "min", 0);
			s.max = num(so, "max", 1000000);
			s.scale = std::max(1, num(so, "scale", 1));
			s.unit = str(so, "unit");
			s.hint = str(so, "hint");
			s.widget = str(so, "widget");
			if (so && json_object_has_member(so, "choices")) {
				JsonArray *ch = json_object_get_array_member(so, "choices");
				for (guint k = 0; ch && k < json_array_get_length(ch); k++) {
					JsonObject *c = json_array_get_object_element(ch, k);
					s.choices.push_back({str(c, "value"), str(c, "label")});
					if (s.choices.back().label.empty())
						s.choices.back().label = s.choices.back().value;
				}
			}
			if (so && json_object_has_member(so, "rc")) {
				JsonObject *rc = json_object_get_object_member(so, "rc");
				s.rc = {str(rc, "path"), str(rc, "attr"), str(rc, "font"), str(rc, "item")};
			}
			if (s.title.empty())
				s.title = s.id;
			/* A setting needs a place in rc.xml, unless an editor of its
			 * own handles it. */
			if (s.type != SettingType::Custom && s.rc.path.empty() && s.rc.font.empty())
				continue;
			cat->settings.push_back(std::move(s));
		}
	}
	g_object_unref(parser);
	return true;
}

std::vector<SettingCategory> load_settings_schema(const std::vector<std::string> &dirs,
		std::vector<std::string> *errors) {
	std::vector<SettingCategory> out;
	for (const std::string &dir : dirs) {
		std::error_code ec;
		std::vector<fs::path> files;
		fs::directory_iterator it(dir, ec), end;
		for (; !ec && it != end; it.increment(ec))
			if (it->path().extension() == ".json")
				files.push_back(it->path());
		std::sort(files.begin(), files.end());
		for (const fs::path &f : files) {
			std::ifstream in(f);
			std::stringstream text;
			text << in.rdbuf();
			std::string err;
			if (!merge_settings_schema(out, text.str(), &err) && errors)
				errors->push_back(f.string() + ": " + err);
		}
	}
	/* Categories without settings (all skipped) aren't shown. */
	out.erase(std::remove_if(out.begin(), out.end(),
			[](const SettingCategory &c) { return c.settings.empty(); }), out.end());
	return out;
}

bool setting_matches(const SettingDef &s, std::string_view category_title,
		std::string_view query, bool modified) {
	std::string hay = lower(s.title + "\n" + s.description + "\n" + s.keywords + "\n" +
			s.id + "\n" + s.section + "\n" + std::string(category_title));
	std::istringstream words{std::string(query)};
	std::string w;
	while (words >> w) {
		std::string lw = lower(w);
		if (lw == "@modified") {
			if (!modified)
				return false;
		} else if (lw.rfind("@id:", 0) == 0) {
			if (lower(s.id).find(lw.substr(4)) == std::string::npos)
				return false;
		} else if (hay.find(lw) == std::string::npos) {
			return false;
		}
	}
	return true;
}

bool setting_true(std::string_view value) {
	std::string v = lower(value);
	return v == "yes" || v == "true" || v == "on" || v == "1";
}

}  // namespace wb
