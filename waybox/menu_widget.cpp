/*
 * Interactive root/context menu widget: the wlroots/Cairo glue around the pure
 * menu model. Renders one Cairo panel per open level into the scene graph and
 * routes grabbed input to selection/submenu navigation.
 */
#include "waybox/stall.hpp"
#include "waybox/menu_widget.hpp"
#include "waybox/applications.hpp"
#include "waybox/app_menu.hpp"

#include "script.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <sys/wait.h>
#include <unistd.h>

#include <cairo.h>


#include "waybox/icon.hpp"
#include "waybox/icon_loader.hpp"
#include "waybox/render.hpp"
#include "waybox/server.h"
#include "waybox/session_lock.h"

namespace wb {

namespace {

/* Draw a small right-pointing triangle (the submenu arrow) centred in the
 * column reserved at the right of an item row. `w` is both the horizontal reach
 * and half the vertical extent, so the arrow stays visually balanced. */
void paint_arrow(cairo_t *cr, double x, double y, double h, double w,
		const Color &c) {
	double cy = y + h / 2.0;
	cairo_set_source_rgba(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0);
	cairo_move_to(cr, x, cy - w);
	cairo_line_to(cr, x + w, cy);
	cairo_line_to(cr, x, cy + w);
	cairo_close_path(cr);
	cairo_fill(cr);
}

}  // namespace

MenuWidget::MenuWidget(struct wb_server *server, const MenuFile &menus,
		const MenuStyle &style, const MenuBehavior &behavior)
	: server_(server), menus_(menus), style_(style), behavior_(behavior),
	  metrics_(menu_metrics(style)) {
	/* The previous menu's script entries go (its actions already ran). */
	script_menu_reset(server);
	/* Size rows from the font's real cell height (incl. ascenders/descenders),
	 * not the point size, so text is centred and descenders are never clipped.
	 * "Ag" exercises both extremes. Row height is computed dynamically from the
	 * measured font plus the style's compactness (item_pad_y); an explicit
	 * themed item_height, if larger, wins. */
	{
		StallScope timing("menu-font");
		text_height_ = measure_text("Ag", style_.item_text.font).height;
	}
	int comfortable = text_height_ + 2 * style_.item_pad_y;
	if (metrics_.item_height < comfortable)
		metrics_.item_height = comfortable;
	if (metrics_.pad_y < 2)
		metrics_.pad_y = 2;  /* always a little breathing room top/bottom */

	/* Horizontal text inset should scale with the font, but stay compact: a
	 * small fraction of the line height reads better than a large gap. */
	int comfortable_x = (text_height_ * 2) / 5;
	if (metrics_.pad_x < comfortable_x)
		metrics_.pad_x = comfortable_x;

	/* Icons are square, sized to the row's text height; the column also gets a
	 * little padding to the right of the icon. */
	icon_size_ = menu_icon_size(style_);
	icon_theme_ = detect_icon_theme();

	tree_ = wlr_scene_tree_create(&server_->scene->tree);
	hover_timer_ = wl_event_loop_add_timer(server_->wl_event_loop, [](void *data) {
		auto *self = static_cast<MenuWidget *>(data);
		size_t depth = self->hover_depth_;
		int item = self->hover_item_;
		self->hover_item_ = -1;
		/* Still resting on it? Then open it. */
		if (item >= 0 && depth < self->levels_.size() &&
				self->levels_[depth].hovered == item)
			self->open_submenu(depth, item);
		return 0;
	}, this);
	wlr_scene_node_raise_to_top(&tree_->node);
	session_lock_keep_on_top(server_);

	MenuItem loading;
	loading.kind = MenuItem::Kind::Separator;
	loading.label = "Loading…";
	loading_menu_.items.push_back(std::move(loading));
}

MenuWidget::~MenuWidget() {
	if (hover_timer_ != nullptr)
		wl_event_source_remove(hover_timer_);
	/* Stop running pipe-menu commands: nothing will read their output. */
	while (!pipe_jobs_.empty())
		stop_pipe(pipe_jobs_.begin()->second.get(), true);
	levels_.clear();  /* destroy canvases (scene buffers) before their tree */
	for (auto &[name, surf] : icon_cache_) {
		if (surf != nullptr)
			cairo_surface_destroy(surf);
	}
	if (tree_ != nullptr)
		wlr_scene_node_destroy(&tree_->node);
}

/* The icon column width for `menu`: icon size + a small gap when any item has
 * an icon that actually loads, else zero (so menus whose icons can't be
 * resolved are not left with an empty, over-wide left margin). */
int MenuWidget::menu_icon_column(const Menu *menu) {
	if (menu == nullptr || icon_size_ <= 0 || !behavior_.show_icons)
		return 0;
	for (const MenuItem &item : menu->items) {
		if (!item.icon.empty() && icon_surface(item.icon) != nullptr)
			return icon_size_ + 4;
	}
	return 0;
}


/* Resolve and load an icon once, caching the surface (or nullptr on miss, so we
 * never retry). PNG is loaded with Cairo; SVG with librsvg when built with it
 * (otherwise SVG candidates are skipped safely). */
cairo_surface_t *MenuWidget::icon_surface(const std::string &name) {
	if (name.empty())
		return nullptr;
	if (auto it = icon_cache_.find(name); it != icon_cache_.end())
		return it->second;
	/* From the server's cache, shared by every opening (this map holds this
	 * menu's own references). */
	cairo_surface_t *loaded = server_->icon_cache != nullptr
			? server_->icon_cache->get(name, icon_theme_, icon_size_)
			: load_themed_icon(name, icon_theme_, icon_size_);
	icon_cache_[name] = loaded;
	return loaded;
}

int MenuWidget::text_width(std::string_view label) const {
	return measure_text(label, style_.item_text.font).width;
}

Rect MenuWidget::output_bounds(int lx, int ly) const {
	struct wlr_output *output = wlr_output_layout_output_at(
			server_->output_layout, lx, ly);
	if (output == nullptr)
		output = wlr_output_layout_output_at(server_->output_layout, 0, 0);
	struct wlr_box box = {};
	if (output != nullptr)
		wlr_output_layout_get_box(server_->output_layout, output, &box);
	if (box.width == 0 || box.height == 0)
		return Rect{0, 0, 1920, 1080};  /* sane fallback for headless */
	return Rect{box.x, box.y, box.width, box.height};
}

float MenuWidget::output_scale(int lx, int ly) const {
	struct wlr_output *output = wlr_output_layout_output_at(
			server_->output_layout, lx, ly);
	return (output != nullptr && output->scale > 0) ? output->scale : 1.0f;
}

void MenuWidget::push_menu(const Menu *menu, const Rect &rect, int parent_item) {
	Level level;
	level.menu = menu;
	level.icon_col = menu_icon_column(menu);
	MenuMetrics m = metrics_;
	m.icon_column = level.icon_col;
	{
		StallScope timing("menu-layout");
		level.layout = layout_menu(*menu, m,
				[this](std::string_view s) { return text_width(s); });
	}
	level.rect = rect;
	level.parent_item = parent_item;
	{
		StallScope timing("menu-canvas");
		level.canvas = std::make_unique<SceneCanvas>(tree_, rect.width, rect.height,
				output_scale(rect.x, rect.y));
		level.canvas->set_position(rect.x, rect.y);
	}
	levels_.push_back(std::move(level));
	StallScope timing("menu-draw");
	render_level(levels_.back());
}

bool MenuWidget::open(std::string_view root_id, int lx, int ly) {
	return open(root_id, lx, ly, Placement{});
}

bool MenuWidget::open(std::string_view root_id, int lx, int ly, const Placement &placement) {
	const Menu *menu = with_labels(resolve_menu(root_id));
	if (menu == nullptr)
		return false;
	wlr_log(WLR_INFO, "wb-menu open %s", std::string(root_id).c_str());
	root_id_ = std::string(root_id);
	root_x_ = lx;
	root_y_ = ly;
	std::string pending;
	if (!menu->execute.empty()) {
		pending = menu->id;
		start_pipe(*menu);
		menu = &loading_menu_;
	}
	MenuMetrics m = metrics_;
	m.icon_column = menu_icon_column(menu);
	MenuLayout layout = layout_menu(*menu, m,
			[this](std::string_view s) { return text_width(s); });
	Rect rect;
	std::optional<PlacedAt> at;
	if (!placement.x.empty() && placement.area_w > 0)
		at = place_in_area(placement.x, placement.y, layout.width, layout.height,
				placement.area_x, placement.area_y, placement.area_w, placement.area_h);
	if (at) {
		rect = Rect{at->x, at->y, layout.width, layout.height};
		/* Its submenus kept to the monitor it's on. */
		root_x_ = at->x;
		root_y_ = at->y;
	} else {
		Rect bounds = output_bounds(lx, ly);
		rect = place_root_menu(lx, ly, layout.width, layout.height, bounds);
	}
	push_menu(menu, rect, -1);
	levels_.back().pending_id = pending;
	wlr_log(WLR_INFO, "wb-menu at %d,%d %dx%d", rect.x, rect.y, rect.width, rect.height);
	return true;
}

void MenuWidget::render_level(Level &level) {
	cairo_t *cr = level.canvas->cr();
	const Rect &r = level.rect;

	/* Border first (full panel), then the interior fill inset by the border. */
	const Border &b = style_.panel.border;
	if (b.width > 0)
		paint_rect(cr, 0, 0, r.width, r.height, b.color);
	paint_fill(cr, b.width, b.width, r.width - 2 * b.width,
			r.height - 2 * b.width, style_.panel.fill);

	for (size_t i = 0; i < level.menu->items.size() &&
			i < level.layout.item_rects.size(); ++i) {
		const MenuItem &item = level.menu->items[i];
		const Rect &ir = level.layout.item_rects[i];

		if (item.kind == MenuItem::Kind::Separator && item.label.empty()) {
			int sy = ir.y + ir.height / 2;
			paint_rect(cr, ir.x + metrics_.pad_x, sy,
					ir.width - 2 * metrics_.pad_x, 1, style_.separator);
			continue;
		}
		/* A labelled separator, a title (bold); a text line: read only. */
		if (item.kind == MenuItem::Kind::Separator || item.kind == MenuItem::Kind::Text) {
			const StateStyle &st = style_.item.for_state(WidgetState::Normal);
			FontSpec font = style_.item_text.font;
			if (item.kind == MenuItem::Kind::Separator)
				font.weight = 700;
			paint_text(cr, ir.x + metrics_.pad_x + level.icon_col,
					ir.y + (ir.height - text_height_) / 2, item.label, st.fg, font);
			continue;
		}

		bool active = static_cast<int>(i) == level.hovered;
		const StateStyle &st = style_.item.for_state(
				active ? WidgetState::Hover : WidgetState::Normal);
		if (active)
			paint_fill(cr, ir.x, ir.y, ir.width, ir.height, st.fill);

		/* Optional icon in the reserved column, vertically centred. */
		if (level.icon_col > 0 && !item.icon.empty()) {
			if (cairo_surface_t *icon = icon_surface(item.icon)) {
				int iw = cairo_image_surface_get_width(icon);
				int ih = cairo_image_surface_get_height(icon);
				if (iw > 0 && ih > 0) {
					double s = static_cast<double>(icon_size_) /
							std::max(iw, ih);
					double dx = ir.x + metrics_.pad_x;
					double dy = ir.y + (ir.height - ih * s) / 2.0;
					cairo_save(cr);
					cairo_translate(cr, dx, dy);
					cairo_scale(cr, s, s);
					cairo_set_source_surface(cr, icon, 0, 0);
					cairo_paint(cr);
					cairo_restore(cr);
				}
			}
		}

		int text_x = ir.x + metrics_.pad_x + level.icon_col;
		int text_y = ir.y + (ir.height - text_height_) / 2;
		paint_text(cr, text_x, text_y, item.label, st.fg,
				style_.item_text.font);

		if (item.kind == MenuItem::Kind::Submenu) {
			/* Right-aligned arrow, its right tip pad_x from the panel edge and
			 * vertically centred on the row (matching the text). */
			int arrow_w = 4;
			double ax = ir.x + ir.width - metrics_.pad_x - arrow_w;
			paint_arrow(cr, ax, ir.y, ir.height, arrow_w, st.fg);
		}
	}

	level.canvas->commit();
}

void MenuWidget::close_below(size_t keep) {
	while (levels_.size() > keep + 1) {
		wlr_log(WLR_INFO, "wb-menu close %s",
				levels_.back().menu ? levels_.back().menu->id.c_str() : "");
		levels_.pop_back();
	}
}

void MenuWidget::open_submenu(size_t level_index, int item_index) {
	Level &parent = levels_[level_index];
	const MenuItem &item = parent.menu->items[item_index];
	const Menu *submenu = with_labels(resolve_menu(item.submenu_id));
	if (submenu == nullptr)
		return;

	/* Already open for this same item? Nothing to do. */
	if (levels_.size() > level_index + 1 &&
			levels_[level_index + 1].parent_item == item_index)
		return;
	close_below(level_index);
	wlr_log(WLR_INFO, "wb-menu open %s", item.submenu_id.c_str());

	/* A pipe menu whose output isn't in yet: run it, show a placeholder. */
	std::string pending;
	if (!submenu->execute.empty()) {
		pending = submenu->id;
		start_pipe(*submenu);
		submenu = &loading_menu_;
	}

	MenuMetrics m = metrics_;
	m.icon_column = menu_icon_column(submenu);
	MenuLayout layout = layout_menu(*submenu, m,
			[this](std::string_view s) { return text_width(s); });
	const Rect &ir = parent.layout.item_rects[item_index];
	Rect bounds = output_bounds(parent.rect.x, parent.rect.y);
	Rect rect = place_submenu(parent.rect, parent.rect.y + ir.y,
			layout.width, layout.height, bounds, style_.panel.border.width);
	push_menu(submenu, rect, item_index);
	levels_.back().pending_id = pending;
}

namespace {

/* $XDG_CURRENT_DESKTOP's desktops (colon-separated). */
std::vector<std::string> current_desktops() {
	std::vector<std::string> out;
	const char *v = getenv("XDG_CURRENT_DESKTOP");
	std::string_view rest = v != nullptr ? v : "";
	while (!rest.empty()) {
		size_t colon = rest.find(':');
		if (colon != 0)
			out.emplace_back(rest.substr(0, colon));
		if (colon == std::string_view::npos)
			break;
		rest.remove_prefix(colon + 1);
	}
	return out;
}

std::string message_locale() {
	for (const char *var : {"LC_ALL", "LC_MESSAGES", "LANG"})
		if (const char *v = getenv(var); v != nullptr && v[0] != '\0')
			return v;
	return "";
}

}  // namespace

const Menu *MenuWidget::resolve_menu(std::string_view id) {
	/* Built already while this menu is open (as pipe menus: again the
	 * next time it opens), else a script's, else menu.xml's. */
	for (const auto &[built_id, file] : pipe_results_)
		if (const Menu *m = file->find(id))
			return m;
	std::string key(id);
	if (auto file = script_build_menu(server_, key)) {
		pipe_results_[key] = std::move(file);
		return find_menu(id);
	}
	const Menu *menu = find_menu(id);
	if (menu != nullptr && menu->type == "applications") {
		/* Kept by the server until an applications directory changes. */
		StallScope timing("applications-menu");
		if (server_->applications_menu == nullptr)
			server_->applications_menu = std::make_unique<ApplicationsMenu>();
		const char *terminal = getenv("TERMINAL");
		const MenuFile &apps = server_->applications_menu->get(menu->id, menu->label,
				application_dirs(getenv("XDG_DATA_HOME"), getenv("HOME"),
						getenv("XDG_DATA_DIRS")),
				locale_names(message_locale()), current_desktops(),
				terminal != nullptr && terminal[0] != '\0' ? terminal : "xdg-terminal-exec");
		pipe_results_[key] = std::make_unique<MenuFile>(apps);
		return find_menu(id);
	}
	return menu;
}

const Menu *MenuWidget::with_labels(const Menu *menu) {
	if (menu == nullptr)
		return nullptr;
	auto unlabelled = [](const MenuItem &item) {
		return item.kind == MenuItem::Kind::Submenu && item.label.empty();
	};
	if (std::none_of(menu->items.begin(), menu->items.end(), unlabelled))
		return menu;
	Menu copy = *menu;
	for (MenuItem &item : copy.items)
		if (unlabelled(item)) {
			item.label = script_menu_label(server_, item.submenu_id);
			wlr_log(WLR_INFO, "wb-menu label %s: %s", item.submenu_id.c_str(),
					item.label.c_str());
		}
	labelled_.push_back(std::move(copy));
	return &labelled_.back();
}

const Menu *MenuWidget::find_menu(std::string_view id) const {
	for (const auto &[pipe_id, file] : pipe_results_) {
		if (const Menu *m = file->find(id))
			return m;
	}
	return menus_.find(id);
}

void MenuWidget::start_pipe(const Menu &menu) {
	if (pipe_jobs_.count(menu.id) || pipe_results_.count(menu.id))
		return;
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0) {
		wlr_log(WLR_ERROR, "pipe menu %s: pipe: %s", menu.id.c_str(), strerror(errno));
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log(WLR_ERROR, "pipe menu %s: fork: %s", menu.id.c_str(), strerror(errno));
		close(fds[0]);
		close(fds[1]);
		return;
	}
	if (pid == 0) {
		/* As wb_spawn: don't inherit the compositor's blocked signals. */
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, nullptr);
		/* Own process group, so a timeout can stop the whole command
		 * (sh -c, the script, and anything it started), not just sh. */
		setpgid(0, 0);
		dup2(fds[1], STDOUT_FILENO);
		int devnull = ::open("/dev/null", O_RDONLY);
		if (devnull >= 0)
			dup2(devnull, STDIN_FILENO);
		execl("/bin/sh", "/bin/sh", "-c", menu.execute.c_str(), (char *) nullptr);
		_exit(127);
	}
	setpgid(pid, pid);  /* also from the parent, so there is no race */
	close(fds[1]);
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

	auto job = std::make_unique<PipeJob>();
	job->owner = this;
	job->id = menu.id;
	job->label = menu.label;
	job->pid = pid;
	job->fd = fds[0];
	job->read_source = wl_event_loop_add_fd(server_->wl_event_loop, job->fd,
			WL_EVENT_READABLE, &MenuWidget::on_pipe_readable, job.get());
	job->timer = wl_event_loop_add_timer(server_->wl_event_loop,
			&MenuWidget::on_pipe_timeout, job.get());
	/* A pipe menu that takes longer than this is treated as failed. */
	wl_event_source_timer_update(job->timer, 5000);
	pipe_jobs_[menu.id] = std::move(job);
}

