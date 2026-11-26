/*
 * A font control (a page's font { }): family (searchable; "Default" first:
 * the program's own) and size in a row, then a button saying its style
 * ("Regular", "Bold · Italic") opening a popover with weight, width and
 * italic and a larger preview; a line of text in the font below. Written as the page's format says: as
 * Pango writes it ("Inter Bold 11", mako), changed on a copy of what was
 * there (variations kept); or as fontconfig names ("Iosevka,Noto Color
 * Emoji:size=12:weight=bold", foot and fuzzel), fallbacks and the
 * properties it doesn't edit kept.
 */
#include "wbconf_ui.hpp"

#include "waybox/font_spec.hpp"

#include <pango/pangocairo.h>
#include <set>

namespace wbconf {

namespace {

namespace font = wb::font;

/* Pango's stretches, in the order widths() has them. */
int width_of(PangoStretch s) {
	size_t i = static_cast<size_t>(s);
	return i < font::widths().size() ? font::widths()[i].value : 100;
}

PangoStretch stretch_of(int width) {
	int w = font::nearest_width(width);
	for (size_t i = 0; i < font::widths().size(); ++i)
		if (font::widths()[i].value == w)
			return static_cast<PangoStretch>(i);
	return PANGO_STRETCH_NORMAL;
}

/* What the value says, as a spec (for Pango: kept too, to change). */
font::Spec read_value(FontControl *c, const std::string &value) {
	if (c->pango_desc != nullptr) {
		pango_font_description_free(c->pango_desc);
		c->pango_desc = nullptr;
	}
	if (c->fontconfig)
		return font::from_fontconfig(value);
	font::Spec s;
	if (value.empty())
		return s;
	c->pango_desc = pango_font_description_from_string(value.c_str());
	PangoFontMask set = pango_font_description_get_set_fields(c->pango_desc);
	if (const char *f = pango_font_description_get_family(c->pango_desc)) {
		std::string all = f;
		size_t start = 0;
		while (start <= all.size()) {
			size_t comma = all.find(',', start);
			std::string one = all.substr(start, comma == std::string::npos ? std::string::npos
					: comma - start);
			while (!one.empty() && one.front() == ' ')
				one.erase(0, 1);
			if (!one.empty())
				s.families.push_back(one);
			if (comma == std::string::npos)
				break;
			start = comma + 1;
		}
	}
	if (set & PANGO_FONT_MASK_SIZE)
		s.size = pango_font_description_get_size(c->pango_desc) / double(PANGO_SCALE);
	/* Pango gives weight, style and width as set whether written or not:
	 * their normal ones are "not given" (the same font, either way). */
	if ((set & PANGO_FONT_MASK_WEIGHT) &&
			pango_font_description_get_weight(c->pango_desc) != PANGO_WEIGHT_NORMAL)
		s.weight = pango_font_description_get_weight(c->pango_desc);
	if (set & PANGO_FONT_MASK_STYLE) {
		PangoStyle st = pango_font_description_get_style(c->pango_desc);
		s.slant = st == PANGO_STYLE_ITALIC ? "italic" : st == PANGO_STYLE_OBLIQUE ? "oblique" : "";
	}
	if ((set & PANGO_FONT_MASK_STRETCH) &&
			pango_font_description_get_stretch(c->pango_desc) != PANGO_STRETCH_NORMAL)
		s.width = width_of(pango_font_description_get_stretch(c->pango_desc));
	return s;
}

std::string write_value(FontControl *c, const font::Spec &s) {
	if (s.families.empty())
		return "";
	if (c->fontconfig)
		return font::to_fontconfig(s);
	PangoFontDescription *d = c->pango_desc != nullptr
			? pango_font_description_copy(c->pango_desc) : pango_font_description_new();
	std::string families;
	for (size_t i = 0; i < s.families.size(); ++i)
		families += (i ? "," : "") + s.families[i];
	pango_font_description_set_family(d, families.c_str());
	if (s.size > 0)
		pango_font_description_set_size(d, static_cast<gint>(s.size * PANGO_SCALE));
	else
		pango_font_description_unset_fields(d, PANGO_FONT_MASK_SIZE);
	if (s.weight > 0)
		pango_font_description_set_weight(d, static_cast<PangoWeight>(s.weight));
	else
		pango_font_description_unset_fields(d, PANGO_FONT_MASK_WEIGHT);
	if (!s.slant.empty())
		pango_font_description_set_style(d, s.slant == "oblique" ? PANGO_STYLE_OBLIQUE
				: PANGO_STYLE_ITALIC);
	else
		pango_font_description_unset_fields(d, PANGO_FONT_MASK_STYLE);
	if (s.width > 0 && s.width != 100)
		pango_font_description_set_stretch(d, stretch_of(s.width));
	else
		pango_font_description_unset_fields(d, PANGO_FONT_MASK_STRETCH);
	char *t = pango_font_description_to_string(d);
	std::string out = t;
	g_free(t);
	pango_font_description_free(d);
	return out;
}

/* "Regular", "Bold · Italic · Condensed": what the style button says. */
std::string style_text(const font::Spec &s) {
	std::vector<std::string> parts;
	for (const font::Named &n : font::weights())
		if (s.weight > 0 && n.value == font::nearest_weight(s.weight))
			parts.push_back(_(n.label));
	if (!s.slant.empty())
		parts.push_back(_("Italic"));
	for (const font::Named &n : font::widths())
		if (s.width > 0 && s.width != 100 && n.value == font::nearest_width(s.width))
			parts.push_back(_(n.label));
	if (parts.empty())
		return _("Regular");
	std::string out;
	for (size_t i = 0; i < parts.size(); ++i)
		out += (i ? " \xC2\xB7 " : "") + parts[i];
	return out;
}

void set_font_attrs(GtkWidget *label, PangoFontDescription *d) {
	PangoAttrList *attrs = pango_attr_list_new();
	pango_attr_list_insert(attrs, pango_attr_font_desc_new(d));
	gtk_label_set_attributes(GTK_LABEL(label), attrs);
	pango_attr_list_unref(attrs);
}

/* The sample line (and the popover's preview), in the font. */
void show_sample(FontControl *c) {
	bool any = !c->spec.families.empty();
	gtk_widget_set_visible(c->sample, any);
	gtk_widget_set_sensitive(GTK_WIDGET(c->size), any);
	gtk_widget_set_sensitive(c->style, any);
	gtk_label_set_text(GTK_LABEL(c->style_label), style_text(c->spec).c_str());
	if (!any)
		return;
	PangoFontDescription *d = pango_font_description_new();
	pango_font_description_set_family(d, c->spec.families[0].c_str());
	pango_font_description_set_size(d, static_cast<gint>((c->spec.size > 0 ? c->spec.size
			: 11) * PANGO_SCALE));
	if (c->spec.weight > 0)
		pango_font_description_set_weight(d, static_cast<PangoWeight>(c->spec.weight));
	if (!c->spec.slant.empty())
		pango_font_description_set_style(d, PANGO_STYLE_ITALIC);
	if (c->spec.width > 0)
		pango_font_description_set_stretch(d, stretch_of(c->spec.width));
	set_font_attrs(c->sample, d);
	/* The preview: larger, whatever the size. */
	pango_font_description_set_size(d, 16 * PANGO_SCALE);
	set_font_attrs(c->preview, d);
	pango_font_description_free(d);
}

/* What a family comes in: its faces' weights and widths. A variable one,
 * or one Pango doesn't know (an alias, "monospace"): everything. Bold can
 * be made from a regular face (emboldened); a width can't. */
struct Available {
	bool all = true;
	std::set<int> weights, widths;
};

Available available(const std::string &family) {
	Available a;
	if (family.empty())
		return a;
	PangoFontMap *map = pango_cairo_font_map_get_default();
	PangoFontFamily *fam = pango_font_map_get_family(map, family.c_str());
	if (fam == nullptr || pango_font_family_is_variable(fam))
		return a;
	PangoFontFace **faces = nullptr;
	int n = 0;
	pango_font_family_list_faces(fam, &faces, &n);
	if (n == 0) {
		g_free(faces);
		return a;
	}
	a.all = false;
	for (int i = 0; i < n; ++i) {
		PangoFontDescription *d = pango_font_face_describe(faces[i]);
		a.weights.insert(font::nearest_weight(pango_font_description_get_weight(d)));
		a.widths.insert(font::nearest_width(width_of(pango_font_description_get_stretch(d))));
		pango_font_description_free(d);
	}
	g_free(faces);
	if (*a.weights.begin() <= 500)
		a.weights.insert(700);
	return a;
}

/* A list's entries: the font's own, then those offered; one set but not
 * offered (from the file) kept, saying so. */
void fill(GtkDropDown *drop, std::vector<int> &values, const std::vector<font::Named> &names,
		const std::set<int> &offered, bool all, int current) {
	GtkStringList *model = gtk_string_list_new(nullptr);
	values.assign(1, 0);
	gtk_string_list_append(model, _("The font's own"));
	for (const font::Named &n : names) {
		bool there = all || offered.count(n.value);
		if (!there && n.value != current)
			continue;
		std::string label = _(n.label);
		if (!there)
			label += std::string(" ") + _("(not in this font)");
		gtk_string_list_append(model, label.c_str());
		values.push_back(n.value);
	}
	gtk_drop_down_set_model(drop, G_LIST_MODEL(model));
	g_object_unref(model);
}

/* The weight and width lists for the family chosen, the spec's selected. */
void offer_styles(FontControl *c) {
	bool was = c->syncing;
	c->syncing = true;
	std::string family = c->spec.families.empty() ? "" : c->spec.families[0];
	Available a = available(family);
	int weight = c->spec.weight > 0 ? font::nearest_weight(c->spec.weight) : 0;
	int width = c->spec.width > 0 ? font::nearest_width(c->spec.width) : 0;
	fill(c->weight, c->weight_values, font::weights(), a.weights, a.all, weight);
	fill(c->width, c->width_values, font::widths(), a.widths, a.all, width);
	auto select = [](GtkDropDown *d, const std::vector<int> &values, int v) {
		guint at = 0;
		for (size_t i = 0; i < values.size(); ++i)
			if (values[i] == v)
				at = static_cast<guint>(i);
		gtk_drop_down_set_selected(d, at);
	};
	select(c->weight, c->weight_values, weight);
	select(c->width, c->width_values, width);
	/* One width only (and none set otherwise): nothing to choose. */
	bool one_width = c->width_values.size() <= 2 && !a.all &&
			(c->width_values.size() == 1 || c->width_values[1] == 100);
	gtk_widget_set_sensitive(GTK_WIDGET(c->width), !one_width);
	gtk_widget_set_tooltip_text(GTK_WIDGET(c->width), one_width
			? _("This font comes in one width") : _("Width"));
	c->syncing = was;
}

/* The widgets as the spec says. */
void show(FontControl *c) {
	c->syncing = true;
	const font::Spec &s = c->spec;
	guint fam = 0;  /* Default */
	if (!s.families.empty()) {
		auto it = std::find(c->families.begin(), c->families.end(), s.families[0]);
		if (it == c->families.end()) {
			/* Not installed: shown as it is, not as the first one. */
			gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(c->family)),
					s.families[0].c_str());
			c->families.push_back(s.families[0]);
			it = c->families.end() - 1;
		}
		fam = static_cast<guint>(it - c->families.begin());
	}
	gtk_drop_down_set_selected(c->family, fam);
	double size = s.size > 0 ? s.size : c->default_size;
	gtk_spin_button_set_digits(c->size, size == std::floor(size) ? 0 : 1);
	gtk_spin_button_set_value(c->size, size);
	offer_styles(c);
	gtk_switch_set_active(c->italic, !s.slant.empty());
	show_sample(c);
	c->syncing = false;
}

