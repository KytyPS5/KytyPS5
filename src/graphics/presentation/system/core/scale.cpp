#include "graphics/presentation/system/core/scale.h"

#include <algorithm>
#include <cmath>

namespace Libs::Graphics::SystemUi {

namespace {

constexpr float PERCENT = 100.0f;

// `value` when it is finite, otherwise `fallback`. Sizes and options come from the window and
// from settings, and a single NaN would otherwise spread to every rectangle.
float FiniteOr(float value, float fallback) {
	return std::isfinite(value) ? value : fallback;
}

// An output length, with anything that is not a positive finite number treated as empty.
float OutputLength(float value) {
	return value > 0.0f && std::isfinite(value) ? value : 0.0f;
}

// The scale. The smaller axis ratio wins, so the UI fits both ways. Height then drives ultrawide
// screens and width drives narrow windows. An empty output falls back to the minimum scale.
float ComputeScale(const ScaleRules& rules, float width, float height, float user_scale) {
	const float user = std::clamp(FiniteOr(user_scale, ScaleOptions {}.user_scale),
	                              rules.min_user_scale, rules.max_user_scale);
	const float raw =
	    std::min(width / rules.reference_width, height / rules.reference_height) * user;
	return std::clamp(raw, rules.min_scale, rules.max_scale);
}

// The inset from each edge, rounded to whole pixels so hairlines along it stay sharp. It is a
// percentage of the screen, measured against the screen's height and against its width capped at
// the content aspect, so ultrawide sides don't get a huge margin. It is never less than the
// theme's minimum edge distance. On an output too small for that the area collapses to the
// screen's centre.
Rect ComputeSafeArea(const ScaleRules& rules, const Rect& screen, float scale, float percent) {
	const float clamped  = std::clamp(FiniteOr(percent, 0.0f), 0.0f, rules.max_safe_area_percent);
	const float fraction = clamped / PERCENT;
	const float edge     = rules.edge_min.value * scale;
	const float span_x   = std::min(screen.width, screen.height * rules.content_aspect);
	return screen.Inset(EdgeInsets(SnapPixel(std::max(edge, fraction * screen.height)),
	                               SnapPixel(std::max(edge, fraction * span_x))));
}

// The safe area, narrowed to the content aspect when it is wider and centred on whole pixels.
// Narrower areas are used as they are, so tall and 4:3 outputs keep their full height.
Rect ComputeContentFrame(const ScaleRules& rules, const Rect& area) {
	const float width = std::min(area.width, SnapPixel(area.height * rules.content_aspect));
	return {area.x + std::floor((area.width - width) / 2.0f), area.y, width, area.height};
}

} // namespace

ScaleContext::ScaleContext(const ScaleRules& rules, float width, float height, ScaleOptions options)
    : m_scale(ComputeScale(rules, OutputLength(width), OutputLength(height), options.user_scale)),
      m_screen {0.0f, 0.0f, OutputLength(width), OutputLength(height)},
      m_safe_area(ComputeSafeArea(rules, m_screen, m_scale, options.safe_area_percent)),
      m_content_frame(ComputeContentFrame(rules, m_safe_area)) {}

} // namespace Libs::Graphics::SystemUi
