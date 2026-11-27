#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_CONVERT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_CONVERT_H_

#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/geometry.h"
#include "imgui.h"

#include <utility>

// The one place the system UI's plain types meet ImGui's, so the rest of the framework stays free
// of ImGui headers and its packed colour layout.
namespace Libs::Graphics::SystemUi {

/// A colour packed the way ImGui expects. IM_COL32 follows the build's packing, so this stays
/// right even if IMGUI_USE_BGRA_PACKED_COLOR is ever defined.
[[nodiscard]] constexpr ImU32 ToImU32(Color color) {
	return IM_COL32(color.r, color.g, color.b, color.a);
}

/// A point or size as an ImGui vector.
[[nodiscard]] constexpr ImVec2 ToImVec2(Vec2 value) {
	return {value.x, value.y};
}

/// A rectangle's top-left and bottom-right corners, as ImGui's draw calls take them.
[[nodiscard]] constexpr std::pair<ImVec2, ImVec2> ToImRect(const Rect& rect) {
	const ImVec2 top_left {rect.x, rect.y};
	const ImVec2 bottom_right {rect.Right(), rect.Bottom()};
	return {top_left, bottom_right};
}

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_CONVERT_H_
