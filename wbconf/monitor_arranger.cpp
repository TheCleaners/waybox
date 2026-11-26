/*
 * The monitor arranger: each monitor a box at its size on the desktop
 * (mode over scale, turned for rotation), placed as they're laid out.
 * Drag one to move it: it snaps to the others' edges and corners, and
 * when dropped it goes to the nearest place that touches another and
 * overlaps none; then the arrangement starts at 0,0 again. Arrow keys
 * nudge the selected one (Shift: by 1). Clicking one shows its settings.
 *
 * What it shows is the monitors' settings in their cards, "Automatic"
 * being what each monitor has now; moving one writes every monitor's
 * position.
 */
#include "wbconf_ui.hpp"

#include <pango/pangocairo.h>

#include "waybox/monitor_layout.hpp"

namespace wbconf {

namespace {

constexpr int kMargin = 16;
constexpr int kSnap = 14;   /* canvas pixels */
constexpr int kOffGap = 18; /* between the layout and switched-off monitors */

/* A monitor as it would be with its settings now. */
struct Effective {
	int x, y, w, h;
	bool enabled;
	int mode_w, mode_h;
	double scale;
};

Effective effective(const Ui::Monitor &m) {
	Effective e{};
	e.enabled = gtk_switch_get_active(m.enabled);
	std::string mode = selected_value(m.mode, m.modes);
	int mw = m.info.width, mh = m.info.height;
	if (!mode.empty())
		sscanf(mode.c_str(), "%dx%d", &mw, &mh);
	if (mw <= 0 || mh <= 0) {
		mw = 1920;
		mh = 1080;
	}
	std::string scale = selected_value(m.scale, m.scales);
	double sc = scale.empty() ? m.info.scale : g_ascii_strtod(scale.c_str(), nullptr);
	/* Its size depends on the rotation only (a flip keeps it): the card's,
	 * or with that on Automatic (or written by hand), the one it has now. */
	std::string tr = wb::split_transform(monitor_transform(m))
			.value_or(std::pair{std::string(), false}).first;
	if (tr.empty())
		tr = m.info.transform;
	auto [w, h] = wb::logical_size(mw, mh, sc, tr);
	e.w = w;
	e.h = h;
	e.mode_w = mw;
	e.mode_h = mh;
	e.scale = sc > 0 ? sc : 1;
	e.x = m.info.x;
	e.y = m.info.y;
	if (auto p = wb::parse_position(gtk_editable_get_text(GTK_EDITABLE(m.position)))) {
		e.x = p->first;
		e.y = p->second;
	}
	return e;
}

/* The switched-on monitors as boxes, and which monitor each is. */
std::vector<wb::LayoutBox> boxes(Ui *ui, std::vector<int> *which) {
	std::vector<wb::LayoutBox> out;
	which->clear();
	for (size_t i = 0; i < ui->monitors.size(); ++i) {
		Effective e = effective(ui->monitors[i]);
		if (!e.enabled)
			continue;
		out.push_back({ui->monitors[i].name, e.x, e.y, e.w, e.h});
		which->push_back(static_cast<int>(i));
	}
	return out;
}

/* Where a monitor is drawn, in canvas pixels. */
struct Drawn {
	int monitor;
	double x, y, w, h;
	bool enabled;
};

/* Fit the layout (and a row of switched-off monitors under it) in the
 * canvas; while dragging, the scale stays put. */
std::vector<Drawn> layout(Ui *ui, int width, int height) {
	auto &a = ui->arranger;
	std::vector<int> which;
	std::vector<wb::LayoutBox> b = boxes(ui, &which);
	if (a.dragging >= 0)
		for (size_t k = 0; k < which.size(); ++k)
			if (which[k] == a.dragging) {
				b[k].x = a.drag_x;
				b[k].y = a.drag_y;
			}
	std::vector<int> off;
	int off_w = 0, off_h = 0;
	for (size_t i = 0; i < ui->monitors.size(); ++i) {
		Effective e = effective(ui->monitors[i]);
		if (!e.enabled) {
			off.push_back(static_cast<int>(i));
			off_w += e.w + (off_w ? 200 : 0);
			off_h = std::max(off_h, e.h);
		}
	}
	if (a.dragging < 0) {
		int minx = 0, miny = 0, maxx = 1, maxy = 1;
		bool first = true;
		for (const wb::LayoutBox &x : b) {
			if (first) {
				minx = x.x; miny = x.y; maxx = x.x + x.w; maxy = x.y + x.h;
				first = false;
			}
			minx = std::min(minx, x.x);
			miny = std::min(miny, x.y);
			maxx = std::max(maxx, x.x + x.w);
			maxy = std::max(maxy, x.y + x.h);
		}
		double lw = maxx - minx, lh = maxy - miny;
		double extra_h = off.empty() ? 0 : off_h;  /* in logical pixels */
		lw = std::max(lw, double(off_w));
		/* Room around, so a monitor can be dragged past the others. */
		double sx = (width - 2.0 * kMargin) / (lw * 1.25);
		double sy = (height - 2.0 * kMargin - (off.empty() ? 0 : kOffGap)) /
				((lh + extra_h) * 1.15);
		a.scale = std::max(0.005, std::min(sx, sy));
		a.origin_x = minx;
		a.origin_y = miny;
		a.margin_x = (width - (maxx - minx) * a.scale) / 2;
		a.margin_y = kMargin + std::max(0.0, (height - 2.0 * kMargin -
				(lh + extra_h) * a.scale - (off.empty() ? 0 : kOffGap)) / 2);
	}
	std::vector<Drawn> out;
	for (size_t k = 0; k < b.size(); ++k)
		out.push_back({which[k], (b[k].x - a.origin_x) * a.scale + a.margin_x,
				(b[k].y - a.origin_y) * a.scale + a.margin_y, b[k].w * a.scale,
				b[k].h * a.scale, true});
	/* Switched off: in a row under the layout, to select and switch on. */
	double maxy = a.margin_y;
	for (const Drawn &d : out)
		maxy = std::max(maxy, d.y + d.h);
	double x = kMargin;
	for (int i : off) {
		Effective e = effective(ui->monitors[i]);
		out.push_back({i, x, maxy + kOffGap, e.w * a.scale, e.h * a.scale, false});
		x += e.w * a.scale + 12;
	}
	return out;
}

std::string caption_of(const MonitorInfo &i) {
	return wb::monitor_caption(i.make, i.model, i.description, i.name);
}

void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
	r = std::min({r, w / 2, h / 2});
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
	cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
	cairo_close_path(cr);
}

void draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	GdkRGBA fg;
	gtk_widget_get_color(GTK_WIDGET(area), &fg);
	for (const Drawn &d : layout(ui, width, height)) {
		const Ui::Monitor &m = ui->monitors[d.monitor];
		bool sel = d.monitor == ui->arranger.selected;
		rounded(cr, d.x + 1, d.y + 1, d.w - 2, d.h - 2, 6);
		if (!d.enabled)
			cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.06);
		else if (sel)
			cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, 0.85);
		else
			cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.18);
		cairo_fill_preserve(cr);
		cairo_set_line_width(cr, sel ? 2.5 : 1);
		if (!d.enabled) {
			double dash[] = {4, 3};
			cairo_set_dash(cr, dash, 2, 0);
		}
		cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, sel ? 0.9 : 0.5);
		cairo_stroke(cr);
		cairo_set_dash(cr, nullptr, 0, 0);
		/* The name on one line; under it, make and model (or "Off") in as
		 * many lines as fit, broken anywhere if a word is too long; each
		 * cut short with "…". The full text is in the tooltip. */
		double inner_w = d.w - 10, inner_h = d.h - 8;
		if (inner_w < 8 || inner_h < 8)
			continue;
		PangoLayout *title = gtk_widget_create_pango_layout(GTK_WIDGET(area), m.name.c_str());
		PangoAttrList *bold = pango_attr_list_new();
		pango_attr_list_insert(bold, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
		pango_layout_set_attributes(title, bold);
		pango_attr_list_unref(bold);
		pango_layout_set_alignment(title, PANGO_ALIGN_CENTER);
		pango_layout_set_width(title, static_cast<int>(inner_w * PANGO_SCALE));
		pango_layout_set_ellipsize(title, PANGO_ELLIPSIZE_END);
		pango_layout_set_single_paragraph_mode(title, TRUE);
		int tw, th;
		pango_layout_get_pixel_size(title, &tw, &th);
		std::string caption = d.enabled ? caption_of(m.info) : _("Off");
		PangoLayout *sub = nullptr;
		int sw = 0, sh = 0;
		if (!caption.empty() && inner_h - th >= 10) {
			sub = gtk_widget_create_pango_layout(GTK_WIDGET(area), caption.c_str());
			PangoAttrList *small = pango_attr_list_new();
			pango_attr_list_insert(small, pango_attr_scale_new(PANGO_SCALE_SMALL));
			pango_layout_set_attributes(sub, small);
			pango_attr_list_unref(small);
			pango_layout_set_alignment(sub, PANGO_ALIGN_CENTER);
			pango_layout_set_width(sub, static_cast<int>(inner_w * PANGO_SCALE));
			pango_layout_set_wrap(sub, PANGO_WRAP_WORD_CHAR);
			pango_layout_set_height(sub, static_cast<int>((inner_h - th) * PANGO_SCALE));
			pango_layout_set_ellipsize(sub, PANGO_ELLIPSIZE_END);
			pango_layout_get_pixel_size(sub, &sw, &sh);
			if (sh > inner_h - th) {  /* not even one line fits */
				g_object_unref(sub);
				sub = nullptr;
				sh = 0;
			}
		}
		if (sel)
			cairo_set_source_rgb(cr, 1, 1, 1);
		else
			cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, d.enabled ? 0.95 : 0.6);
		cairo_save(cr);
		cairo_rectangle(cr, d.x + 2, d.y + 2, d.w - 4, d.h - 4);
		cairo_clip(cr);
		double top = d.y + (d.h - th - sh) / 2;
		cairo_move_to(cr, d.x + 5, top);
		pango_cairo_show_layout(cr, title);
		if (sub != nullptr) {
			cairo_move_to(cr, d.x + 5, top + th);
			pango_cairo_show_layout(cr, sub);
			g_object_unref(sub);
		}
		cairo_restore(cr);
		g_object_unref(title);
	}
}

