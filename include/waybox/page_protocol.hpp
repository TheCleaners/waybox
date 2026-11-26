#ifndef WB_PAGE_PROTOCOL_HPP
#define WB_PAGE_PROTOCOL_HPP

/*
 * Settings pages from another process (waybox-integration(5), Page=NAME.lua):
 * a page runner (wbconf-lua) describes a page, and wbconf draws it with its
 * own controls. They talk in JSON lines, one object per line.
 *
 * wbconf -> runner:
 *   {"open": {"script": PATH, "files": [...], "runs": [...], "home": DIR}}
 *   {"change": {"key": KEY, "value": TEXT}}
 *   {"click": ID}
 *   {"save": {KEY: TEXT, ...}}
 * runner -> wbconf, any number of these, then {"done": true}:
 *   {"page": PAGE}                         (answering open)
 *   {"update": {KEY: {"value"|"visible"|"enabled"|"problem": ...}}}
 *   {"write": {"path": PATH, "text": TEXT}}  (to do on save)
 *   {"run": [ARGV...]}
 *   {"error": {"line": N, "message": TEXT}}
 *
 * Values are text, as an integration option's are: a number as written,
 * a switch "on" or "". Not tied to Lua: any program speaking this can
 * describe a page. Pure; unit-tested.
 */
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "waybox/json.hpp"

namespace wb::page {

enum class Kind {
	Group,    /* a titled group of the controls in it */
	Label,    /* text */
	Text, Number, Switch, Choice, Color, File, Folder, Image,  /* settings */
	Button,   /* a button: "click" with its key */
};

struct Control {
	Kind kind = Kind::Text;
	std::string key;          /* a setting's key, a button's id */
	std::string label;        /* a group's title, a label's text */
	std::string description;
	std::string value;        /* settings: as text */
	std::string unit, hint;
	std::vector<std::string> choices;  /* Choice: the values */
	std::vector<std::string> choice_labels;  /* as shown ("" : the value) */
	bool wide = false;                 /* Text: the row's whole width */
	std::optional<double> min, max;    /* Number */
	bool visible = true, enabled = true;
	std::string problem;      /* shown under it while not empty */
	std::vector<Control> children;     /* Group */
	bool operator==(const Control &) const = default;
};

struct Page {
	std::vector<Control> controls;
	bool operator==(const Page &) const = default;
};

/* "choice" -> Kind::Choice; nullopt for an unknown name. */
std::optional<Kind> kind_from_name(const std::string &name);
const char *kind_name(Kind k);
/* A setting (with a value), as opposed to a group, label or button. */
bool is_setting(Kind k);

/* From/to the "page" message's JSON. A page that can't be drawn (an
 * unknown kind, a setting or button without a key, two with the same key,
 * groups nested more than 4 deep) is nullopt, with why in `error`. */
std::optional<Page> page_from_json(const json::Value &v, std::string *error = nullptr);
json::Value page_to_json(const Page &p);

/* Every setting's key -> value, depth first. */
std::vector<std::pair<std::string, std::string>> page_values(const Page &p);
/* The control with that key (a setting or a button), or nullptr. */
Control *find_control(Page &p, const std::string &key);

/* An "update" message's fields applied to the page; false (nothing
 * changed) for an unknown key. */
bool apply_update(Page &p, const std::string &key, const json::Value &fields);

/* What a page may touch (from the integration file): a write's path must
 * be one of `files` ("~/" expanded with `home`); a run's program (its
 * first word, without a folder) one of `runs`. */
bool path_allowed(const std::string &path, const std::vector<std::string> &files,
		const std::string &home);
bool run_allowed(const std::vector<std::string> &argv, const std::vector<std::string> &runs);
std::string expand_home(const std::string &path, const std::string &home);

/* Approval: a page from the user's own integrations folder (where files
 * from anywhere land) runs only once allowed; pages installed with Waybox
 * or by packages (the data folders) are trusted. */
bool page_trusted(const std::string &dir, const std::string &user_dir);

/* The approvals file: "ID SHA256" lines (# comments, blank lines
 * ignored); an id's approval holds while its hash (of the integration file
 * and its script together) is the same. */
std::map<std::string, std::string> parse_approvals(const std::string &text);
/* The file's text with `id` approved for `hash`: its line replaced, or
 * added; the rest kept as it was. */
std::string approvals_with(const std::string &text, const std::string &id,
		const std::string &hash);

}  // namespace wb::page

#endif
