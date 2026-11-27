#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_EDGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_EDGE_H_

#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/geometry.h"

struct ImDrawList;

namespace Libs::Graphics::SystemUi {

/// Fills the band `width` design units tall along the top edge of a box filling `rect`, whose
/// corners are rounded by `radius` design units as DrawBox rounds them, in `color`. The band is
/// the box's own rounded shape cut to that strip, so it follows both corner curves exactly and
/// never pokes past the box's outline. Its width converts as a border's does, to whole pixels, and
/// never exceeds the box's height. No width, a clear colour or a width that isn't finite draws
/// nothing, and `opacity` fades it. Draw it over the box's fill and under its border. It needs an
/// open ContextScope.
void DrawEdgeBand(ImDrawList& list, const Rect& rect, float radius, float width, Color color,
                  float opacity = 1.0f);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_DRAW_EDGE_H_
