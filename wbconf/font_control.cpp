/*
 * A font control (a page's font { }): family (searchable; "Default" first:
 * the program's own), size, weight, width and italic in one row, and a
 * line of text in the font below. Written as the page's format says: as
 * Pango writes it ("Inter Bold 11", mako), changed on a copy of what was
 * there (variations kept); or as fontconfig names ("Iosevka,Noto Color
 * Emoji:size=12:weight=bold", foot and fuzzel), fallbacks and the
 * properties it doesn't edit kept.
 */
#include "wbconf_ui.hpp"

#include "waybox/font_spec.hpp"

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

/* The sample line, in the font. */
void show_sample(FontControl *c) {
	bool any = !c->spec.families.empty();
	gtk_widget_set_visible(c->sample, any);
	for (GtkWidget *w : {GTK_WIDGET(c->size), GTK_WIDGET(c->weight), GTK_WIDGET(c->width),
			GTK_WIDGET(c->italic)})
		gtk_widget_set_sensitive(w, any);
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
	PangoAttrList *attrs = pango_attr_list_new();
	pango_attr_list_insert(attrs, pango_attr_font_desc_new(d));
	gtk_label_set_attributes(GTK_LABEL(c->sample), attrs);
	pango_attr_list_unref(attrs);
	pango_font_description_free(d);
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
	double size = s.size > 0 ? s.size : 11;
	gtk_spin_button_set_digits(c->size, size == std::floor(size) ? 0 : 1);
	gtk_spin_button_set_value(c->size, size);
	guint w = 0;
	for (size_t i = 0; i < font::weights().size(); ++i)
		if (s.weight > 0 && font::weights()[i].value == font::nearest_weight(s.weight))
			w = static_cast<guint>(i + 1);
	gtk_drop_down_set_selected(c->weight, w);
	guint wd = 0;
	for (size_t i = 0; i < font::widths().size(); ++i)
		if (s.width > 0 && font::widths()[i].value == font::nearest_width(s.width))
			wd = static_cast<guint>(i + 1);
	gtk_drop_down_set_selected(c->width, wd);
	gtk_toggle_button_set_active(c->italic, !s.slant.empty());
	show_sample(c);
	c->syncing = false;
}

/* The user changed one: the spec from the widgets, the rest kept. */
void on_widget(FontControl *c) {
	if (c->syncing)
		return;
	font::Spec &s = c->spec;
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
	s.weight = w > 0 && w <= font::weights().size() ? font::weights()[w - 1].value : 0;
	guint wd = gtk_drop_down_get_selected(c->width);
	s.width = wd > 0 && wd <= font::widths().size() ? font::widths()[wd - 1].value : 0;
	bool italic = gtk_toggle_button_get_active(c->italic);
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

GtkDropDown *named_drop(const char *first, const std::vector<font::Named> &names,
		const char *tip) {
	std::vector<std::string> labels = {first};
	for (const font::Named &n : names)
		labels.push_back(_(n.label));
	std::vector<const char *> items;
	for (const std::string &l : labels)
		items.push_back(l.c_str());
	items.push_back(nullptr);
	GtkWidget *d = gtk_drop_down_new_from_strings(items.data());
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

	c->size = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(4, 96, 0.5));
	gtk_widget_set_tooltip_text(GTK_WIDGET(c->size), _("Size, in points"));
	gtk_box_append(GTK_BOX(row), GTK_WIDGET(c->size));
	c->weight = named_drop(_("Weight"), font::weights(), _("Weight (Default: the font's own)"));
	gtk_box_append(GTK_BOX(row), GTK_WIDGET(c->weight));
	c->width = named_drop(_("Width"), font::widths(), _("Width (Default: the font's own)"));
	gtk_box_append(GTK_BOX(row), GTK_WIDGET(c->width));
	c->italic = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
	GtkWidget *i = gtk_label_new(nullptr);
	gtk_label_set_markup(GTK_LABEL(i), "<i>I</i>");
	gtk_button_set_child(GTK_BUTTON(c->italic), i);
	gtk_widget_set_tooltip_text(GTK_WIDGET(c->italic), _("Italic"));
	gtk_box_append(GTK_BOX(row), GTK_WIDGET(c->italic));
	gtk_box_append(GTK_BOX(c->box), row);

	c->sample = gtk_label_new(_("The quick brown fox jumps over the lazy dog · 0123456789"));
	gtk_label_set_xalign(GTK_LABEL(c->sample), 0);
	gtk_label_set_ellipsize(GTK_LABEL(c->sample), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(c->box), c->sample);

	g_signal_connect(fam, "notify::selected", G_CALLBACK(on_notify), c.get());
	g_signal_connect(c->size, "value-changed", G_CALLBACK(on_signal), c.get());
	g_signal_connect(c->weight, "notify::selected", G_CALLBACK(on_notify), c.get());
	g_signal_connect(c->width, "notify::selected", G_CALLBACK(on_notify), c.get());
	g_signal_connect(c->italic, "toggled", G_CALLBACK(on_signal), c.get());
	show(c.get());
	return c;
}

void font_control_set(FontControl *c, const std::string &value) {
	c->value = value;
	c->spec = read_value(c, value);
	show(c);
}

/* Tests: "family:NAME" (as picked from the list), "size:N", "weight:N",
 * "width:N", "italic:on|off". */
bool font_control_test(FontControl *c, const std::string &action) {
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
	} else if (what == "weight" || what == "width") {
		const auto &names = what == "weight" ? font::weights() : font::widths();
		int v = atoi(arg.c_str());
		guint pick = 0;
		for (size_t i = 0; i < names.size(); ++i)
			if (names[i].value == v)
				pick = static_cast<guint>(i + 1);
		gtk_drop_down_set_selected(what == "weight" ? c->weight : c->width, pick);
	} else if (what == "italic") {
		gtk_toggle_button_set_active(c->italic, arg == "on");
	} else {
		return false;
	}
	return true;
}

}  // namespace wbconf