int hit(Ui *ui, double x, double y) {
	int w = gtk_widget_get_width(ui->arranger.area), h = gtk_widget_get_height(ui->arranger.area);
	std::vector<Drawn> all = layout(ui, w, h);
	for (auto it = all.rbegin(); it != all.rend(); ++it)
		if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h)
			return it->monitor;
	return -1;
}

/* Every switched-on monitor's position, from boxes moved to start at 0,0. */
void write_positions(Ui *ui, std::vector<wb::LayoutBox> b, const std::vector<int> &which) {
	wb::normalize_layout(b);
	for (size_t k = 0; k < b.size(); ++k) {
		std::string pos = std::to_string(b[k].x) + "," + std::to_string(b[k].y);
		gtk_editable_set_text(GTK_EDITABLE(ui->monitors[which[k]].position), pos.c_str());
	}
}

/* Monitor `index` dropped (or nudged) to x,y: placed, or refused. With
 * `exact`, only there (a nudge): no other place. */
bool drop(Ui *ui, int index, int x, int y, bool exact) {
	std::vector<int> which;
	std::vector<wb::LayoutBox> b = boxes(ui, &which);
	auto at = std::find(which.begin(), which.end(), index);
	if (at == which.end())
		return false;
	size_t k = static_cast<size_t>(at - which.begin());
	auto p = wb::place_box(b, k, x, y);
	if (!p || (exact && *p != std::pair{x, y}))
		return false;
	b[k].x = p->first;
	b[k].y = p->second;
	write_positions(ui, b, which);
	return true;
}

