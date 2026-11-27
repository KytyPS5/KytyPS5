#include "graphics/presentation/system/views/trophyUnlocked/trophyUnlocked.h"

#include "graphics/presentation/system/components/glyphs/trophy.h"
#include "graphics/presentation/system/components/toast.h"
#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/imgui/imageCache.h"

#include <array>
#include <span>

namespace Libs::Graphics::SystemUi {

namespace {

// The game's grade numbers.
constexpr int32_t GRADE_PLATINUM = 1;
constexpr int32_t GRADE_GOLD     = 2;
constexpr int32_t GRADE_SILVER   = 3;
constexpr int32_t GRADE_BRONZE   = 4;

// A trophy toast's narrowest width, room for its icon and a short name, and the icon's side.
constexpr Dp MIN_TROPHY_WIDTH = Dp(360.0f);
constexpr Dp TROPHY_ICON_SIZE = Dp(64.0f);

// The cups' heights, before the subtitle and on the tile in the icon's place.
constexpr Dp TROPHY_CUP_SIZE      = Dp(16.0f);
constexpr Dp TROPHY_TILE_CUP_SIZE = Dp(32.0f);

// Two lines keep a trophy toast no taller than a notice with three.
constexpr int TROPHY_TITLE_LINES = 2;

// The trophy's `icon` with a fine outline over it when it can be shown, or a tile with a cup in
// `color` in its place, the same size either way, so a toast doesn't change height without one.
// Either way it glows in `color` around it, its corners following the icon's.
AnyElement IconSlot(const TrophyUnlockedTokens& trophy, const EncodedImage& icon, bool shown,
                    Color color) {
	const ItemLayout square {.width = trophy.icon_size, .height = trophy.icon_size};
	const BoxGlow    glow {.glow = trophy.icon_glow, .color = color};
	if (!shown) {
		const std::array<AnyElement, 1> cup {
		    TrophyGlyph({.size = trophy.tile_cup_size, .self = {.styles = {.color = color}}})};
		return Box({.layout = {.justify = Justify::Center, .align = Align::Center},
		            .self   = {.layout = square,
		                       .styles = {.fill   = trophy.tile_fill,
		                                  .radius = trophy.icon_radius,
		                                  .glow   = glow}}},
		           cup);
	}
	const std::array<AnyElement, 2> layers {
	    Image({.self   = {.styles = {.fit = ImageFit::Cover, .radius = trophy.icon_radius}},
	           .source = &icon}),
	    Box({.self = {.styles = {.border       = trophy.icon_outline,
	                             .border_width = trophy.icon_outline_width,
	                             .radius       = trophy.icon_radius}}}),
	};
	// The stack draws nothing of its own but the glow, rounded as the icon is.
	return Box({.layout = {.stacked = true},
	            .self = {.layout = square, .styles = {.radius = trophy.icon_radius, .glow = glow}}},
	           layers);
}

// A trophy's name over its subtitle, after a cup in `color` when `with_cup`, between the widths
// that leave the trophy toast its icon and padding. The title stretches across the column and
// wraps there, and the subtitle shrinks to fit it.
AnyElement TrophyText(const TrophyUnlockedProps& props, const TrophyUnlockedTokens& trophy,
                      Color color, bool with_cup) {
	const ThemeContext& theme  = CurrentThemeContext();
	const ToastTokens   tokens = ToastDefaults(theme);
	// The text's limits are the toast's less the padding, the icon and the gap. The padding is the
	// vertical one on both sides, as the content uses it all round.
	const Dp         inset = 2.0f * tokens.padding_vertical + trophy.icon_size + trophy.gap;
	const ItemLayout width {.min_main = trophy.min_width - inset,
	                        .max_main = tokens.max_width - inset};
	const AnyElement subtitle = Text({.text = props.subtitle,
	                                  .self = {.styles = {.type          = trophy.subtitle,
	                                                      .color         = trophy.subtitle_color,
	                                                      .shrink_to_fit = true}}});
	const std::array<AnyElement, 2> subtitle_parts {
	    with_cup ? TrophyGlyph({.size = trophy.cup_size, .self = {.styles = {.color = color}}})
	             : AnyElement {},
	    subtitle};
	const std::span<const AnyElement> shown_parts =
	    with_cup ? std::span<const AnyElement>(subtitle_parts)
	             : std::span<const AnyElement>(subtitle_parts).last(1);
	const std::array<AnyElement, 2> lines {
	    Text({.text = props.name,
	          .self = {.styles = {.type      = trophy.title,
	                              .color     = trophy.title_color,
	                              .overflow  = TextOverflow::Wrap,
	                              .max_lines = trophy.title_lines}}}),
	    Box({.layout = {.align = Align::Center, .gap = theme.space.s2}}, shown_parts),
	};
	// The limits are on the column, which sits in the content row, so they limit its width.
	return Box({.layout = {.direction = FlexDirection::Column, .align = Align::Stretch},
	            .self   = {.layout = width}},
	           lines);
}

} // namespace

TrophyGrade GradeOf(int32_t raw) {
	switch (raw) {
		case GRADE_PLATINUM: return TrophyGrade::Platinum;
		case GRADE_GOLD: return TrophyGrade::Gold;
		case GRADE_SILVER: return TrophyGrade::Silver;
		case GRADE_BRONZE: return TrophyGrade::Bronze;
		default: return TrophyGrade::Unknown;
	}
}

Color TrophyGradeColor(const ThemeContext& theme, TrophyGrade grade) {
	switch (grade) {
		case TrophyGrade::Platinum: return theme.colors.trophy.platinum;
		case TrophyGrade::Gold: return theme.colors.trophy.gold;
		case TrophyGrade::Silver: return theme.colors.trophy.silver;
		case TrophyGrade::Bronze: return theme.colors.trophy.bronze;
		case TrophyGrade::Unknown: break;
	}
	return theme.colors.status.success;
}

TrophyUnlockedTokens TrophyUnlockedDefaults(const ThemeContext& theme) {
	return {.min_width          = MIN_TROPHY_WIDTH,
	        .icon_size          = TROPHY_ICON_SIZE,
	        .icon_radius        = theme.radius.sm,
	        .icon_outline       = theme.colors.border.standard,
	        .icon_outline_width = theme.border.hair,
	        .icon_glow          = theme.glow.trophy,
	        .tile_fill          = theme.colors.surface.inset,
	        .tile_cup_size      = TROPHY_TILE_CUP_SIZE,
	        .cup_size           = TROPHY_CUP_SIZE,
	        .gap                = theme.space.s3,
	        .title              = theme.type.label,
	        .title_color        = theme.colors.text.primary,
	        .title_lines        = TROPHY_TITLE_LINES,
	        .subtitle           = theme.type.caption,
	        .subtitle_color     = theme.colors.text.secondary};
}

BoxElement TrophyUnlocked(const TrophyUnlockedProps& props) {
	const ThemeContext&        theme  = CurrentThemeContext();
	const TrophyUnlockedTokens trophy = TrophyUnlockedDefaults(theme);
	const ToastTokens          tokens = ToastDefaults(theme);
	const Color                color  = TrophyGradeColor(theme, props.grade);
	// Asked once, so the icon and the cup before the subtitle agree. The cup is built only when
	// the icon shows, as the tile carries one otherwise.
	const bool                      show_icon = ImageReady(props.icon);
	const std::array<AnyElement, 2> row {
	    IconSlot(trophy, props.icon, show_icon, color),
	    TrophyText(props, trophy, color, show_icon),
	};
	// The same padding on every side, the toast's narrower one, so the space around the square icon
	// looks even rather than wider beside it than above and below, and the toast stays no taller.
	const std::array<AnyElement, 1> content {
	    Box({.layout = {.align   = Align::Center,
	                    .gap     = trophy.gap,
	                    .padding = DpInsets(tokens.padding_vertical)}},
	        row),
	};
	// Plain, as a cup in the grade's colour marks it rather than a band.
	return Toast({}, content);
}

} // namespace Libs::Graphics::SystemUi