int MenuWidget::on_pipe_readable(int fd, uint32_t mask, void *data) {
	auto *job = static_cast<PipeJob *>(data);
	/* Pipe menus are small; refuse runaway output. */
	constexpr size_t kMaxOutput = 1 << 20;
	char buf[8192];
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n > 0) {
			job->output.append(buf, static_cast<size_t>(n));
			if (job->output.size() > kMaxOutput) {
				wlr_log(WLR_ERROR, "pipe menu %s: output over %zu bytes",
						job->id.c_str(), kMaxOutput);
				job->owner->finish_pipe(job, false);
				return 0;
			}
			continue;
		}
		if (n == 0) {  /* EOF: the command closed its output */
			job->owner->finish_pipe(job, true);
			return 0;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;  /* more later */
		job->owner->finish_pipe(job, false);
		return 0;
	}
}

int MenuWidget::on_pipe_timeout(void *data) {
	auto *job = static_cast<PipeJob *>(data);
	wlr_log(WLR_ERROR, "pipe menu %s: no complete output after 5s", job->id.c_str());
	job->owner->finish_pipe(job, false);
	return 0;
}

void MenuWidget::stop_pipe(PipeJob *job, bool abandon) {
	if (job->read_source != nullptr)
		wl_event_source_remove(job->read_source);
	if (job->timer != nullptr)
		wl_event_source_remove(job->timer);
	if (job->fd >= 0)
		close(job->fd);
	if (job->pid > 0) {
		/* Abandoned (no complete output in time, or the menu closed): its
		 * whole process group stopped, the shell or not (it may have exited
		 * leaving a child that holds the pipe); done: the shell, if it's
		 * still running. Either way before the shell is reaped, so that
		 * the group's id is still its own. */
		if (abandon || waitpid(job->pid, nullptr, WNOHANG) == 0)
			kill(-job->pid, SIGKILL);
		while (waitpid(job->pid, nullptr, 0) < 0 && errno == EINTR) {
		}
	}
	pipe_jobs_.erase(job->id);  /* frees job */
}

