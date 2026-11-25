/*
 * A widget for each type of integration option (Type= in an
 * .integration file): wbconf draws them without knowing the program.
 *
 * Whatever the type, the option's value is one piece of text, as on the
 * command line. Types that take text (paths, colours, numbers) keep a text
 * field as the value, with the chooser, preview or colour button kept in
 * step beside it, so a value can still be left unset, or be "~/x" or
 * "$VAR".
 */
#include <sys/stat.h>

#include <cstdlib>
#include <cstring>

#include "wbconf_ui.hpp"

namespace wbconf {

namespace {

using wb::OptionType;

bool is_path_type(OptionType t) {
	return t == OptionType::File || t == OptionType::Folder || t == OptionType::Image ||
			t == OptionType::ImageFolder || t == OptionType::ImageOrFolder;
}

bool is_image_type(OptionType t) {
	return t == OptionType::Image || t == OptionType::ImageFolder ||
			t == OptionType::ImageOrFolder;
}

/* "~/x", "$HOME/x", "${HOME}/x" -> the path; anything else with a $ or a
 * shell's quoting can't be known without a shell: "". */
std::string expand_home(const std::string &v) {
	const char *home = getenv("HOME");
	std::string h = home != nullptr ? home : "";
	std::string out;
	if (v == "~" || v.rfind("~/", 0) == 0)
		out = h + v.substr(1);
	else if (v.rfind("$HOME/", 0) == 0)
		out = h + v.substr(5);
	else if (v.rfind("${HOME}/", 0) == 0)
		out = h + v.substr(7);
	else
		out = v;
	if (h.empty() && out != v)
		return "";
	if (out.find_first_of("$`\"'\\*?") != std::string::npos)
		return "";
	return out;
}

void update_helpers(OptionField *f) {
	std::string v = gtk_editable_get_text(GTK_EDITABLE(f->entry));
	if (f->thumb != nullptr) {
		std::string path = expand_home(v);
		struct stat st;
		bool file = !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
		gtk_picture_set_filename(f->thumb, file ? path.c_str() : nullptr);
		gtk_widget_set_visible(GTK_WIDGET(f->thumb), file);
	}
	if (f->color != nullptr) {
		/* Left out: the program's own colour (Default=), if known. */
		std::string shown = v.empty() ? f->opt->default_value : v;
		GdkRGBA rgba;
		if (!shown.empty() && gdk_rgba_parse(&rgba, shown.c_str())) {
			f->syncing = true;
			gtk_color_dialog_button_set_rgba(f->color, &rgba);
			f->syncing = false;
		}
	}
	bool valid = wb::option_value_valid(*f->opt, v);
	if (valid)
		gtk_widget_remove_css_class(GTK_WIDGET(f->entry), "error");
	else
		gtk_widget_add_css_class(GTK_WIDGET(f->entry), "error");
}

void changed(OptionField *f) {
	if (f->syncing)
		return;
	if (f->entry != nullptr)
		update_helpers(f);
	if (f->on_change)
		f->on_change();
}

void on_entry_changed(GtkEditable *, gpointer data) {
	changed(static_cast<OptionField *>(data));
}

void on_color_set(GObject *, GParamSpec *, gpointer data) {
	auto *f = static_cast<OptionField *>(data);
	if (f->syncing)
		return;
	const GdkRGBA *c = gtk_color_dialog_button_get_rgba(f->color);
	char hex[8];
	g_snprintf(hex, sizeof hex, "#%02x%02x%02x", (int)(c->red * 255 + 0.5),
			(int)(c->green * 255 + 0.5), (int)(c->blue * 255 + 0.5));
	gtk_editable_set_text(GTK_EDITABLE(f->entry), hex);  /* -> on_entry_changed */
}

void on_drop_selected(GObject *, GParamSpec *, gpointer data) {
	changed(static_cast<OptionField *>(data));
}

void on_switch_set(GObject *, GParamSpec *, gpointer data) {
	changed(static_cast<OptionField *>(data));
}

/* The file dialog's answer. */
void on_chosen(GObject *source, GAsyncResult *result, gpointer data, bool folder) {
	auto *f = static_cast<OptionField *>(data);
	GError *error = nullptr;
	GFile *file = folder
			? gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error)
			: gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
	if (file == nullptr) {
		g_clear_error(&error);  /* cancelled */
		return;
	}
	if (char *path = g_file_get_path(file)) {
		/* Under the home folder: written as ~/..., as people type it. */
		std::string p = path;
		const char *home = getenv("HOME");
		if (home != nullptr && home[0] && p.rfind(std::string(home) + "/", 0) == 0)
			p = "~" + p.substr(strlen(home));
		gtk_editable_set_text(GTK_EDITABLE(f->entry), p.c_str());
		g_free(path);
	}
	g_object_unref(file);
}

void choose(OptionField *f, bool folder) {
	GtkFileDialog *dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, f->opt->label.c_str());
	std::string current = expand_home(gtk_editable_get_text(GTK_EDITABLE(f->entry)));
	if (!current.empty()) {
		GFile *at = g_file_new_for_path(current.c_str());
		struct stat st;
		if (stat(current.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
			gtk_file_dialog_set_initial_folder(dialog, at);
		else if (stat(current.c_str(), &st) == 0)
			gtk_file_dialog_set_initial_file(dialog, at);
		g_object_unref(at);
	}
	if (!folder && is_image_type(f->opt->type)) {
		GtkFileFilter *images = gtk_file_filter_new();
		gtk_file_filter_set_name(images, _("Images"));
		gtk_file_filter_add_mime_type(images, "image/*");
		GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
		g_list_store_append(filters, images);
		gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
		gtk_file_dialog_set_default_filter(dialog, images);
		g_object_unref(filters);
		g_object_unref(images);
	}
	GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(f->widget));
	if (folder)
		gtk_file_dialog_select_folder(dialog, parent, nullptr,
				[](GObject *s, GAsyncResult *r, gpointer d) { on_chosen(s, r, d, true); }, f);
	else
		gtk_file_dialog_open(dialog, parent, nullptr,
				[](GObject *s, GAsyncResult *r, gpointer d) { on_chosen(s, r, d, false); }, f);
	g_object_unref(dialog);
}

GtkWidget *chooser_button(OptionField *f, bool folder) {
	GtkWidget *b = gtk_button_new_from_icon_name(folder ? "folder-open-symbolic"
			: "document-open-symbolic");
	gtk_widget_set_tooltip_text(b, folder ? _("Choose a folder…") : _("Choose a file…"));
	g_object_set_data(G_OBJECT(b), "wb-folder", GINT_TO_POINTER(folder ? 1 : 0));
	g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer data) {
		choose(static_cast<OptionField *>(data),
				GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "wb-folder")) != 0);
	}), f);
	return b;
}

}  // namespace