/* The user changed one: the spec from the widgets, the rest kept. */
void on_widget(FontControl *c) {
	if (c->syncing)
		return;
	font::Spec &s = c->spec;
	std::string was_family = s.families.empty() ? "" : s.families[0];
	guint fam = gtk_drop_down_get_selected(c->family);
	if (fam == 0 || fam >= c->families.size()) {
		s.families.clear();
	} else if (s.families.empty()) {
		s.families.push_back(c->families[fam]);
	} else {
		s.families[0] = c->families[fam];  /* fallbacks after it kept */
	}
	bool had = !s.families.empty();
	s.size = had ? gtk_spin_button_get_value(c->size) : 0;
	guint w = gtk_drop_down_get_selected(c->weight);
	s.weight = w < c->weight_values.size() ? c->weight_values[w] : 0;
	guint wd = gtk_drop_down_get_selected(c->width);
	s.width = wd < c->width_values.size() ? c->width_values[wd] : 0;
	/* Another family: what it comes in. */
	if ((s.families.empty() ? "" : s.families[0]) != was_family)
		offer_styles(c);
	bool italic = gtk_switch_get_active(c->italic);
	if (!italic)
		s.slant.clear();
	else if (s.slant.empty())
		s.slant = "italic";
	c->value = write_value(c, s);
	show_sample(c);
	if (c->on_change)
		c->on_change();
}

