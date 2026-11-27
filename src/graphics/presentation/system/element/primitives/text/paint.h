#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_TEXT_PAINT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_TEXT_PAINT_H_

#include "graphics/presentation/system/element/element.h"
#include "graphics/presentation/system/element/frame/frameHooks.h"

// The Text element's painting. Internal to the text primitive, so components don't include it.
namespace Libs::Graphics::SystemUi {

/// Draws a text with the lines laying it out made this frame, centred vertically where it was
/// arranged and faded by `state`, as the text's paint hook. Text taller than its rectangle starts
/// at the top and spills below. A text not laid out this frame draws nothing.
void PaintText(const FrameBuilder& frame, AnyElement text, ImDrawList& list,
               const PaintState& state);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_TEXT_PAINT_H_