void MenuWidget::finish_pipe(PipeJob *job, bool ok) {
	std::string id = job->id;
	auto file = std::make_unique<MenuFile>();
	if (ok)
		*file = parse_pipe_menu(job->output, id, job->label);
	if (file->find(id) == nullptr) {
		/* Failed, timed out or not a pipe menu document: say so in place. */
		wlr_log(WLR_ERROR, "pipe menu %s: no usable output", id.c_str());
		Menu failed;
		failed.id = id;
		failed.label = job->label;
		MenuItem note;
		note.kind = MenuItem::Kind::Separator;
		note.label = "(pipe menu failed)";
		failed.items.push_back(std::move(note));
		file = std::make_unique<MenuFile>();
		file->menus.push_back(std::move(failed));
	}
	stop_pipe(job, !ok);  /* not ok: timed out or failed reading */
	pipe_results_[id] = std::move(file);
	refresh_pending(id);
}

void MenuWidget::refresh_pending(const std::string &id) {
	for (size_t depth = 0; depth < levels_.size(); depth++) {
		if (levels_[depth].pending_id != id)
			continue;
		if (depth == 0) {
			/* The root itself was a pipe menu: reopen it in place. */
			levels_.clear();
			open(root_id_, root_x_, root_y_);
		} else {
			size_t parent = depth - 1;
			int item = levels_[depth].parent_item;
			close_below(parent);
			open_submenu(parent, item);
		}
		return;
	}
}

