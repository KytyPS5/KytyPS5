#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_POINTER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_POINTER_H_

#include "graphics/presentation/system/core/geometry.h"

#include <optional>

namespace Libs::Graphics::SystemUi {

/// The pointer as whoever draws read it for this frame, in the display's units the system UI lays
/// out in. Without a position there is no pointer, such as with no mouse or while it is outside
/// the window. The wheel is in notches, as ImGui reports them, a positive y toward the start of a
/// column and a positive x toward the start of a row. A notch may be fractional, as trackpads and
/// smooth wheels report part of one.
struct PointerContext {
	std::optional<Vec2> position;
	Vec2                wheel;

	constexpr bool operator==(const PointerContext&) const = default;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_POINTER_H_
