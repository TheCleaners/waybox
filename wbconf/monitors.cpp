#include "wbconf_ui.hpp"

#include "waybox/monitor_layout.hpp"

namespace wbconf {

/* "60" for 60.000, "59.997" otherwise. */
std::string format_hz(double hz) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%.3f", hz);
	std::string s = buf;
	while (!s.empty() && s.back() == '0')
		s.pop_back();
	if (!s.empty() && s.back() == '.')
		s.pop_back();
	return s;
}

/* The connected monitors, or nullopt (with why) when wlr-randr can't say. */
std::optional<std::vector<MonitorInfo>> query_monitors(std::string *why) {
	gchar *out = nullptr;
	gint status = 0;
	GError *err = nullptr;
	if (!g_spawn_command_line_sync("wlr-randr --json", &out, nullptr, &status, &err) ||
			!g_spawn_check_wait_status(status, nullptr)) {
		*why = _("Monitors are listed with wlr-randr; install it to set them here.");
		if (err != nullptr)
			g_error_free(err);
		g_free(out);
		return std::nullopt;
	}
	std::vector<MonitorInfo> list;
	JsonParser *parser = json_parser_new();
	if (json_parser_load_from_data(parser, out, -1, nullptr)) {
		JsonNode *root = json_parser_get_root(parser);
		JsonArray *arr = JSON_NODE_HOLDS_ARRAY(root) ? json_node_get_array(root) : nullptr;
		for (guint i = 0; arr != nullptr && i < json_array_get_length(arr); i++) {
			JsonObject *o = json_array_get_object_element(arr, i);
			if (o == nullptr || !json_object_has_member(o, "name"))
				continue;
			MonitorInfo m;
			auto str = [o](const char *key) -> std::string {
				const char *v = json_object_has_member(o, key)
						? json_object_get_string_member_with_default(o, key, nullptr) : nullptr;
				return v != nullptr ? v : "";
			};
			/* Valid UTF-8 whatever the monitor's EDID says. */
			bool identity_ok = true;
			auto valid = [&identity_ok](const std::string &s) {
				char *v = g_utf8_make_valid(s.c_str(), static_cast<gssize>(s.size()));
				std::string out = v;
				g_free(v);
				identity_ok = identity_ok && out == s;
				return out;
			};
			m.name = valid(str("name"));
			m.description = valid(str("description"));
			identity_ok = true;  /* the name and description don't go in rules */
			m.make = valid(str("make"));
			m.model = valid(str("model"));
			m.serial = valid(str("serial"));
			m.identity_ok = identity_ok;
			m.adaptive_sync = json_object_get_boolean_member_with_default(o, "adaptive_sync",
					FALSE);
			m.enabled = json_object_get_boolean_member_with_default(o, "enabled", TRUE);
			m.scale = json_object_get_double_member_with_default(o, "scale", 1);
			if (std::string t = str("transform"); !t.empty())
				m.transform = t;
			if (JsonObject *pos = json_object_has_member(o, "position")
					? json_object_get_object_member(o, "position") : nullptr) {
				m.x = static_cast<int>(json_object_get_int_member_with_default(pos, "x", 0));
				m.y = static_cast<int>(json_object_get_int_member_with_default(pos, "y", 0));
			}
			if (json_object_has_member(o, "modes")) {
				JsonArray *modes = json_object_get_array_member(o, "modes");
				for (guint j = 0; j < json_array_get_length(modes); j++) {
					JsonObject *md = json_array_get_object_element(modes, j);
					if (md == nullptr)
						continue;
					if (json_object_get_boolean_member_with_default(md, "current", FALSE)) {
						m.width = static_cast<int>(json_object_get_int_member(md, "width"));
						m.height = static_cast<int>(json_object_get_int_member(md, "height"));
					}
					std::string v = std::to_string(json_object_get_int_member(md, "width")) +
							"x" + std::to_string(json_object_get_int_member(md, "height"));
					double hz = json_object_get_double_member(md, "refresh");
					if (hz > 0)
						v += "@" + format_hz(hz);
					if (std::find(m.modes.begin(), m.modes.end(), v) == m.modes.end())
						m.modes.push_back(v);
				}
			}
			list.push_back(m);
		}
	}
	g_object_unref(parser);
	g_free(out);
	if (list.empty())
		*why = _("No monitors found.");
	return list;
}

