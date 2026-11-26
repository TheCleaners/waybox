/*
 * The wallpaper page (Page=wallpaper in an integration file: swaybg,
 * waybox-wallpaper).
 *
 * A preview shaped like the monitor, drawn as the program would (mode,
 * colour); the wallpapers in a folder as thumbnails to choose from; and,
 * where the program takes a folder (waybox-wallpaper), a random one from
 * it, changed every so many minutes.
 *
 * It knows only the program's row: it sets the program's options as their
 * own fields would (by type: the image option, the colour, a "mode"
 * choice, an "every" number), so the command stays in step, both ways.
 */
#include "../wbconf_ui.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace wbconf {

namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxImages = 400;   /* per folder: thumbnails are made for each */
constexpr int kThumbW = 128, kThumbH = 80;

std::string home_dir() {
	const char *h = getenv("HOME");
	return h != nullptr ? h : "";
}

/* "~/x" and "$HOME/x" as a path; "" for anything only a shell knows. */
std::string expand(const std::string &v) {
	std::string h = home_dir();
	std::string out = v;
	if (v == "~" || v.rfind("~/", 0) == 0)
		out = h + v.substr(1);
	else if (v.rfind("$HOME/", 0) == 0)
		out = h + v.substr(5);
	else if (v.rfind("${HOME}/", 0) == 0)
		out = h + v.substr(7);
	if (out.find_first_of("$`\"'\\*?") != std::string::npos)
		return "";
	return out;
}

/* A path as people write it: ~/... under the home folder. */
std::string tilde(const std::string &path) {
	std::string h = home_dir();
	if (!h.empty() && path.rfind(h + "/", 0) == 0)
		return "~" + path.substr(h.size());
	return path;
}

bool is_image_name(const std::string &name) {
	size_t dot = name.rfind('.');
	if (dot == std::string::npos)
		return false;
	std::string ext = name.substr(dot + 1);
	for (char &c : ext)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	static const char *kExts[] = {"png", "jpg", "jpeg", "webp", "bmp", "tif", "tiff",
			"gif", "jxl", "avif", "svg"};
	for (const char *e : kExts)
		if (ext == e)
			return true;
	return false;
}

/* The images in a folder and the folders under it (wallpaper packs nest
 * theirs: contents/images/...), sorted; at most kMaxImages. */