/* Open the submenu of the entry the pointer rests on: after the hover
 * delay, or at once without one. */
void MenuWidget::hover_submenu(size_t depth, int item) {
	bool open = levels_.size() > depth + 1 && levels_[depth + 1].parent_item == item;
	if (open || (hover_item_ == item && hover_depth_ == depth))
		return;  /* already open, or already waiting for it */
	if (behavior_.submenu_delay_ms <= 0 || hover_timer_ == nullptr) {
		open_submenu(depth, item);
		return;
	}
	hover_depth_ = depth;
	hover_item_ = item;
	wl_event_source_timer_update(hover_timer_, behavior_.submenu_delay_ms);
}

void MenuWidget::on_motion(double lx, double ly) {
	/* Topmost (deepest) level under the pointer wins. */
	for (size_t depth = levels_.size(); depth-- > 0;) {
		Level &level = levels_[depth];
		const Rect &r = level.rect;
		if (lx < r.x || lx >= r.x + r.width || ly < r.y || ly >= r.y + r.height)
			continue;

		int item = menu_item_at(*level.menu, level.layout,
				static_cast<int>(lx) - r.x, static_cast<int>(ly) - r.y);

		/* On another entry than the one whose submenu is open: drop the
		 * deeper menus. (Staying on that entry keeps its submenu open, rather
		 * than closing and rebuilding it on every motion.) */
		if (levels_.size() > depth + 1 && levels_[depth + 1].parent_item != item)
			close_below(depth);

		if (level.hovered != item) {
			level.hovered = item;
			render_level(level);
		}
		if (item >= 0 &&
				level.menu->items[item].kind == MenuItem::Kind::Submenu &&
				behavior_.submenu_open == SubmenuOpen::Hover)
			hover_submenu(depth, item);
		else if (hover_item_ >= 0 && depth <= hover_depth_)
			hover_item_ = -1;  /* moved off before the delay was up */
		return;
	}
	/* Pointer is over no panel: leave the menus open (a click outside
	 * dismisses), but clear the root's hover so nothing looks selected. */
}