namespace {

wb::OutputMatch identity_of(const Ui::Monitor &m) {
	return {m.info.name, m.info.make, m.info.model, m.info.serial};
}

bool by_monitor(const wb::OutputMatch &match) {
	return match.name.empty() && !match.empty();
}

/* Follow this monitor: its make, model and serial (those it has). */
wb::OutputMatch follow_match(const Ui::Monitor &m) {
	return {"", m.info.make, m.info.model, m.info.serial};
}

bool can_follow(const Ui::Monitor &m) {
	return m.info.identity_ok && !follow_match(m).empty();
}

/* What the card says its rule matches. */
void show_matched_by(Ui *ui, Ui::Monitor &m) {
	if (m.matched_by == nullptr)
		return;
	std::string text;
	bool follow = gtk_switch_get_active(m.follow);
	if (m.shared) {
		std::string others;
		for (const std::string &n : m.shared_with)
			others += (others.empty() ? "" : ", ") + n;
		char *t = g_strdup_printf(_("Its rule applies to %s too: a change here gives %s a "
				"rule of its own."), others.c_str(), m.name.c_str());
		text = t;
		g_free(t);
	} else if (follow) {
		std::string what = wb::monitor_caption(m.info.make, m.info.model, "", m.name);
		char *t = g_strdup_printf(_("Follows this monitor (%s) on any port."), what.c_str());
		text = t;
		g_free(t);
		if (m.info.serial.empty()) {
			text += std::string(" ") + _("It doesn't say its serial number: another of the "
					"same model would get these settings too.");
			for (const Ui::Monitor &o : ui->monitors)
				if (&o != &m && o.info.make == m.info.make && o.info.model == m.info.model) {
					char *w = g_strdup_printf(_("%s is one, connected now."), o.name.c_str());
					text += std::string(" ") + w;
					g_free(w);
				}
		}
	} else {
		char *t = g_strdup_printf(_("Applies to whatever is connected to %s."), m.name.c_str());
		text = t;
		g_free(t);
	}
	gtk_label_set_text(m.matched_by, text.c_str());
}

}  // namespace

std::string monitor_transform(const Ui::Monitor &m) {
	if (auto it = m.kept.find("transform"); it != m.kept.end())
		return it->second;  /* written by hand, unknown: as is */
	return wb::join_transform(selected_value(m.transform, m.rotations),
			gtk_check_button_get_active(m.flip));
}

namespace {

/* The card's settings as rule attributes (not what it matches). */
std::map<std::string, std::string> card_attrs(const Ui::Monitor &m) {
	struct { std::map<std::string, std::string> attrs; } out{m.kept};
	if (!gtk_switch_get_active(m.enabled))
		out.attrs["enabled"] = "no";
	auto put = [&out](const char *key, const std::string &v) {
		if (!v.empty())
			out.attrs[key] = v;
	};
	put("mode", selected_value(m.mode, m.modes));
	put("scale", selected_value(m.scale, m.scales));
	put("position", gtk_editable_get_text(GTK_EDITABLE(m.position)));
	if (m.kept.count("transform") == 0)
		put("transform", monitor_transform(m));
	guint a = gtk_drop_down_get_selected(m.adaptive);
	if (a == m.adaptive_initial && !m.adaptive_raw.empty())
		out.attrs["adaptiveSync"] = m.adaptive_raw;  /* unchanged: as written */
	else if (a == 1 || a == 2)
		out.attrs["adaptiveSync"] = a == 1 ? "yes" : "no";
	return out.attrs;
}

}  // namespace

