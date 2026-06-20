/*
 * Shared widget-styling primitives (pure helpers).
 */
#include "waybox/widget.hpp"

namespace wb {

const StateStyle &ControlStyle::for_state(WidgetState state) const {
	switch (state) {
	case WidgetState::Hover:
		return hover;
	case WidgetState::Pressed:
		return pressed;
	case WidgetState::Disabled:
		return disabled;
	case WidgetState::Normal:
		break;
	}
	return normal;
}

Paint paint_from_texture(const Texture &tex) {
	if (tex.type == TextureType::Gradient)
		return GradientPaint{tex.color, tex.color_to, GradientDir::Vertical};
	return SolidPaint{tex.color};
}

std::optional<Color> solid_color(const Paint &paint) {
	if (const auto *s = std::get_if<SolidPaint>(&paint))
		return s->color;
	return std::nullopt;
}

Paint effective_paint(const Paint &paint, const RenderCaps &caps) {
	/* Solid and gradient fills are always safe (the CPU rasteriser handles
	 * them); image and shader fills degrade to their fallback colour when the
	 * backend or the user's settings can't run them — never failing. */
	if (const auto *img = std::get_if<ImagePaint>(&paint)) {
		if (caps.images)
			return paint;
		return SolidPaint{img->fallback};
	}
	if (const auto *sh = std::get_if<ShaderPaint>(&paint)) {
		if (caps.effects_enabled && caps.gpu_shaders)
			return paint;
		return SolidPaint{sh->fallback};
	}
	return paint;
}

Effects effective_effects(const Effects &effects, const RenderCaps &caps) {
	if (caps.effects_enabled && caps.gpu_shaders)
		return effects;
	return Effects{};  /* no shadow, no blur when effects are off/unsupported */
}

}  // namespace wb
