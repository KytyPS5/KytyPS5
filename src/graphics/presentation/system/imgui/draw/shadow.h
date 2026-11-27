#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_SHADOW_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_SHADOW_H_

#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/geometry.h"

struct ImDrawList;

namespace Libs::Graphics::SystemUi {

/// How a shadow looks. Lengths are design units, converted when it is drawn.
struct ShadowStyle {
	Color color;
	// How far the shadow spreads past the element on every side, and how far right and down it is
	// moved.
	float blur     = 0.0f;
	float offset_x = 0.0f;
	float offset_y = 0.0f;
	// How strong it is under the element's edge, as a fraction of the colour's own alpha.
	float peak_alpha = 0.0f;
};

/// A soft shadow under `rect`, whose corners are rounded by `radius` design units as a box's
/// are, moved by the style's offsets. It is strongest under the element's edge and fades
/// smoothly to nothing at the blur's outer edge, and it is filled under the element too, so draw
/// it before the element. It is one mesh with its alpha set at each vertex, so it shows no steps
/// and costs one layer of overdraw. No blur, no strength, a clear colour or an offset or blur that
/// isn't finite draws nothing, and `opacity` fades all of it. It needs an open ContextScope.
void DrawShadow(ImDrawList& list, const Rect& rect, float radius, const ShadowStyle& style,
                float opacity = 1.0f);

/// How a glow looks. Its radius is design units, converted when it is drawn.
struct GlowStyle {
	Color color;
	// How far the glow reaches past the element on every side.
	float radius = 0.0f;
	// How strong it is at the element's edge, as a fraction of the colour's own alpha.
	float peak_alpha = 0.0f;
};

/// A soft glow around `rect`, whose corners are rounded by `radius` design units as a box's are.
/// It is strongest at the element's edge and fades smoothly to nothing `style.radius` outside it.
/// It is hollow, leaving the element itself uncovered, so a translucent fill in front isn't tinted
/// by it. No reach, no strength, a clear colour or a reach that isn't finite draws nothing, and
/// `opacity` fades all of it. It needs an open ContextScope.
void DrawGlow(ImDrawList& list, const Rect& rect, float radius, const GlowStyle& style,
              float opacity = 1.0f);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_SHADOW_H_
