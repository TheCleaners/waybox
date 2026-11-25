#include "wbconf_ui.hpp"
#include "waybox/titlebar_draw.hpp"

namespace wbconf {

void to_cairo(cairo_t *cr, const wb::Color &c) {
	cairo_set_source_rgba(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0,
			c.a / 255.0);
}



/* A theme as it would look now: loaded once, with the fonts as set
 * (saved or not) on top. */
wb::Theme preview_theme(Ui *ui, const std::string &name) {
	auto it = ui->theme_cache.find(name);
	if (it == ui->theme_cache.end())
		it = ui->theme_cache.emplace(name, name.empty() ? wb::default_theme()
				: wb::load_theme(name)).first;
	wb::Theme theme = it->second;
	auto apply_font = [](const std::optional<std::string> &v, wb::FontSpec &f) {
		if (!v)
			return;
		PangoFontDescription *d = pango_font_description_from_string(v->c_str());
		if (const char *fam = pango_font_description_get_family(d))
			f.family = fam;
		int sz = pango_font_description_get_size(d);
		if (sz > 0)
			f.size_pt = sz / PANGO_SCALE;
		f.bold = pango_font_description_get_weight(d) >= PANGO_WEIGHT_BOLD;
		pango_font_description_free(d);
	};
	apply_font(setting_value(ui, "theme.font.activeWindow"), theme.font_active_title);
	apply_font(setting_value(ui, "theme.font.inactiveWindow"), theme.font_inactive_title);
	apply_font(setting_value(ui, "theme.font.menuItem"), theme.font_menu_item);
	apply_font(setting_value(ui, "theme.font.osd"), theme.font_osd);
	return theme;
}

/* The titlebar settings as set now: button order, padding, button size
 * (unset ones automatic, as Waybox would have them). */
struct TitlebarLook {
	wb::TitleLayout layout;
	int pad_y = -1, button_size = 0;
};

TitlebarLook titlebar_look(Ui *ui) {
	TitlebarLook look;
	std::string order = setting_value(ui, "theme.titleLayout");
	look.layout = wb::parse_title_layout(order.empty() ? "NLIMC" : order);
	if (auto v = setting_if_set(ui, "waybox.titlebar.paddingY"))
		look.pad_y = atoi(v->c_str());
	if (auto v = setting_if_set(ui, "waybox.titlebar.buttonSize"))
		look.button_size = atoi(v->c_str());
	return look;
}

/* A sample window of `theme`, `client_w` wide, drawn by the same code as
 * Waybox's frames. Returns its height. */
int draw_sample_window(cairo_t *cr, const wb::Theme &theme, const TitlebarLook &look,
		bool active, double x, double y, int client_w, int client_h, const char *title) {
	wb::FrameStyle fs = wb::frame_style_from_theme(theme, active);
	wb::FrameMetrics m = wb::frame_metrics_for(theme, fs, look.pad_y, look.button_size, -1);
	wb::TitlebarState st;
	st.title = title;
	wb::draw_frame_sample(cr, x, y, client_w, client_h, m, fs, look.layout.buttons,
			look.layout.left, st, wb::Color{238, 238, 238, 255});
	return wb::frame_height(client_h, m);
}

void preview_draw(GtkDrawingArea *, cairo_t *cr, int width, int height,
		gpointer data) {
	Ui *ui = static_cast<Ui *>(data);
	wb::WayboxSettings s = collect(ui);  /* the theme selected now */
	wb::Theme theme = preview_theme(ui, s.theme_name.value_or(""));
	TitlebarLook look = titlebar_look(ui);

	/* Backdrop. */
	cairo_set_source_rgb(cr, 0.18, 0.18, 0.2);
	cairo_rectangle(cr, 0, 0, width, height);
	cairo_fill(cr);

	int margin = 12;
	int w = width - 2 * margin - 2;
	int y = margin;
	y += draw_sample_window(cr, theme, look, true, margin, y, w, 26, _("Active window")) + 10;
	y += draw_sample_window(cr, theme, look, false, margin, y, w, 0, _("Inactive window")) + 14;

	/* A small menu sample. */
	wb::MenuStyle ms = wb::menu_style_from_theme(theme);
	int mh = wb::measure_text("Ag", ms.item_text.font).height + 6;
	const char *items[] = {_("Terminal"), _("Web Browser"), _("Files")};
	int mw = w / 2;
	for (int i = 0; i < 3; i++) {
		bool hot = (i == 1);
		const wb::StateStyle &st = ms.item.for_state(
				hot ? wb::WidgetState::Hover : wb::WidgetState::Normal);
		wb::paint_fill(cr, margin, y, mw, mh, st.fill);
		wb::paint_text(cr, margin + 8, y + 3, items[i], st.fg,
				ms.item_text.font);
		y += mh;
	}
}

/* A theme's thumbnail in the list: its active and inactive titlebars,
 * with the button order and sizes as set (as ObConf's list does). */
void thumb_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data) {
	Ui *ui = static_cast<Ui *>(data);
	const char *name = static_cast<const char *>(g_object_get_data(G_OBJECT(area), "wb-theme"));
	wb::Theme theme = preview_theme(ui, name ? name : "");
	TitlebarLook look = titlebar_look(ui);
	int y = 2;
	y += draw_sample_window(cr, theme, look, true, 2, y, width - 6, 0, name ? name : "") + 3;
	draw_sample_window(cr, theme, look, false, 2, y, width - 6, 0, _("Inactive"));
	(void)height;
}


void preview_refresh(Ui *ui) {
	if (ui->preview != nullptr)
		gtk_widget_queue_draw(ui->preview);
	for (GtkWidget *t : ui->theme_thumbs)
		gtk_widget_queue_draw(t);
}

/* The theme list, with a live preview beside it. */
GtkWidget *build_theme_block(Ui *ui) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
			GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(scroller, 250, 300);
	gtk_widget_add_css_class(scroller, "frame");
	GtkWidget *list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_BROWSE);
	ui->theme_list = GTK_LIST_BOX(list);
	g_signal_connect_swapped(list, "selected-rows-changed",
			G_CALLBACK(preview_refresh), ui);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list);
	gtk_box_append(GTK_BOX(box), scroller);

	GtkWidget *area = gtk_drawing_area_new();
	gtk_widget_set_size_request(area, 320, 300);
	gtk_widget_set_hexpand(area, TRUE);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), preview_draw, ui,
			nullptr);
	ui->preview = area;
	gtk_box_append(GTK_BOX(box), area);
	return box;
}

}  // namespace wbconf