std::vector<std::string> images_in(const std::string &dir) {
	std::vector<std::string> out;
	std::error_code ec;
	fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
	for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
		if (it.depth() > 3) {
			it.disable_recursion_pending();
			continue;
		}
		const fs::directory_entry &e = *it;
		std::string name = e.path().filename().string();
		if (!name.empty() && name[0] == '.') {
			if (e.is_directory(ec))
				it.disable_recursion_pending();
			continue;
		}
		if (e.is_regular_file(ec) && is_image_name(name)) {
			out.push_back(e.path().string());
			if (out.size() >= kMaxImages)
				break;
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

bool is_dir(const std::string &p) {
	struct stat st;
	return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string &p) {
	struct stat st;
	return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/* Where wallpapers are kept: the user's, then the system's. */
std::vector<std::string> wallpaper_folders() {
	std::vector<std::string> dirs;
	std::string h = home_dir();
	const char *xdh = getenv("XDG_DATA_HOME");
	std::string data_home = xdh != nullptr && xdh[0] ? xdh : h + "/.local/share";
	for (const std::string &d : {data_home + "/wallpapers", data_home + "/backgrounds",
			h + "/Pictures/Wallpapers", h + "/Pictures/wallpapers", h + "/Pictures"})
		dirs.push_back(d);
	const char *xdd = getenv("XDG_DATA_DIRS");
	std::string data_dirs = xdd != nullptr && xdd[0] ? xdd : "/usr/local/share:/usr/share";
	size_t start = 0;
	while (start <= data_dirs.size()) {
		size_t colon = data_dirs.find(':', start);
		std::string d = data_dirs.substr(start, colon == std::string::npos ? std::string::npos
				: colon - start);
		if (!d.empty()) {
			dirs.push_back(d + "/backgrounds");
			dirs.push_back(d + "/wallpapers");
		}
		if (colon == std::string::npos)
			break;
		start = colon + 1;
	}
	return dirs;
}

/* gdk-pixbuf's pixels as a cairo surface (premultiplied ARGB), for the
 * preview and the thumbnails: drawn at the size wanted, whatever the
 * image's (GTK deprecated its own way). */
cairo_surface_t *surface_of(GdkPixbuf *pb) {
	int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
	int n = gdk_pixbuf_get_n_channels(pb), stride = gdk_pixbuf_get_rowstride(pb);
	bool alpha = gdk_pixbuf_get_has_alpha(pb);
	const guint8 *src = gdk_pixbuf_read_pixels(pb);
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_surface_flush(s);
	unsigned char *dst = cairo_image_surface_get_data(s);
	int dstride = cairo_image_surface_get_stride(s);
	for (int y = 0; y < h; ++y) {
		const guint8 *p = src + static_cast<size_t>(y) * stride;
		auto *q = reinterpret_cast<uint32_t *>(dst + static_cast<size_t>(y) * dstride);
		for (int x = 0; x < w; ++x, p += n) {
			uint32_t a = alpha ? p[3] : 255;
			uint32_t r = p[0] * a / 255, g = p[1] * a / 255, b = p[2] * a / 255;
			q[x] = a << 24 | r << 16 | g << 8 | b;
		}
	}
	cairo_surface_mark_dirty(s);
	return s;
}

struct Page {
	Ui::ProgramRow *row = nullptr;
	OptionField *image = nullptr, *colour = nullptr, *mode = nullptr, *every = nullptr;
	bool takes_folder = false;  /* the image option takes a folder: random */
	bool updating = false;

	GtkWidget *preview = nullptr;
	cairo_surface_t *shown = nullptr;   /* the image, scaled for the preview */
	std::string shown_path;
	int image_w = 0, image_h = 0;       /* its real size */

	GtkDropDown *folder_drop = nullptr;
	GtkStringList *folder_model = nullptr;
	std::vector<std::string> folders;
	std::string folder;                 /* shown in the grid */
	GtkFlowBox *grid = nullptr;
	GtkLabel *grid_note = nullptr;
	GCancellable *loading = nullptr;    /* the grid's thumbnails */

	GtkCheckButton *random = nullptr;
	GtkCheckButton *every_on = nullptr;
	GtkSpinButton *every_spin = nullptr;
	GtkWidget *every_box = nullptr;
	GtkDropDown *mode_drop = nullptr;

	~Page() {
		if (loading != nullptr) {
			g_cancellable_cancel(loading);
			g_object_unref(loading);
		}
		if (shown != nullptr)
			cairo_surface_destroy(shown);
	}
};

std::string value(OptionField *f) {
	return f != nullptr ? option_field_value(*f) : "";
}

/* Set an option as its field would, the command following. */
void set(Page *p, OptionField *f, const std::string &v) {
	if (f == nullptr || option_field_value(*f) == v)
		return;
	p->updating = true;
	set_option_field_value(*f, v);
	if (f->on_change)
		f->on_change();
	p->updating = false;
}

std::string current_mode(Page *p) {
	std::string m = value(p->mode);
	if (m.empty() && p->mode != nullptr)
		m = p->mode->opt->default_value;
	return m.empty() ? "fill" : m;
}

/* ---- The preview ----------------------------------------------------- */

void load_preview(Page *p, const std::string &path) {
	if (path == p->shown_path)
		return;
	p->shown_path = path;
	if (p->shown != nullptr) {
		cairo_surface_destroy(p->shown);
		p->shown = nullptr;
	}
	p->image_w = p->image_h = 0;
	if (is_file(path) && gdk_pixbuf_get_file_info(path.c_str(), &p->image_w, &p->image_h)) {
		GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path.c_str(), 1280, 1280, TRUE, nullptr);
		if (pb != nullptr) {
			p->shown = surface_of(pb);
			g_object_unref(pb);
		}
	}
	gtk_widget_queue_draw(p->preview);
}

/* The image the program would show now: the one chosen, or (random) the
 * first in the folder, as an example. */
std::string preview_path(Page *p) {
	std::string v = expand(value(p->image));
	if (is_dir(v)) {
		std::vector<std::string> in = images_in(v);
		return in.empty() ? "" : in.front();
	}
	return v;
}

void monitor_size(GtkWidget *w, int *mw, int *mh) {
	*mw = 1920;
	*mh = 1080;
	GdkDisplay *d = gtk_widget_get_display(w);
	GListModel *monitors = d != nullptr ? gdk_display_get_monitors(d) : nullptr;
	if (monitors == nullptr || g_list_model_get_n_items(monitors) == 0)
		return;
	auto *m = static_cast<GdkMonitor *>(g_list_model_get_item(monitors, 0));
	GdkRectangle r;
	gdk_monitor_get_geometry(m, &r);
	if (r.width > 0 && r.height > 0) {
		*mw = r.width;
		*mh = r.height;
	}
	g_object_unref(m);
}

void draw_preview(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
	auto *p = static_cast<Page *>(data);
	int mw, mh;
	monitor_size(GTK_WIDGET(area), &mw, &mh);
	/* The monitor, as big as fits, with a bezel. */
	double bezel = 6;
	double sx = (w - 2 * bezel) / mw, sy = (h - 2 * bezel) / mh;
	double s = std::min(sx, sy);
	double W = mw * s, H = mh * s;
	double X = (w - W) / 2, Y = (h - H) / 2;
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
	cairo_rectangle(cr, X - bezel, Y - bezel, W + 2 * bezel, H + 2 * bezel);
	cairo_fill(cr);
	cairo_save(cr);
	cairo_rectangle(cr, X, Y, W, H);
	cairo_clip(cr);
	GdkRGBA bg = {0, 0, 0, 1};
	std::string c = value(p->colour);
	if (c.empty() && p->colour != nullptr)
		c = p->colour->opt->default_value;
	if (!c.empty())
		gdk_rgba_parse(&bg, c.c_str());
	gdk_cairo_set_source_rgba(cr, &bg);
	cairo_paint(cr);
	std::string mode = current_mode(p);
	if (p->shown != nullptr && p->image_w > 0 && mode != "solid_color") {
		double iw = p->image_w, ih = p->image_h;
		double sw = cairo_image_surface_get_width(p->shown);
		double k;  /* preview pixels per image pixel */
		if (mode == "stretch") {
			cairo_translate(cr, X, Y);
			cairo_scale(cr, W / sw, H / cairo_image_surface_get_height(p->shown));
			cairo_set_source_surface(cr, p->shown, 0, 0);
			cairo_paint(cr);
		} else if (mode == "tile") {
			k = s;  /* the image at its size on the screen */
			cairo_translate(cr, X, Y);
			cairo_scale(cr, k * iw / sw, k * iw / sw);
			cairo_set_source_surface(cr, p->shown, 0, 0);
			cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
			cairo_paint(cr);
		} else {
			if (mode == "fit")
				k = std::min(W / iw, H / ih);
			else if (mode == "center")
				k = s;
			else  /* fill */
				k = std::max(W / iw, H / ih);
			double dw = iw * k, dh = ih * k;
			cairo_translate(cr, X + (W - dw) / 2, Y + (H - dh) / 2);
			cairo_scale(cr, dw / sw, dw / sw);
			cairo_set_source_surface(cr, p->shown, 0, 0);
			cairo_paint(cr);
		}
	}
	cairo_restore(cr);
}

/* ---- The folder's thumbnails ----------------------------------------- */

struct ThumbJob {
	std::string path;
	GtkWidget *area;  /* ref'd */
	int scale;        /* the screen's: thumbnails as sharp as it shows */
};

/* A thumbnail: its image cover-fitted to the area. */
void draw_thumb(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer data) {
	auto *s = static_cast<cairo_surface_t *>(data);
	if (s == nullptr) {
		cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.15);
		cairo_paint(cr);
		return;
	}
	double sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s);
	double k = std::max(w / sw, h / sh);
	cairo_translate(cr, (w - sw * k) / 2, (h - sh * k) / 2);
	cairo_scale(cr, k, k);
	cairo_set_source_surface(cr, s, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
}

void thumb_thread(GTask *task, gpointer, gpointer data, GCancellable *cancel) {
	auto *job = static_cast<ThumbJob *>(data);
	if (g_cancellable_is_cancelled(cancel)) {
		g_task_return_pointer(task, nullptr, nullptr);
		return;
	}
	/* Big enough to cover the thumbnail: the image's shorter side fits. */
	int iw = 0, ih = 0;
	GdkPixbuf *pb = nullptr;
	if (gdk_pixbuf_get_file_info(job->path.c_str(), &iw, &ih) != nullptr && iw > 0 && ih > 0) {
		double k = std::max(double(kThumbW) / iw, double(kThumbH) / ih) * job->scale;
		pb = gdk_pixbuf_new_from_file_at_scale(job->path.c_str(),
				std::max(1, int(iw * k + 0.5)), std::max(1, int(ih * k + 0.5)), TRUE, nullptr);
	}
	g_task_return_pointer(task, pb, g_object_unref);
}

void thumb_done(GObject *, GAsyncResult *res, gpointer data) {
	auto *job = static_cast<ThumbJob *>(data);
	auto *pb = static_cast<GdkPixbuf *>(g_task_propagate_pointer(G_TASK(res), nullptr));
	if (pb != nullptr) {
		cairo_surface_t *s = surface_of(pb);
		g_object_unref(pb);
		gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(job->area), draw_thumb, s,
				[](gpointer s) { cairo_surface_destroy(static_cast<cairo_surface_t *>(s)); });
	} else if (GtkWidget *child = gtk_widget_get_ancestor(job->area, GTK_TYPE_FLOW_BOX_CHILD)) {
		gtk_widget_set_visible(child, FALSE);  /* not an image after all */
	}
	g_object_unref(job->area);
	delete job;
}

