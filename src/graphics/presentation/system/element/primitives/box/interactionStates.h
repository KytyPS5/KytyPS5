#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_INTERACTION_STATES_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_INTERACTION_STATES_H_

#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/theme.h"
#include "graphics/presentation/system/element/primitives/box/box.h"

#include <array>
#include <cstddef>

// The merging of a box's state styles into its look, for the box's paint hook. Internal to the box,
// so components don't include it.
namespace Libs::Graphics::SystemUi {

/// How many states a box responds to, and so the most overlays it can draw. It and InOrder in
/// interactionStates.cpp change with StateStyles.
inline constexpr size_t BOX_STATE_COUNT = 5;

/// One overlay a state draws over the box, faded by how strong the state is.
struct StateOverlay {
	Color color;
	float weight = 0.0f;
};

/// A box's look once its states are merged into its base style. Lengths are design units, as in
/// BoxStyles, and the radius is not yet clamped to the painted rectangle.
struct ResolvedStyle {
	Color fill;
	Color border;
	Dp    border_width;
	Dp    radius;
	// The shadow under the box, blended layer by layer by its states.
	Shadow shadow;
	// The factor the box and its subtree fade by.
	float opacity = 1.0f;
	// How strong the focus ring and the selection are, from 0 to 1. Zero draws neither.
	float focus_ring = 0.0f;
	float selection  = 0.0f;
	// The overlays to draw in order, and how many are used.
	std::array<StateOverlay, BOX_STATE_COUNT> overlays      = {};
	size_t                                    overlay_count = 0;
	// The fill and border as gradients, drawn in place of the colours above when set. Set when
	// the style or a state with any weight has one, so a box with no gradient draws as before.
	std::optional<LinearGradient> fill_gradient;
	std::optional<LinearGradient> border_gradient;
};

/// The look of a box with `styles` for `input`. The states apply in the order selected, hover,
/// focus, pressed, disabled, and each moves the value it sets toward its own by the state's
/// weight, so a state at 0 changes nothing. Hover and focus that set the same value share the
/// larger of their weights instead of approaching it twice, as an accent button blends its hover
/// fill. A disabled input fades by the theme's disabled opacity unless the disabled state sets
/// one. A state's opacity, length or shadow with a value that isn't finite is ignored, as if the
/// state left it unset, and so is a gradient whose angle isn't. A fill or border becomes a
/// gradient when the style or a state with any weight gives it one. A plain colour then blends
/// as a gradient of that one colour, and two gradients blend end by end, their angle turning the
/// short way round. A state that sets both a colour and a gradient moves toward its gradient. A
/// resolved angle may lie outside 0 to 360, which draws the same. A state's scale isn't resolved
/// yet, as nothing draws it.
[[nodiscard]] ResolvedStyle ResolveStyle(const ThemeContext& theme, const BoxStyles& styles,
                                         const StateInput& input);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_INTERACTION_STATES_H_