void on_notify(GObject *, GParamSpec *, gpointer data) {
	on_widget(static_cast<FontControl *>(data));
}

void on_signal(GtkWidget *, gpointer data) {
	on_widget(static_cast<FontControl *>(data));
}

GtkDropDown *style_drop(const char *tip) {
	GtkWidget *d = gtk_drop_down_new(nullptr, nullptr);
	gtk_widget_set_tooltip_text(d, tip);
	return GTK_DROP_DOWN(d);
}

}  // namespace

FontControl::~FontControl() {
	if (pango_desc != nullptr)
		pango_font_description_free(pango_desc);
}

std::unique_ptr<FontControl> make_font_control(const std::string &format,
		const std::string &hint) {
	auto c = std::make_unique<FontControl>();
	c->fontconfig = format == "fontconfig";
	/* "monospace 10", "monospace:size=10": the size the default has. */
	double hinted = c->fontconfig ? font::from_fontconfig(hint).size
			: font::from_pango(hint).size;
	if (hinted > 0)
		c->default_size = hinted;
	c->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

	/* Family: "Default" (with the program's, if the page says it), then
	 * the installed ones, searchable. */
	c->families = {""};
	for (const std::string &f : font_families())
		c->families.push_back(f);
	std::string def = hint.empty() ? _("Default") : std::string(_("Default")) + " (" + hint + ")";
	std::vector<const char *> items = {def.c_str()};
	for (size_t i = 1; i < c->families.size(); ++i)
		items.push_back(c->families[i].c_str());
	items.push_back(nullptr);
	GtkWidget *fam = gtk_drop_down_new_from_strings(items.data());
	gtk_widget_set_size_request(fam, 220, -1);
	gtk_drop_down_set_expression(GTK_DROP_DOWN(fam),
			gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string"));
	gtk_drop_down_set_enable_search(GTK_DROP_DOWN(fam), TRUE);
#if GTK_CHECK_VERSION(4, 12, 0)
	gtk_drop_down_set_search_match_mode(GTK_DROP_DOWN(fam),
			GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
#endif
	gtk_widget_set_tooltip_text(fam, _("Font"));
	c->family = GTK_DROP_DOWN(fam);
	gtk_box_append(GTK_BOX(row), fam);

	/* Whole points a step (what's shown is what it holds: a step of 0.5
	 * shown as a whole number went back on losing focus). */
	c->size = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(4, 96, 1));
	gtk_widget_set_tooltip_text(GTK_WIDGET(c->size), _("Size, in points"));
	gtk_box_append(GTK_BOX(row), GTK_WIDGET(c->size));
	/* The style: a button saying it, opening the rest. */
	c->style = gtk_menu_button_new();
	c->style_label = gtk_label_new("");
	gtk_label_set_ellipsize(GTK_LABEL(c->style_label), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(c->style_label), 28);
	GtkWidget *style_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(style_line), c->style_label);
	gtk_box_append(GTK_BOX(style_line), gtk_image_new_from_icon_name("pan-down-symbolic"));
	gtk_menu_button_set_child(GTK_MENU_BUTTON(c->style), style_line);
	gtk_widget_set_tooltip_text(c->style, _("Weight, width and italic"));
	gtk_box_append(GTK_BOX(row), c->style);
	gtk_box_append(GTK_BOX(c->box), row);

	GtkWidget *pop = gtk_popover_new();
	GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_start(pbox, 12);
	gtk_widget_set_margin_end(pbox, 12);
	gtk_widget_set_margin_top(pbox, 12);
	gtk_widget_set_margin_bottom(pbox, 12);
	c->preview = gtk_label_new(_("The quick brown fox jumps over the lazy dog\n"
			"ABCDEFGHIJKLM abcdefghijklm 0123456789"));
	gtk_label_set_xalign(GTK_LABEL(c->preview), 0);
	gtk_label_set_wrap(GTK_LABEL(c->preview), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(c->preview), 52);
	/* A fixed size, whatever the font: the popover doesn't move. */
	gtk_label_set_lines(GTK_LABEL(c->preview), 2);
	gtk_label_set_ellipsize(GTK_LABEL(c->preview), PANGO_ELLIPSIZE_END);
	gtk_widget_set_size_request(c->preview, 540, 64);
	gtk_widget_set_valign(c->preview, GTK_ALIGN_CENTER);
	/* In a tinted, outlined box: the font on its own. */
	GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(frame, "wb-font-preview");
	gtk_box_append(GTK_BOX(frame), c->preview);
	/* One row: Weight, Width, Italic, and the way back to the font's own;
	 * the preview under them. */
	c->weight = style_drop(_("Weight"));
	c->width = style_drop(_("Width"));
	c->italic = GTK_SWITCH(gtk_switch_new());
	gtk_widget_set_valign(GTK_WIDGET(c->italic), GTK_ALIGN_CENTER);
	GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	const std::pair<const char *, GtkWidget *> parts[] = {{_("Weight"), GTK_WIDGET(c->weight)},
			{_("Width"), GTK_WIDGET(c->width)}, {_("Italic"), GTK_WIDGET(c->italic)}};
	bool first = true;
	for (const auto &[text, w] : parts) {
		GtkWidget *l = gtk_label_new(text);
		gtk_widget_add_css_class(l, "dim-label");
		if (!first)
			gtk_widget_set_margin_start(l, 10);
		first = false;
		gtk_box_append(GTK_BOX(line), l);
		gtk_box_append(GTK_BOX(line), w);
	}
	GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(spacer, TRUE);
	gtk_box_append(GTK_BOX(line), spacer);
	GtkWidget *reset = gtk_button_new_from_icon_name("edit-undo-symbolic");
	gtk_widget_add_css_class(reset, "flat");
	gtk_widget_set_valign(reset, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(reset, _("The font's own style"));
	g_signal_connect_swapped(reset, "clicked", G_CALLBACK(+[](FontControl *fc) {
		gtk_drop_down_set_selected(fc->weight, 0);
		gtk_drop_down_set_selected(fc->width, 0);
		gtk_switch_set_active(fc->italic, FALSE);
	}), c.get());
	gtk_box_append(GTK_BOX(line), reset);
	gtk_box_append(GTK_BOX(pbox), line);
	gtk_box_append(GTK_BOX(pbox), frame);
	gtk_popover_set_child(GTK_POPOVER(pop), pbox);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(c->style), pop);

	c->sample = gtk_label_new(_("The quick brown fox jumps over the lazy dog · 0123456789"));
	gtk_label_set_xalign(GTK_LABEL(c->sample), 0);
	gtk_label_set_ellipsize(GTK_LABEL(c->sample), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(c->box), c->sample);

	g_signal_connect(fam, "notify::selected", G_CALLBACK(on_notify), c.get());
	g_signal_connect(c->size, "value-changed", G_CALLBACK(on_signal), c.get());
	g_signal_connect(c->weight, "notify::selected", G_CALLBACK(on_notify), c.get());
	g_signal_connect(c->width, "notify::selected", G_CALLBACK(on_notify), c.get());
	g_signal_connect(c->italic, "notify::active", G_CALLBACK(on_notify), c.get());
	show(c.get());
	return c;
}

