#include "graphics/presentation/system/gallery/pages/themePage.h"

#include "graphics/presentation/system/components/card.h"
#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/core/scale.h"
#include "graphics/presentation/system/gallery/components/page.h"
#include "graphics/presentation/system/gallery/pages/parts/format.h"
#include "graphics/presentation/system/gallery/pages/parts/swatch.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

constexpr std::string_view RULE =
    "The theme is set in three tiers. Palette colours have no meaning, semantic tokens give each a "
    "role, and a component's tokens are built from those roles. Read the tier above yours and "
    "never a palette colour in a component, so one change to the theme reaches every screen.";

// How wide a tier's card is, in design units, room for its words to wrap.
constexpr Dp TIER_CARD_WIDTH = Dp(240.0f);
// The length shown scaling, in design units, as long as the theme's space.s4.
constexpr float SCALED_LENGTH = 16.0f;

// A screen the scale is shown at and what it is called.
struct NamedScreen {
	std::string_view name;
	float            width;
	float            height;
};

constexpr std::array<NamedScreen, 4> SCREENS {
    NamedScreen {"720p", 1280.0f, 720.0f}, NamedScreen {"1080p", 1920.0f, 1080.0f},
    NamedScreen {"1440p", 2560.0f, 1440.0f}, NamedScreen {"4K", 3840.0f, 2160.0f}};

// Words in the body style and `color`, wrapped to whatever width they are given.
AnyElement Words(const ThemeContext& theme, std::string_view text, Color color) {
	return Text({.text = text,
	             .self = {.styles = {.type     = theme.type.body_small,
	                                 .color    = color,
	                                 .overflow = TextOverflow::Wrap}}});
}

// A card at the component tier, holding a line that says where its fill comes from.
AnyElement TierCard(const ThemeContext& theme, const CardProps& props, std::string_view text) {
	CardProps sized         = props;
	sized.self.layout.width = TIER_CARD_WIDTH;
	return Card(sized, {Words(theme, text, theme.colors.text.primary)});
}

// One colour followed through the tiers, a palette grey, the raised surface's role made from it,
// and the card whose fill reads that role.
AnyElement Tiers(const ThemeContext& theme) {
	const AnyElement palette =
	    Captioned(theme, Swatch(theme, {"Palette::GRAY_150", Palette::GRAY_150}), "1. Palette");
	const AnyElement semantic =
	    Captioned(theme, Swatch(theme, {"surface.raised", theme.colors.surface.raised}),
	              "2. Semantic, GRAY_150 at the panel's opacity");
	const AnyElement component = Captioned(
	    theme, TierCard(theme, {}, "CardTokens.fill reads surface.raised"), "3. Component");
	return Box({.layout = {.align = Align::Start, .gap = theme.space.s8}},
	           {palette, semantic, component});
}

// The theme's card beside one patched to a smaller radius and the accent's line, which keeps the
// rest of the theme's look.
AnyElement Patched(const ThemeContext& theme) {
	const CardProps patched {
	    .self = {.styles = {.border = theme.colors.accent.line, .radius = theme.radius.sm}}};
	return Box({.layout = {.align = Align::Start, .gap = theme.space.s8}},
	           {Captioned(theme, TierCard(theme, {}, "The theme's card"), "Card({})"),
	            Captioned(theme, TierCard(theme, patched, "Patched, the rest is the theme's"),
	                      "styles = {.border = accent.line, .radius = radius.sm}")});
}

// How many pixels SCALED_LENGTH design units take on each screen, as ScaleContext works them out
// from the theme's reference size.
AnyElement Scale(const ThemeContext& theme) {
	std::vector<AnyElement> lines;
	for (const NamedScreen& screen: SCREENS) {
		const ScaleContext scale(theme.scale, screen.width, screen.height);
		lines.push_back(TokenLine(theme, screen.name,
		                          LengthText(SCALED_LENGTH) + " units are " +
		                              LengthText(scale.Px(SCALED_LENGTH)) + " pixels, scale " +
		                              LengthText(scale.Scale())));
	}
	return Lines(theme, lines);
}

} // namespace

AnyElement BuildThemePage() {
	const ThemeContext& theme = CurrentThemeContext();
	return Page({.title = "Theme", .subtitle = RULE},
	            {Section(theme, "Three tiers, from a palette colour to a card", Tiers(theme)),
	             Section(theme, "Patching one component", Patched(theme)),
	             Section(theme, "One length on each screen", Scale(theme))});
}

} // namespace Libs::Graphics::SystemUi::Gallery