void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	int i = hit(ui, x, y);
	gtk_widget_grab_focus(ui->arranger.area);
	if (i < 0) {
		gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
		return;
	}
	arranger_select(ui, i);
	Effective e = effective(ui->monitors[i]);
	if (!e.enabled)
		return;
	ui->arranger.dragging = i;
	ui->arranger.start_x = ui->arranger.drag_x = e.x;
	ui->arranger.start_y = ui->arranger.drag_y = e.y;
}

void on_drag_update(GtkGestureDrag *, double dx, double dy, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	auto &a = ui->arranger;
	if (a.dragging < 0)
		return;
	int x = a.start_x + static_cast<int>(std::lround(dx / a.scale));
	int y = a.start_y + static_cast<int>(std::lround(dy / a.scale));
	std::vector<int> which;
	std::vector<wb::LayoutBox> b = boxes(ui, &which);
	auto at = std::find(which.begin(), which.end(), a.dragging);
	if (at != which.end()) {
		auto s = wb::snap_position(b, static_cast<size_t>(at - which.begin()), x, y,
				static_cast<int>(kSnap / a.scale));
		x = s.first;
		y = s.second;
	}
	a.drag_x = x;
	a.drag_y = y;
	gtk_widget_queue_draw(a.area);
}

void on_drag_end(GtkGestureDrag *, double, double, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	auto &a = ui->arranger;
	if (a.dragging < 0)
		return;
	int i = a.dragging;
	a.dragging = -1;
	if (a.drag_x != a.start_x || a.drag_y != a.start_y)
		drop(ui, i, a.drag_x, a.drag_y, false);
	arranger_changed(ui);
}

