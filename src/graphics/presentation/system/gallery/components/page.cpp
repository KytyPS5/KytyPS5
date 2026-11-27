#include "graphics/presentation/system/gallery/components/page.h"

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"

#include <algorithm>
#include <array>
#include <vector>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

// How wide a page's subtitle runs before it wraps, in design units, a comfortable line to read.
constexpr Dp SUBTITLE_WIDTH = Dp(720.0f);

// A page's title in the heading style over its subtitle, the rule the page follows, wrapped.
BoxElement Header(const ThemeContext& theme, const PageProps& props) {
	const AnyElement title = Text(
	    {.text = props.title,
	     .self = {.styles = {.type = theme.type.heading, .color = theme.colors.text.primary}}});
	const AnyElement subtitle = Text({.text = props.subtitle,
	                                  .self = {.layout = {.width = SUBTITLE_WIDTH},
	                                           .styles = {.type     = theme.type.body_small,
	                                                      .color    = theme.colors.text.secondary,
	                                                      .overflow = TextOverflow::Wrap}}});
	return Box({.layout = {.direction = FlexDirection::Column}}, {title, subtitle});
}

} // namespace

BoxElement Page(const PageProps& props, const Children& children) {
	const ThemeContext& theme = CurrentThemeContext();
	// No fill of its own, as the pane it sits in fills the page area, and a second translucent
	// fill would darken it.
	return Box({.layout = {.direction = FlexDirection::Column,
	                       .gap       = theme.space.s6,
	                       .padding   = DpInsets(theme.space.s8)},
	            .self   = {.layout = {.grow = 1.0f}}},
	           {Header(theme, props), children});
}

AnyElement SectionTitle(const ThemeContext& theme, std::string_view title) {
	return Text(
	    {.text = title,
	     .self = {.styles = {.type = theme.type.label, .color = theme.colors.text.primary}}});
}

AnyElement Section(const ThemeContext& theme, std::string_view title, AnyElement content) {
	const std::array<AnyElement, 2> parts {SectionTitle(theme, title), content};
	return Box({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s3}}, parts);
}

AnyElement Captioned(const ThemeContext& theme, AnyElement shown, std::string_view caption,
                     Color caption_color) {
	const std::array<AnyElement, 2> parts {
	    shown,
	    Text({.text = caption,
	          .self = {.styles = {.type = theme.type.caption, .color = caption_color}}}),
	};
	return Box({.layout = {.direction = FlexDirection::Column,
	                       .align     = Align::Center,
	                       .gap       = theme.space.s2}},
	           parts);
}

AnyElement Captioned(const ThemeContext& theme, AnyElement shown, std::string_view caption) {
	return Captioned(theme, shown, caption, theme.colors.text.secondary);
}

AnyElement NameAndValue(const ThemeContext& theme, std::string_view name, std::string_view value,
                        float width) {
	const ItemLayout slot {.width = Dp(width)};
	// One line, clipped at the edge, so the values of swatches side by side line up whatever
	// their names.
	const std::array<AnyElement, 2> lines {
	    Text({.text = name,
	          .self = {.layout = slot,
	                   .styles = {.type     = theme.type.caption,
	                              .color    = theme.colors.text.primary,
	                              .overflow = TextOverflow::Clip}}}),
	    Text({.text = value,
	          .self = {.layout = slot,
	                   .styles = {.type  = theme.type.caption,
	                              .color = theme.colors.text.secondary}}}),
	};
	return Box({.layout = {.direction = FlexDirection::Column}}, lines);
}

AnyElement CaptionCell(const ThemeContext& theme, std::string_view text, float width, Color color) {
	return Text({.text = text,
	             .self = {.layout = {.width = Dp(width)},
	                      .styles = {.type = theme.type.caption, .color = color}}});
}

AnyElement TokenLine(const ThemeContext& theme, std::string_view name, std::string_view value,
                     float name_width) {
	const std::array<AnyElement, 2> cells {
	    CaptionCell(theme, name, name_width, theme.colors.text.secondary),
	    Text(
	        {.text = value,
	         .self = {.styles = {.type = theme.type.caption, .color = theme.colors.text.primary}}}),
	};
	return Box({}, cells);
}

AnyElement Lines(const ThemeContext& theme, const Children& lines) {
	return Box({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s1}}, lines);
}

AnyElement Grid(const ThemeContext& theme, const Children& items, size_t columns) {
	const std::span<const AnyElement> all     = items;
	const size_t                      per_row = std::max<size_t>(columns, 1);
	std::vector<AnyElement>           rows;
	for (size_t start = 0; start < all.size(); start += per_row) {
		const size_t count = std::min(per_row, all.size() - start);
		rows.push_back(Box({.layout = {.gap = theme.space.s3}}, all.subspan(start, count)));
	}
	return Box({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s3}}, rows);
}

AnyElement BesideTokens(const ThemeContext& theme, AnyElement specimen, AnyElement tokens) {
	const std::array<AnyElement, 2> parts {specimen, tokens};
	return Box({.layout = {.gap = theme.space.s7}}, parts);
}

float ShadowReach(const Shadow& shadow) {
	float reach = 0.0f;
	for (size_t index = 0; index < shadow.Count(); ++index) {
		const ShadowLayer& layer = shadow.layers[index];
		reach = std::max(reach, layer.offset_y.value + layer.blur.value + layer.spread.value);
	}
	return reach;
}

AnyElement ShadowWell(const ThemeContext& theme, const Shadow& shadow, std::string_view caption,
                      Color fill, const ItemLayout& size) {
	const BoxStyles plain {.fill         = fill,
	                       .border       = theme.colors.border.subtle,
	                       .border_width = theme.border.hair,
	                       .radius       = theme.radius.md};
	BoxStyles       raised = plain;
	raised.shadow          = shadow;
	const std::array<AnyElement, 2> pair {
	    Captioned(theme, Box({.self = {.layout = size, .styles = plain}}), "No shadow"),
	    Captioned(theme, Box({.self = {.layout = size, .styles = raised}}), caption),
	};
	// Room below for the furthest a layer reaches, so a high shadow stays on the well.
	const Dp below = Dp(std::max(theme.space.s6.value, ShadowReach(shadow) + theme.space.s3.value));
	return Box(
	    {.layout = {.gap     = theme.space.s6,
	                .padding = DpInsets(theme.space.s6, theme.space.s6, below, theme.space.s6)},
	     .self   = {.styles = {.fill = theme.colors.surface.overlay, .radius = theme.radius.md}}},
	    pair);
}

} // namespace Libs::Graphics::SystemUi::Gallery
