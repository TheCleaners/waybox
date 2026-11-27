#include "waybox/about_card.hpp"

#include <algorithm>
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
	render();
	wlr_log(WLR_INFO, "wb-about open %s", info_.version.c_str());
}

AboutCard::~AboutCard() {
	wlr_log(WLR_INFO, "%s", "wb-about close");
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
	if (logo_ != nullptr) {
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

}  // namespace wb
