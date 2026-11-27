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

#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "waybox/json.hpp"
#include "waybox/page_protocol.hpp"
#include "waybox/setting_problems.hpp"

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

std::string config_dir() {
	const char *x = getenv("XDG_CONFIG_HOME");
	return x != nullptr && *x ? x : home_dir() + "/.config";
}

std::string user_integrations_dir() {
	return config_dir() + "/waybox/integrations";
}

/* wbconf's own view settings ("pages-open=1"): not Waybox's, not rc.xml. */
std::string view_path() {
	return config_dir() + "/waybox/wbconf-view";
}

bool pages_open_pref() {
	return view_get("pages-open") == "1";
}

std::string approvals_path() {
	return config_dir() + "/waybox/approved-pages";
}

std::string read_text(const std::string &path) {
	gchar *text = nullptr;
	gsize len = 0;
	if (!g_file_get_contents(path.c_str(), &text, &len, nullptr))
		return "";
	std::string s(text, len);
	g_free(text);
	return s;
}

/* What an approval is for: the integration file and its script together. */
std::string page_hash(const wb::Integration &in) {
	std::string both = read_text(in.dir + "/" + in.id + ".integration");
	both.push_back('\0');
	both += read_text(wb::lua_page_path(in));
	gchar *h = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
			reinterpret_cast<const guchar *>(both.data()), both.size());
	std::string out = h != nullptr ? h : "";
	g_free(h);
	return out;
}

/* A line of the "what it may do" list: an icon, then text with the
 * path or program in monospace. */
GtkWidget *permission_line(const char *icon, const std::string &markup) {
	GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *img = gtk_image_new_from_icon_name(icon);
	gtk_widget_add_css_class(img, "dim-label");
	gtk_widget_set_valign(img, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(line), img);
	GtkWidget *l = gtk_label_new(nullptr);
	gtk_label_set_markup(GTK_LABEL(l), markup.c_str());
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_selectable(GTK_LABEL(l), TRUE);
	gtk_box_append(GTK_BOX(line), l);
	return line;
}

std::string mono(const std::string &s) {
	gchar *e = g_markup_escape_text(s.c_str(), -1);
	std::string out = std::string("<tt>") + e + "</tt>";
	g_free(e);
	return out;
}

/* The script, with line numbers, as errors give them. */
std::string numbered(const std::string &code) {
	std::string out;
	int n = 1;
	size_t start = 0;
	int width = 1;
	for (size_t lines = std::count(code.begin(), code.end(), '\n') + 1; lines >= 10;
			lines /= 10)
		++width;
	while (start <= code.size()) {
		size_t nl = code.find('\n', start);
		std::string line = code.substr(start, nl == std::string::npos ? std::string::npos
				: nl - start);
		if (nl == std::string::npos && line.empty())
			break;
		std::string num = std::to_string(n++);
		out += std::string(static_cast<size_t>(width) - num.size(), ' ') + num + "  " + line +
				"\n";
		if (nl == std::string::npos)
			break;
		start = nl + 1;
	}
	return out;
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
	case page::Kind::List: return wb::OptionType::List;
	default: return wb::OptionType::Text;
	}
}

}  // namespace

struct LuaPage {
	Ui *ui = nullptr;
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
	GtkWidget *expander = nullptr;  /* "Waybar Settings": the page, folded or not */
	GtkWidget *content = nullptr;   /* its controls */
	GtkWidget *approval = nullptr;  /* the card asking first, while shown */
	bool approval_changed = false;  /* asking again: it changed since allowed */
	bool approval_full = false;     /* what it may touch is full access */
	std::string reloaded;           /* the last wb.reload(): tests */
	std::string hash;               /* what an approval would be for */
	bool syncing = false;
	/* The program's command, as its row has it now (none: no row). */
	std::function<std::string()> command;
	/* Whether its program is used (started, or bound to a key) as the
	 * window has it now: a page waiting for approval is said on saving. */
	std::function<bool()> in_use;
	std::map<std::string, std::string> options;  /* the command's, as opened */
	std::vector<std::string> allowed;            /* Files= and what the command names */
	guint refresh_source = 0;
	std::weak_ptr<LuaPage> self;

