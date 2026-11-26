/*
 * An integration's settings page written in Lua (Page=NAME.lua): run by
 * wbconf-lua in a process of its own (page_runner.cpp), drawn here with
 * wbconf's own controls, as the typed options are.
 *
 * Every message is answered before wbconf goes on (a page has a short time
 * budget per call; past 3 s it's taken as stuck and shown unavailable).
 * Its changes count as wbconf's changes, listed and saved with Save &
 * Apply: then its writes are made (only files in Files=, each backed up as
 * FILE.bak) and its programs run (only Runs=), checked again here.
 */
#include "wbconf_ui.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "waybox/json.hpp"
#include "waybox/page_protocol.hpp"

namespace wbconf {

namespace json = wb::json;
namespace page = wb::page;

namespace {

constexpr int kAnswerMs = 3000;

std::string runner_path() {
	if (const char *e = getenv("WBCONF_LUA"); e != nullptr && *e)
		return e;
#ifdef WB_PAGE_RUNNER
	return WB_PAGE_RUNNER;
#else
	return "";
#endif
}

wb::OptionType option_type(page::Kind k) {
	switch (k) {
	case page::Kind::Number: return wb::OptionType::Number;
	case page::Kind::Switch: return wb::OptionType::Switch;
	case page::Kind::Choice: return wb::OptionType::Choice;
	case page::Kind::Color: return wb::OptionType::Color;
	case page::Kind::File: return wb::OptionType::File;
	case page::Kind::Folder: return wb::OptionType::Folder;
	case page::Kind::Image: return wb::OptionType::Image;
	default: return wb::OptionType::Text;
	}
}

}  // namespace

struct LuaPage {
	Ui *ui = nullptr;
	Ui::ProgramRow *row = nullptr;
	const wb::Integration *in = nullptr;
	pid_t pid = -1;
	int to = -1, from = -1;
	std::string pending;           /* read, not yet a line */
	bool dead = false;
	page::Page model;
	std::map<std::string, std::string> saved;  /* values as last saved */
	/* Each setting's control, and what's around it. */
	struct Field {
		std::unique_ptr<wb::IntegrationOption> opt;  /* the control keeps a pointer */
		std::unique_ptr<OptionField> field;
		GtkWidget *row = nullptr;        /* label + control, shown or not */
		GtkLabel *problem = nullptr;
		GtkWidget *button = nullptr;     /* buttons */
	};
	std::map<std::string, Field> fields;
	GtkWidget *box = nullptr;
	GtkLabel *error = nullptr;
	bool syncing = false;

	/* The runner gone: its pipes closed, the process ended and reaped. */
	void stop() {
		if (to >= 0)
			close(to);
		if (from >= 0)
			close(from);
		to = from = -1;
		if (pid > 0) {
			kill(pid, SIGKILL);
			waitpid(pid, nullptr, 0);
			pid = -1;
		}
	}

	~LuaPage() {
		stop();
	}

	void fail(const std::string &why) {
		if (error != nullptr) {
			gtk_label_set_text(error, why.c_str());
			gtk_widget_set_visible(GTK_WIDGET(error), TRUE);
		}
		g_warning("%s's page: %s", in->name.c_str(), why.c_str());
	}

