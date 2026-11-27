#include "graphics/presentation/system/components/card.h"

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/core/patch.h"

namespace Libs::Graphics::SystemUi {

namespace {

// The card's look from its tokens, so cards follow a theme change.
BoxStyles CardStyles(const CardTokens& tokens) {
	return {.fill         = tokens.fill,
	        .border       = tokens.border,
	        .border_width = tokens.border_width,
	        .radius       = tokens.radius,
	        // A card is a surface, not a control, so it doesn't change with a state.
	        .states = {},
	        .shadow = tokens.shadow};
}

} // namespace

CardTokens CardDefaults(const ThemeContext& theme) {
	return {.fill         = theme.colors.surface.raised,
	        .border       = theme.colors.border.subtle,
	        .border_width = theme.border.hair,
	        .radius       = theme.radius.lg,
	        .padding      = theme.space.s5,
	        .shadow       = theme.shadows.low};
}

BoxElement Card(const CardProps& props, const Children& children) {
	const CardTokens tokens = CardDefaults(CurrentThemeContext());
	BoxLayout        layout = props.layout;
	// The theme's padding unless the caller set one, so cards match by default.
	if (!layout.padding.has_value()) {
		layout.padding = DpInsets(tokens.padding);
	}
	return Box({.layout = layout,
	            .self   = {.layout = props.self.layout,
	                       .styles = Patch(CardStyles(tokens), props.self.styles)},
	            .key    = props.key},
	           children);
}

} // namespace Libs::Graphics::SystemUi