void select_current(Page *p) {
	std::string v = expand(value(p->image));
	for (GtkWidget *c = gtk_widget_get_first_child(GTK_WIDGET(p->grid)); c != nullptr;
			c = gtk_widget_get_next_sibling(c)) {
		auto *path = static_cast<const char *>(g_object_get_data(G_OBJECT(c), "wb-path"));
		if (path != nullptr && v == path) {
			gtk_flow_box_select_child(p->grid, GTK_FLOW_BOX_CHILD(c));
			return;
		}
	}
	gtk_flow_box_unselect_all(p->grid);
}

void fill_grid(Page *p, const std::string &dir) {
	if (dir == p->folder)
		return;
	p->folder = dir;
	if (p->loading != nullptr) {
		g_cancellable_cancel(p->loading);
		g_object_unref(p->loading);
	}
	p->loading = g_cancellable_new();
	while (GtkWidget *c = gtk_widget_get_first_child(GTK_WIDGET(p->grid)))
		gtk_flow_box_remove(p->grid, c);
	std::vector<std::string> images = dir.empty() ? std::vector<std::string>{} : images_in(dir);
	for (const std::string &path : images) {
		GtkWidget *pic = gtk_drawing_area_new();
		gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(pic), kThumbW);
		gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(pic), kThumbH);
		gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(pic), draw_thumb, nullptr, nullptr);
		gtk_widget_set_halign(pic, GTK_ALIGN_CENTER);
		gtk_widget_set_valign(pic, GTK_ALIGN_CENTER);
		GtkWidget *child = gtk_flow_box_child_new();
		gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), pic);
		g_object_set_data_full(G_OBJECT(child), "wb-path", g_strdup(path.c_str()), g_free);
		gtk_widget_set_tooltip_text(child, fs::path(path).filename().c_str());
		gtk_flow_box_append(p->grid, child);
		auto *job = new ThumbJob{path, GTK_WIDGET(g_object_ref(pic)),
				std::max(1, gtk_widget_get_scale_factor(GTK_WIDGET(p->grid)))};
		GTask *task = g_task_new(nullptr, p->loading, thumb_done, job);
		g_task_set_task_data(task, job, nullptr);
		g_task_run_in_thread(task, thumb_thread);
		g_object_unref(task);
	}
	std::string note;
	if (images.empty())
		note = _("No images in this folder.");
	else if (images.size() >= kMaxImages)
		note = _("Showing the first 400 images.");
	gtk_label_set_text(p->grid_note, note.c_str());
	gtk_widget_set_visible(GTK_WIDGET(p->grid_note), !note.empty());
	select_current(p);
}

