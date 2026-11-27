#include "waybox/about_card.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <variant>
#include <string>
#include <utility>
#include <vector>

#include "waybox/icon_loader.hpp"
#include "waybox/render.hpp"
#include "waybox/server.h"
#include "waybox/session_lock.h"

#ifndef WB_ICON_PATH
#define WB_ICON_PATH ""
#endif

namespace wb {

namespace {

constexpr int kLogo = 72;   /* the icon's edge, in layout pixels */
constexpr int kPad = 24;    /* around everything */
constexpr int kGap = 16;    /* between a row's name and its value */

Color mix(Color a, Color b, double t) {
	auto m = [t](uint8_t x, uint8_t y) {
		return static_cast<uint8_t>(x + (y - x) * t);
	};
	return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

/* A paint's main color: a solid's, a gradient's middle, else gray. */
Color color_of(const Paint &p) {
	if (const auto *s = std::get_if<SolidPaint>(&p))
		return s->color;
	if (const auto *g = std::get_if<GradientPaint>(&p))
		return mix(g->from, g->to, 0.5);
	return {128, 128, 128, 255};
}

/* How light a color is, 0..1. */
double lightness(Color c) {
	return (0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b) / 255.0;
}

/* The theme's text color, or black or white where it barely stands out
 * from the panel (some themes' OSD text is a faint gray on light gray). */
Color readable(Color fg, Color bg) {
	if (std::abs(lightness(fg) - lightness(bg)) >= 0.6)
		return fg;
	return lightness(bg) > 0.5 ? Color{20, 20, 20, 255} : Color{240, 240, 240, 255};
}

std::string short_address() {
	std::string u(kProjectUrl);
	size_t p = u.find("://");
	return p == std::string::npos ? u : u.substr(p + 3);
}

std::vector<std::pair<std::string, std::string>> rows(const AboutInfo &i) {
	return {{"wlroots", i.wlroots}, {"Renderer", i.renderer},
			{"X11 programs", i.xwayland ? "on (Xwayland)" : "off"},
			{"Monitors", std::to_string(i.monitors)}};
}

constexpr int kConfettiMargin = 26;  /* around the logo, for the confetti */

/* A rounded rectangle, in the logo's 48-unit coordinates. */
void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

/* data/waybox.svg, drawn: the window (its titlebar, its body), and its
 * mark ("wb", three rounded shapes with their counters cut out) turned
 * by `angle` about its middle, kept inside the window's body. In 48
 * units; the caller scales. */
void draw_logo(cairo_t *cr, double angle) {
	cairo_pattern_t *title = cairo_pattern_create_linear(0, 0, 0, 10);
	cairo_pattern_add_color_stop_rgb(title, 0, 0x58 / 255.0, 0x9b / 255.0, 0xda / 255.0);
	cairo_pattern_add_color_stop_rgb(title, 1, 0x3c / 255.0, 0x7c / 255.0, 0xb7 / 255.0);
	cairo_rectangle(cr, 0, 0, 48, 10);
	cairo_set_source(cr, title);
	cairo_fill(cr);
	cairo_pattern_destroy(title);
	cairo_rectangle(cr, 0, 10, 48, 38);
	cairo_set_source_rgb(cr, 0xea / 255.0, 0xea / 255.0, 0xea / 255.0);
	cairo_fill(cr);

	cairo_save(cr);
	cairo_rectangle(cr, 0, 10, 48, 38);
	cairo_clip(cr);
	cairo_push_group(cr);
	const double cx = 23.9, cy = 29.6;  /* the mark's middle */
	cairo_translate(cr, cx, cy);
	cairo_rotate(cr, angle);
	cairo_translate(cr, -cx, -cy);
	cairo_pattern_t *mark = cairo_pattern_create_linear(0, 19.3, 0, 39.9);
	cairo_pattern_add_color_stop_rgb(mark, 0, 0x4a / 255.0, 0x8c / 255.0, 0xcc / 255.0);
	cairo_pattern_add_color_stop_rgb(mark, 1, 0x2f / 255.0, 0x6a / 255.0, 0xa3 / 255.0);
	rounded(cr, 7.1, 25.7, 17.7, 14.2, 4.5);
	rounded(cr, 26.3, 19.3, 5.1, 13.4, 2.5);
	rounded(cr, 26.3, 25.7, 14.4, 14.2, 4.8);
	cairo_set_source(cr, mark);
	cairo_fill(cr);
	cairo_pattern_destroy(mark);
	/* The counters: cut out. */
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	rounded(cr, 11.5, 24.2, 3.1, 10.7, 1.5);
	rounded(cr, 17.4, 24.2, 3.1, 10.7, 1.5);
	rounded(cr, 32.7, 29.1, 3.6, 7.3, 1.8);
	cairo_fill(cr);
	cairo_pop_group_to_source(cr);
	cairo_paint(cr);
	cairo_restore(cr);
}

/* New Year's Eve or Day, here (WAYBOX_TEST_DATE=MM-DD for a test). */
bool new_year() {
	int month = 0, day = 0;
	if (const char *t = getenv("WAYBOX_TEST_DATE"); t != nullptr && *t)
		sscanf(t, "%d-%d", &month, &day);
	else {
		time_t now = time(nullptr);
		struct tm tm {};
		localtime_r(&now, &tm);
		month = tm.tm_mon + 1;
		day = tm.tm_mday;
	}
	return (month == 12 && day == 31) || (month == 1 && day == 1);
}

}  // namespace

AboutCard::AboutCard(struct wb_server *server, const SwitcherStyle &style, AboutInfo info)
	: server_(server), style_(style), info_(std::move(info)) {
	/* The installed icon, else the theme's "waybox". */
	logo_ = load_themed_icon(WB_ICON_PATH, "", kLogo * 2);
	if (logo_ == nullptr)
		logo_ = load_themed_icon("waybox", detect_icon_theme(), kLogo * 2);

	/* The monitor under the pointer. */
	int cx = static_cast<int>(server_->cursor->cursor->x);
	int cy = static_cast<int>(server_->cursor->cursor->y);
	struct wlr_output *output = wlr_output_layout_output_at(server_->output_layout, cx, cy);
	struct wlr_box b = {0, 0, 1920, 1080};
	if (output != nullptr)
		wlr_output_layout_get_box(server_->output_layout, output, &b);

	/* Its size: the widest line, the rows' two columns. */
	const FontSpec &font = style_.item_text.font;
	FontSpec title = font;
	title.size_pt = font.size_pt * 17 / 10;
	title.weight = 700;
	int line_h = measure_text("Ag", font).height;
	int names_w = 0, values_w = 0;
	for (const auto &[k, v] : rows(info_)) {
		names_w = std::max(names_w, measure_text(k, font).width);
		values_w = std::max(values_w, measure_text(v, font).width);
	}
	int width = std::max({2 * std::max(names_w, values_w) + kGap,
			measure_text(info_.version, font).width,
			measure_text(short_address(), font).width,
			measure_text("Click or press a key to close", font).width,
			measure_text("Waybox", title).width, 240});
	int height = kLogo + 12 + measure_text("Waybox", title).height + 4 + line_h + 16 +
			static_cast<int>(rows(info_).size()) * (line_h + 4) + 16 + line_h + 12 + line_h;
	rect_.width = width + 2 * kPad;
	rect_.height = height + 2 * kPad;
	rect_.x = b.x + (b.width - rect_.width) / 2;
	rect_.y = b.y + (b.height - rect_.height) / 2;

	tree_ = wlr_scene_tree_create(&server_->scene->tree);
	wlr_scene_node_raise_to_top(&tree_->node);
	session_lock_keep_on_top(server_);
	float scale = output != nullptr && output->scale > 0 ? output->scale : 1.0f;
	canvas_ = std::make_unique<SceneCanvas>(tree_, rect_.width, rect_.height, scale);
	canvas_->set_position(rect_.x, rect_.y);
	/* Moving: its mark turning where the renderer is a GPU (a frame of
	 * this is a few hundredths of a millisecond; the GPU puts it on
	 * screen), confetti on New Year's Eve and Day (still, on software). */
	spin_ = info_.renderer.rfind("GPU", 0) == 0 || getenv("WAYBOX_TEST_SPIN") != nullptr;
	confetti_ = new_year();
	render();
	if (spin_ || confetti_) {
		/* Room above and beside it; below, only a little (the title). */
		int side = kLogo + 2 * kConfettiMargin;
		logo_canvas_ = std::make_unique<SceneCanvas>(tree_, side, kLogo + kConfettiMargin + 6,
				scale);
		logo_canvas_->set_position(rect_.x + (rect_.width - side) / 2,
				rect_.y + kPad - kConfettiMargin);
		render_logo();
		if (spin_) {
			tick_ = wl_event_loop_add_timer(server_->wl_event_loop, [](void *data) {
				auto *card = static_cast<AboutCard *>(data);
				card->t_ += 1.0 / 30;
				card->render_logo();
				wl_event_source_timer_update(card->tick_, 33);
				return 0;
			}, this);
			wl_event_source_timer_update(tick_, 33);
		}
	}
	wlr_log(WLR_INFO, "wb-about open %s%s%s", info_.version.c_str(), spin_ ? " spin" : "",
			confetti_ ? " confetti" : "");
}

AboutCard::~AboutCard() {
	wlr_log(WLR_INFO, "%s", "wb-about close");
	if (tick_ != nullptr)
		wl_event_source_remove(tick_);
	logo_canvas_.reset();
	canvas_.reset();
	if (tree_ != nullptr)
		wlr_scene_node_destroy(&tree_->node);
	if (logo_ != nullptr)
		cairo_surface_destroy(logo_);
}

bool AboutCard::on_link(double x, double y) const {
	double lx = x - rect_.x, ly = y - rect_.y;
	return lx >= link_.x && lx < link_.x + link_.width && ly >= link_.y &&
			ly < link_.y + link_.height;
}

void AboutCard::render() {
	if (canvas_ == nullptr)
		return;
	cairo_t *cr = canvas_->cr();
	paint_panel(cr, rect_.width, rect_.height, style_.panel);

	const FontSpec &font = style_.item_text.font;
	FontSpec title = font;
	title.size_pt = font.size_pt * 17 / 10;
	title.weight = 700;
	const Color bg = color_of(style_.panel.fill);
	const Color fg = readable(style_.item.for_state(WidgetState::Normal).fg, bg);
	const Color strong = lightness(bg) > 0.5 ? Color{15, 15, 15, 255} : Color{245, 245, 245, 255};
	const Color dim = mix(fg, bg, 0.45);
	const Color faint = mix(fg, bg, 0.6);
	const Color accent = color_of(style_.item.for_state(WidgetState::Hover).fill);
	int line_h = measure_text("Ag", font).height;
	auto centered = [&](const std::string &t, int y, const Color &c, const FontSpec &f) {
		int w = measure_text(t, f).width;
		paint_text(cr, (rect_.width - w) / 2.0, y, t, c, f);
		return w;
	};

	int y = kPad;
	if (logo_ != nullptr && !(spin_ || confetti_)) {
		int iw = cairo_image_surface_get_width(logo_);
		int ih = cairo_image_surface_get_height(logo_);
		if (iw > 0 && ih > 0) {
			double s = static_cast<double>(kLogo) / std::max(iw, ih);
			cairo_save(cr);
			cairo_translate(cr, (rect_.width - iw * s) / 2.0, y);
			cairo_scale(cr, s, s);
			cairo_set_source_surface(cr, logo_, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
			cairo_restore(cr);
		}
	}
	y += kLogo + 12;
	centered("Waybox", y, strong, title);
	y += measure_text("Waybox", title).height + 4;
	centered(info_.version, y, dim, font);
	y += line_h + 16;
	/* Name | value, around the middle. */
	for (const auto &[k, v] : rows(info_)) {
		int kw = measure_text(k, font).width;
		paint_text(cr, rect_.width / 2.0 - kGap / 2.0 - kw, y, k, dim, font);
		paint_text(cr, rect_.width / 2.0 + kGap / 2.0, y, v, fg, font);
		y += line_h + 4;
	}
	y += 16;
	/* The address: the theme's highlight, if it stands out from the panel,
	 * else the text color; underlined. */
	bool distinct = std::abs(accent.r - bg.r) + std::abs(accent.g - bg.g) +
			std::abs(accent.b - bg.b) > 120;
	Color link = distinct ? accent : fg;
	int lw = centered(short_address(), y, link, font);
	link_ = {(rect_.width - lw) / 2, y, lw, line_h};
	paint_rect(cr, link_.x, y + line_h - 1, lw, 1, link);
	y += line_h + 12;
	centered("Click or press a key to close", y, faint, font);

	canvas_->commit();
	wlr_scene_buffer_set_opacity(canvas_->node(),
			static_cast<float>(std::clamp(style_.panel.opacity, 0.1, 1.0)));
}

/* The logo's own canvas: the logo (its mark turned, when it spins: once
 * every 6 s, eased), confetti falling around it on New Year's. */
void AboutCard::render_logo() {
	if (logo_canvas_ == nullptr)
		return;
	cairo_t *cr = logo_canvas_->cr();
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_restore(cr);
	double angle = spin_ ? std::fmod(t_, 6.0) / 6.0 * 2 * M_PI : 0;
	cairo_save(cr);
	cairo_translate(cr, kConfettiMargin, kConfettiMargin);
	cairo_scale(cr, kLogo / 48.0, kLogo / 48.0);
	draw_logo(cr, angle);
	cairo_restore(cr);
	if (confetti_) {
		/* Pieces with fixed seeds: each its column, speed, sway, colour,
		 * tumble; falling and wrapping round (still where nothing moves). */
		static const double palette[][3] = {{0.95, 0.30, 0.35}, {1.0, 0.78, 0.20},
				{0.30, 0.75, 0.45}, {0.35, 0.55, 0.95}, {0.80, 0.45, 0.90}, {1.0, 1.0, 1.0}};
		const double side = kLogo + 2 * kConfettiMargin;
		const double tall = kLogo + kConfettiMargin + 6;
		unsigned seed = 12345;
		auto rnd = [&seed]() {
			seed = seed * 1103515245u + 12345u;
			return ((seed >> 8) & 0xffff) / 65535.0;
		};
		for (int i = 0; i < 30; ++i) {
			double x0 = rnd() * side, y0 = rnd() * tall, speed = 14 + rnd() * 22;
			double sway = 3 + rnd() * 5, phase = rnd() * 6.28, spin = (rnd() - 0.5) * 6;
			const double *c = palette[i % 6];
			double y = std::fmod(y0 + speed * t_, tall + 8) - 4;
			double x = x0 + sway * std::sin(phase + t_ * 2);
			cairo_save(cr);
			cairo_translate(cr, x, y);
			cairo_rotate(cr, phase + spin * t_);
			cairo_rectangle(cr, -1.5, -3, 3, 6);
			cairo_set_source_rgba(cr, c[0], c[1], c[2], 0.9);
			cairo_fill(cr);
			cairo_restore(cr);
		}
	}
	logo_canvas_->commit();
}

}  // namespace wb
