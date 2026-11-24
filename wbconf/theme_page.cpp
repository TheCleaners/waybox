#include "wbconf_ui.hpp"

namespace wbconf {

void to_cairo(cairo_t *cr, const wb::Color &c) {
	cairo_set_source_rgba(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0,
			c.a / 255.0);
}

/* Draw one of the three glyph shapes (iconify, maximize, close) centred in a
 * square, in the given colour. */
void preview_glyph(cairo_t *cr, double cx, double cy, double s, int kind,
		const wb::Color &fg) {
	to_cairo(cr, fg);
	cairo_set_line_width(cr, 1.5);
	if (kind == 0) {  /* iconify: bottom line */
		cairo_move_to(cr, cx - s, cy + s);
		cairo_line_to(cr, cx + s, cy + s);
	} else if (kind == 1) {  /* maximize: square */
		cairo_rectangle(cr, cx - s, cy - s, 2 * s, 2 * s);
	} else {  /* close: X */
		cairo_move_to(cr, cx - s, cy - s);
		cairo_line_to(cr, cx + s, cy + s);
		cairo_move_to(cr, cx + s, cy - s);
		cairo_line_to(cr, cx - s, cy + s);
	}
	cairo_stroke(cr);
}

/* Draw a single titlebar sample (active or inactive) with the theme's colours
 * and label font, plus the three window buttons. Returns the height drawn. */
int preview_titlebar(cairo_t *cr, const wb::Theme &theme, bool active,
		int x, int y, int w, const char *title) {
	wb::FrameStyle fs = wb::frame_style_from_theme(theme, active);
	int text_h = wb::measure_text("Ag", fs.label.font).height;
	int pad = std::max(theme.padding_y, 3);
	int h = text_h + 2 * pad;
	if (h < 18)
		h = 18;

	/* border + titlebar fill */
	to_cairo(cr, fs.border.color);
	cairo_rectangle(cr, x - 1, y - 1, w + 2, h + 2);
	cairo_fill(cr);
	wb::paint_fill(cr, x, y, w, h, fs.title_bar.fill);

	/* label */
	int ty = y + (h - text_h) / 2;
	wb::paint_text(cr, x + 6, ty, title, fs.label.color, fs.label.font);

	/* three buttons on the right */
	int bs = (h * 2) / 3;
	int by = y + (h - bs) / 2;
	int bx = x + w - 6 - 3 * (bs + 2);
	const wb::StateStyle &btn = fs.button.for_state(wb::WidgetState::Normal);
	for (int i = 0; i < 3; i++) {
		int cx = bx + i * (bs + 2) + bs / 2;
		preview_glyph(cr, cx, by + bs / 2.0, bs * 0.28, i, btn.fg);
	}
	return h;
}

void preview_draw(GtkDrawingArea *, cairo_t *cr, int width, int height,
		gpointer data) {
	Ui *ui = static_cast<Ui *>(data);
	wb::WayboxSettings s = collect(ui);  /* the theme selected now */

	/* Resolve the theme being previewed (the live selection), with the live
	 * font overrides applied on top. */
	wb::Theme theme = s.theme_name ? wb::load_theme(*s.theme_name)
			: wb::default_theme();
	auto apply_font = [](const std::optional<std::string> &v, wb::FontSpec &f) {
		if (!v)
			return;
		PangoFontDescription *d = pango_font_description_from_string(v->c_str());
		if (const char *fam = pango_font_description_get_family(d))
			f.family = fam;
		int sz = pango_font_description_get_size(d);
		if (sz > 0)
			f.size_pt = pango_font_description_get_size_is_absolute(d)
					? sz / PANGO_SCALE
					: sz / PANGO_SCALE;
		f.bold = pango_font_description_get_weight(d) >= PANGO_WEIGHT_BOLD;
		pango_font_description_free(d);
	};
	/* The fonts as set now (unsaved too), or their defaults. */
	apply_font(setting_value(ui, "theme.font.activeWindow"), theme.font_active_title);
	apply_font(setting_value(ui, "theme.font.inactiveWindow"), theme.font_inactive_title);
	apply_font(setting_value(ui, "theme.font.menuItem"), theme.font_menu_item);
	apply_font(setting_value(ui, "theme.font.osd"), theme.font_osd);

	/* Backdrop. */
	cairo_set_source_rgb(cr, 0.18, 0.18, 0.2);
	cairo_rectangle(cr, 0, 0, width, height);
	cairo_fill(cr);

	int margin = 12;
	int w = width - 2 * margin;
	int y = margin;
	y += preview_titlebar(cr, theme, true, margin, y, w, _("Active window")) + 4;
	/* a thin client area below the active titlebar */
	cairo_set_source_rgb(cr, 0.93, 0.93, 0.93);
	cairo_rectangle(cr, margin, y, w, 26);
	cairo_fill(cr);
	y += 26 + 12;
	y += preview_titlebar(cr, theme, false, margin, y, w, _("Inactive window")) + 4;

	/* A small menu sample. */
	wb::MenuStyle ms = wb::menu_style_from_theme(theme);
	y += 12;
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

void preview_refresh(Ui *ui) {
	if (ui->preview != nullptr)
		gtk_widget_queue_draw(ui->preview);
}

/* The theme list, with a live preview beside it. */
GtkWidget *build_theme_block(Ui *ui) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
			GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(scroller, 200, 230);
	gtk_widget_add_css_class(scroller, "frame");
	GtkWidget *list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_BROWSE);
	ui->theme_list = GTK_LIST_BOX(list);
	g_signal_connect_swapped(list, "selected-rows-changed",
			G_CALLBACK(preview_refresh), ui);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list);
	gtk_box_append(GTK_BOX(box), scroller);

	GtkWidget *area = gtk_drawing_area_new();
	gtk_widget_set_size_request(area, 320, 230);
	gtk_widget_set_hexpand(area, TRUE);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), preview_draw, ui,
			nullptr);
	ui->preview = area;
	gtk_box_append(GTK_BOX(box), area);
	return box;
}

}  // namespace wbconf