	std::map<std::string, std::string> options_now() const {
		if (!command)
			return {};
		return wb::read_options(*in, command());
	}

	/* Files= and the files the command names through a file or folder
	 * option (waybar -c PATH) that the user set: not one its Exec= names
	 * (its author's: that goes in Files=, shown and graded on the card). */
	std::vector<std::string> allowed_files(const std::map<std::string, std::string> &opts) const {
		std::vector<std::string> out = in->files;
		auto authors = wb::read_options(*in, in->exec);
		for (const wb::IntegrationOption &o : in->options) {
			if (o.type != wb::OptionType::File && o.type != wb::OptionType::Folder)
				continue;
			auto it = opts.find(o.id);
			if (it == opts.end() || it->second.empty())
				continue;
			auto a = authors.find(o.id);
			if (a != authors.end() && a->second == it->second)
				continue;
			out.push_back(it->second);
		}
		return out;
	}

	/* Its command changed (an option, or the command line): the page opened
	 * again for it, unless it has changes not saved. */
	void command_changed() {
		if (refresh_source != 0)
			g_source_remove(refresh_source);
		auto *weak = new std::weak_ptr<LuaPage>(self);
		refresh_source = g_timeout_add_full(G_PRIORITY_DEFAULT, 500, [](gpointer d) {
			if (auto p = static_cast<std::weak_ptr<LuaPage> *>(d)->lock()) {
				p->refresh_source = 0;
				p->reopen_if_changed();
			}
			return G_SOURCE_REMOVE;
		}, weak, [](gpointer d) { delete static_cast<std::weak_ptr<LuaPage> *>(d); });
	}

