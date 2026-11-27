#include "graphics/presentation/system/components/toast.h"

#include "graphics/presentation/system/components/card.h"
#include "graphics/presentation/system/core.h"

#include <array>

namespace Libs::Graphics::SystemUi {

namespace {

// The toast's width limits in design units, narrower than the old trophy toast's 520, so a toast
// covers less of the game.
constexpr Dp MIN_TOAST_WIDTH = Dp(280.0f);
constexpr Dp MAX_TOAST_WIDTH = Dp(480.0f);

// The room around a toast's content. A plain toast has none, as its content brings its own. A
// toast with a status starts its content the padding past its band.
DpInsets ContentPadding(const ToastTokens& tokens, bool banded) {
	if (!banded) {
		return {};
	}
	// Only the top also clears the band.
	return DpInsets(tokens.edge_width + tokens.padding_vertical, tokens.padding_horizontal,
	                tokens.padding_vertical, tokens.padding_horizontal);
}

} // namespace

ToastTokens ToastDefaults(const ThemeContext& theme) {
	return {.min_width          = MIN_TOAST_WIDTH,
	        .max_width          = MAX_TOAST_WIDTH,
	        .padding_horizontal = theme.space.s4,
	        .padding_vertical   = theme.space.s3,
	        .edge_width         = theme.border.bar,
	        .slide_distance     = theme.space.s6};
}

BoxElement Toast(const ToastProps& props, const Children& children) {
	const ThemeContext& theme  = CurrentThemeContext();
	const ToastTokens   tokens = Patch(ToastDefaults(theme), props.tokens);
	const bool          banded = props.status.has_value();
	// The band is the card's own edge, so it follows the rounded corners.
	const EdgeBand edge =
	    banded ? EdgeBand {.color = StatusColor(theme, *props.status), .width = tokens.edge_width}
	           : EdgeBand {};
	const DpInsets padding = ContentPadding(tokens, banded);
	// The limits are on the column, which sits in the card's row, so they limit its width, less
	// the padding, which the card adds around it. Its children stretch across it, so text wraps
	// there.
	const float                     inset = padding.left.value + padding.right.value;
	const std::array<AnyElement, 1> column {
	    Box({.layout = {.direction = FlexDirection::Column, .align = Align::Stretch},
	         .self   = {.layout = {.min_main = Dp(tokens.min_width.value - inset),
	                               .max_main = Dp(tokens.max_width.value - inset)}}},
	        children),
	};
	// Raised like a card, as it sits over the game rather than over another surface, with smaller
	// corners, as it is a passing notice. Its border is a step stronger than a card's, to part it
	// from a bright frame, and the card's shadow lifts it off dark ones. Only the fields that
	// differ from the card's are set, and the card's fill, hairline and shadow stay.
	const BoxStyles surface = {
	    .border = theme.colors.border.standard, .radius = theme.radius.md, .edge = edge};
	return Card(
	    {.layout = {.padding = padding}, .self = {.layout = props.self.layout, .styles = surface}},
	    column);
}

} // namespace Libs::Graphics::SystemUi