std::optional<wb::OutputSetting> monitor_rule(const Ui::Monitor &m) {
	wb::OutputSetting out;
	out.attrs = card_attrs(m);
	/* What it matches: as loaded, unless "follow" was switched. */
	bool follow = gtk_switch_get_active(m.follow);
	wb::OutputMatch match = follow == m.follow_initial && !m.match_initial.empty()
			? m.match_initial
			: follow && can_follow(m) ? follow_match(m) : wb::OutputMatch{m.name, "", "", ""};
	/* Never a rule matching nothing: that would be the same as none. */
	if (match.empty())
		match = {m.name, "", "", ""};
	out.name = match.name;
	out.make = match.make;
	out.model = match.model;
	out.serial = match.serial;
	if (m.shared) {
		/* Unchanged: the shared rule stays as it is (passed through). */
		if (out.attrs == m.shared_attrs && follow == m.follow_initial)
			return std::nullopt;
		out.before = m.shared_match;
		return out;
	}
	out.from = m.rule_from;
	if (out.attrs.empty() && out.from.empty())
		return std::nullopt;
	return out;
}

bool monitor_test_set(Ui *ui, const std::string &name, const std::string &key,
		const std::string &value) {
	for (Ui::Monitor &m : ui->monitors) {
		if (m.name != name)
			continue;
		if (key == "rotation")
			select_value(m.transform, m.rotations, value);
		else if (key == "flip")
			gtk_check_button_set_active(m.flip, value == "on");
		else if (key == "adaptive")
			gtk_drop_down_set_selected(m.adaptive, value == "on" ? 1 : value == "off" ? 2 : 0);
		else if (key == "follow")
			gtk_switch_set_active(m.follow, value == "on");
		else if (key == "scale")
			select_value(m.scale, m.scales, value);
		else
			return false;
		return true;
	}
	return false;
}