void on_thumb_activated(GtkFlowBox *, GtkFlowBoxChild *child, gpointer data) {
	auto *p = static_cast<Page *>(data);
	auto *path = static_cast<const char *>(g_object_get_data(G_OBJECT(child), "wb-path"));
	if (path == nullptr)
		return;
	set(p, p->image, tilde(path));
	if (p->random != nullptr) {
		p->updating = true;
		gtk_check_button_set_active(p->random, FALSE);
		p->updating = false;
	}
	wallpaper_refresh(p);
}

/* ---- Folders --------------------------------------------------------- */

void set_folders(Page *p, const std::string &want) {
	std::vector<std::string> list;
	auto add = [&](const std::string &d) {
		if (is_dir(d) && std::find(list.begin(), list.end(), d) == list.end())
			list.push_back(d);
	};
	add(want);
	for (const std::string &d : wallpaper_folders())
		if (!images_in(d).empty())
			add(d);
	for (const std::string &d : p->folders)  /* added with "Other folder…" */
		add(d);
	p->updating = true;
	guint n = g_list_model_get_n_items(G_LIST_MODEL(p->folder_model));
	gtk_string_list_splice(p->folder_model, 0, n, nullptr);
	for (const std::string &d : list)
		gtk_string_list_append(p->folder_model, tilde(d).c_str());
	p->folders = list;
	auto it = std::find(list.begin(), list.end(), want);
	if (it != list.end())
		gtk_drop_down_set_selected(p->folder_drop, static_cast<guint>(it - list.begin()));
	p->updating = false;
	fill_grid(p, it != list.end() ? want : (list.empty() ? "" : list.front()));
}