	/* One message out; the replies in, up to {"done": true}. */
	std::vector<json::Value> ask(const json::Value &msg) {
		std::vector<json::Value> out;
		if (dead)
			return out;
		std::string line = json::dump(msg) + "\n";
		if (write(to, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
			dead = true;
			fail(_("The page stopped."));
			return out;
		}
		gint64 end = g_get_monotonic_time() / 1000 + kAnswerMs;
		while (true) {
			size_t nl;
			while ((nl = pending.find('\n')) != std::string::npos) {
				std::string one = pending.substr(0, nl);
				pending.erase(0, nl + 1);
				auto v = json::parse(one);
				if (!v)
					continue;
				if (v->get("done") != nullptr)
					return out;
				out.push_back(std::move(*v));
			}
			gint64 left = end - g_get_monotonic_time() / 1000;
			struct pollfd p = {from, POLLIN, 0};
			if (left <= 0 || poll(&p, 1, static_cast<int>(left)) <= 0) {
				dead = true;
				stop();
				fail(_("The page didn't answer in time, and was stopped."));
				return out;
			}
			char buf[4096];
			ssize_t n = read(from, buf, sizeof(buf));
			if (n <= 0) {
				dead = true;
				if (out.empty())
					fail(_("The page stopped."));
				return out;
			}
			pending.append(buf, static_cast<size_t>(n));
		}
	}

	/* Errors shown at the page's top: "page.lua:17: message". */
	void show_errors(const std::vector<json::Value> &replies) {
		std::string text;
		for (const json::Value &r : replies)
			if (const json::Value *e = r.get("error")) {
				const double *line = e->get("line") ? e->get("line")->number() : nullptr;
				std::string where = in->page;
				if (line != nullptr && *line > 0)
					where += ":" + std::to_string(static_cast<int>(*line));
				if (!text.empty())
					text += "\n";
				text += where + ": " + e->str("message");
			}
		/* A page that stopped keeps saying why. */
		if (error == nullptr || (text.empty() && dead))
			return;
		gtk_label_set_text(error, text.c_str());
		gtk_widget_set_visible(GTK_WIDGET(error), !text.empty());
	}

	void show_control(const page::Control &c) {
		auto it = fields.find(c.key);
		if (it == fields.end())
			return;
		Field &f = it->second;
		if (f.row != nullptr) {
			gtk_widget_set_visible(f.row, c.visible);
			gtk_widget_set_sensitive(f.row, c.enabled);
		}
		if (f.field != nullptr && option_field_value(*f.field) != c.value) {
			syncing = true;
			set_option_field_value(*f.field, c.value);
			syncing = false;
		}
		if (f.problem != nullptr) {
			gtk_label_set_text(f.problem, c.problem.c_str());
			gtk_widget_set_visible(GTK_WIDGET(f.problem), !c.problem.empty());
		}
		if (f.button != nullptr)
			gtk_button_set_label(GTK_BUTTON(f.button), c.label.c_str());
	}

	/* Updates and runs from a reply (writes only from a save's). */
	void handle(const std::vector<json::Value> &replies) {
		for (const json::Value &r : replies) {
			if (const json::Value *u = r.get("update"); u != nullptr && u->object()) {
				for (const auto &[key, fields_v] : *u->object())
					if (page::apply_update(model, key, fields_v))
						if (page::Control *c = page::find_control(model, key))
							show_control(*c);
			} else if (const json::Value *run = r.get("run"); run != nullptr && run->array()) {
				std::vector<std::string> argv;
				for (const json::Value &w : *run->array())
					if (w.string() != nullptr)
						argv.push_back(*w.string());
				run_program(argv);
			}
		}
		show_errors(replies);
		refresh_save_state(ui);
	}

	void run_program(const std::vector<std::string> &argv) {
		if (!page::run_allowed(argv, in->runs)) {
			g_warning("%s's page: running %s isn't allowed (Runs=)", in->name.c_str(),
					argv.empty() ? "nothing" : argv[0].c_str());
			return;
		}
		std::vector<char *> args;
		for (const std::string &a : argv)
			args.push_back(const_cast<char *>(a.c_str()));
		args.push_back(nullptr);
		GError *err = nullptr;
		if (!g_spawn_async(nullptr, args.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr,
				nullptr, nullptr, &err)) {
			fail(std::string(_("Can't run ")) + argv[0] + ": " + err->message);
			g_error_free(err);
		}
	}

	void changed(const std::string &key) {
		if (syncing)
			return;
		auto it = fields.find(key);
		page::Control *c = page::find_control(model, key);
		if (it == fields.end() || c == nullptr || it->second.field == nullptr)
			return;
		c->value = option_field_value(*it->second.field);
		handle(ask(json::Object{{"change", json::Object{{"key", key}, {"value", c->value}}}}));
	}

	void build(GtkWidget *parent, const std::vector<page::Control> &controls) {
		GtkWidget *grid = nullptr;
		int r = 0;
		for (const page::Control &c : controls) {
			if (c.kind == page::Kind::Group) {
				GtkWidget *title = gtk_label_new(c.label.c_str());
				gtk_widget_add_css_class(title, "heading");
				gtk_label_set_xalign(GTK_LABEL(title), 0);
				gtk_widget_set_margin_top(title, 6);
				gtk_box_append(GTK_BOX(parent), title);
				GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
				gtk_widget_set_margin_start(inner, 12);
				gtk_box_append(GTK_BOX(parent), inner);
				build(inner, c.children);
				grid = nullptr;
				continue;
			}
			if (c.kind == page::Kind::Label) {
				GtkWidget *l = gtk_label_new(c.label.c_str());
				gtk_label_set_wrap(GTK_LABEL(l), TRUE);
				gtk_label_set_xalign(GTK_LABEL(l), 0);
				gtk_box_append(GTK_BOX(parent), l);
				grid = nullptr;
				continue;
			}
			Field &f = fields[c.key];
			if (c.kind == page::Kind::Button) {
				f.button = gtk_button_new_with_label(c.label.c_str());
				gtk_widget_set_halign(f.button, GTK_ALIGN_START);
				g_object_set_data_full(G_OBJECT(f.button), "wb-key", g_strdup(c.key.c_str()),
						g_free);
				g_signal_connect(f.button, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer d) {
					auto *self = static_cast<LuaPage *>(d);
					const char *k = static_cast<const char *>(g_object_get_data(G_OBJECT(b),
							"wb-key"));
					self->handle(self->ask(json::Object{{"click", std::string(k)}}));
				}), this);
				if (!c.description.empty())
					gtk_widget_set_tooltip_text(f.button, c.description.c_str());
				f.row = f.button;
				gtk_box_append(GTK_BOX(parent), f.button);
				grid = nullptr;
				show_control(c);
				continue;
			}
			/* A setting, in a grid of label and control. */
			if (grid == nullptr)
				grid = section_grid(parent);
			f.opt = std::make_unique<wb::IntegrationOption>();
			f.opt->id = c.key;
			f.opt->label = c.label.empty() ? c.key : c.label;
			f.opt->type = option_type(c.kind);
			f.opt->choices = c.choices;
			f.opt->min = c.min;
			f.opt->max = c.max;
			f.opt->hint = c.hint;
			std::string key = c.key;
			f.field = make_option_field(*f.opt, [this, key] { changed(key); });
			GtkWidget *label = grid_label(grid, r, (f.opt->label + ":").c_str());
			GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
			GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
			gtk_box_append(GTK_BOX(line), f.field->widget);
			if (!c.unit.empty()) {
				GtkWidget *u = gtk_label_new(c.unit.c_str());
				gtk_widget_add_css_class(u, "dim-label");
				gtk_box_append(GTK_BOX(line), u);
			}
			gtk_box_append(GTK_BOX(cell), line);
			if (!c.description.empty()) {
				GtkWidget *d = gtk_label_new(c.description.c_str());
				gtk_label_set_wrap(GTK_LABEL(d), TRUE);
				gtk_label_set_xalign(GTK_LABEL(d), 0);
				gtk_widget_add_css_class(d, "dim-label");
				gtk_box_append(GTK_BOX(cell), d);
			}
			f.problem = GTK_LABEL(gtk_label_new(""));
			gtk_label_set_xalign(f.problem, 0);
			gtk_label_set_wrap(f.problem, TRUE);
			gtk_widget_add_css_class(GTK_WIDGET(f.problem), "warning");
			gtk_box_append(GTK_BOX(cell), GTK_WIDGET(f.problem));
			grid_control(grid, r++, cell);
			/* Shown or hidden with its label. */
			f.row = cell;
			g_object_set_data(G_OBJECT(cell), "wb-label", label);
			g_signal_connect(cell, "notify::visible", G_CALLBACK(+[](GObject *o, GParamSpec *,
					gpointer) {
				auto *l = static_cast<GtkWidget *>(g_object_get_data(o, "wb-label"));
				if (l != nullptr)
					gtk_widget_set_visible(l, gtk_widget_get_visible(GTK_WIDGET(o)));
			}), nullptr);
			show_control(c);
		}
	}

