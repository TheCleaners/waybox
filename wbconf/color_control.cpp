/*
 * The color control, for every color wbconf edits: the value as text (so
 * it can be left unset, or written "#rrggbbaa"), GTK's color button
 * beside it, and an eyedropper that picks a color off the screen (Waybox's
 * pick_color). The text field is tinted with its color, its text dark or
 * light to stay readable (waybox/color_text.cpp).
 *
 * The owner listens to the field ("changed") as for any text field: the
 * button and the eyedropper only set its text.
 */
#include "wbconf_ui.hpp"

#include "waybox/color_text.hpp"

namespace wbconf {

namespace {

int next_class = 0;

/* The field's tint, in a stylesheet of its own attached to the field only:
 * a change restyles that field, not the whole window (one stylesheet for
 * every field, reloaded on each change, restyled all of wbconf each time:
 * a color picked froze it for minutes). GTK deprecates a widget's own
 * provider, without a replacement for this. */
void set_tint(ColorControl *c, const std::string &css) {
	if (css == c->tint_css)
		return;
	if (c->tint == nullptr) {
		c->tint = gtk_css_provider_new();
		G_GNUC_BEGIN_IGNORE_DEPRECATIONS
		gtk_style_context_add_provider(gtk_widget_get_style_context(GTK_WIDGET(c->entry)),
				GTK_STYLE_PROVIDER(c->tint), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
		G_GNUC_END_IGNORE_DEPRECATIONS
	}
#if GTK_CHECK_VERSION(4, 12, 0)
	gtk_css_provider_load_from_string(c->tint, css.c_str());
#else
	gtk_css_provider_load_from_data(c->tint, css.c_str(), -1);
#endif
	c->tint_css = css;
}

std::string hex_of(const GdkRGBA *c, bool alpha) {
	char buf[16];
	auto byte = [](float f) { return static_cast<int>(std::lround(std::clamp(f, 0.0f, 1.0f) * 255)); };
	if (alpha && byte(c->alpha) < 255)
		g_snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", byte(c->red), byte(c->green),
				byte(c->blue), byte(c->alpha));
	else
		g_snprintf(buf, sizeof buf, "#%02x%02x%02x", byte(c->red), byte(c->green), byte(c->blue));
	return buf;
}

/* "#rrggbbaa" too (GDK reads it the CSS way, which is the same). */
bool rgba_of(const std::string &v, GdkRGBA *out) {
	auto c = wb::parse_color(v);
	if (!c)
		return false;
	*out = {c->r / 255.0f, c->g / 255.0f, c->b / 255.0f, c->a / 255.0f};
	return true;
}

/* Whether Waybox can pick a color (its scripting socket answers): asked
 * once, for every control. */
bool can_pick() {
	static int known = -1;
	if (known < 0) {
		JsonObject *o = waybox_request("{\"request\":\"get_version\"}");
		known = o != nullptr;
		if (o != nullptr)
			json_object_unref(o);
	}
	return known == 1;
}

void on_text(GtkEditable *, gpointer data) {
	auto *c = static_cast<ColorControl *>(data);
	color_control_refresh(c);
}

/* The button's color into the field: the first change at once, then while
 * it keeps changing the latest one every 100 ms. Dragging in GTK's color
 * editor changes it many times a second, and each change of the field is
 * a setting changed (its tint, the unsaved changes, the problems checked
 * again): one at a time, a drag froze wbconf. */
gboolean on_button_settled(gpointer data) {
	auto *c = static_cast<ColorControl *>(data);
	if (c->pending == gtk_editable_get_text(GTK_EDITABLE(c->entry))) {
		c->pending_timer = 0;  /* quiet for 100 ms: the next change goes at once */
		return G_SOURCE_REMOVE;
	}
	gtk_editable_set_text(GTK_EDITABLE(c->entry), c->pending.c_str());
	return G_SOURCE_CONTINUE;  /* and wait another 100 ms */
}

void on_button(GObject *, GParamSpec *, gpointer data) {
	auto *c = static_cast<ColorControl *>(data);
	if (c->syncing)
		return;
	c->pending = hex_of(gtk_color_dialog_button_get_rgba(c->button), c->alpha);
	if (c->pending_timer != 0)
		return;  /* changing fast: the latest when the 100 ms are up */
	gtk_editable_set_text(GTK_EDITABLE(c->entry), c->pending.c_str());
	c->pending_timer = g_timeout_add(100, on_button_settled, c);
}

/* The eyedropper: Waybox waits for a click, so not on this thread. */
void on_pick(GtkButton *, gpointer data) {
	auto *c = static_cast<ColorControl *>(data);
	if (c->picking)
		return;
	c->picking = true;
	gtk_widget_set_sensitive(c->pick, FALSE);
	GTask *task = g_task_new(nullptr, nullptr, [](GObject *, GAsyncResult *res, gpointer data) {
		auto *alive = static_cast<std::shared_ptr<ColorControl *> *>(data);
		auto *o = static_cast<JsonObject *>(g_task_propagate_pointer(G_TASK(res), nullptr));
		ColorControl *c = **alive;
		delete alive;
		if (c != nullptr) {
			c->picking = false;
			gtk_widget_set_sensitive(c->pick, TRUE);
			const char *color = o != nullptr
					? json_object_get_string_member_with_default(o, "color", nullptr) : nullptr;
			if (color != nullptr)
				gtk_editable_set_text(GTK_EDITABLE(c->entry), color);
		}
		if (o != nullptr)
			json_object_unref(o);
	}, new std::shared_ptr<ColorControl *>(c->self));
	g_task_run_in_thread(task, [](GTask *task, gpointer, gpointer, GCancellable *) {
		/* Waybox gives up after 30 s: wait a little longer. */
		g_task_return_pointer(task, waybox_request("{\"request\":\"pick_color\"}", 35),
				[](gpointer o) { json_object_unref(static_cast<JsonObject *>(o)); });
	});
	g_object_unref(task);
}

}  // namespace

void color_control_refresh(ColorControl *c) {
	std::string v = gtk_editable_get_text(GTK_EDITABLE(c->entry));
	/* The field's own background, as the theme is: light under dark text. */
	GdkRGBA fg;
	gtk_widget_get_color(GTK_WIDGET(c->entry), &fg);
	bool light = 0.2126 * fg.red + 0.7152 * fg.green + 0.0722 * fg.blue < 0.5;
	wb::Color base = light ? wb::Color{255, 255, 255, 255} : wb::Color{0x24, 0x24, 0x24, 255};
	auto tint = wb::field_tint(v, base);
	set_tint(c, tint ? wb::field_tint_css(c->css_class, *tint) : std::string());
	/* The button shows the value; left out, the default, else the example
	 * the field shows greyed; else nothing (clear), never GTK's own red. */
	GdkRGBA rgba;
	std::string shown = !v.empty() ? v : !c->default_value.empty() ? c->default_value : c->hint;
	if (shown.empty() || !rgba_of(shown, &rgba)) {
		if (!v.empty())
			return;  /* being typed: the last good one stays */
		rgba = GdkRGBA{0, 0, 0, 0};
	}
	c->syncing = true;
	gtk_color_dialog_button_set_rgba(c->button, &rgba);
	c->syncing = false;
}

std::unique_ptr<ColorControl> make_color_control(const std::string &hint,
		const std::string &default_value, bool alpha) {
	auto c = std::make_unique<ColorControl>();
	c->self = std::make_shared<ColorControl *>(c.get());
	c->alpha = alpha;
	c->default_value = default_value;
	c->hint = hint;
	c->css_class = "wb-color-" + std::to_string(next_class++);
	c->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkColorDialog *dialog = gtk_color_dialog_new();
	gtk_color_dialog_set_with_alpha(dialog, alpha);
	c->button = GTK_COLOR_DIALOG_BUTTON(gtk_color_dialog_button_new(dialog));
	g_signal_connect(c->button, "notify::rgba", G_CALLBACK(on_button), c.get());
	gtk_box_append(GTK_BOX(c->box), GTK_WIDGET(c->button));
	c->entry = GTK_ENTRY(gtk_entry_new());
	gtk_entry_set_placeholder_text(c->entry, hint.c_str());
	gtk_editable_set_width_chars(GTK_EDITABLE(c->entry), alpha ? 11 : 9);
	gtk_widget_add_css_class(GTK_WIDGET(c->entry), c->css_class.c_str());
	g_signal_connect(c->entry, "changed", G_CALLBACK(on_text), c.get());
	gtk_box_append(GTK_BOX(c->box), GTK_WIDGET(c->entry));
	c->pick = gtk_button_new_from_icon_name("color-select-symbolic");
	if (can_pick()) {
		gtk_widget_set_tooltip_text(c->pick, _("Pick a color from the screen (Escape to "
				"cancel)"));
	} else {
		gtk_widget_set_sensitive(c->pick, FALSE);
		gtk_widget_set_tooltip_text(c->pick, _("Picking from the screen needs Waybox's "
				"scripting socket, turned off in rc.xml (<ipc enabled=\"no\"/>)"));
	}
	g_signal_connect(c->pick, "clicked", G_CALLBACK(on_pick), c.get());
	gtk_box_append(GTK_BOX(c->box), c->pick);
	color_control_refresh(c.get());
	return c;
}

ColorControl::~ColorControl() {
	*self = nullptr;  /* a pick answering later finds it gone */
	if (pending_timer != 0)
		g_source_remove(pending_timer);
	if (tint != nullptr)
		g_object_unref(tint);
}

}  // namespace wbconf
