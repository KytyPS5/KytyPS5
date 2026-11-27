#include "graphics/presentation/system/gallery/pointer.h"

#include "imgui.h"

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

// A state's full weight, for a pointer that is over or pressing a control.
constexpr float FULL_WEIGHT = 1.0f;

} // namespace

PointerInput PointerNow() {
	const ImGuiIO& io = ImGui::GetIO();
	return {.position = {io.MousePos.x, io.MousePos.y},
	        .down     = ImGui::IsMouseDown(ImGuiMouseButton_Left),
	        .released = ImGui::IsMouseReleased(ImGuiMouseButton_Left)};
}

PointerContext WheelNow() {
	const ImGuiIO& io    = ImGui::GetIO();
	Vec2           wheel = {io.MouseWheelH, io.MouseWheel};
	if (io.KeyShift && wheel.x == 0.0f) {
		wheel = {wheel.y, 0.0f};
	}
	PointerContext pointer {.wheel = wheel};
	if (ImGui::IsMousePosValid()) {
		pointer.position = Vec2 {io.MousePos.x, io.MousePos.y};
	}
	return pointer;
}

StateInput PointerState(const PointerInput& pointer, const Rect& rect) {
	const bool over = rect.Contains(pointer.position);
	return {.hover   = over ? FULL_WEIGHT : 0.0f,
	        .pressed = over && pointer.down ? FULL_WEIGHT : 0.0f};
}

} // namespace Libs::Graphics::SystemUi::Gallery