	void reopen_if_changed() {
		if (approval != nullptr || options_now() == options)
			return;
		bool unsaved = false;
		for (const auto &[k, v] : values())
			unsaved = unsaved || saved[k] != v;
		if (unsaved) {
			fail(_("Its command changed: save, or undo the page's changes, to see the page "
					"for the new command."));
			return;
		}
		stop();
		dead = false;
		fields.clear();
		model = page::Page{};
		saved.clear();
		while (GtkWidget *c = gtk_widget_get_first_child(content))
			gtk_box_remove(GTK_BOX(content), c);
		gtk_widget_set_visible(GTK_WIDGET(error), FALSE);
		start();
	}

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
		if (refresh_source != 0)
			g_source_remove(refresh_source);
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
			} else if (r.get("reload") != nullptr) {
				reload();
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

	/* wb.reload(): as the integration's Reload= says. A signal goes to
	 * its program, yours and in this session only (never Waybox). */
	void reload() {
		using K = wb::IntegrationReload::Kind;
		const wb::IntegrationReload &r = in->reload;
		if (r.kind == K::Command) {
			std::vector<char *> args;
			for (const std::string &a : r.argv)
				args.push_back(const_cast<char *>(a.c_str()));
			args.push_back(nullptr);
			GError *err = nullptr;
			if (!g_spawn_async(nullptr, args.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr,
					nullptr, nullptr, &err)) {
				fail(std::string(_("Can't run ")) + r.argv[0] + ": " + err->message);
				g_error_free(err);
			}
			reloaded = "command";
		} else if (r.kind == K::Signal) {
			int sig = r.signal == "USR1" ? SIGUSR1 : r.signal == "USR2" ? SIGUSR2 : SIGHUP;
			std::string name = in->binary.substr(0, 15);
			int sent = 0;
			for (const ProcessInfo &pi : list_processes())
				if (pi.uid == getuid() && pi.name == name && name != "waybox" &&
						pi.pid != getpid() && in_this_session(pi.pid) &&
						kill(pi.pid, sig) == 0)
					++sent;
			reloaded = "signal:" + std::to_string(sent);
		} else if (r.kind == K::Restart) {
			/* Waybox stops it and starts it again (it started it). */
			std::string req = json::dump(json::Object{{"request", "restart_program"},
					{"name", in->id}, {"binary", in->binary}});
			JsonObject *o = waybox_request(req);
			bool ok = o != nullptr && json_object_get_boolean_member_with_default(o, "ok", FALSE);
			if (!ok)
				ui->save_notes.push_back(in->name + _(" reads its settings when it starts: "
						"restart it to see them (Waybox couldn't: it didn't start it, or its "
						"scripting socket is off)."));
			if (o != nullptr)
				json_object_unref(o);
			reloaded = ok ? "restart" : "restart:failed";
		} else if (r.kind == K::Unset) {
			/* Nothing said: it takes them when it starts. */
			ui->save_notes.push_back(in->name + _(" reads its settings when it starts: "
					"restart it to see them."));
			reloaded = "unset";
		} else {
			reloaded = "none";
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
		GtkWidget *buttons = nullptr;  /* buttons one after another: side by side */
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
				buttons = nullptr;
				continue;
			}
			if (c.kind == page::Kind::Label) {
				GtkWidget *l = gtk_label_new(c.label.c_str());
				gtk_label_set_wrap(GTK_LABEL(l), TRUE);
				gtk_label_set_xalign(GTK_LABEL(l), 0);
				gtk_box_append(GTK_BOX(parent), l);
				grid = nullptr;
				buttons = nullptr;
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
				if (buttons == nullptr) {
					buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
					gtk_widget_set_margin_top(buttons, 6);
					gtk_box_append(GTK_BOX(parent), buttons);
				}
				gtk_box_append(GTK_BOX(buttons), f.button);
				grid = nullptr;
				show_control(c);
				continue;
			}
			buttons = nullptr;
			/* A setting, in a grid of label and control. */
			if (grid == nullptr)
				grid = section_grid(parent);
			f.opt = std::make_unique<wb::IntegrationOption>();
			f.opt->id = c.key;
			f.opt->label = c.label.empty() ? c.key : c.label;
			f.opt->type = option_type(c.kind);
			f.opt->choices = c.choices;
			f.opt->choice_labels = c.choice_labels;
			f.opt->no_default_choice = true;
			f.opt->min = c.min;
			f.opt->max = c.max;
			f.opt->hint = c.hint;
			f.opt->unit = c.unit;
			f.opt->width_chars = c.kind == page::Kind::Text && !c.wide ? 24 : 0;
			f.opt->color_alpha = true;  /* a page's colors may be #rrggbbaa */
			std::string key = c.key;
			if (c.kind == page::Kind::Font) {
				f.field = std::make_unique<OptionField>();
				f.field->opt = f.opt.get();
				f.field->font = make_font_control(c.format, c.hint, c.monospace);
				f.field->font->on_change = [this, key] { changed(key); };
				f.field->widget = f.field->font->box;
				set_option_field_value(*f.field, c.value);
			} else if (c.kind == page::Kind::Sections) {
				f.field = std::make_unique<OptionField>();
				f.field->opt = f.opt.get();
				f.field->sections = make_sections_control(c);
				f.field->sections->on_change = [this, key] { changed(key); };
				f.field->widget = f.field->sections->box;
				set_option_field_value(*f.field, c.value);
			} else {
				f.field = make_option_field(*f.opt, [this, key] { changed(key); });
			}
			GtkWidget *label = grid_label(grid, r, (f.opt->label + ":").c_str());
			/* Level with the field, not with the field and its description. */
			gtk_widget_set_valign(label, GTK_ALIGN_START);
			gtk_widget_set_margin_top(label, 8);
			GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
			GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
			gtk_box_append(GTK_BOX(line), f.field->widget);
			/* Wide: the row's whole width (a list of names). */
			if (c.wide) {
				gtk_widget_set_hexpand(f.field->widget, TRUE);
				gtk_widget_set_hexpand(line, TRUE);
				gtk_widget_set_hexpand(cell, TRUE);
			}
			/* A number's unit comes with its field; another kind's after it. */
			if (!c.unit.empty() && c.kind != page::Kind::Number) {
				GtkWidget *u = gtk_label_new(c.unit.c_str());
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
			gtk_widget_add_css_class(GTK_WIDGET(f.problem), "wb-problem");
			gtk_box_append(GTK_BOX(cell), GTK_WIDGET(f.problem));
			grid_control(grid, r++, cell);
			if (c.wide || c.kind == page::Kind::List || c.kind == page::Kind::Sections)
				gtk_widget_set_halign(cell, GTK_ALIGN_FILL);
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
		options = options_now();
		allowed = allowed_files(options);
		json::Array files, runs;
		json::Object opts;
		for (const auto &[k, v] : options)
			opts[k] = v;
		for (const std::string &f : allowed)
			files.emplace_back(f);
		for (const std::string &r : in->runs)
			runs.emplace_back(r);
		auto replies = ask(json::Object{{"open", json::Object{
				{"script", wb::lua_page_path(*in)}, {"home", std::string(home ? home : "")},
				{"files", std::move(files)}, {"runs", std::move(runs)},
				{"options", std::move(opts)}}}});
		for (const json::Value &r : replies)
			if (const json::Value *p = r.get("page")) {
				std::string why;
				if (auto m = page::page_from_json(*p, &why))
					model = std::move(*m);
				else
					replies.push_back(json::Object{{"error", json::Object{{"line", 0},
							{"message", "the page can't be drawn: " + why}}}});
			}
		build(content, model.controls);
		gtk_widget_set_visible(expander, TRUE);
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
		/* Still asking: nothing of the page's done, and said so (the
		 * program itself starts as its row says, allowed or not). */
		if (approval != nullptr) {
			if (in_use && in_use())
				ui->save_notes.push_back(in->name + _("'s settings page isn't allowed yet, so "
						"its settings were left as they were."));
			return true;
		}
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
			if (!page::path_allowed(path, allowed, g_get_home_dir())) {
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

namespace {

/* Allow: remembered, the card gone, the page started. */
void on_allow(GtkButton *, gpointer data) {
	auto *p = static_cast<LuaPage *>(data);
	std::string path = approvals_path();
	std::string text = page::approvals_with(read_text(path), p->in->id, p->hash);
	g_mkdir_with_parents((config_dir() + "/waybox").c_str(), 0755);
	std::string err;
	if (!write_file_atomic(path, text, &err)) {
		p->fail(std::string(_("Can't remember the approval: ")) + err);
		return;
	}
	if (p->approval != nullptr) {
		gtk_box_remove(GTK_BOX(p->box), p->approval);
		p->approval = nullptr;
		gtk_expander_set_expanded(GTK_EXPANDER(p->expander), TRUE);  /* just asked for */
	}
	p->start();
}

/* Why a page is asked about. */
enum class Asking { Own, Writable, Changed };

/* Something you (or a program you run) can change: the page could be
 * anyone's. A package's files under /usr aren't. */
bool writable(const std::string &path) {
	return access(path.c_str(), W_OK) == 0;
}

/* Before a page runs that wbconf can't tell came with Waybox or a
 * package: where it's from, what it may touch (full access said so), its
 * script, and Allow. */
GtkWidget *approval_card(LuaPage *p, Asking why) {
	const wb::Integration &in = *p->in;
	const std::string home = home_dir();
	/* How far each reaches: the integration's own (plain), another program
	 * or file (a caution), full access (said line by line, and in the
	 * heading). */
	std::vector<wb::PageAccess> runs, files;
	for (const std::string &r : in.runs)
		runs.push_back(wb::run_access(in, r));
	for (const std::string &f : in.files)
		files.push_back(wb::file_access(in, page::expand_home(f, home), home));
	using RK = wb::IntegrationReload::Kind;
	std::optional<wb::PageAccess> reload_run;
	if (in.reload.kind == RK::Command)
		reload_run = wb::run_access(in, in.reload.argv[0]);
	auto is_full = [](const wb::PageAccess &a) { return a.reach == wb::Reach::Full; };
	bool full = std::any_of(runs.begin(), runs.end(), is_full) ||
			std::any_of(files.begin(), files.end(), is_full) ||
			(reload_run && is_full(*reload_run));
	p->approval_full = full;
	GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_add_css_class(card, "wb-approval");

	GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *icon = gtk_image_new_from_icon_name(full ? "dialog-warning-symbolic" : "channel-secure-symbolic");
	gtk_box_append(GTK_BOX(head), icon);
	GtkWidget *title = gtk_label_new(full ? _("This page asks for full access to your account")
			: why == Asking::Changed ? _("This page changed since you allowed it")
			: why == Asking::Writable ? _("A settings page in a folder you can change")
			: _("A settings page from outside Waybox"));
	gtk_widget_add_css_class(title, "heading");
	gtk_label_set_xalign(GTK_LABEL(title), 0);
	gtk_label_set_wrap(GTK_LABEL(title), TRUE);
	gtk_box_append(GTK_BOX(head), title);
	gtk_box_append(GTK_BOX(card), head);

	std::string where = wb::tilde(in.dir, home);
	gchar *intro = g_markup_printf_escaped(why == Asking::Changed
			? _("%s's page in %s isn't the one you allowed. Before it runs, it may:")
			: why == Asking::Writable
			? _("%s's page is in %s, which you (or a program you run) can change, so Waybox "
					"can't tell whether it's the one it came with. Before it runs, it may:")
			: _("%s's page comes from %s, not with Waybox. Before it runs, it may:"),
			in.name.c_str(), where.c_str());
	GtkWidget *body = gtk_label_new(nullptr);
	gtk_label_set_markup(GTK_LABEL(body), intro);
	g_free(intro);
	gtk_label_set_xalign(GTK_LABEL(body), 0);
	gtk_label_set_wrap(GTK_LABEL(body), TRUE);
	gtk_box_append(GTK_BOX(card), body);

	GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_margin_start(list, 4);
	/* "Run sh: full access, it can run any command." */
	auto note = [&in](const wb::PageAccess &a, bool file) {
		if (a.reach == wb::Reach::Own)
			return std::string();
		if (a.reach == wb::Reach::Other) {
			gchar *e = g_markup_printf_escaped(file ? _(": not %s's own file")
					: _(": another program, with your account's access"), in.name.c_str());
			std::string s = e;
			g_free(e);
			return s;
		}
		gchar *e = g_markup_escape_text(a.why.c_str(), -1);
		std::string s = std::string(": <b>") + _("full access") + "</b>, " + e;
		g_free(e);
		return s;
	};
	auto line_icon = [](const wb::PageAccess &a, const char *own) {
		return a.reach == wb::Reach::Own ? own : "dialog-warning-symbolic";
	};
	for (size_t i = 0; i < in.files.size(); ++i)
		gtk_box_append(GTK_BOX(list), permission_line(line_icon(files[i], "document-edit-symbolic"),
				std::string(_("Read and change")) + " " + mono(in.files[i]) +
				note(files[i], true)));
	/* "Read and change the file its command names (-c, Config file)". */
	for (const wb::IntegrationOption &o : in.options)
		if (o.type == wb::OptionType::File || o.type == wb::OptionType::Folder) {
			gchar *t = g_markup_printf_escaped(_("Read and change the file you give it with %s "
					"(%s), if you give one"), o.flag.c_str(), o.label.c_str());
			gtk_box_append(GTK_BOX(list), permission_line("document-edit-symbolic", t));
			g_free(t);
		}
	for (size_t i = 0; i < in.runs.size(); ++i)
		gtk_box_append(GTK_BOX(list), permission_line(line_icon(runs[i], "system-run-symbolic"),
				std::string(_("Run")) + " " + mono(in.runs[i]) + note(runs[i], false)));
	/* Reloading: "Reload it with makoctl reload", "Reload it: send USR2 to
	 * waybar". */
	if (reload_run) {
		std::string cmd;
		for (const std::string &w : in.reload.argv)
			cmd += (cmd.empty() ? "" : " ") + w;
		gtk_box_append(GTK_BOX(list), permission_line(
				line_icon(*reload_run, "view-refresh-symbolic"),
				std::string(_("Reload it with")) + " " + mono(cmd) + note(*reload_run, false)));
	} else if (in.reload.kind == RK::Signal) {
		gchar *t = g_markup_printf_escaped(_("Reload it: send %s to %s"),
				in.reload.signal.c_str(), in.binary.c_str());
		gtk_box_append(GTK_BOX(list), permission_line("view-refresh-symbolic", t));
		g_free(t);
	} else if (in.reload.kind == RK::Restart) {
		gchar *t = g_markup_printf_escaped(_("Reload it: Waybox restarts %s"),
				in.binary.c_str());
		gtk_box_append(GTK_BOX(list), permission_line("view-refresh-symbolic", t));
		g_free(t);
	}
	if (in.files.empty() && in.runs.empty() && !reload_run && in.reload.kind != RK::Signal &&
			in.reload.kind != RK::Restart)
		gtk_box_append(GTK_BOX(list), permission_line("channel-secure-symbolic",
				_("Nothing outside the page itself")));
	gtk_box_append(GTK_BOX(card), list);
	/* What allowing it is about, and what it isn't. */
	gchar *scope = g_strdup_printf(_("This is only about the page: %s starts (or not) as "
			"set here either way, allowed or not."), in.name.c_str());
	GtkWidget *scope_label = gtk_label_new(scope);
	g_free(scope);
	gtk_label_set_xalign(GTK_LABEL(scope_label), 0);
	gtk_label_set_wrap(GTK_LABEL(scope_label), TRUE);
	gtk_widget_add_css_class(scope_label, "dim-label");
	gtk_box_append(GTK_BOX(card), scope_label);

	/* The script, to read before allowing it. */
	GtkWidget *expander = gtk_expander_new(_("View the Script"));
	GtkWidget *view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 4);
	gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 4);
	std::string code = numbered(read_text(wb::lua_page_path(in)));
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), code.c_str(), -1);
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
			GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
	gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 140);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 280);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
	gtk_widget_add_css_class(scroll, "wb-script");
	gtk_widget_set_margin_top(scroll, 4);
	gtk_expander_set_child(GTK_EXPANDER(expander), scroll);

	/* The script the card's whole width; the one action under it, at the
	 * end, as dialogs have it. */
	gtk_box_append(GTK_BOX(card), expander);
	GtkWidget *allow = gtk_button_new_with_label(full ? _("Allow Full Access")
			: _("Allow This Page"));
	gtk_widget_add_css_class(allow, full ? "destructive-action" : "suggested-action");
	gtk_widget_set_halign(allow, GTK_ALIGN_END);
	g_signal_connect(allow, "clicked", G_CALLBACK(on_allow), p);
	g_object_set_data(G_OBJECT(card), "wb-allow", allow);
	g_object_set_data(G_OBJECT(card), "wb-script", expander);
	gtk_box_append(GTK_BOX(card), allow);
	return card;
}

}  // namespace

