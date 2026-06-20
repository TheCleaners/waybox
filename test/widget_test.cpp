#include "wb_test.hpp"

#include <variant>

#include "waybox/widget.hpp"

using wb::Color;
using wb::ControlStyle;
using wb::StateStyle;
using wb::WidgetState;

WB_TEST(insets_sum_helpers) {
	wb::Insets in{2, 4, 6, 8};  /* top, right, bottom, left */
	WB_CHECK(in.horizontal() == 12);
	WB_CHECK(in.vertical() == 8);
}

WB_TEST(control_for_state_selects_and_falls_back) {
	ControlStyle c;
	c.normal.fg = Color{1, 1, 1, 255};
	c.hover.fg = Color{2, 2, 2, 255};
	c.pressed.fg = Color{3, 3, 3, 255};
	c.disabled.fg = Color{4, 4, 4, 255};

	WB_CHECK(c.for_state(WidgetState::Normal).fg == (Color{1, 1, 1, 255}));
	WB_CHECK(c.for_state(WidgetState::Hover).fg == (Color{2, 2, 2, 255}));
	WB_CHECK(c.for_state(WidgetState::Pressed).fg == (Color{3, 3, 3, 255}));
	WB_CHECK(c.for_state(WidgetState::Disabled).fg == (Color{4, 4, 4, 255}));
}

WB_TEST(paint_from_texture_maps_solid_and_gradient) {
	wb::Texture solid;
	solid.type = wb::TextureType::Solid;
	solid.color = Color{10, 20, 30, 255};
	WB_CHECK(wb::solid_color(wb::paint_from_texture(solid)) == (Color{10, 20, 30, 255}));

	wb::Texture grad;
	grad.type = wb::TextureType::Gradient;
	grad.color = Color{1, 2, 3, 255};
	grad.color_to = Color{4, 5, 6, 255};
	wb::Paint p = wb::paint_from_texture(grad);
	WB_CHECK(!wb::solid_color(p).has_value());  /* not a flat fill */
	WB_CHECK(std::holds_alternative<wb::GradientPaint>(p));
}

WB_TEST(effects_and_shader_degrade_safely_without_gpu) {
	/* a shader fill with a safe fallback colour */
	wb::ShaderPaint neon;
	neon.name = "neon-border";
	neon.animated = true;
	neon.fallback = Color{90, 0, 90, 255};
	wb::Paint p = neon;

	/* no opt-in / no GPU => falls back to the solid colour, never the shader */
	wb::RenderCaps off{};  /* effects_enabled=false, gpu_shaders=false */
	WB_CHECK(wb::solid_color(wb::effective_paint(p, off)) == (Color{90, 0, 90, 255}));

	/* opted-in on a capable backend => the shader is kept */
	wb::RenderCaps on{true, true, true};
	WB_CHECK(std::holds_alternative<wb::ShaderPaint>(wb::effective_paint(p, on)));

	/* effects requested but no GPU => stripped */
	wb::Effects fx;
	fx.shadow.enabled = true;
	fx.backdrop_blur = 8;
	wb::RenderCaps no_gpu{true, false, true};
	WB_CHECK(!wb::effective_effects(fx, no_gpu).shadow.enabled);
	WB_CHECK(wb::effective_effects(fx, no_gpu).backdrop_blur == 0);
	WB_CHECK(wb::effective_effects(fx, on).shadow.enabled);
}

WB_TEST(image_paint_degrades_without_image_support) {
	wb::ImagePaint img;
	img.path = "/x.png";
	img.fallback = Color{7, 7, 7, 255};
	wb::RenderCaps no_img{false, false, false};
	WB_CHECK(wb::solid_color(wb::effective_paint(wb::Paint{img}, no_img)) ==
			(Color{7, 7, 7, 255}));
}
