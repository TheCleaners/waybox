#include "wbconf_ui.hpp"

namespace wbconf {

/* ---- Fonts + live theme preview -------------------------------------- */

/* The installed font families, sorted and de-duplicated. Uses the default
 * Cairo/Pango font map, so no widget is needed. */
const std::vector<std::string> &font_families() {
	static const std::vector<std::string> families = [] {
		std::vector<std::string> out;
		PangoFontMap *fm = pango_cairo_font_map_get_default();
		PangoFontFamily **fams = nullptr;
		int n = 0;
		pango_font_map_list_families(fm, &fams, &n);
		out.reserve(n);
		for (int i = 0; i < n; i++) {
			const char *name = pango_font_family_get_name(fams[i]);
			if (name != nullptr)
				out.emplace_back(name);
		}
		g_free(fams);
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}();
	return families;
}

/* A custom, instant font picker: a family dropdown, a size spin button and a
 * bold toggle, in one row. (GtkFontDialogButton loads its list
 * asynchronously and only shows the current font after a delay.) */
GtkWidget *make_font_picker(FontPicker *out, GCallback on_change, gpointer user) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

	out->families = font_families();
	std::vector<const char *> items;
	items.reserve(out->families.size() + 1);
	for (const std::string &f : out->families)
		items.push_back(f.c_str());
	items.push_back(nullptr);
	GtkWidget *fam = gtk_drop_down_new_from_strings(items.data());
	gtk_widget_set_size_request(fam, 220, -1);
	/* Make the long list searchable by typing. The search needs an
	 * expression naming the text to match (the GtkStringObject's "string");
	 * without one it matches nothing and the list never filters. Match
	 * anywhere in the name ("mono" finds "DejaVu Sans Mono"), not just the
	 * start. */
	gtk_drop_down_set_expression(GTK_DROP_DOWN(fam),
			gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string"));
	gtk_drop_down_set_enable_search(GTK_DROP_DOWN(fam), TRUE);
#if GTK_CHECK_VERSION(4, 12, 0)
	gtk_drop_down_set_search_match_mode(GTK_DROP_DOWN(fam),
			GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
#endif
	out->family = GTK_DROP_DOWN(fam);
	gtk_box_append(GTK_BOX(box), fam);

	GtkWidget *size = gtk_spin_button_new_with_range(4, 96, 1);
	out->size = GTK_SPIN_BUTTON(size);
	gtk_box_append(GTK_BOX(box), size);

	GtkWidget *bold = gtk_toggle_button_new_with_label(_("B"));
	gtk_widget_set_tooltip_text(bold, _("Bold"));
	out->bold = GTK_TOGGLE_BUTTON(bold);
	gtk_box_append(GTK_BOX(box), bold);

	if (on_change != nullptr) {
		g_signal_connect_swapped(fam, "notify::selected", on_change, user);
		g_signal_connect_swapped(size, "value-changed", on_change, user);
		g_signal_connect_swapped(bold, "toggled", on_change, user);
	}
	return box;
}

/* Read a picker as a Pango-style "Family [Bold] Size" string. */
std::optional<std::string> font_value(const FontPicker *p) {
	guint idx = gtk_drop_down_get_selected(p->family);
	if (idx == GTK_INVALID_LIST_POSITION || idx >= p->families.size())
		return std::nullopt;
	std::string out = p->families[idx];
	if (gtk_toggle_button_get_active(p->bold))
		out += " Bold";
	out += " " + std::to_string(gtk_spin_button_get_value_as_int(p->size));
	return out;
}

/* Set a picker from a Pango-style "Family [Bold] Size" string. */
void font_set(FontPicker *p, const std::optional<std::string> &v) {
	if (!v)
		return;
	PangoFontDescription *d = pango_font_description_from_string(v->c_str());
	const char *fam = pango_font_description_get_family(d);
	std::string family = fam ? fam : "Sans";
	int size = pango_font_description_get_size(d);
	int size_pt = size > 0 ? size / PANGO_SCALE : 10;
	bool bold = pango_font_description_get_weight(d) >= PANGO_WEIGHT_BOLD;
	pango_font_description_free(d);

	/* A family that isn't installed stays as it is (an extra entry), rather
	 * than becoming whichever font happens to be first. */
	auto it = std::find(p->families.begin(), p->families.end(), family);
	if (it == p->families.end()) {
		gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(p->family)),
				family.c_str());
		p->families.push_back(family);
		it = p->families.end() - 1;
	}
	gtk_drop_down_set_selected(p->family,
			static_cast<guint>(it - p->families.begin()));
	gtk_spin_button_set_value(p->size, size_pt);
	gtk_toggle_button_set_active(p->bold, bold);
}

}  // namespace wbconf