void lua_page_command_changed(const std::shared_ptr<void> &page) {
	if (page)
		std::static_pointer_cast<LuaPage>(page)->command_changed();
}

GtkWidget *build_lua_page(Ui *ui, Ui::ProgramRow *row) {
	GtkEntry *cmd = row->command;
	GtkSwitch *on = row->on;
	GtkWidget *w = build_lua_page_for(ui, row->in, &row->page, [cmd] {
		return std::string(gtk_editable_get_text(GTK_EDITABLE(cmd)));
	}, [on] { return on != nullptr && gtk_switch_get_active(on); });
	std::weak_ptr<LuaPage> weak = std::static_pointer_cast<LuaPage>(row->page);
	row->page_refresh = [weak] {
		if (auto p = weak.lock())
			p->command_changed();
	};
	return w;
}

GtkWidget *build_lua_page_for(Ui *ui, const wb::Integration *in_ptr,
		std::shared_ptr<void> *holder, std::function<std::string()> command,
		std::function<bool()> in_use) {
	auto p = std::make_shared<LuaPage>();
	p->ui = ui;
	p->in = in_ptr;
	p->command = std::move(command);
	p->in_use = std::move(in_use);
	p->self = p;
	p->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	p->error = GTK_LABEL(gtk_label_new(""));
	gtk_label_set_xalign(p->error, 0);
	gtk_label_set_wrap(p->error, TRUE);
	gtk_label_set_selectable(p->error, TRUE);
	gtk_widget_add_css_class(GTK_WIDGET(p->error), "error");
	gtk_widget_set_visible(GTK_WIDGET(p->error), FALSE);
	gtk_box_append(GTK_BOX(p->box), GTK_WIDGET(p->error));
	/* The page under "NAME Settings", folded unless the menu's "Keep Program
	 * Settings Open" is on; shown once it runs. */
	std::string title = in_ptr->name + " " + _("Settings");
	p->expander = gtk_expander_new(title.c_str());
	gtk_widget_add_css_class(p->expander, "wb-page-expander");
	p->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_margin_top(p->content, 6);
	gtk_expander_set_child(GTK_EXPANDER(p->expander), p->content);
	gtk_expander_set_expanded(GTK_EXPANDER(p->expander), pages_open_pref());
	gtk_widget_set_visible(p->expander, FALSE);
	gtk_box_append(GTK_BOX(p->box), p->expander);
	GtkWidget *box = p->box;
	*holder = p;
	ui->lua_pages.push_back(p);
	/* Trusted only when no one but root could have changed it (a package's
	 * files): its folder, its integration file and its script. Else it
	 * runs once allowed, for what it is now; changing it asks again. */
	const wb::Integration &in = *in_ptr;
	bool trusted = !writable(in.dir) && !writable(in.dir + "/" + in.id + ".integration") &&
			!writable(wb::lua_page_path(in));
	if (!trusted) {
		p->hash = page_hash(in);
		auto approved = page::parse_approvals(read_text(approvals_path()));
		auto it = approved.find(in.id);
		if (it == approved.end() || it->second != p->hash) {
			p->approval_changed = it != approved.end();
			Asking why = p->approval_changed ? Asking::Changed
					: page::expand_home(in.dir, home_dir()) == user_integrations_dir()
					? Asking::Own : Asking::Writable;
			p->approval = approval_card(p.get(), why);
			gtk_box_append(GTK_BOX(p->box), p->approval);
			return box;
		}
	}
	p->start();
	return box;
}