void populate_monitors(Ui *ui, const std::vector<wb::OutputSetting> &outputs) {
	/* Which rule applies to each connected monitor, as Waybox decides. */
	std::vector<int> applying;
	for (const Ui::Monitor &m : ui->monitors)
		applying.push_back(wb::applying_output_rule(outputs, identity_of(m)));
	/* Rules no card edits (for monitors not connected, shared ones, ones
	 * that apply to nothing now) are passed through as they are. */
	ui->other_outputs.clear();
	for (size_t r = 0; r < outputs.size(); ++r) {
		int users = 0;
		for (int a : applying)
			users += a == static_cast<int>(r);
		if (users != 1)
			ui->other_outputs.push_back(outputs[r]);
	}
	for (size_t i = 0; i < ui->monitors.size(); ++i) {
		Ui::Monitor &m = ui->monitors[i];
		std::map<std::string, std::string> attrs;
		m.rule_from = {};
		m.shared = false;
		m.shared_match = {};
		m.shared_attrs.clear();
		m.shared_with.clear();
		m.match_initial = {};
		int r = applying[i];
		if (r >= 0) {
			attrs = outputs[r].attrs;
			for (size_t j = 0; j < ui->monitors.size(); ++j)
				if (j != i && applying[j] == r)
					m.shared_with.push_back(ui->monitors[j].name);
			if (m.shared_with.empty()) {
				m.rule_from = outputs[r].from;
				m.match_initial = outputs[r].match();
			} else {
				m.shared = true;
				m.shared_match = outputs[r].from;
				m.shared_attrs = outputs[r].attrs;
			}
		}
		auto take = [&attrs](const char *key) {
			auto it = attrs.find(key);
			std::string v = it == attrs.end() ? "" : it->second;
			if (it != attrs.end())
				attrs.erase(it);
			return v;
		};
		std::string enabled = take("enabled");
		gtk_switch_set_active(m.enabled, enabled != "no");
		if (!enabled.empty() && enabled != "no")
			attrs["enabled"] = enabled;  /* an explicit "yes" stays */
		select_value(m.mode, m.modes, take("mode"));
		select_value(m.scale, m.scales, take("scale"));
		gtk_editable_set_text(GTK_EDITABLE(m.position), take("position").c_str());
		std::string transform = take("transform");
		auto split = wb::split_transform(transform);
		bool known = split.has_value();
		if (!known)
			attrs["transform"] = transform;  /* kept as written */
		select_value(m.transform, m.rotations, known ? split->first : "");
		gtk_check_button_set_active(m.flip, known && split->second);
		gtk_widget_set_sensitive(GTK_WIDGET(m.transform), known);
		gtk_widget_set_sensitive(GTK_WIDGET(m.flip), known);
		if (!known) {
			char *t = g_strdup_printf(_("Set by hand in rc.xml: %s"), transform.c_str());
			gtk_widget_set_tooltip_text(GTK_WIDGET(m.transform), t);
			g_free(t);
		} else {
			gtk_widget_set_tooltip_text(GTK_WIDGET(m.transform), nullptr);
		}
		m.adaptive_raw = take("adaptiveSync");
		const std::string &a = m.adaptive_raw;
		m.adaptive_initial = a.empty() ? 0
				: (a == "yes" || a == "true" || a == "on" || a == "1") ? 1
				: (a == "no" || a == "false" || a == "off" || a == "0") ? 2 : 0;
		if (!a.empty() && m.adaptive_initial == 0)
			attrs["adaptiveSync"] = a;  /* not understood: kept as written */
		if (attrs.count("adaptiveSync"))
			m.adaptive_raw.clear();
		gtk_drop_down_set_selected(m.adaptive, m.adaptive_initial);
		gtk_widget_set_sensitive(GTK_WIDGET(m.adaptive), attrs.count("adaptiveSync") == 0);
		m.kept = attrs;
		if (m.shared)
			/* What the card shows if left alone: the shared rule's values. */
			m.shared_attrs = card_attrs(m);
		m.follow_initial = by_monitor(m.shared ? wb::OutputMatch{} : m.match_initial);
		gtk_switch_set_active(m.follow, m.follow_initial);
		/* Only a monitor that says what it is can be followed; one followed
		 * already can always be switched back. */
		gtk_widget_set_sensitive(GTK_WIDGET(m.follow), can_follow(m) || m.follow_initial);
		if (!m.info.identity_ok)
			gtk_widget_set_tooltip_text(GTK_WIDGET(m.follow), _("Its make, model or serial "
					"can't be written exactly in rc.xml."));
		else if (follow_match(m).empty())
			gtk_widget_set_tooltip_text(GTK_WIDGET(m.follow), _("This monitor doesn't say its "
					"make, model or serial."));
		show_matched_by(ui, m);
	}
	arranger_changed(ui);
}