void on_folder_chosen(GtkDropDown *, GParamSpec *, gpointer data) {
	auto *p = static_cast<Page *>(data);
	if (p->updating)
		return;
	guint i = gtk_drop_down_get_selected(p->folder_drop);
	if (i >= p->folders.size())
		return;
	fill_grid(p, p->folders[i]);
	/* Random: from the folder shown. */
	if (p->random != nullptr && gtk_check_button_get_active(p->random)) {
		set(p, p->image, tilde(p->folders[i]));
		wallpaper_refresh(p);
	}
}

void on_other_folder(GtkButton *b, gpointer data) {
	auto *p = static_cast<Page *>(data);
	GtkFileDialog *dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, _("Folder of wallpapers"));
	gtk_file_dialog_select_folder(dialog, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(b))),
			nullptr, [](GObject *s, GAsyncResult *r, gpointer data) {
		auto *p = static_cast<Page *>(data);
		GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(s), r, nullptr);
		if (f == nullptr)
			return;
		if (char *path = g_file_get_path(f)) {
			p->folders.push_back(path);
			set_folders(p, path);
			if (p->random != nullptr && gtk_check_button_get_active(p->random)) {
				set(p, p->image, tilde(path));
				wallpaper_refresh(p);
			}
			g_free(path);
		}
		g_object_unref(f);
	}, p);
	g_object_unref(dialog);
}

/* ---- Random, every, mode --------------------------------------------- */

void on_random_toggled(GtkCheckButton *, gpointer data) {
	auto *p = static_cast<Page *>(data);
	if (p->updating)
		return;
	if (gtk_check_button_get_active(p->random)) {
		if (!p->folder.empty())
			set(p, p->image, tilde(p->folder));
	} else {
		/* Back to one image: the one previewed. */
		std::string first = preview_path(p);
		set(p, p->image, first.empty() ? "" : tilde(first));
		set(p, p->every, "");
	}
	wallpaper_refresh(p);
}

void on_every_changed(GtkWidget *, gpointer data) {
	auto *p = static_cast<Page *>(data);
	if (p->updating)
		return;
	bool on = gtk_check_button_get_active(p->every_on);
	gtk_widget_set_sensitive(GTK_WIDGET(p->every_spin), on);
	set(p, p->every, on ? std::to_string(gtk_spin_button_get_value_as_int(p->every_spin)) : "");
}

void on_mode_chosen(GtkDropDown *, GParamSpec *, gpointer data) {
	auto *p = static_cast<Page *>(data);
	if (p->updating || p->mode == nullptr)
		return;
	guint i = gtk_drop_down_get_selected(p->mode_drop);
	/* The page's list is the option's own: "Default", then the choices
	 * (and a value of the command's that isn't one). */
	set(p, p->mode, i < p->mode->drop_values.size() ? p->mode->drop_values[i] : "");
	gtk_widget_queue_draw(p->preview);
}

OptionField *field(Ui::ProgramRow *row, wb::OptionType type, const char *id = nullptr) {
	for (auto &[oid, f] : row->options) {
		if (id != nullptr && oid != id)
			continue;
		if (type == wb::OptionType::Image && (f->opt->type == wb::OptionType::Image ||
				f->opt->type == wb::OptionType::ImageOrFolder))
			return f.get();
		if (f->opt->type == type)
			return f.get();
	}
	return nullptr;
}

}  // namespace

