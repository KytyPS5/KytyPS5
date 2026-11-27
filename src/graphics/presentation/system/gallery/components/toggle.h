#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_COMPONENTS_TOGGLE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_COMPONENTS_TOGGLE_H_

#include "graphics/presentation/system/element.h"

#include <cstdint>
#include <string_view>

namespace Libs::Graphics::SystemUi::Gallery {

/// What a toggle shows and how the pointer is acting on it. The caller keeps whether it is on and
/// flips it on a click, as the toggle only draws.
struct ToggleProps {
	// The words beside the switch, which take the room the switch leaves.
	std::string_view label;
	// Whether the switch is on, with its knob at the end and the accent fill.
	bool on = false;
	// How many times the switch has flipped, which keys the knob's slide, so each flip slides the
	// knob in again. Zero means it never flipped, so the knob doesn't slide.
	uint32_t flips = 0;
	// Tells toggles on one screen apart, as every knob is keyed under one name and keys are frame
	// wide, so it would otherwise share its slide with another that flipped as often. Give each
	// toggle shown its own.
	uint32_t key = 0;
	// The pointer's hover and press on the whole toggle, from last frame's rect.
	StateInput state;
};

/// A labelled switch, one target hovered and pressed as a whole, its track grey with a strong edge
/// when off and the accent when on. The knob slides in from the side it left each time the switch
/// flips, and with reduced motion appears in its place. It reads the current theme, so it needs an
/// open FrameScope.
[[nodiscard]] BoxElement Toggle(const ToggleProps& props);

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_COMPONENTS_TOGGLE_H_