namespace {

/* The pages shown now, as they are (those of rows gone left out). */
std::vector<LuaPage *> live_pages(Ui *ui) {
	std::vector<LuaPage *> out;
	ui->lua_pages.erase(std::remove_if(ui->lua_pages.begin(), ui->lua_pages.end(),
			[](const std::weak_ptr<void> &w) { return w.expired(); }), ui->lua_pages.end());
	for (auto &w : ui->lua_pages)
		if (auto sp = w.lock())
			out.push_back(static_cast<LuaPage *>(sp.get()));
	return out;
}

}  // namespace

void lua_page_changes(Ui *ui, const std::string &id, const std::string &where,
		GtkWidget *target, std::vector<UnsavedChange> &out) {
	for (LuaPage *p : live_pages(ui)) {
		if (p->in == nullptr || p->in->id != id)
			continue;
		for (const auto &[k, v] : p->values()) {
			auto it = p->saved.find(k);
			if (it != p->saved.end() && it->second != v) {
				const page::Control *c = page::find_control(p->model, k);
				std::string name = c != nullptr && !c->label.empty() ? c->label : k;
				out.push_back({{where + " \xE2\x80\xBA " + name, it->second, v}, target});
			}
		}
	}
}

std::string lua_page_fold(Ui *ui, const std::string &id) {
	for (LuaPage *p : live_pages(ui))
		if (p->in != nullptr && p->in->id == id && p->expander != nullptr)
			return !gtk_widget_get_visible(p->expander) ? "hidden"
					: gtk_expander_get_expanded(GTK_EXPANDER(p->expander)) ? "open" : "folded";
	return "no page";
}