/* One block per connected monitor. */
GtkWidget *build_monitors_block(Ui *ui) {
	GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

	std::string why;
	auto monitors = query_monitors(&why);
	if (!monitors || monitors->empty()) {
		GtkWidget *msg = gtk_label_new(why.c_str());
		gtk_label_set_wrap(GTK_LABEL(msg), TRUE);
		gtk_label_set_xalign(GTK_LABEL(msg), 0);
		gtk_box_append(GTK_BOX(inner), msg);
		return inner;
	}
	ui->arranger.manager_notice = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_visible(ui->arranger.manager_notice, FALSE);
	gtk_box_append(GTK_BOX(inner), ui->arranger.manager_notice);
	/* Once the window is whole: the program rows say where things start. */
	g_idle_add_once([](gpointer data) {
		refresh_output_manager_notice(static_cast<Ui *>(data));
	}, ui);
	gtk_box_append(GTK_BOX(inner), build_arranger(ui));
	ui->monitors.reserve(monitors->size());
	for (const MonitorInfo &info : *monitors) {
		std::string title = info.name;
		std::string caption = wb::monitor_caption(info.make, info.model, info.description,
				info.name);
		if (!caption.empty())
			title += "  " + caption;
		/* A framed card per monitor. */
		GtkWidget *frame = gtk_frame_new(nullptr);
		GtkWidget *label = gtk_label_new(nullptr);
		char *markup = g_markup_printf_escaped("<b>%s</b>", title.c_str());
		gtk_label_set_markup(GTK_LABEL(label), markup);
		g_free(markup);
		gtk_frame_set_label_widget(GTK_FRAME(frame), label);
		gtk_widget_set_halign(frame, GTK_ALIGN_START);
		GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		gtk_widget_set_margin_top(card, 10);
		gtk_widget_set_margin_bottom(card, 12);
		gtk_widget_set_margin_start(card, 14);
		gtk_widget_set_margin_end(card, 14);
		gtk_frame_set_child(GTK_FRAME(frame), card);
		gtk_box_append(GTK_BOX(inner), frame);
		GtkWidget *grid = section_grid(card);
		Ui::Monitor m;
		m.name = info.name;
		m.info = info;
		m.card = frame;
		m.enabled = add_switch(grid, 0, _("On:"));
		m.modes.push_back("");
		for (const std::string &v : info.modes)
			m.modes.push_back(v);
		grid_label(grid, 1, _("Mode:"));
		m.mode = values_dropdown(m.modes);
		grid_control(grid, 1, GTK_WIDGET(m.mode));
		m.scales = {"", "1", "1.25", "1.5", "1.75", "2", "2.5", "3"};
		grid_label(grid, 2, _("Scale:"));
		m.scale = values_dropdown(m.scales);
		grid_control(grid, 2, GTK_WIDGET(m.scale));
		grid_label(grid, 3, _("Position (x,y):"));
		m.position = GTK_ENTRY(grid_control(grid, 3, gtk_entry_new()));
		gtk_entry_set_placeholder_text(m.position, _("Automatic"));
		grid_label(grid, 4, _("Rotation:"));
		GtkWidget *turn = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
		m.rotations = {"", "normal", "90", "180", "270"};
		m.transform = values_dropdown(m.rotations);
		gtk_box_append(GTK_BOX(turn), GTK_WIDGET(m.transform));
		m.flip = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Flipped")));
		gtk_widget_set_tooltip_text(GTK_WIDGET(m.flip), _("Mirrored left to right"));
		gtk_box_append(GTK_BOX(turn), GTK_WIDGET(m.flip));
		grid_control(grid, 4, turn);
		grid_label(grid, 5, _("Adaptive sync:"));
		const char *adaptive[] = {_("Automatic"), _("On"), _("Off"), nullptr};
		m.adaptive = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(adaptive));
		gtk_widget_set_tooltip_text(GTK_WIDGET(m.adaptive), info.adaptive_sync
				? _("Variable refresh rate (on now)") : _("Variable refresh rate (off now)"));
		grid_control(grid, 5, GTK_WIDGET(m.adaptive));
		GtkWidget *hdr = gtk_check_button_new_with_label(
				_("HDR10 (Vulkan renderer and an HDR monitor)"));
		grid_control(grid, 6, hdr);
		ui->hdr[info.name] = GTK_CHECK_BUTTON(hdr);
		m.looks_like = GTK_LABEL(gtk_label_new(""));
		gtk_label_set_xalign(m.looks_like, 0);
		gtk_widget_add_css_class(GTK_WIDGET(m.looks_like), "dim-label");
		grid_label(grid, 7, _("Size on the desktop:"));
		grid_control(grid, 7, GTK_WIDGET(m.looks_like));
		m.follow = add_switch(grid, 8, _("Follow this monitor:"));
		gtk_widget_set_tooltip_text(GTK_WIDGET(m.follow), _("Match it by make, model and serial "
				"number, not the port: its settings follow it through docks and adapters"));
		m.matched_by = GTK_LABEL(gtk_label_new(""));
		gtk_label_set_xalign(m.matched_by, 0);
		gtk_label_set_wrap(m.matched_by, TRUE);
		gtk_label_set_max_width_chars(m.matched_by, 52);
		gtk_widget_add_css_class(GTK_WIDGET(m.matched_by), "dim-label");
		grid_control(grid, 9, GTK_WIDGET(m.matched_by));
		ui->monitors.push_back(std::move(m));
	}
	/* Signals once the list won't move again (they point into it). */
	for (Ui::Monitor &m : ui->monitors) {
		/* Any change shows in the arranger. */
		g_signal_connect_swapped(m.enabled, "notify::active", G_CALLBACK(arranger_changed), ui);
		for (GtkDropDown *dd : {m.mode, m.scale, m.transform})
			g_signal_connect_swapped(dd, "notify::selected", G_CALLBACK(arranger_changed), ui);
		g_signal_connect_swapped(m.flip, "toggled", G_CALLBACK(arranger_changed), ui);
		g_signal_connect_swapped(m.position, "changed", G_CALLBACK(arranger_changed), ui);
		g_object_set_data(G_OBJECT(m.follow), "wb-ui", ui);
		g_signal_connect(m.follow, "notify::active", G_CALLBACK(+[](GtkSwitch *, GParamSpec *,
				gpointer data) {
			auto *m = static_cast<Ui::Monitor *>(data);
			show_matched_by(static_cast<Ui *>(g_object_get_data(G_OBJECT(m->follow), "wb-ui")),
					*m);
		}), &m);
	}
	arranger_select(ui, 0);
	arranger_changed(ui);
	return inner;
}

}  // namespace wbconf

