#include "waybox/page_protocol.hpp"

#include <set>

namespace wb::page {

namespace {

struct KindName {
	Kind kind;
	const char *name;
};

const KindName kKinds[] = {
	{Kind::Group, "group"}, {Kind::Label, "label"}, {Kind::Text, "text"},
	{Kind::Number, "number"}, {Kind::Switch, "switch"}, {Kind::Choice, "choice"},
	{Kind::Color, "color"}, {Kind::File, "file"}, {Kind::Folder, "folder"},
	{Kind::Image, "image"}, {Kind::List, "list"}, {Kind::Sections, "sections"},
	{Kind::Font, "font"},
	{Kind::Button, "button"},
};

/* A value as text: a number without a needless ".0", a bool "on"/"". */
std::string as_text(const json::Value &v) {
	if (const std::string *s = v.string())
		return *s;
	if (const double *d = v.number()) {
		std::string t = json::dump(json::Value(*d));
		return t;
	}
	if (const bool *b = v.boolean())
		return *b ? "on" : "";
	return "";
}

bool read_control(const json::Value &v, int depth, Control &out, std::set<std::string> &keys,
		std::string *error) {
	auto fail = [error](const std::string &why) {
		if (error != nullptr)
			*error = why;
		return false;
	};
	if (v.object() == nullptr)
		return fail("a control isn't an object");
	std::string kind = v.str("kind");
	auto k = kind_from_name(kind);
	if (!k)
		return fail("unknown kind of control \"" + kind + "\"");
	out.kind = *k;
	out.key = v.str("key");
	out.label = v.str("label");
	out.description = v.str("description");
	out.unit = v.str("unit");
	out.hint = v.str("hint");
	if (const json::Value *val = v.get("value"))
		out.value = as_text(*val);
	/* A choice: "value", or ["value", "Label"], or {"value": .., "label": ..}. */
	if (const json::Value *c = v.get("choices"); c != nullptr && c->array() != nullptr)
		for (const json::Value &e : *c->array()) {
			std::string value, label;
			if (const json::Array *pair = e.array(); pair != nullptr && !pair->empty()) {
				value = as_text((*pair)[0]);
				if (pair->size() > 1)
					label = as_text((*pair)[1]);
			} else if (e.object() != nullptr) {
				value = e.str("value");
				label = e.str("label");
			} else {
				value = as_text(e);
			}
			if (value.empty())
				continue;
			out.choices.push_back(value);
			out.choice_labels.push_back(label);
		}
	out.format = v.str("format");
	if (out.kind == Kind::Font && !out.format.empty() && out.format != "pango" &&
			out.format != "fontconfig")
		return fail("the font \"" + out.key + "\": its format is pango or fontconfig, not " +
				out.format);
	if (const json::Value *b = v.get("wide"); b != nullptr && b->boolean() != nullptr)
		out.wide = *b->boolean();
	if (const json::Value *b = v.get("monospace"); b != nullptr && b->boolean() != nullptr)
		out.monospace = *b->boolean();
	if (const json::Value *n = v.get("min"); n != nullptr && n->number() != nullptr)
		out.min = *n->number();
	if (const json::Value *n = v.get("max"); n != nullptr && n->number() != nullptr)
		out.max = *n->number();
	if (const json::Value *b = v.get("visible"); b != nullptr && b->boolean() != nullptr)
		out.visible = *b->boolean();
	if (const json::Value *b = v.get("enabled"); b != nullptr && b->boolean() != nullptr)
		out.enabled = *b->boolean();
	out.problem = v.str("problem");
	if (is_setting(out.kind) || out.kind == Kind::Button) {
		if (out.key.empty())
			return fail(std::string("a ") + kind_name(out.kind) + " without a key");
		if (!keys.insert(out.key).second)
			return fail("two controls with the key \"" + out.key + "\"");
	}
	if (out.kind == Kind::Choice && out.choices.empty())
		return fail("the choice \"" + out.key + "\" has nothing to choose");
	if (out.kind == Kind::Sections) {
		/* Its fields: settings, their keys its own (each section has them). */
		std::set<std::string> field_keys;
		if (const json::Value *c = v.get("children"); c != nullptr && c->array() != nullptr)
			for (const json::Value &e : *c->array()) {
				Control child;
				if (!read_control(e, depth + 1, child, field_keys, error))
					return false;
				if (!is_setting(child.kind) || child.kind == Kind::Sections ||
						child.kind == Kind::List)
					return fail("the sections \"" + out.key + "\" can only have settings "
							"as fields (text, number, switch, choice, color...)");
				out.children.push_back(std::move(child));
			}
	}
	if (out.kind == Kind::Group) {
		if (depth >= 4)
			return fail("groups nested more than 4 deep");
		if (const json::Value *c = v.get("children"); c != nullptr && c->array() != nullptr)
			for (const json::Value &e : *c->array()) {
				Control child;
				if (!read_control(e, depth + 1, child, keys, error))
					return false;
				out.children.push_back(std::move(child));
			}
	}
	return true;
}

json::Value write_control(const Control &c) {
	json::Object o;
	o["kind"] = kind_name(c.kind);
	if (!c.key.empty()) o["key"] = c.key;
	if (!c.label.empty()) o["label"] = c.label;
	if (!c.description.empty()) o["description"] = c.description;
	if (is_setting(c.kind)) o["value"] = c.value;
	if (!c.unit.empty()) o["unit"] = c.unit;
	if (!c.hint.empty()) o["hint"] = c.hint;
	if (!c.format.empty()) o["format"] = c.format;
	if (!c.choices.empty()) {
		json::Array a;
		for (size_t i = 0; i < c.choices.size(); ++i) {
			const std::string label = i < c.choice_labels.size() ? c.choice_labels[i] : "";
			if (label.empty())
				a.emplace_back(c.choices[i]);
			else
				a.emplace_back(json::Array{json::Value(c.choices[i]), json::Value(label)});
		}
		o["choices"] = std::move(a);
	}
	if (c.wide) o["wide"] = true;
	if (c.monospace) o["monospace"] = true;
	if (c.min) o["min"] = *c.min;
	if (c.max) o["max"] = *c.max;
	if (!c.visible) o["visible"] = false;
	if (!c.enabled) o["enabled"] = false;
	if (!c.problem.empty()) o["problem"] = c.problem;
	if (c.kind == Kind::Group || c.kind == Kind::Sections) {
		json::Array a;
		for (const Control &ch : c.children)
			a.push_back(write_control(ch));
		o["children"] = std::move(a);
	}
	return o;
}

void collect(const std::vector<Control> &cs,
		std::vector<std::pair<std::string, std::string>> &out) {
	for (const Control &c : cs) {
		if (is_setting(c.kind))
			out.emplace_back(c.key, c.value);
		if (c.kind == Kind::Group)  /* a sections' fields are each section's */
			collect(c.children, out);
	}
}

Control *find_in(std::vector<Control> &cs, const std::string &key) {
	for (Control &c : cs) {
		if (!c.key.empty() && c.key == key && c.kind != Kind::Group)
			return &c;
		if (c.kind != Kind::Group)  /* a sections' fields aren't the page's */
			continue;
		if (Control *f = find_in(c.children, key))
			return f;
	}
	return nullptr;
}

}  // namespace

std::optional<Kind> kind_from_name(const std::string &name) {
	for (const KindName &k : kKinds)
		if (name == k.name)
			return k.kind;
	return std::nullopt;
}

const char *kind_name(Kind k) {
	for (const KindName &n : kKinds)
		if (n.kind == k)
			return n.name;
	return "text";
}

std::vector<Section> sections_from_text(const std::string &text) {
	std::vector<Section> out;
	auto v = json::parse(text);
	if (!v || v->array() == nullptr)
		return out;
	for (const json::Value &e : *v->array()) {
		if (e.object() == nullptr)
			continue;
		Section s;
		s.name = e.str("name");
		if (const json::Value *vals = e.get("values"); vals != nullptr && vals->object())
			for (const auto &[k, x] : *vals->object())
				if (std::string t = as_text(x); !t.empty())
					s.values[k] = t;
		out.push_back(std::move(s));
	}
	return out;
}

std::string sections_to_text(const std::vector<Section> &sections) {
	json::Array a;
	for (const Section &s : sections) {
		json::Object vals;
		for (const auto &[k, x] : s.values)
			if (!x.empty())
				vals[k] = x;
		a.push_back(json::Object{{"name", s.name}, {"values", std::move(vals)}});
	}
	return json::dump(a);
}

bool is_setting(Kind k) {
	return k != Kind::Group && k != Kind::Label && k != Kind::Button;
}

std::optional<Page> page_from_json(const json::Value &v, std::string *error) {
	const json::Value *cs = v.get("controls");
	if (cs == nullptr || cs->array() == nullptr) {
		if (error != nullptr)
			*error = "a page without \"controls\"";
		return std::nullopt;
	}
	Page p;
	std::set<std::string> keys;
	for (const json::Value &e : *cs->array()) {
		Control c;
		if (!read_control(e, 0, c, keys, error))
			return std::nullopt;
		p.controls.push_back(std::move(c));
	}
	return p;
}

json::Value page_to_json(const Page &p) {
	json::Array a;
	for (const Control &c : p.controls)
		a.push_back(write_control(c));
	return json::Object{{"controls", std::move(a)}};
}

std::vector<std::pair<std::string, std::string>> page_values(const Page &p) {
	std::vector<std::pair<std::string, std::string>> out;
	collect(p.controls, out);
	return out;
}

Control *find_control(Page &p, const std::string &key) {
	return find_in(p.controls, key);
}

bool apply_update(Page &p, const std::string &key, const json::Value &fields) {
	Control *c = find_control(p, key);
	if (c == nullptr || fields.object() == nullptr)
		return false;
	if (const json::Value *v = fields.get("value"); v != nullptr && is_setting(c->kind))
		c->value = as_text(*v);
	if (const json::Value *b = fields.get("visible"); b != nullptr && b->boolean() != nullptr)
		c->visible = *b->boolean();
	if (const json::Value *b = fields.get("enabled"); b != nullptr && b->boolean() != nullptr)
		c->enabled = *b->boolean();
	if (const json::Value *s = fields.get("problem"))
		c->problem = s->string() != nullptr ? *s->string() : "";
	if (const json::Value *s = fields.get("label"); s != nullptr && s->string() != nullptr)
		c->label = *s->string();
	return true;
}

std::string expand_home(const std::string &path, const std::string &home) {
	if (path == "~")
		return home;
	if (path.rfind("~/", 0) == 0 && !home.empty())
		return home + path.substr(1);
	return path;
}

bool path_allowed(const std::string &path, const std::vector<std::string> &files,
		const std::string &home) {
	std::string p = expand_home(path, home);
	/* Only exactly a declared file, written plainly: no "." or ".." steps,
	 * no "//" (each another spelling of a path, past the checks that name
	 * what a file does, such as runs at login). */
	if (p.empty() || p[0] != '/' || p.find("/../") != std::string::npos ||
			p.find("/./") != std::string::npos || p.find("//") != std::string::npos ||
			p.ends_with("/.") || p.ends_with("/..") || p.ends_with("/"))
		return false;
	for (const std::string &f : files)
		if (expand_home(f, home) == p)
			return true;
	return false;
}

bool run_allowed(const std::vector<std::string> &argv, const std::vector<std::string> &runs) {
	if (argv.empty() || argv[0].empty() || argv[0].find('/') != std::string::npos)
		return false;
	for (const std::string &r : runs)
		if (r == argv[0])
			return true;
	return false;
}

std::map<std::string, std::string> parse_approvals(const std::string &text) {
	std::map<std::string, std::string> out;
	size_t start = 0;
	while (start < text.size()) {
		size_t nl = text.find('\n', start);
		std::string line = text.substr(start, nl == std::string::npos ? std::string::npos
				: nl - start);
		start = nl == std::string::npos ? text.size() : nl + 1;
		size_t b = line.find_first_not_of(" \t");
		if (b == std::string::npos || line[b] == '#')
			continue;
		size_t sp = line.find_first_of(" \t", b);
		if (sp == std::string::npos)
			continue;
		size_t h = line.find_first_not_of(" \t", sp);
		if (h == std::string::npos)
			continue;
		size_t e = line.find_first_of(" \t\r", h);
		out[line.substr(b, sp - b)] = line.substr(h, e == std::string::npos ? std::string::npos
				: e - h);
	}
	return out;
}

std::string approvals_with(const std::string &text, const std::string &id,
		const std::string &hash) {
	std::string out, line;
	bool done = false;
	size_t start = 0;
	while (start < text.size()) {
		size_t nl = text.find('\n', start);
		line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
		start = nl == std::string::npos ? text.size() : nl + 1;
		size_t b = line.find_first_not_of(" \t");
		bool mine = b != std::string::npos && line[b] != '#' &&
				line.compare(b, id.size(), id) == 0 &&
				(line.size() == b + id.size() || line[b + id.size()] == ' ' ||
				 line[b + id.size()] == '\t');
		if (mine) {
			if (!done)
				out += id + " " + hash + "\n";
			done = true;
		} else {
			out += line + "\n";
		}
	}
	if (!done) {
		if (out.empty())
			out = "# Lua pages from your own integrations folder, allowed to run: id and the\n"
					"# SHA-256 of its integration file and script (changing either asks again).\n";
		out += id + " " + hash + "\n";
	}
	return out;
}

}  // namespace wb::page