/* Everything shown from the options' values (the command changed). */
void wallpaper_refresh(void *page) {
	auto *p = static_cast<Page *>(page);
	if (p->updating)
		return;
	p->updating = true;
	std::string v = expand(value(p->image));
	bool random = p->takes_folder && is_dir(v);
	if (p->random != nullptr)
		gtk_check_button_set_active(p->random, random);
	if (p->every_box != nullptr) {
		gtk_widget_set_visible(p->every_box, random);
		std::string e = value(p->every);
		gtk_check_button_set_active(p->every_on, !e.empty());
		gtk_widget_set_sensitive(GTK_WIDGET(p->every_spin), !e.empty());
		if (!e.empty())
			gtk_spin_button_set_value(p->every_spin, g_ascii_strtod(e.c_str(), nullptr));
	}
	if (p->mode_drop != nullptr && p->mode != nullptr)
		gtk_drop_down_set_selected(p->mode_drop, gtk_drop_down_get_selected(p->mode->drop));
	p->updating = false;
	std::string folder = random ? v : (is_file(v) ? fs::path(v).parent_path().string() : "");
	if (folder.empty() && p->folder.empty())
		set_folders(p, "");
	else if (!folder.empty() && folder != p->folder)
		set_folders(p, folder);
	else
		select_current(p);
	load_preview(p, preview_path(p));
	gtk_widget_queue_draw(p->preview);
}

GtkWidget *build_wallpaper_page(Ui *, Ui::ProgramRow *row) {
	auto page = std::make_shared<Page>();
	Page *p = page.get();
	p->row = row;
	p->image = field(row, wb::OptionType::Image);
	if (p->image == nullptr)
		return nullptr;  /* nothing to choose: just the options */
	p->takes_folder = p->image->opt->type == wb::OptionType::ImageOrFolder;
	p->colour = field(row, wb::OptionType::Color);
	p->mode = field(row, wb::OptionType::Choice, "mode");
	p->every = field(row, wb::OptionType::Number, "every");
	row->page = page;
	row->page_refresh = [p] { wallpaper_refresh(p); };

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_margin_bottom(box, 6);

	GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_box_append(GTK_BOX(box), top);
	p->preview = gtk_drawing_area_new();
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(p->preview), 288);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(p->preview), 180);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(p->preview), draw_preview, p, nullptr);
	gtk_box_append(GTK_BOX(top), p->preview);

	GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_valign(side, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(top), side);
	GtkWidget *grid = section_grid(side);
	int r = 0;
	if (p->mode != nullptr) {
		grid_label(grid, r, (p->mode->opt->label + ":").c_str());
		/* The option's own list, shared: a drop-down takes the reference
		 * it's given, so it gets one of its own. */
		g_object_ref(p->mode->drop_model);
		p->mode_drop = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(p->mode->drop_model), nullptr));
		grid_control(grid, r++, GTK_WIDGET(p->mode_drop));
		g_signal_connect(p->mode_drop, "notify::selected", G_CALLBACK(on_mode_chosen), p);
	}
	if (p->colour != nullptr) {
		grid_label(grid, r, (p->colour->opt->label + ":").c_str());
		/* The option's own colour button and field: moved here. */
		GtkWidget *w = p->colour->widget;
		g_object_ref(w);
		if (GtkWidget *parent = gtk_widget_get_parent(w); parent && GTK_IS_GRID(parent)) {
			int col, at, wd, ht;
			gtk_grid_query_child(GTK_GRID(parent), w, &col, &at, &wd, &ht);
			if (GtkWidget *label = gtk_grid_get_child_at(GTK_GRID(parent), 0, at))
				gtk_grid_remove(GTK_GRID(parent), label);
			gtk_grid_remove(GTK_GRID(parent), w);
		}
		grid_control(grid, r++, w);
		g_object_unref(w);
	}
	if (p->takes_folder) {
		p->random = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
				_("A random one from the folder")));
		g_signal_connect(p->random, "toggled", G_CALLBACK(on_random_toggled), p);
		gtk_box_append(GTK_BOX(side), GTK_WIDGET(p->random));
		if (p->every != nullptr) {
			p->every_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
			gtk_widget_set_margin_start(p->every_box, 24);
			p->every_on = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Change every")));
			double lo = p->every->opt->min.value_or(1), hi = p->every->opt->max.value_or(1440);
			p->every_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(lo, hi, 1));
			gtk_spin_button_set_value(p->every_spin, std::max(lo, 60.0));
			gtk_box_append(GTK_BOX(p->every_box), GTK_WIDGET(p->every_on));
			gtk_box_append(GTK_BOX(p->every_box), GTK_WIDGET(p->every_spin));
			gtk_box_append(GTK_BOX(p->every_box), gtk_label_new(_("minutes")));
			g_signal_connect(p->every_on, "toggled", G_CALLBACK(on_every_changed), p);
			g_signal_connect(p->every_spin, "value-changed", G_CALLBACK(on_every_changed), p);
			gtk_box_append(GTK_BOX(side), p->every_box);
		}
	}

	GtkWidget *folders = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(box), folders);
	p->folder_model = gtk_string_list_new(nullptr);
	p->folder_drop = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(p->folder_model), nullptr));
	gtk_widget_set_hexpand(GTK_WIDGET(p->folder_drop), TRUE);
	g_signal_connect(p->folder_drop, "notify::selected", G_CALLBACK(on_folder_chosen), p);
	gtk_box_append(GTK_BOX(folders), GTK_WIDGET(p->folder_drop));
	GtkWidget *other = gtk_button_new_with_label(_("Other Folder…"));
	g_signal_connect(other, "clicked", G_CALLBACK(on_other_folder), p);
	gtk_box_append(GTK_BOX(folders), other);

	p->grid = GTK_FLOW_BOX(gtk_flow_box_new());
	gtk_flow_box_set_selection_mode(p->grid, GTK_SELECTION_SINGLE);
	gtk_flow_box_set_activate_on_single_click(p->grid, TRUE);
	gtk_flow_box_set_homogeneous(p->grid, TRUE);
	gtk_flow_box_set_min_children_per_line(p->grid, 2);
	gtk_flow_box_set_max_children_per_line(p->grid, 8);
	gtk_flow_box_set_row_spacing(p->grid, 6);
	gtk_flow_box_set_column_spacing(p->grid, 6);
	g_signal_connect(p->grid, "child-activated", G_CALLBACK(on_thumb_activated), p);
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
			GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 240);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 360);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(p->grid));
	gtk_box_append(GTK_BOX(box), scroll);
	p->grid_note = GTK_LABEL(gtk_label_new(""));
	gtk_label_set_xalign(p->grid_note, 0);
	gtk_widget_add_css_class(GTK_WIDGET(p->grid_note), "dim-label");
	gtk_widget_set_visible(GTK_WIDGET(p->grid_note), FALSE);
	gtk_box_append(GTK_BOX(box), GTK_WIDGET(p->grid_note));
	return box;
}