	bool start() {
		std::string runner = runner_path();
		if (runner.empty() || access(runner.c_str(), X_OK) != 0) {
			fail(_("Pages written in Lua need wbconf-lua, which comes with Waybox built "
					"with Lua."));
			return false;
		}
		int in_pipe[2], out_pipe[2];
		if (pipe(in_pipe) != 0)
			return false;
		if (pipe(out_pipe) != 0) {
			close(in_pipe[0]);
			close(in_pipe[1]);
			return false;
		}
		pid = fork();
		if (pid == 0) {
			dup2(in_pipe[0], 0);
			dup2(out_pipe[1], 1);
			for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1]})
				close(fd);
			execl(runner.c_str(), runner.c_str(), static_cast<char *>(nullptr));
			_exit(127);
		}
		close(in_pipe[0]);
		close(out_pipe[1]);
		to = in_pipe[1];
		from = out_pipe[0];
		fcntl(to, F_SETFD, FD_CLOEXEC);
		fcntl(from, F_SETFD, FD_CLOEXEC);
		if (pid < 0) {
			fail(_("Can't start the page."));
			return false;
		}
		const char *home = g_get_home_dir();
		json::Array files, runs;
		for (const std::string &f : in->files)
			files.emplace_back(f);
		for (const std::string &r : in->runs)
			runs.emplace_back(r);
		auto replies = ask(json::Object{{"open", json::Object{
				{"script", wb::lua_page_path(*in)}, {"home", std::string(home ? home : "")},
				{"files", std::move(files)}, {"runs", std::move(runs)}}}});
		for (const json::Value &r : replies)
			if (const json::Value *p = r.get("page")) {
				std::string why;
				if (auto m = page::page_from_json(*p, &why))
					model = std::move(*m);
				else
					replies.push_back(json::Object{{"error", json::Object{{"line", 0},
							{"message", "the page can't be drawn: " + why}}}});
			}
		build(box, model.controls);
		for (const auto &[k, v] : page::page_values(model))
			saved[k] = v;
		handle(replies);
		return true;
	}

	/* The values now, and those changed since the last save. */
	std::vector<std::pair<std::string, std::string>> values() const {
		return page::page_values(model);
	}

	bool save(std::string *err) {
		bool changed_any = false;
		json::Object vals;
		for (const auto &[k, v] : values()) {
			vals[k] = v;
			changed_any = changed_any || saved[k] != v;
		}
		if (!changed_any || dead)
			return true;
		auto replies = ask(json::Object{{"save", std::move(vals)}});
		std::vector<std::vector<std::string>> runs;
		for (const json::Value &r : replies) {
			const json::Value *w = r.get("write");
			if (w == nullptr)
				continue;
			std::string path = w->str("path");
			if (!page::path_allowed(path, in->files, g_get_home_dir())) {
				*err = in->name + ": " + path + _(" isn't in the integration's Files=");
				g_warning("%s's page: writing %s isn't allowed (Files=): refused",
						in->name.c_str(), path.c_str());
				return false;
			}
			/* A backup of what was there, then the new text. */
			gchar *old = nullptr;
			gsize len = 0;
			if (g_file_get_contents(path.c_str(), &old, &len, nullptr)) {
				g_file_set_contents((path + ".bak").c_str(), old, static_cast<gssize>(len),
						nullptr);
				g_free(old);
			}
			std::string dir = path.substr(0, path.rfind('/'));
			g_mkdir_with_parents(dir.c_str(), 0755);
			if (!write_file_atomic(path, w->str("text"), err))
				return false;
		}
		/* Errors said; runs (reloading the program) after the writes. */
		std::vector<json::Value> rest;
		for (const json::Value &r : replies)
			if (r.get("write") == nullptr)
				rest.push_back(r);
		handle(rest);
		for (const auto &[k, v] : values())
			saved[k] = v;
		return true;
	}
};

