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
			auto valid = [](const std::string &s) {
				char *v = g_utf8_make_valid(s.c_str(), static_cast<gssize>(s.size()));
				std::string out = v;
				g_free(v);
				return out;
			};
			m.name = valid(str("name"));
			m.description = valid(str("description"));
			m.make = valid(str("make"));
			m.model = valid(str("model"));
			m.serial = valid(str("serial"));
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

void populate_monitors(Ui *ui, const std::vector<wb::OutputSetting> &outputs) {
	ui->other_outputs.clear();
	for (const wb::OutputSetting &o : outputs) {
		bool connected = false;
		for (const Ui::Monitor &m : ui->monitors)
			connected = connected || m.name == o.name;
		if (!connected)
			ui->other_outputs.push_back(o);
	}
	for (Ui::Monitor &m : ui->monitors) {
		std::map<std::string, std::string> attrs;
		for (const wb::OutputSetting &o : outputs)
			if (o.name == m.name)
				attrs = o.attrs;
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
		select_value(m.transform, kTransforms, take("transform"));
		m.kept = attrs;
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
		m.transform = values_dropdown(kTransforms);
		grid_control(grid, 4, GTK_WIDGET(m.transform));
		GtkWidget *hdr = gtk_check_button_new_with_label(
				_("HDR10 (Vulkan renderer and an HDR monitor)"));
		grid_control(grid, 5, hdr);
		ui->hdr[info.name] = GTK_CHECK_BUTTON(hdr);
		m.looks_like = GTK_LABEL(gtk_label_new(""));
		gtk_label_set_xalign(m.looks_like, 0);
		gtk_widget_add_css_class(GTK_WIDGET(m.looks_like), "dim-label");
		grid_label(grid, 6, _("Size on the desktop:"));
		grid_control(grid, 6, GTK_WIDGET(m.looks_like));
		/* Any change shows in the arranger. */
		g_signal_connect_swapped(m.enabled, "notify::active", G_CALLBACK(arranger_changed), ui);
		for (GtkDropDown *dd : {m.mode, m.scale, m.transform})
			g_signal_connect_swapped(dd, "notify::selected", G_CALLBACK(arranger_changed), ui);
		g_signal_connect_swapped(m.position, "changed", G_CALLBACK(arranger_changed), ui);
		ui->monitors.push_back(std::move(m));
	}
	arranger_select(ui, 0);
	arranger_changed(ui);
	return inner;
}

}  // namespace wbconf

namespace wbconf {

namespace {

/* Monitor settings back to `before`'s: written, shown, and Waybox told. */
void revert_monitors(Ui *ui, const wb::WayboxSettings &before) {
	std::string problem;
	std::optional<wb::RcDocument> doc = load_document(&problem);
	if (!doc) {
		set_status(ui, problem);
		return;
	}
	wb::WayboxSettings back = ui->shown;
	back.outputs = before.outputs;
	back.hdr_outputs = before.hdr_outputs;
	doc->apply(back, ui->shown);
	std::string err;
	if (!doc->save_file(user_rc_path(), &err)) {
		set_status(ui, std::string(_("Could not put the monitor settings back: ")) + err);
		return;
	}
	populate_monitors(ui, back.outputs);
	populate_hdr(ui, back.hdr_outputs);
	ui->shown = collect(ui);
	signal_waybox_reload();
	refresh_save_state(ui);
	set_status(ui, _("The previous monitor settings are back."));
}

struct KeepDialog {
	Ui *ui;
	wb::WayboxSettings before;
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
		revert_monitors(k->ui, k->before);
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

void ask_keep_monitors(Ui *ui, const wb::WayboxSettings &before) {
	/* Tests answer with WBCONF_TEST_CHOICE=monitors-keep|monitors-revert,
	 * and shorten the wait with WBCONF_KEEP_SECONDS. */
	if (ui->test_choice == "monitors-keep" || ui->test_choice == "monitors-revert") {
		if (ui->test_choice == "monitors-revert")
			revert_monitors(ui, before);
		return;
	}
	auto *k = new KeepDialog{ui, before};
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
