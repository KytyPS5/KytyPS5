#include "graphics/presentation/system/gallery/pages/spacingPage.h"

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/gallery/components/page.h"
#include "graphics/presentation/system/gallery/pages/parts/format.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

constexpr std::string_view RULE =
    "Use the spacing scale, with hair for strokes only. Menus live in the content frame. Anything "
    "pinned to an edge uses the safe area.";

// The columns of the scale's rows, in design units.
constexpr float NAME_WIDTH  = 160.0f;
constexpr float VALUE_WIDTH = 80.0f;
// The side of the squares the radii and borders are shown on, and how high a space bar is.
constexpr Dp SQUARE_SIDE = Dp(72.0f);
constexpr Dp BAR_HEIGHT  = Dp(12.0f);
// The widest a caption under a square may be.
constexpr float SQUARE_CAPTION_WIDTH = 120.0f;

// A length of a scale and its name in the code.
template <class Scale>
struct NamedLength {
	std::string_view name;
	Dp Scale::* length;
};

constexpr std::array<NamedLength<SpaceScale>, 10> SPACES {
    NamedLength<SpaceScale> {"space.hair", &SpaceScale::hair},
    NamedLength<SpaceScale> {"space.s1", &SpaceScale::s1},
    NamedLength<SpaceScale> {"space.s2", &SpaceScale::s2},
    NamedLength<SpaceScale> {"space.s3", &SpaceScale::s3},
    NamedLength<SpaceScale> {"space.s4", &SpaceScale::s4},
    NamedLength<SpaceScale> {"space.s5", &SpaceScale::s5},
    NamedLength<SpaceScale> {"space.s6", &SpaceScale::s6},
    NamedLength<SpaceScale> {"space.s7", &SpaceScale::s7},
    NamedLength<SpaceScale> {"space.s8", &SpaceScale::s8},
    NamedLength<SpaceScale> {"space.s9", &SpaceScale::s9}};

constexpr std::array<NamedLength<RadiusScale>, 5> RADII {
    NamedLength<RadiusScale> {"radius.xs", &RadiusScale::xs},
    NamedLength<RadiusScale> {"radius.sm", &RadiusScale::sm},
    NamedLength<RadiusScale> {"radius.md", &RadiusScale::md},
    NamedLength<RadiusScale> {"radius.lg", &RadiusScale::lg},
    NamedLength<RadiusScale> {"radius.xl", &RadiusScale::xl}};

constexpr std::array<NamedLength<BorderWidths>, 4> BORDERS {
    NamedLength<BorderWidths> {"border.hair", &BorderWidths::hair},
    NamedLength<BorderWidths> {"border.thin", &BorderWidths::thin},
    NamedLength<BorderWidths> {"border.thick", &BorderWidths::thick},
    NamedLength<BorderWidths> {"border.bar", &BorderWidths::bar}};

// Each scale holds lengths and nothing else, so a length added to one and not here fails to build,
// though a table naming one twice could still hide another.
static_assert(sizeof(SpaceScale) == SPACES.size() * sizeof(Dp));
static_assert(sizeof(RadiusScale) == RADII.size() * sizeof(Dp));
static_assert(sizeof(BorderWidths) == BORDERS.size() * sizeof(Dp));

// One length of the spacing scale, its name, its value and a bar that long.
AnyElement SpaceRow(const ThemeContext& theme, const NamedLength<SpaceScale>& named) {
	const Dp                        length = theme.space.*named.length;
	const std::array<AnyElement, 3> cells {
	    CaptionCell(theme, named.name, NAME_WIDTH, theme.colors.text.secondary),
	    CaptionCell(theme, LengthText(length.value), VALUE_WIDTH, theme.colors.text.tertiary),
	    Box({.self = {.layout = {.width = length, .height = BAR_HEIGHT},
	                  .styles = {.fill = theme.colors.accent.fill}}}),
	};
	return Box({.layout = {.align = Align::Center}}, cells);
}

// The spacing scale, smallest first.
AnyElement Spaces(const ThemeContext& theme) {
	std::vector<AnyElement> rows;
	for (const auto& named: SPACES) {
		rows.push_back(SpaceRow(theme, named));
	}
	return Box({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s2}}, rows);
}

// A square in the look `style` gives, captioned with `name` and `value`.
AnyElement Square(const ThemeContext& theme, const BoxStyles& style, std::string_view name,
                  std::string_view value) {
	const std::array<AnyElement, 2> parts {
	    Box({.self = {.layout = {.width = SQUARE_SIDE, .height = SQUARE_SIDE}, .styles = style}}),
	    NameAndValue(theme, name, value, SQUARE_CAPTION_WIDTH),
	};
	return Box({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s2}}, parts);
}

// Each radius on a filled square, then the full radius, which is half the shorter side.
AnyElement Radii(const ThemeContext& theme) {
	std::vector<AnyElement> squares;
	for (const auto& named: RADII) {
		const Dp radius = theme.radius.*named.length;
		squares.push_back(Square(theme, {.fill = theme.colors.accent.fill, .radius = radius},
		                         named.name, LengthText(radius.value)));
	}
	squares.push_back(Square(theme, {.fill = theme.colors.accent.fill, .radius = RADIUS_FULL},
	                         "RADIUS_FULL", "half the short side"));
	return Box({.layout = {.gap = theme.space.s5}}, squares);
}

// Each border width on an empty square, never thinner than a pixel once scaled.
AnyElement Borders(const ThemeContext& theme) {
	std::vector<AnyElement> squares;
	for (const auto& named: BORDERS) {
		const Dp width = theme.border.*named.length;
		squares.push_back(Square(theme,
		                         {.border       = theme.colors.border.strong,
		                          .border_width = width,
		                          .radius       = theme.radius.sm},
		                         named.name, LengthText(width.value)));
	}
	return Box({.layout = {.gap = theme.space.s5}}, squares);
}

// The rules that turn design units into output pixels, the safe area and the content frame.
AnyElement Scaling(const ThemeContext& theme) {
	const ScaleRules&               rules = theme.scale;
	const std::array<AnyElement, 7> rows {
	    TokenLine(theme, "scale.reference_width x height",
	              LengthText(rules.reference_width) + " x " + LengthText(rules.reference_height)),
	    TokenLine(theme, "scale.min_scale to max_scale",
	              LengthText(rules.min_scale) + " to " + LengthText(rules.max_scale) +
	                  ", 720p to 8K"),
	    TokenLine(theme, "scale.min_user_scale to max_user_scale",
	              LengthText(rules.min_user_scale) + " to " + LengthText(rules.max_user_scale)),
	    TokenLine(theme, "scale.default_safe_area_percent",
	              LengthText(rules.default_safe_area_percent) + "% of the screen"),
	    TokenLine(theme, "scale.max_safe_area_percent",
	              LengthText(rules.max_safe_area_percent) + "%"),
	    TokenLine(theme, "scale.edge_min",
	              LengthText(rules.edge_min.value) + ", the least safe area inset"),
	    TokenLine(theme, "scale.content_aspect",
	              LengthText(rules.content_aspect) + ", menus no wider than 16 by 9"),
	};
	return Box({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s1}}, rows);
}

} // namespace

AnyElement BuildSpacingPage() {
	const ThemeContext& theme = CurrentThemeContext();
	return Page({.title = "Spacing", .subtitle = RULE},
	            {
	                Section(theme, "Space", Spaces(theme)),
	                Section(theme, "Radius", Radii(theme)),
	                Section(theme, "Border width", Borders(theme)),
	                Section(theme, "Scaling", Scaling(theme)),
	            });
}

} // namespace Libs::Graphics::SystemUi::Gallery
