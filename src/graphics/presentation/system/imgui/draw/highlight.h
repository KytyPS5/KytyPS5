#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_HIGHLIGHT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_HIGHLIGHT_H_

#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/geometry.h"

struct ImDrawList;

// How focus and selection are shown. A controller drives the UI, so the focused element must be
// obvious on any game frame, light or dark.
namespace Libs::Graphics::SystemUi {

/// How a focus ring looks. Lengths are design units, converted when it is drawn.
struct FocusRingStyle {
	Color ring;
	Color keyline;
	Color glow;
	float ring_width    = 0.0f;
	float keyline_width = 0.0f;
	// How far the glow reaches past the ring, and how strong it is next to it, from 0 to 1.
	float glow_radius = 0.0f;
	float glow_peak   = 0.0f;
};

/// How a selection looks. Lengths are design units, converted when it is drawn.
struct SelectionStyle {
	Color tint;
	Color bar;
	float bar_width = 0.0f;
};

/// The focus ring around `rect`, whose corners are rounded by `radius` design units as a box's
/// are. A thin dark keyline against the element, then a light ring, read on light and dark frames
/// alike, and a soft glow touching the ring draws the eye. It starts `cover` design units
/// inside the element's edge, so it lies over a border that wide and the ring takes the border's
/// place, and with no cover it starts at the edge and covers nothing of the element. `opacity`
/// fades all of it. It needs an open ContextScope.
void DrawFocusRing(ImDrawList& list, const Rect& rect, float radius, const FocusRingStyle& style,
                   float opacity = 1.0f, float cover = 0.0f);

/// The selected look of a row or card filling `rect`, whose corners are rounded by `radius`
/// design units as a box's are. A faint tint covers it and a rounded bar marks its left edge
/// between the rounded corners, so selection still shows when focus is elsewhere. An element
/// rounded so fully that no straight edge is left gets no bar. `opacity` fades both. It needs an
/// open ContextScope.
void DrawSelection(ImDrawList& list, const Rect& rect, float radius, const SelectionStyle& style,
                   float opacity = 1.0f);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_HIGHLIGHT_H_
