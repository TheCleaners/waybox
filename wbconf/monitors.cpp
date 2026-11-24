#include "wbconf_ui.hpp"

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
			m.name = json_object_get_string_member(o, "name");
			if (json_object_has_member(o, "description"))
				m.description = json_object_get_string_member(o, "description");
			if (json_object_has_member(o, "modes")) {
				JsonArray *modes = json_object_get_array_member(o, "modes");
				for (guint j = 0; j < json_array_get_length(modes); j++) {
					JsonObject *md = json_array_get_object_element(modes, j);
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
}

GtkWidget *build_monitors_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_widget_set_vexpand(scroller, TRUE);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
			GTK_POLICY_AUTOMATIC);
	GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), inner);
	gtk_box_append(GTK_BOX(page), scroller);

	GtkWidget *note = gtk_label_new(_("Applied when a monitor is connected and on "
			"reload. While kanshi (or another output manager) runs, its profiles "
			"win; a monitor it changed keeps that for the session."));
	gtk_label_set_wrap(GTK_LABEL(note), TRUE);
	gtk_label_set_xalign(GTK_LABEL(note), 0);
	gtk_widget_add_css_class(note, "dim-label");
	gtk_box_append(GTK_BOX(inner), note);

	std::string why;
	auto monitors = query_monitors(&why);
	if (!monitors || monitors->empty()) {
		GtkWidget *msg = gtk_label_new(why.c_str());
		gtk_label_set_wrap(GTK_LABEL(msg), TRUE);
		gtk_label_set_xalign(GTK_LABEL(msg), 0);
		gtk_box_append(GTK_BOX(inner), msg);
		return page;
	}
	ui->monitors.reserve(monitors->size());
	for (const MonitorInfo &info : *monitors) {
		std::string title = info.name;
		if (!info.description.empty())
			title += "  " + info.description;
		GtkWidget *grid = section_grid(section(inner, title.c_str()));
		Ui::Monitor m;
		m.name = info.name;
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
		ui->monitors.push_back(std::move(m));
	}
	return page;
}

}  // namespace wbconf
