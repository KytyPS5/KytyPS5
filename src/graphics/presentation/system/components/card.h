#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMPONENTS_CARD_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMPONENTS_CARD_H_

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"

namespace Libs::Graphics::SystemUi {

/// A card's tokens, its look and padding from the theme by CardDefaults, so every card matches and
/// a theme change reaches them all.
struct CardTokens {
	// The raised surface, its hairline edge and its large rounded corners.
	Color fill;
	Color border;
	Dp    border_width;
	Dp    radius;
	// The room between the card's edge and what it holds.
	Dp padding;
	// The low shadow, lifting the card a little off what is under it.
	Shadow shadow;
};

/// The theme's card, its surface, edge, corners, padding and shadow.
[[nodiscard]] CardTokens CardDefaults(const ThemeContext& theme);

/// How a card arranges its children and what it asks of its container. Its look is the theme's
/// unless its styles change a few fields, so cards match by default.
struct CardProps {
	/// What a card asks of its container and how it differs from the theme's card, following the
	/// element kinds' Self convention.
	struct Self {
		ItemLayout layout;
		// Fields of the card's look the caller changes, such as a smaller radius, patched over the
		// theme's card styles, built from CardDefaults. A field left at zero keeps the theme's, so
		// a card can't be patched to no border or a transparent fill. Empty by default, so a card
		// has the theme's look.
		BoxStyles styles = {};
	};

	// How the card arranges its children. With no padding set it has the theme's card padding, so
	// cards match by default, and a padding the caller sets, even zero, replaces it.
	BoxLayout layout;
	Self      self;
	// The card's box's key, which a card that scrolls keeps its position by, as BoxProps::key.
	Key key;
};

/// The theme's card, a raised surface with a subtle hairline border, large rounded corners and the
/// theme's low shadow, holding `children` inside the theme's card padding, or the caller's when its
/// layout sets one, with any fields its styles set in place of the theme's, built in the current
/// frame. It is a box, so its element is a BoxElement. It reads the current theme, so it needs an
/// open FrameScope. A refused card is a none handle, and a RefusalScope open around the call says
/// why. A card that scrolls keeps its position by its key, as Box does.
[[nodiscard]] BoxElement Card(const CardProps& props, const Children& children = {});

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMPONENTS_CARD_H_
