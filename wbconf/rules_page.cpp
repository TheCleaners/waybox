/*
 * Window rules (#237): rc.xml's <applications>, as a list in the order
 * they're tried (the first that matches a window wins): what each matches
 * (the program, a title, a tag) and what it does, in words. Read-only for
 * now: editing, a window picker and live matches come on top of this.
 */
#include "wbconf_ui.hpp"

#include "waybox/applications.hpp"

namespace wbconf {

namespace {

GtkWidget *dim(const std::string &text) {
	GtkWidget *l = gtk_label_new(text.c_str());
	gtk_widget_add_css_class(l, "dim-label");
	return l;
}

GtkWidget *chip(const std::string &text) {
	GtkWidget *l = gtk_label_new(text.c_str());
	gtk_widget_add_css_class(l, "wb-keycap");
	gtk_widget_add_css_class(l, "wb-rule-pattern");
	return l;
}

void clear(GtkWidget *box) {
	while (GtkWidget *c = gtk_widget_get_first_child(box))
		gtk_box_remove(GTK_BOX(box), c);
}

/* "foot  titled  *scratch*": what a rule matches. */
GtkWidget *match_widget(const wb::AppRule &r) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	bool any = false;
	/* Openbox's class and name both match the app id here. */
	std::string app = !r.class_pattern.empty() ? r.class_pattern : r.name_pattern;
	if (!app.empty()) {
		gtk_box_append(GTK_BOX(box), chip(app));
		any = true;
	}
	if (!r.class_pattern.empty() && !r.name_pattern.empty() && r.name_pattern != r.class_pattern) {
		gtk_box_append(GTK_BOX(box), dim(_("and")));
		gtk_box_append(GTK_BOX(box), chip(r.name_pattern));
	}
	if (!r.title_pattern.empty()) {
		gtk_box_append(GTK_BOX(box), dim(any ? _("titled") : _("Titled")));
		gtk_box_append(GTK_BOX(box), chip(r.title_pattern));
		any = true;
	}
	if (!r.tag_pattern.empty()) {
		gtk_box_append(GTK_BOX(box), dim(any ? _("tagged") : _("Tagged")));
		gtk_box_append(GTK_BOX(box), chip(r.tag_pattern));
		any = true;
	}
	if (!any) {
		GtkWidget *l = gtk_label_new(_("Every window"));
		gtk_widget_add_css_class(l, "wb-rule-every");
		gtk_box_append(GTK_BOX(box), l);
	}
	if (r.patterns != wb::AppRule::Patterns::Glob) {
		GtkWidget *l = dim(r.patterns == wb::AppRule::Patterns::IRegex ? _("regex, any case")
				: _("regex"));
		gtk_widget_add_css_class(l, "wb-rule-kind");
		gtk_widget_set_tooltip_text(l, _("Its patterns are regular expressions, as egrep reads "
				"them: anywhere in the text unless anchored with ^ and $"));
		gtk_box_append(GTK_BOX(box), l);
	}
	return box;
}

std::string join(const std::vector<std::string> &parts, const char *sep) {
	std::string out;
	for (const std::string &p : parts)
		out += (out.empty() ? "" : sep) + p;
	return out;
}

}  // namespace

GtkWidget *build_rules_block(Ui *ui) {
	ui->rules_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	return ui->rules_box;
}

void populate_rules(Ui *ui, const wb::RcDocument *doc) {
	if (doc == nullptr || ui->rules_box == nullptr)
		return;
	clear(ui->rules_box);
	ui->rule_lines.clear();
	std::vector<wb::AppRuleEntry> rules = doc->app_rules();
	if (rules.empty()) {
		GtkWidget *l = dim(_("No rules yet. A rule sets how a program's windows open: on which "
				"desktop, where and how big, maximized or full screen, with or without a "
				"title bar."));
		gtk_label_set_wrap(GTK_LABEL(l), TRUE);
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_widget_add_css_class(l, "wb-rules-empty");
		gtk_box_append(GTK_BOX(ui->rules_box), l);
		return;
	}
	GtkSizeGroup *column = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
	for (size_t i = 0; i < rules.size(); ++i) {
		const wb::AppRuleEntry &e = rules[i];
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
		gtk_widget_add_css_class(row, "wb-rule-row");

		/* Its place: the first rule that matches wins. */
		GtkWidget *n = gtk_label_new(std::to_string(i + 1).c_str());
		gtk_widget_add_css_class(n, "wb-rule-order");
		gtk_widget_set_valign(n, GTK_ALIGN_START);
		gtk_widget_set_tooltip_text(n, _("Rules are tried in this order; the first that "
				"matches a window is the one used"));
		gtk_box_append(GTK_BOX(row), n);

		GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_widget_set_valign(cell, GTK_ALIGN_START);
		gtk_box_append(GTK_BOX(cell), match_widget(e.rule));
		gtk_size_group_add_widget(column, cell);
		gtk_box_append(GTK_BOX(row), cell);

		GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		gtk_widget_set_hexpand(right, TRUE);
		std::vector<std::string> effects = wb::app_rule_effects(e.rule);
		std::string does = effects.empty() ? std::string(_("Nothing set")) : join(effects, " · ");
		GtkWidget *l = gtk_label_new(does.c_str());
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_label_set_wrap(GTK_LABEL(l), TRUE);
		if (effects.empty())
			gtk_widget_add_css_class(l, "dim-label");
		gtk_box_append(GTK_BOX(right), l);
		if (std::string err = wb::app_rule_pattern_error(e.rule); !err.empty()) {
			GtkWidget *w = gtk_label_new((std::string(_("Never matches: a pattern isn't a regular "
					"expression (")) + err + ")").c_str());
			gtk_label_set_xalign(GTK_LABEL(w), 0);
			gtk_label_set_wrap(GTK_LABEL(w), TRUE);
			gtk_widget_add_css_class(w, "wb-rule-error");
			gtk_box_append(GTK_BOX(right), w);
		}
		if (!e.kept.empty() || e.position_kept) {
			std::vector<std::string> kept = e.kept;
			if (e.position_kept)
				kept.insert(kept.begin(), "position");
			GtkWidget *k = dim(std::string(_("Also, as written in rc.xml: ")) + join(kept, ", "));
			gtk_label_set_xalign(GTK_LABEL(k), 0);
			gtk_label_set_wrap(GTK_LABEL(k), TRUE);
			gtk_widget_add_css_class(k, "wb-rule-kept");
			gtk_widget_set_tooltip_text(k, _("Openbox settings Waybox doesn't use: kept in "
					"rc.xml as they are"));
			gtk_box_append(GTK_BOX(right), k);
		}
		gtk_box_append(GTK_BOX(row), right);
		gtk_box_append(GTK_BOX(ui->rules_box), row);

		std::string kept_text = join(e.kept, ",") + (e.position_kept ? "|position" : "") +
				(e.rule.patterns == wb::AppRule::Patterns::Regex ? "|regex"
				 : e.rule.patterns == wb::AppRule::Patterns::IRegex ? "|iregex" : "") +
				(wb::app_rule_pattern_error(e.rule).empty() ? "" : "|never");
		ui->rule_lines.push_back("rule|" + e.rule.class_pattern + "|" + e.rule.title_pattern + "|" +
				join(effects, ";") + "|" + kept_text);
	}
	g_object_unref(column);
}

}  // namespace wbconf