namespace wbconf {

namespace {

/* The monitor rules back as they were (snapshots of <outputs> and <hdr>):
 * written, shown, and Waybox told. */
void revert_monitors(Ui *ui, const std::string &outputs_xml, const std::string &hdr_xml) {
	std::string problem;
	std::optional<wb::RcDocument> doc = load_document(&problem);
	if (!doc) {
		set_status(ui, problem);
		return;
	}
	doc->restore({"waybox", "outputs"}, outputs_xml);
	doc->restore({"waybox", "hdr"}, hdr_xml);
	std::string err;
	if (!doc->save_file(user_rc_path(), &err)) {
		set_status(ui, std::string(_("Could not put the monitor settings back: ")) + err);
		return;
	}
	wb::WayboxSettings back = doc->read();
	populate_monitors(ui, back.outputs);
	populate_hdr(ui, back.hdr_outputs);
	ui->shown = collect(ui);
	signal_waybox_reload();
	refresh_save_state(ui);
	set_status(ui, _("The previous monitor settings are back."));
}

struct KeepDialog {
	Ui *ui;
	std::string outputs_xml, hdr_xml;
	GtkWindow *window = nullptr;
	GtkLabel *label = nullptr;
	guint timer = 0;
	int left = 15;
	bool done = false;
};

void finish(KeepDialog *k, bool keep) {
	if (k->done)
		return;
	k->done = true;
	if (k->timer != 0)
		g_source_remove(k->timer);
	k->timer = 0;
	if (!keep)
		revert_monitors(k->ui, k->outputs_xml, k->hdr_xml);
	else
		set_status(k->ui, _("Kept the new monitor settings."));
	if (k->window != nullptr)
		gtk_window_destroy(k->window);
	delete k;
}

void show_left(KeepDialog *k) {
	char *t = g_strdup_printf(ngettext("Keep these monitor settings? Going back to the "
			"previous ones in %d second.", "Keep these monitor settings? Going back to the "
			"previous ones in %d seconds.", k->left), k->left);
	gtk_label_set_text(k->label, t);
	g_free(t);
}

}  // namespace

void ask_keep_monitors(Ui *ui, const std::string &outputs_xml, const std::string &hdr_xml) {
	/* Tests answer with WBCONF_TEST_CHOICE=monitors-keep|monitors-revert,
	 * and shorten the wait with WBCONF_KEEP_SECONDS. */
	if (ui->test_choice == "monitors-keep" || ui->test_choice == "monitors-revert") {
		if (ui->test_choice == "monitors-revert")
			revert_monitors(ui, outputs_xml, hdr_xml);
		return;
	}
	auto *k = new KeepDialog{ui, outputs_xml, hdr_xml};
	if (const char *s = getenv("WBCONF_KEEP_SECONDS"); s != nullptr && atoi(s) > 0)
		k->left = std::min(atoi(s), 120);
	k->window = GTK_WINDOW(gtk_window_new());
	gtk_window_set_title(k->window, _("Monitor Settings"));
	gtk_window_set_transient_for(k->window, ui->window);
	gtk_window_set_modal(k->window, TRUE);
	gtk_window_set_resizable(k->window, FALSE);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
	gtk_widget_set_margin_top(box, 18);
	gtk_widget_set_margin_bottom(box, 18);
	gtk_widget_set_margin_start(box, 18);
	gtk_widget_set_margin_end(box, 18);
	k->label = GTK_LABEL(gtk_label_new(""));
	gtk_label_set_wrap(k->label, TRUE);
	gtk_label_set_max_width_chars(k->label, 40);
	gtk_box_append(GTK_BOX(box), GTK_WIDGET(k->label));
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	GtkWidget *revert = gtk_button_new_with_mnemonic(_("_Revert"));
	GtkWidget *keep = gtk_button_new_with_mnemonic(_("_Keep"));
	gtk_widget_add_css_class(keep, "suggested-action");
	gtk_box_append(GTK_BOX(buttons), revert);
	gtk_box_append(GTK_BOX(buttons), keep);
	gtk_box_append(GTK_BOX(box), buttons);
	gtk_window_set_child(k->window, box);
	g_signal_connect_swapped(revert, "clicked", G_CALLBACK(+[](KeepDialog *k) {
		finish(k, false);
	}), k);
	g_signal_connect_swapped(keep, "clicked", G_CALLBACK(+[](KeepDialog *k) {
		finish(k, true);
	}), k);
	/* Closed without an answer: as Revert. */
	g_signal_connect_swapped(k->window, "close-request", G_CALLBACK(+[](KeepDialog *k) {
		k->window = nullptr;  /* closing it already */
		finish(k, false);
		return FALSE;
	}), k);
	show_left(k);
	/* The countdown goes on whether or not the dialog can be seen: a mode
	 * the monitor can't show must come back by itself. */
	k->timer = g_timeout_add_seconds(1, [](gpointer data) -> gboolean {
		auto *k = static_cast<KeepDialog *>(data);
		if (--k->left <= 0) {
			k->timer = 0;
			finish(k, false);
			return G_SOURCE_REMOVE;
		}
		show_left(k);
		return G_SOURCE_CONTINUE;
	}, k);
	gtk_window_present(k->window);
	gtk_widget_grab_focus(revert);
}

}  // namespace wbconf