/* wbconf's view settings: one KEY=VALUE a line. */
std::string view_get(const std::string &key) {
	gchar *text = nullptr;
	std::string out;
	if (g_file_get_contents(view_path().c_str(), &text, nullptr, nullptr)) {
		std::istringstream in(text);
		for (std::string line; std::getline(in, line);)
			if (line.rfind(key + "=", 0) == 0)
				out = line.substr(key.size() + 1);
		g_free(text);
	}
	return out;
}

void view_set(const std::string &key, const std::string &value) {
	std::string path = view_path();
	gchar *text = nullptr;
	std::string out;
	if (g_file_get_contents(path.c_str(), &text, nullptr, nullptr)) {
		std::istringstream in(text);
		for (std::string line; std::getline(in, line);)
			if (!line.empty() && line.rfind(key + "=", 0) != 0)
				out += line + "\n";
		g_free(text);
	}
	out += key + "=" + value + "\n";
	gchar *dir = g_path_get_dirname(path.c_str());
	g_mkdir_with_parents(dir, 0700);
	g_free(dir);
	g_file_set_contents(path.c_str(), out.c_str(), -1, nullptr);
}

bool lua_pages_open() {
	return pages_open_pref();
}

void set_lua_pages_open(Ui *ui, bool open) {
	view_set("pages-open", open ? "1" : "0");
	for (LuaPage *p : live_pages(ui))
		if (p->expander != nullptr)
			gtk_expander_set_expanded(GTK_EXPANDER(p->expander), open);
}