/* "ID=PATH" for tests: choose that wallpaper in the grid, as a click. */
bool wallpaper_test_choose(void *page, const std::string &what) {
	auto *p = static_cast<Page *>(page);
	if (what == "random=on" || what == "random=off") {
		if (p->random == nullptr)
			return false;
		gtk_check_button_set_active(p->random, what == "random=on");
		return true;
	}
	if (what.rfind("every=", 0) == 0) {
		if (p->every_on == nullptr)
			return false;
		gtk_check_button_set_active(p->every_on, TRUE);
		gtk_spin_button_set_value(p->every_spin, g_ascii_strtod(what.c_str() + 6, nullptr));
		return true;
	}
	if (what.rfind("mode=", 0) == 0) {
		if (p->mode_drop == nullptr)
			return false;
		for (size_t i = 0; i < p->mode->drop_values.size(); ++i)
			if (p->mode->drop_values[i] == what.substr(5))
				gtk_drop_down_set_selected(p->mode_drop, static_cast<guint>(i));
		return true;
	}
	std::string want = expand(what);
	for (GtkWidget *c = gtk_widget_get_first_child(GTK_WIDGET(p->grid)); c != nullptr;
			c = gtk_widget_get_next_sibling(c)) {
		auto *path = static_cast<const char *>(g_object_get_data(G_OBJECT(c), "wb-path"));
		if (path != nullptr && want == path) {
			g_signal_emit_by_name(p->grid, "child-activated", c);
			return true;
		}
	}
	return false;
}

}  // namespace wbconf