std::unique_ptr<OptionField> make_option_field(const wb::IntegrationOption &o,
		std::function<void()> on_change) {
	auto f = std::make_unique<OptionField>();
	f->opt = &o;
	f->on_change = std::move(on_change);
	OptionField *fp = f.get();
	switch (o.type) {
	case OptionType::Switch:
		f->sw = GTK_SWITCH(gtk_switch_new());
		gtk_widget_set_halign(GTK_WIDGET(f->sw), GTK_ALIGN_START);
		gtk_widget_set_valign(GTK_WIDGET(f->sw), GTK_ALIGN_CENTER);
		g_signal_connect(f->sw, "notify::active", G_CALLBACK(on_switch_set), fp);
		f->widget = GTK_WIDGET(f->sw);
		return f;
	case OptionType::Choice: {
		/* The first item leaves the option out: the program's default. */
		f->drop_model = gtk_string_list_new(nullptr);
		if (o.default_value.empty()) {
			gtk_string_list_append(f->drop_model, _("Default"));
		} else {
			char *d = g_strdup_printf(_("Default (%s)"), o.default_value.c_str());
			gtk_string_list_append(f->drop_model, d);
			g_free(d);
		}
		f->drop_values.push_back("");
		for (const std::string &c : o.choices) {
			gtk_string_list_append(f->drop_model, c.c_str());
			f->drop_values.push_back(c);
		}
		f->drop = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(f->drop_model), nullptr));
		g_signal_connect(f->drop, "notify::selected", G_CALLBACK(on_drop_selected), fp);
		f->widget = GTK_WIDGET(f->drop);
		return f;
	}
	default:
		break;
	}
	f->entry = GTK_ENTRY(gtk_entry_new());
	gtk_entry_set_placeholder_text(f->entry, o.hint.c_str());
	gtk_widget_set_hexpand(GTK_WIDGET(f->entry), TRUE);
	g_signal_connect(f->entry, "changed", G_CALLBACK(on_entry_changed), fp);
	if (o.type == OptionType::Text) {
		f->widget = GTK_WIDGET(f->entry);
		return f;
	}
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	f->widget = box;
	if (o.type == OptionType::Number) {
		gtk_entry_set_input_purpose(f->entry, GTK_INPUT_PURPOSE_NUMBER);
		gtk_editable_set_width_chars(GTK_EDITABLE(f->entry), 8);
		gtk_widget_set_hexpand(GTK_WIDGET(f->entry), FALSE);
		gtk_box_append(GTK_BOX(box), GTK_WIDGET(f->entry));
		if (o.min || o.max) {
			char *range = nullptr;
			if (o.min && o.max)
				range = g_strdup_printf(_("%g to %g"), *o.min, *o.max);
			else if (o.min)
				range = g_strdup_printf(_("at least %g"), *o.min);
			else
				range = g_strdup_printf(_("at most %g"), *o.max);
			GtkWidget *l = gtk_label_new(range);
			gtk_widget_add_css_class(l, "dim-label");
			gtk_box_append(GTK_BOX(box), l);
			g_free(range);
		}
		return f;
	}
	if (o.type == OptionType::Color) {
		GtkColorDialog *dialog = gtk_color_dialog_new();
		gtk_color_dialog_set_with_alpha(dialog, FALSE);
		f->color = GTK_COLOR_DIALOG_BUTTON(gtk_color_dialog_button_new(dialog));
		g_signal_connect(f->color, "notify::rgba", G_CALLBACK(on_color_set), fp);
		gtk_editable_set_width_chars(GTK_EDITABLE(f->entry), 10);
		gtk_widget_set_hexpand(GTK_WIDGET(f->entry), FALSE);
		gtk_box_append(GTK_BOX(box), GTK_WIDGET(f->color));
		gtk_box_append(GTK_BOX(box), GTK_WIDGET(f->entry));
		return f;
	}
	if (is_path_type(o.type)) {
		gtk_widget_set_size_request(GTK_WIDGET(f->entry), 260, -1);
		if (is_image_type(o.type)) {
			f->thumb = GTK_PICTURE(gtk_picture_new());
			gtk_picture_set_content_fit(f->thumb, GTK_CONTENT_FIT_COVER);
			gtk_widget_set_size_request(GTK_WIDGET(f->thumb), 64, 40);
			gtk_widget_set_visible(GTK_WIDGET(f->thumb), FALSE);
			gtk_box_append(GTK_BOX(box), GTK_WIDGET(f->thumb));
		}
		gtk_box_append(GTK_BOX(box), GTK_WIDGET(f->entry));
		bool files = o.type == OptionType::File || o.type == OptionType::Image ||
				o.type == OptionType::ImageOrFolder;
		bool folders = o.type == OptionType::Folder || o.type == OptionType::ImageFolder ||
				o.type == OptionType::ImageOrFolder;
		if (files)
			gtk_box_append(GTK_BOX(box), chooser_button(fp, false));
		if (folders)
			gtk_box_append(GTK_BOX(box), chooser_button(fp, true));
	}
	return f;
}

std::string option_field_value(const OptionField &f) {
	if (f.sw != nullptr)
		return gtk_switch_get_active(f.sw) ? "on" : "";
	if (f.drop != nullptr) {
		guint i = gtk_drop_down_get_selected(f.drop);
		return i < f.drop_values.size() ? f.drop_values[i] : "";
	}
	return gtk_editable_get_text(GTK_EDITABLE(f.entry));
}

void set_option_field_value(OptionField &f, const std::string &value) {
	f.syncing = true;
	if (f.sw != nullptr) {
		gtk_switch_set_active(f.sw, !value.empty());
	} else if (f.drop != nullptr) {
		size_t i = 0;
		while (i < f.drop_values.size() && f.drop_values[i] != value)
			++i;
		if (i == f.drop_values.size()) {
			/* Not one of the choices (typed in the command): kept, as is. */
			gtk_string_list_append(f.drop_model, value.c_str());
			f.drop_values.push_back(value);
		}
		gtk_drop_down_set_selected(f.drop, static_cast<guint>(i));
	} else {
		gtk_editable_set_text(GTK_EDITABLE(f.entry), value.c_str());
		update_helpers(&f);
	}
	f.syncing = false;
}

}  // namespace wbconf