gboolean on_key(GtkEventControllerKey *, guint key, guint, GdkModifierType mods,
		gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	int i = ui->arranger.selected;
	if (i < 0 || i >= static_cast<int>(ui->monitors.size()))
		return FALSE;
	int step = (mods & GDK_SHIFT_MASK) ? 1 : 10;
	int dx = 0, dy = 0;
	switch (key) {
	case GDK_KEY_Left: dx = -step; break;
	case GDK_KEY_Right: dx = step; break;
	case GDK_KEY_Up: dy = -step; break;
	case GDK_KEY_Down: dy = step; break;
	default: return FALSE;
	}
	Effective e = effective(ui->monitors[i]);
	if (e.enabled && drop(ui, i, e.x + dx, e.y + dy, true))
		arranger_changed(ui);
	else
		gtk_widget_error_bell(ui->arranger.area);
	return TRUE;
}

/* Everything about the monitor under the pointer, uncut. */
gboolean on_tooltip(GtkWidget *, int x, int y, gboolean, GtkTooltip *tip, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	int i = hit(ui, x, y);
	if (i < 0)
		return FALSE;
	const MonitorInfo &m = ui->monitors[i].info;
	std::string text = m.name;
	auto line = [&text](const char *label, const std::string &v) {
		if (!v.empty())
			text += std::string("\n") + label + v;
	};
	line(_("Make: "), m.make);
	line(_("Model: "), m.model);
	line(_("Serial: "), m.serial);
	if (m.make.empty() && m.model.empty())
		line("", m.description);
	gtk_tooltip_set_text(tip, text.c_str());
	return TRUE;
}

void show_cards(Ui *ui) {
	bool all = ui->arranger.advanced != nullptr &&
			gtk_check_button_get_active(ui->arranger.advanced);
	for (size_t i = 0; i < ui->monitors.size(); ++i)
		if (ui->monitors[i].card != nullptr)
			gtk_widget_set_visible(ui->monitors[i].card,
					all || static_cast<int>(i) == ui->arranger.selected);
}

}  // namespace

void arranger_select(Ui *ui, int index) {
	ui->arranger.selected = index;
	show_cards(ui);
	if (ui->arranger.area != nullptr)
		gtk_widget_queue_draw(ui->arranger.area);
}