namespace wbconf {

namespace {

/* Our own running copies of `binary` (by process name), in this session
 * only: never another session's, which the notice isn't about. */
std::vector<pid_t> own_copies(const std::string &binary) {
	std::vector<pid_t> out;
	uid_t me = getuid();
	std::string name = binary.substr(0, 15);
	for (const ProcessInfo &p : list_processes())
		if (p.uid == me && p.name == name && p.pid > 1 && in_this_session(p.pid))
			out.push_back(p.pid);
	return out;
}

/* Its row in Waybox's startup list, if it's on there (and on). */
Ui::ProgramRow *startup_row(Ui *ui, const wb::Integration &in) {
	for (auto &row : ui->programs)
		if (row->in == &in && !row->removed && row->listed && gtk_switch_get_active(row->on))
			return row.get();
	return nullptr;
}

void stop_copies(Ui *ui, const wb::Integration *in) {
	for (pid_t pid : own_copies(in->binary))
		kill(pid, SIGTERM);
	char *t = g_strdup_printf(_("Stopped %s for this session."), in->name.c_str());
	set_status(ui, t);
	g_free(t);
	/* Gone in a moment: look again then. */
	g_timeout_add_once(500, [](gpointer data) {
		refresh_output_manager_notice(static_cast<Ui *>(data));
	}, ui);
}

}  // namespace

void refresh_output_manager_notice(Ui *ui) {
	GtkWidget *box = ui->arranger.manager_notice;
	if (box == nullptr)
		return;
	ui->arranger.manager_stop = nullptr;  /* its buttons are made again */
	while (GtkWidget *c = gtk_widget_get_first_child(box))
		gtk_box_remove(GTK_BOX(box), c);
	bool any = false;
	for (const wb::Integration &in : ui->integrations) {
		if (!in.manages_outputs || own_copies(in.binary).empty())
			continue;
		any = true;
		Ui::ProgramRow *row = startup_row(ui, in);
		GtkWidget *frame = gtk_frame_new(nullptr);
		gtk_widget_add_css_class(frame, "wb-notice");
		GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
		gtk_widget_set_margin_top(v, 8);
		gtk_widget_set_margin_bottom(v, 8);
		gtk_widget_set_margin_start(v, 10);
		gtk_widget_set_margin_end(v, 10);
		gtk_frame_set_child(GTK_FRAME(frame), v);
		char *t = g_strdup_printf(_("%s is running. It sets the monitors itself, from its own "
				"profiles, whenever one is connected (and after switching to a text console "
				"and back), so the settings here can be undone by it."), in.name.c_str());
		GtkWidget *label = gtk_label_new(t);
		g_free(t);
		gtk_label_set_wrap(GTK_LABEL(label), TRUE);
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_box_append(GTK_BOX(v), label);
		GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		if (row != nullptr) {
			/* Waybox would start it again: off in its startup list. */
			GtkWidget *off = gtk_button_new_with_label(_("Turn It Off"));
			gtk_widget_set_tooltip_text(off, _("Off in Waybox's startup list: stopped when "
					"you save"));
			g_signal_connect_swapped(off, "clicked", G_CALLBACK(+[](GtkSwitch *s) {
				gtk_switch_set_active(s, FALSE);
			}), row->on);
			gtk_box_append(GTK_BOX(buttons), off);
			ui->arranger.manager_stop = off;
		} else {
			GtkWidget *stop = gtk_button_new_with_label(_("Stop It for This Session"));
			gtk_widget_set_tooltip_text(stop, _("It starts again next time; see where it "
					"starts to switch it off"));
			g_object_set_data(G_OBJECT(stop), "wb-integration", const_cast<wb::Integration *>(&in));
			g_signal_connect(stop, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer data) {
				stop_copies(static_cast<Ui *>(data), static_cast<const wb::Integration *>(
						g_object_get_data(G_OBJECT(b), "wb-integration")));
			}), ui);
			gtk_box_append(GTK_BOX(buttons), stop);
			ui->arranger.manager_stop = stop;
		}
		GtkWidget *where = gtk_button_new_with_label(_("Where It Starts"));
		g_object_set_data_full(G_OBJECT(where), "wb-name", g_strdup(in.binary.c_str()), g_free);
		g_signal_connect(where, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer data) {
			auto *ui = static_cast<Ui *>(data);
			gtk_editable_set_text(GTK_EDITABLE(ui->search), static_cast<const char *>(
					g_object_get_data(G_OBJECT(b), "wb-name")));
		}), ui);
		gtk_box_append(GTK_BOX(buttons), where);
		gtk_box_append(GTK_BOX(v), buttons);
		gtk_box_append(GTK_BOX(box), frame);
	}
	gtk_widget_set_visible(box, any);
}

bool output_manager_test_stop(Ui *ui) {
	refresh_output_manager_notice(ui);
	if (ui->arranger.manager_stop == nullptr)
		return false;
	g_signal_emit_by_name(ui->arranger.manager_stop, "clicked");
	return true;
}

}  // namespace wbconf
