#include "graphics/presentation/system/gallery/pages/cardPage.h"

#include "graphics/presentation/system/components/card.h"
#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/gallery/components/page.h"
#include "graphics/presentation/system/gallery/pages/parts/format.h"

#include <array>
#include <string_view>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

constexpr std::string_view RULE =
    "A card groups related content, and cards don't nest. Raise elevation only for things that "
    "float.";

// How wide a card specimen's reading text wraps, as a card's would.
constexpr float CARD_TEXT_WIDTH = 360.0f;
// How wide each card in the row of related groups is.
constexpr float GROUP_WIDTH = 240.0f;

// A card holding `title` over `body`, its body wrapped at `width`.
AnyElement TitledCard(const ThemeContext& theme, std::string_view title, std::string_view body,
                      float width) {
	const std::array<AnyElement, 2> content {
	    Text(
	        {.text = title,
	         .self = {.styles = {.type = theme.type.heading, .color = theme.colors.text.primary}}}),
	    Text({.text = body,
	          .self = {.layout = {.width = Dp(width)},
	                   .styles = {.type     = theme.type.body_small,
	                              .color    = theme.colors.text.secondary,
	                              .overflow = TextOverflow::Wrap}}}),
	};
	return Card({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s2}}, content);
}

// A card beside the tokens of its look.
AnyElement CardSpecimen(const ThemeContext& theme) {
	// Read from the card's own defaults, so the page can't show values the card doesn't draw.
	const CardTokens                card = CardDefaults(theme);
	const std::array<AnyElement, 6> tokens {
	    TokenLine(theme, "fill", HexText(card.fill)),
	    TokenLine(theme, "border", HexText(card.border)),
	    TokenLine(theme, "border_width", LengthText(card.border_width.value)),
	    TokenLine(theme, "radius", LengthText(card.radius.value)),
	    TokenLine(theme, "padding", LengthText(card.padding.value)),
	    TokenLine(theme, "shadow", "shadows.low"),
	};
	return BesideTokens(theme,
	                    TitledCard(theme, "Save data",
	                               "A card groups content that belongs together, on a raised "
	                               "surface with a hairline edge.",
	                               CARD_TEXT_WIDTH),
	                    Lines(theme, tokens));
}

// Cards side by side, each one group of related settings, as a menu lays them out.
AnyElement CardRow(const ThemeContext& theme) {
	const std::array<AnyElement, 3> cards {
	    TitledCard(theme, "Display", "Resolution, scaling and the safe area.", GROUP_WIDTH),
	    TitledCard(theme, "Audio", "Output device and volume.", GROUP_WIDTH),
	    TitledCard(theme, "Controls", "Button layout and stick dead zones.", GROUP_WIDTH),
	};
	return Box({.layout = {.gap = theme.space.s4}}, cards);
}

} // namespace

AnyElement BuildCardPage() {
	const ThemeContext& theme = CurrentThemeContext();
	return Page({.title = "Card", .subtitle = RULE},
	            {
	                Section(theme, "A card and its tokens", CardSpecimen(theme)),
	                Section(theme, "Related groups side by side", CardRow(theme)),
	            });
}

} // namespace Libs::Graphics::SystemUi::Gallery
