#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_BOX_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_BOX_H_

#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/core/gradient.h"

#include <optional>

struct ImDrawList;

namespace Libs::Graphics::SystemUi {

/// How a box looks. Lengths are design units. A transparent border or a zero border width
/// draws no border.
struct DrawBoxStyle {
	Color fill;
	float radius = 0.0f;
	Color border;
	float border_width = 0.0f;
	// Drawn in place of the fill colour when set.
	std::optional<LinearGradient> fill_gradient;
	// Drawn in place of the border colour when set. It spans the whole box, as the fill's does,
	// so a fill and a border with the same gradient line up.
	std::optional<LinearGradient> border_gradient;
};

/// Draws a box filling `rect` at the current scale, its fill and then its border just inside its
/// edge, so the border never makes the box bigger. A gradient follows the box's rounded outline
/// and its smoothed edges as a plain colour does. `opacity` fades all of it with its element. It
/// needs an open ContextScope.
void DrawBox(ImDrawList& list, const Rect& rect, const DrawBoxStyle& style, float opacity = 1.0f);

/// The corner radius in output pixels that DrawBox gives `rect` for `radius` design units at
/// the current scale, never more than half its shorter side. Shared so what is drawn around or over
/// a box rounds its corners exactly as the box does. It needs an open ContextScope.
[[nodiscard]] float BoxRounding(const Rect& rect, float radius);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_BOX_H_