GtkWidget *build_lua_page(Ui *ui, Ui::ProgramRow *row) {
	auto p = std::make_shared<LuaPage>();
	p->ui = ui;
	p->row = row;
	p->in = row->in;
	p->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	p->error = GTK_LABEL(gtk_label_new(""));
	gtk_label_set_xalign(p->error, 0);
	gtk_label_set_wrap(p->error, TRUE);
	gtk_label_set_selectable(p->error, TRUE);
	gtk_widget_add_css_class(GTK_WIDGET(p->error), "error");
	gtk_widget_set_visible(GTK_WIDGET(p->error), FALSE);
	gtk_box_append(GTK_BOX(p->box), GTK_WIDGET(p->error));
	GtkWidget *box = p->box;
	row->page = p;
	p->start();
	return box;
}

void lua_page_changes(Ui *ui, Ui::ProgramRow *row, const std::string &where,
		GtkWidget *target, std::vector<UnsavedChange> &out) {
	(void)ui;
	auto *p = static_cast<LuaPage *>(row->page.get());
	if (p == nullptr || wb::lua_page_path(*row->in).empty())
		return;
	for (const auto &[k, v] : p->values()) {
		auto it = p->saved.find(k);
		if (it != p->saved.end() && it->second != v) {
			const page::Control *c = page::find_control(p->model, k);
			std::string name = c != nullptr && !c->label.empty() ? c->label : k;
			out.push_back({{where + " \xE2\x80\xBA " + name, it->second, v}, target});
		}
	}
}

