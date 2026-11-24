#include "wbconf_ui.hpp"

namespace wbconf {

int index_of(const char *const *list, const std::string &value, int fallback) {
	for (int i = 0; list[i] != nullptr; ++i)
		if (value == list[i])
			return i;
	return fallback;
}

/* ---- obconf-style layout helpers ------------------------------------- */

/* A notebook page: a vertical box with obconf's outer padding. */
GtkWidget *make_page() {
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_top(page, 12);
	gtk_widget_set_margin_bottom(page, 12);
	gtk_widget_set_margin_start(page, 12);
	gtk_widget_set_margin_end(page, 12);
	return page;
}

/* A bold section header followed by an indented content box (obconf's idiom of
 * "<span weight=\"bold\">Title</span>" over a 12px-indented VBox). Returns the
 * content box to which callers append their controls. */
GtkWidget *section(GtkWidget *page, const char *title) {
	GtkWidget *header = gtk_label_new(nullptr);
	char *markup = g_markup_printf_escaped("<span weight=\"bold\">%s</span>",
			title);
	gtk_label_set_markup(GTK_LABEL(header), markup);
	g_free(markup);
	gtk_widget_set_halign(header, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(page), header);

	GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_margin_start(content, 12);
	gtk_box_append(GTK_BOX(page), content);
	return content;
}

/* A grid for label/control rows, appended to a section's content box. */
GtkWidget *section_grid(GtkWidget *content) {
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_box_append(GTK_BOX(content), grid);
	return grid;
}

void grid_label(GtkWidget *grid, int row, const char *text) {
	GtkWidget *label = gtk_label_new(text);
	gtk_widget_set_halign(label, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
}

GtkWidget *grid_control(GtkWidget *grid, int row, GtkWidget *w) {
	gtk_widget_set_halign(w, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
	return w;
}

GtkSpinButton *add_spin(GtkWidget *grid, int row, const char *label,
		int min, int max) {
	grid_label(grid, row, label);
	GtkWidget *spin = gtk_spin_button_new_with_range(min, max, 1);
	grid_control(grid, row, spin);
	return GTK_SPIN_BUTTON(spin);
}

GtkSwitch *add_switch(GtkWidget *grid, int row, const char *label) {
	grid_label(grid, row, label);
	GtkWidget *sw = gtk_switch_new();
	gtk_widget_set_halign(sw, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid), sw, 1, row, 1, 1);
	return GTK_SWITCH(sw);
}

GtkDropDown *add_dropdown(GtkWidget *grid, int row, const char *label,
		const char *const *items) {
	grid_label(grid, row, label);
	GtkWidget *dd = gtk_drop_down_new_from_strings(items);
	grid_control(grid, row, dd);
	return GTK_DROP_DOWN(dd);
}

/* A dropdown over `values`, "" shown as Automatic. */
GtkDropDown *values_dropdown(const std::vector<std::string> &values) {
	GtkStringList *model = gtk_string_list_new(nullptr);
	for (const std::string &v : values)
		gtk_string_list_append(model, v.empty() ? _("Automatic") : v.c_str());
	return GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(model), nullptr));
}

/* A dropdown over values with their own labels. */
GtkDropDown *choice_dropdown(const std::vector<std::pair<std::string, std::string>> &items,
		std::vector<std::string> &values) {
	GtkStringList *model = gtk_string_list_new(nullptr);
	values.clear();
	for (const auto &[value, label] : items) {
		gtk_string_list_append(model, label.c_str());
		values.push_back(value);
	}
	return GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(model), nullptr));
}

std::vector<std::string> split_names(const std::string &list) {
	std::vector<std::string> out;
	std::string cur;
	for (char c : list + " ") {
		if (c == ' ' || c == ',' || c == '\t') {
			if (!cur.empty())
				out.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	return out;
}

/* Select `value` in a values dropdown, adding it if it isn't listed (so a
 * value written by hand stays exactly as written). */
void select_value(GtkDropDown *dd, std::vector<std::string> &values,
		const std::string &value) {
	auto it = std::find(values.begin(), values.end(), value);
	if (it == values.end()) {
		gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(dd)),
				value.c_str());
		values.push_back(value);
		it = values.end() - 1;
	}
	gtk_drop_down_set_selected(dd, static_cast<guint>(it - values.begin()));
}

std::string selected_value(GtkDropDown *dd, const std::vector<std::string> &values) {
	guint i = gtk_drop_down_get_selected(dd);
	return i < values.size() ? values[i] : std::string();
}

}  // namespace wbconf