bool MenuWidget::on_button(uint32_t button, bool pressed) {
	if (!pressed)
		return false;
	double lx = server_->cursor->cursor->x;
	double ly = server_->cursor->cursor->y;

	for (size_t depth = levels_.size(); depth-- > 0;) {
		Level &level = levels_[depth];
		const Rect &r = level.rect;
		if (lx < r.x || lx >= r.x + r.width || ly < r.y || ly >= r.y + r.height)
			continue;
		int item = menu_item_at(*level.menu, level.layout,
				static_cast<int>(lx) - r.x, static_cast<int>(ly) - r.y);
		if (item < 0)
			return false;  /* on padding/border: swallow, keep open */
		const MenuItem &mi = level.menu->items[item];
		if (mi.kind == MenuItem::Kind::Submenu) {
			open_submenu(depth, item);
			return false;
		}
		if (mi.kind == MenuItem::Kind::Entry) {
			pending_ = mi.actions;  /* caller runs these after destroying us */
			return true;  /* dismiss */
		}
		return false;
	}
	(void)button;
	return true;  /* clicked outside every panel: dismiss */
}

bool MenuWidget::on_key(xkb_keysym_t sym) {
	if (levels_.empty())
		return true;
	Level &level = levels_.back();
	const size_t depth = levels_.size() - 1;

	auto reselect = [&](int item) {
		if (level.hovered != item) {
			level.hovered = item;
			render_level(level);
		}
	};

	switch (sym) {
	case XKB_KEY_Escape:
		if (levels_.size() > 1) {
			levels_.pop_back();  /* close the deepest submenu */
			return false;
		}
		return true;  /* close the root: dismiss */

	case XKB_KEY_Up:
	case XKB_KEY_KP_Up:
		reselect(menu_step_selection(*level.menu, level.hovered, -1,
				behavior_.wrap));
		return false;

	case XKB_KEY_Down:
	case XKB_KEY_KP_Down:
		reselect(menu_step_selection(*level.menu, level.hovered, +1,
				behavior_.wrap));
		return false;

	case XKB_KEY_Left:
	case XKB_KEY_KP_Left:
		if (levels_.size() > 1)
			levels_.pop_back();  /* back to the parent menu */
		return false;

	case XKB_KEY_Right:
	case XKB_KEY_KP_Right:
		/* Open the highlighted submenu and step into it. */
		if (level.hovered >= 0 &&
				level.menu->items[level.hovered].kind ==
						MenuItem::Kind::Submenu) {
			open_submenu(depth, level.hovered);
			if (levels_.size() > depth + 1) {
				Level &child = levels_.back();
				child.hovered = menu_step_selection(*child.menu, -1, +1,
						behavior_.wrap);
				render_level(child);
			}
		}
		return false;

	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		if (level.hovered < 0)
			return false;
		switch (level.menu->items[level.hovered].kind) {
		case MenuItem::Kind::Submenu:
			open_submenu(depth, level.hovered);
			if (levels_.size() > depth + 1) {
				Level &child = levels_.back();
				child.hovered = menu_step_selection(*child.menu, -1, +1,
						behavior_.wrap);
				render_level(child);
			}
			return false;
		case MenuItem::Kind::Entry:
			pending_ = level.menu->items[level.hovered].actions;
			return true;  /* dismiss; caller runs the actions */
		case MenuItem::Kind::Separator:
		case MenuItem::Kind::Text:
			return false;
		}
		return false;

	default:
		return false;
	}
}

int menu_icon_size(const MenuStyle &style) {
	if (style.icon_column_width > 0)
		return style.icon_column_width;
	return measure_text("Ag", style.item_text.font).height + 2;
}

}  // namespace wb

void menu_preload_icons(struct wb_server *server) {
	if (server->config == nullptr || !server->icon_cache || !server->worker_sched ||
			!server->config->menu_behavior.show_icons)
		return;
	std::vector<std::string> names;
	for (const wb::Menu &menu : server->config->menu.menus) {
		if (!menu.execute.empty() || !menu.type.empty())
			continue;  /* their items come when they open */
		for (const wb::MenuItem &item : menu.items)
			if (!item.icon.empty())
				names.push_back(item.icon);
	}
	if (names.empty())
		return;
	wb::MenuStyle style = wb::menu_style_from_theme(server->config->theme);
	server->icon_cache->preload(names, wb::detect_icon_theme(), wb::menu_icon_size(style),
			*server->worker_sched, *server->main_sched, [](size_t n) {
				wlr_log(WLR_INFO, "wb-menu icons preloaded %zu", n);
			});
}