bool save_lua_pages(Ui *ui, std::string *error) {
	for (LuaPage *p : live_pages(ui))
		if (!p->save(error))
			return false;
	return true;
}

void lua_page_test(Ui *ui, const std::string &id, const std::string &what,
		const std::string &value) {
	for (LuaPage *p : live_pages(ui)) {
		if (p->in == nullptr || p->in->id != id)
			continue;
		if (what == "script") {
			if (p->approval != nullptr)
				gtk_expander_set_expanded(GTK_EXPANDER(g_object_get_data(
						G_OBJECT(p->approval), "wb-script")), TRUE);
		} else if (what == "approve") {
			if (p->approval != nullptr)
				g_signal_emit_by_name(g_object_get_data(G_OBJECT(p->approval), "wb-allow"),
						"clicked");
		} else if (what == "click") {
			auto it = p->fields.find(value);
			if (it != p->fields.end() && it->second.button != nullptr)
				g_signal_emit_by_name(it->second.button, "clicked");
		} else {
			auto it = p->fields.find(what);
			if (it == p->fields.end() || it->second.field == nullptr)
				continue;
			/* A list: "@add:NAME", "@up:N", "@down:N", "@remove:N", as its
			 * buttons do (and so through its on_change). */
			if (it->second.field->list != nullptr && value.rfind('@', 0) == 0) {
				list_control_test(it->second.field->list.get(), value.substr(1));
				continue;
			}
			if (it->second.field->font != nullptr && value.rfind('@', 0) == 0) {
				font_control_test(it->second.field->font.get(), value.substr(1));
				continue;
			}
			if (it->second.field->sections != nullptr && value.rfind('@', 0) == 0) {
				sections_control_test(it->second.field->sections.get(), value.substr(1));
				continue;
			}
			set_option_field_value(*it->second.field, value);
			p->changed(what);
		}
	}
}

std::string lua_page_dump(Ui *ui, const std::string &id) {
	for (LuaPage *p : live_pages(ui)) {
		if (p->in == nullptr || p->in->id != id)
			continue;
		std::string out;
		if (p->approval != nullptr)
			out += std::string("|approval:") + (p->pid > 0 ? "running!"
					: p->approval_changed ? "changed" : "waiting") +
					(p->approval_full ? ":full" : "");
		for (const auto &[k, v] : p->values()) {
			const page::Control *c = page::find_control(p->model, k);
			std::string shown = v;  /* a list's lines as "\n", the dump one line */
			for (size_t at = 0; (at = shown.find('\n', at)) != std::string::npos; at += 2)
				shown.replace(at, 1, "\\n");
			out += "|" + k + "=" + shown;
			if (c != nullptr && !c->problem.empty())
				out += "!" + c->problem;
			if (c != nullptr && !c->visible)
				out += "(hidden)";
		}
		const char *err = p->error ? gtk_label_get_text(p->error) : "";
		if (err != nullptr && *err)
			out += std::string("|error:") + err;
		if (!p->reloaded.empty())
			out += "|reloaded:" + p->reloaded;
		return out;
	}
	return "|no page";
}

}  // namespace wbconf
