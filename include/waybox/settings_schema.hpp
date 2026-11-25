#ifndef WB_SETTINGS_SCHEMA_HPP
#define WB_SETTINGS_SCHEMA_HPP

/*
 * wbconf's settings schema: every setting it shows, described in JSON
 * (data/wbconf/settings.json; more files may add categories and settings),
 * the way VS Code's settings UI is generated from contributed schemas.
 */
#include <string>
#include <string_view>
#include <vector>

#include "waybox/wbconf.hpp"

namespace wb {

enum class SettingType { Bool, Int, Enum, String, Color, Font, Lines, Custom };

struct SettingChoice {
	std::string value;  /* "" in a choice: not set (the device's or system default) */
	std::string label;
	bool operator==(const SettingChoice &) const = default;
};

struct SettingDef {
	std::string id;           /* "waybox.switcher.icons" */
	std::string section;      /* a heading within its category; may be empty */
	std::string title;
	std::string description;
	std::string keywords;     /* extra words search finds it by */
	SettingType type = SettingType::String;
	std::string def;          /* the default, as rc.xml writes it ("yes", "10") */
	int min = 0, max = 0;
	int scale = 1;            /* shown value = stored / scale (seconds as minutes) */
	std::string unit;
	std::string hint;         /* placeholder text */
	std::vector<SettingChoice> choices;
	RcLocation rc;
	std::string widget;       /* Custom: which editor ("theme", "monitors"...) */
	std::string group;        /* settings in a row with the same group share one
	                           * line: "Margins" (Top, Bottom...) */
	bool operator==(const SettingDef &) const = default;
};

struct SettingCategory {
	std::string id;
	std::string title;
	std::string description;
	std::vector<SettingDef> settings;
};

/* Add one schema file's categories and settings to `into`: a category with
 * an id already there gets the settings appended; a setting whose id is
 * already defined is skipped. False (with `error`) if the JSON is invalid. */
bool merge_settings_schema(std::vector<SettingCategory> &into, std::string_view json,
		std::string *error = nullptr);

/* Every *.json in each of `dirs` (in order, files sorted by name), merged. */
std::vector<SettingCategory> load_settings_schema(const std::vector<std::string> &dirs,
		std::vector<std::string> *errors = nullptr);

/* Whether a setting matches a search: every word must be in its title,
 * description, keywords, id, section or category title (ignoring case);
 * "@modified" keeps only settings set in rc.xml, "@id:text" matches ids. */
bool setting_matches(const SettingDef &s, std::string_view category_title,
		std::string_view query, bool modified);

/* "yes"/"true"/"on"/"1" (any case) */
bool setting_true(std::string_view value);

}  // namespace wb

#endif
