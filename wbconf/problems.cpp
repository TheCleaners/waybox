/*
 * Problems found with settings, shown where they are: a warning sign by
 * the setting's title and on its section and category in the sidebar, a
 * note under it saying what's wrong and why, and a button where wbconf can
 * fix it (as a change, saved with the rest). "@problems" in the search
 * shows only those settings.
 *
 * The checks themselves are plain functions (waybox/setting_problems.cpp);
 * here they're given what they look at and their results are shown.
 */
#include "wbconf_ui.hpp"

#include "waybox/xcursor_themes.hpp"

namespace wbconf {

namespace {

const char *translate(const char *msg) {
	return gettext(msg);
}

/* The problem a row has now, if any. */
std::optional<wb::SettingProblem> check_row(Ui *ui, const Ui::SettingRow &r) {
	if (r.def->type == wb::SettingType::Custom && r.def->widget == "gtk-appearance" &&
			ui->gtk.built)
		return wb::pointer_problem(pointer_state(ui), translate);
	return std::nullopt;
}

void show_problem(Ui::SettingRow &r) {
	bool on = r.problem.has_value();
	if (r.problem_icon != nullptr)
		gtk_widget_set_visible(r.problem_icon, on);
	if (r.problem_note == nullptr)
		return;
	gtk_widget_set_visible(r.problem_note, on);
	if (!on)
		return;
	std::string text = r.problem->text;
	if (r.problem->fixed_on_save)
		text += std::string(" ") + _("Save & Apply fixes it.");
	gtk_label_set_text(r.problem_text, text.c_str());
	bool fix = !r.problem->fix_label.empty();
	gtk_widget_set_visible(r.problem_fix, fix);
	if (fix)
		gtk_button_set_label(GTK_BUTTON(r.problem_fix), r.problem->fix_label.c_str());
	gtk_widget_set_tooltip_text(r.problem_icon, text.c_str());
}

}  // namespace

wb::PointerState pointer_state(Ui *ui) {
	wb::PointerState s;
	if (ui->gtk.built && ui->gtk.cursor != nullptr)
		s.shown = selected_value(ui->gtk.cursor, ui->gtk.cursor_values);
	s.rc_theme = ui->gtk.rc_cursor_theme;
	/* Waybox exports the theme it uses to what it starts, wbconf too. */
	if (const char *t = getenv("XCURSOR_THEME"))
		s.env_theme = t;
	/* Save writes the theme shown when the pointer changed (or its fix
	 * was chosen). */
	Ui::GtkLook now = gtk_look_now(ui);
	s.to_be_written = ui->gtk.pointer_to_rc || now.cursor != ui->gtk.saved.cursor ||
			now.size != ui->gtk.saved.size;
	const char *home = getenv("HOME");
	s.path = wb::xcursor_search_path(getenv("XCURSOR_PATH"), getenv("XDG_DATA_HOME"),
			home != nullptr ? home : "");
	s.home = home != nullptr ? home : "";
	return s;
}

void refresh_problems(Ui *ui) {
	std::set<GtkWidget *> with_problem;
	for (auto &rp : ui->rows) {
		Ui::SettingRow &r = *rp;
		if (r.problem_note == nullptr && r.problem_icon == nullptr)
			continue;  /* rows sharing a group line: not checked */
		r.problem = check_row(ui, r);
		show_problem(r);
		if (r.problem)
			with_problem.insert(r.row);
	}
	/* The sidebar: a section's mark when a row of it has one, a category's
	 * when any of its sections (or rows outside them) has. */
	std::vector<bool> category_has(ui->categories.size(), false);
	std::map<GtkWidget *, bool> section_has;
	for (size_t i = 0; i < ui->categories.size(); ++i)
		for (auto &[heading, rows] : ui->categories[i].sections) {
			bool any = false;
			for (GtkWidget *w : rows)
				any = any || with_problem.count(w) > 0;
			if (heading != nullptr)
				section_has[heading] = section_has[heading] || any;
			category_has[i] = category_has[i] || any;
		}
	for (const Ui::TocEntry &e : ui->toc_entries) {
		if (e.problem_icon == nullptr)
			continue;
		bool on = e.section ? section_has[e.heading]
				: category_has[static_cast<size_t>(e.category)];
		gtk_widget_set_visible(e.problem_icon, on);
		gtk_widget_set_tooltip_text(e.problem_icon, on ? _("A setting here has a problem") : nullptr);
	}
}

void on_problem_fix(GtkButton *button, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	auto *r = static_cast<Ui::SettingRow *>(g_object_get_data(G_OBJECT(button), "wb-row"));
	if (r == nullptr)
		return;
	if (r->def->widget == "gtk-appearance")
		ui->gtk.pointer_to_rc = true;  /* a change: Save writes it */
	refresh_save_state(ui);
}

}  // namespace wbconf