bool save_lua_pages(Ui *ui, std::string *error) {
	for (auto &row : ui->programs) {
		if (row->in == nullptr || wb::lua_page_path(*row->in).empty() || !row->page)
			continue;
		auto *p = static_cast<LuaPage *>(row->page.get());
		if (!p->save(error))
			return false;
	}
	return true;
}

void lua_page_test(Ui *ui, const std::string &id, const std::string &what,
		const std::string &value) {
	for (auto &row : ui->programs) {
		if (row->in == nullptr || row->in->id != id || !row->page ||
				wb::lua_page_path(*row->in).empty())
			continue;
		auto *p = static_cast<LuaPage *>(row->page.get());
		if (what == "click") {
			auto it = p->fields.find(value);
			if (it != p->fields.end() && it->second.button != nullptr)
				g_signal_emit_by_name(it->second.button, "clicked");
		} else {
			auto it = p->fields.find(what);
			if (it != p->fields.end() && it->second.field != nullptr) {
				set_option_field_value(*it->second.field, value);
				p->changed(what);
			}
		}
	}
}

std::string lua_page_dump(Ui *ui, const std::string &id) {
	for (auto &row : ui->programs) {
		if (row->in == nullptr || row->in->id != id || !row->page ||
				wb::lua_page_path(*row->in).empty())
			continue;
		auto *p = static_cast<LuaPage *>(row->page.get());
		std::string out;
		for (const auto &[k, v] : p->values()) {
			const page::Control *c = page::find_control(p->model, k);
			out += "|" + k + "=" + v;
			if (c != nullptr && !c->problem.empty())
				out += "!" + c->problem;
			if (c != nullptr && !c->visible)
				out += "(hidden)";
		}
		const char *err = p->error ? gtk_label_get_text(p->error) : "";
		if (err != nullptr && *err)
			out += std::string("|error:") + err;
		return out;
	}
	return "|no page";
}

}  // namespace wbconf