void font_control_set(FontControl *c, const std::string &value) {
	c->value = value;
	c->spec = read_value(c, value);
	show(c);
}

/* Tests: "family:NAME" (as picked from the list), "size:N",
 * "size-step:up|down", "size-update:", "weight:N",
 * "width:N", "italic:on|off". */
bool font_control_test(FontControl *c, const std::string &action) {
	if (action == "open") {
		gtk_menu_button_popup(GTK_MENU_BUTTON(c->style));
		return true;
	}
	size_t colon = action.find(':');
	if (colon == std::string::npos)
		return false;
	std::string what = action.substr(0, colon), arg = action.substr(colon + 1);
	if (what == "family") {
		auto it = std::find(c->families.begin(), c->families.end(), arg);
		if (it == c->families.end()) {
			gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(c->family)),
					arg.c_str());
			c->families.push_back(arg);
			it = c->families.end() - 1;
		}
		gtk_drop_down_set_selected(c->family, static_cast<guint>(it - c->families.begin()));
	} else if (what == "size") {
		gtk_spin_button_set_value(c->size, atof(arg.c_str()));
	} else if (what == "size-step") {
		/* "+" or "−" clicked; then "size-update": as when it loses focus. */
		gtk_spin_button_spin(c->size, arg == "down" ? GTK_SPIN_STEP_BACKWARD
				: GTK_SPIN_STEP_FORWARD, 0);
	} else if (what == "size-update") {
		gtk_spin_button_update(c->size);
	} else if (what == "weight" || what == "width") {
		const auto &values = what == "weight" ? c->weight_values : c->width_values;
		int v = atoi(arg.c_str());
		guint pick = 0;
		for (size_t i = 0; i < values.size(); ++i)
			if (values[i] == v)
				pick = static_cast<guint>(i);
		gtk_drop_down_set_selected(what == "weight" ? c->weight : c->width, pick);
	} else if (what == "italic") {
		gtk_switch_set_active(c->italic, arg == "on");
	} else {
		return false;
	}
	return true;
}

}  // namespace wbconf