void arranger_changed(Ui *ui) {
	for (Ui::Monitor &m : ui->monitors) {
		if (m.looks_like == nullptr)
			continue;
		Effective e = effective(m);
		char *t = std::abs(e.scale - 1) > 1e-6
				? g_strdup_printf(_("Looks like %d×%d (%d×%d at %s×)"), e.w, e.h, e.mode_w,
						e.mode_h, format_hz(e.scale).c_str())
				: g_strdup_printf(_("%d×%d"), e.w, e.h);
		gtk_label_set_text(m.looks_like, t);
		g_free(t);
	}
	if (ui->arranger.area != nullptr)
		gtk_widget_queue_draw(ui->arranger.area);
}

void arranger_test_describe(Ui *ui, const std::string &what, const std::string &name,
		const std::string &text) {
	for (Ui::Monitor &m : ui->monitors)
		if (m.name == name) {
			std::string &field = what == "make" ? m.info.make : what == "model"
					? m.info.model : what == "serial" ? m.info.serial : m.info.description;
			field = text;
		}
	arranger_changed(ui);
}

bool arranger_test_drop(Ui *ui, const std::string &name, int x, int y) {
	for (size_t i = 0; i < ui->monitors.size(); ++i)
		if (ui->monitors[i].name == name) {
			bool ok = drop(ui, static_cast<int>(i), x, y, false);
			arranger_changed(ui);
			return ok;
		}
	return false;
}

GtkWidget *build_arranger(Ui *ui) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	GtkWidget *area = gtk_drawing_area_new();
	ui->arranger.area = area;
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 200);
	gtk_widget_set_hexpand(area, TRUE);
	gtk_widget_set_focusable(area, TRUE);
	gtk_widget_set_cursor_from_name(area, "grab");
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw, ui, nullptr);
	gtk_widget_set_has_tooltip(area, TRUE);
	g_signal_connect(area, "query-tooltip", G_CALLBACK(on_tooltip), ui);
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), ui);
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), ui);
	g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), ui);
	gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(drag));
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), ui);
	gtk_widget_add_controller(area, keys);
	GtkWidget *frame = gtk_frame_new(nullptr);
	gtk_frame_set_child(GTK_FRAME(frame), area);
	gtk_box_append(GTK_BOX(box), frame);

	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *hint = gtk_label_new(_("Drag a monitor to move it; arrow keys nudge the "
			"selected one. Click one to see its settings."));
	gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
	gtk_label_set_xalign(GTK_LABEL(hint), 0);
	gtk_widget_set_hexpand(hint, TRUE);
	gtk_widget_add_css_class(hint, "dim-label");
	gtk_box_append(GTK_BOX(row), hint);
	ui->arranger.advanced = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
			_("All monitors' settings")));
	g_signal_connect_swapped(ui->arranger.advanced, "toggled", G_CALLBACK(+[](Ui *ui) {
		show_cards(ui);
	}), ui);
	/* Each monitor's name on it, to tell which box is which screen. Waybox
	 * shows it, so it needs the scripting socket. */
	GtkWidget *identify = gtk_button_new_with_mnemonic(_("_Identify"));
	ui->arranger.identify = identify;
	JsonObject *probe = waybox_request("{\"request\":\"get_version\"}");
	if (probe != nullptr) {
		json_object_unref(probe);
		gtk_widget_set_tooltip_text(identify, _("Show each monitor's name on it"));
	} else {
		gtk_widget_set_sensitive(identify, FALSE);
		gtk_widget_set_tooltip_text(identify, _("Needs Waybox's scripting socket: "
				"turned off in rc.xml (<ipc enabled=\"no\"/>)"));
	}
	g_signal_connect_swapped(identify, "clicked", G_CALLBACK(+[](Ui *ui) {
		JsonObject *r = waybox_request("{\"request\":\"identify\"}");
		if (r == nullptr)
			set_status(ui, _("Waybox didn't answer: is its scripting socket on?"));
		else
			json_object_unref(r);
	}), ui);
	gtk_box_append(GTK_BOX(row), identify);
	gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui->arranger.advanced));
	gtk_box_append(GTK_BOX(box), row);
	return box;
}

}  // namespace wbconf
