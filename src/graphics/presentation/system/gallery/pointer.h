#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_POINTER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_POINTER_H_

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/core/pointer.h"
#include "graphics/presentation/system/element.h"

namespace Libs::Graphics::SystemUi::Gallery {

/// The mouse as the gallery reads it each frame, in the display's units, which the system UI lays
/// out in.
struct PointerInput {
	// Where the mouse is, from the window's top left.
	Vec2 position;
	// Whether the left button is held, which presses what it is over.
	bool down = false;
	// Whether the left button came up this frame, which clicks what it is over.
	bool released = false;
};

/// The mouse as ImGui read it this frame. It needs an ImGui context with this frame's input.
[[nodiscard]] PointerInput PointerNow();

/// The pointer and wheel as ImGui read them this frame, for the system UI's scrolling boxes. Shift
/// turns the vertical wheel into the horizontal one, as desktop apps do, so a row scrolls without a
/// horizontal wheel. Without a mouse there is no pointer position.
[[nodiscard]] PointerContext WheelNow();

/// How `pointer` acts on a control that was at `rect` last frame, hovered while over it and pressed
/// while held there. The system UI has no pointer input of its own yet, so the gallery tests the
/// mouse against last frame's rects itself.
[[nodiscard]] StateInput PointerState(const PointerInput& pointer, const Rect& rect);

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_POINTER_H_
