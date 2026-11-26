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
	{Kind::Image, "image"}, {Kind::Button, "button"},
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
	if (const json::Value *c = v.get("choices"); c != nullptr && c->array() != nullptr)
		for (const json::Value &e : *c->array())
			if (std::string t = as_text(e); !t.empty())
				out.choices.push_back(t);
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
	if (!c.choices.empty()) {
		json::Array a;
		for (const std::string &s : c.choices)
			a.emplace_back(s);
		o["choices"] = std::move(a);
	}
	if (c.min) o["min"] = *c.min;
	if (c.max) o["max"] = *c.max;
	if (!c.visible) o["visible"] = false;
	if (!c.enabled) o["enabled"] = false;
	if (!c.problem.empty()) o["problem"] = c.problem;
	if (c.kind == Kind::Group) {
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
		collect(c.children, out);
	}
}

Control *find_in(std::vector<Control> &cs, const std::string &key) {
	for (Control &c : cs) {
		if (!c.key.empty() && c.key == key && c.kind != Kind::Group)
			return &c;
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
	/* No ".." tricks: only exactly a declared file. */
	if (p.empty() || p[0] != '/' || p.find("/../") != std::string::npos ||
			p.find("/./") != std::string::npos)
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

}  // namespace wb::page
