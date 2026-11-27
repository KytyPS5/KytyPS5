#include "graphics/presentation/system/gallery/pages/typographyPage.h"

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
    "Use Bold for titles, labels and headings, and Regular for reading text. Dense styles are for "
    "heads-up displays only. Plan for 35% longer text.";

// The ramp's columns, in design units.
constexpr float NAME_WIDTH    = 160.0f;
constexpr Dp    SAMPLE_WIDTH  = Dp(420.0f);
constexpr float METRICS_WIDTH = 300.0f;
// A slot too short for the long sample, and its label's column.
constexpr Dp    SLOT_WIDTH       = Dp(320.0f);
constexpr float SLOT_LABEL_WIDTH = 240.0f;
constexpr int   WRAP_LINES       = 2;
// How wide the longer languages wrap, and the digits' column.
constexpr Dp LANGUAGE_WIDTH = Dp(480.0f);
constexpr Dp DIGITS_WIDTH   = Dp(200.0f);

constexpr std::string_view SAMPLE      = "Trophy earned";
constexpr std::string_view LONG_SAMPLE = "The quick brown fox jumps over the lazy dog and keeps on "
                                         "running through the whole of the wood";
// Longer than the English they stand for, as translations usually are.
constexpr std::string_view GERMAN_SAMPLE  = "Geschwindigkeitsbegrenzungseinstellungen speichern";
constexpr std::string_view FINNISH_SAMPLE = "Tallennetaanko pelin edistyminen pilvipalveluun?";
constexpr std::array<std::string_view, 4> DIGITS {"1,024", "88", "123,456", "7"};

// A style of the ramp and its name in the code.
struct NamedStyle {
	std::string_view name;
	TypeStyle TypeRamp::* style;
};

constexpr std::array<NamedStyle, 9> RAMP {NamedStyle {"type.display", &TypeRamp::display},
                                          NamedStyle {"type.title", &TypeRamp::title},
                                          NamedStyle {"type.heading", &TypeRamp::heading},
                                          NamedStyle {"type.body", &TypeRamp::body},
                                          NamedStyle {"type.body_small", &TypeRamp::body_small},
                                          NamedStyle {"type.label", &TypeRamp::label},
                                          NamedStyle {"type.caption", &TypeRamp::caption},
                                          NamedStyle {"type.dense", &TypeRamp::dense},
                                          NamedStyle {"type.dense_small", &TypeRamp::dense_small}};
// The ramp holds styles and nothing else, so a style added to it and not here fails to build,
// though a table naming one twice could still hide another.
static_assert(sizeof(TypeRamp) == RAMP.size() * sizeof(TypeStyle));

// A text level and its name in the code.
struct NamedText {
	std::string_view name;
	Color TextColors::* color;
};

constexpr std::array<NamedText, 4> TEXT_LEVELS {
    NamedText {"text.primary", &TextColors::primary},
    NamedText {"text.secondary", &TextColors::secondary},
    NamedText {"text.tertiary", &TextColors::tertiary},
    NamedText {"text.disabled", &TextColors::disabled}};

// How text can give way in a slot, and what each is called.
struct NamedOverflow {
	std::string_view name;
	TextOverflow     overflow;
	int              max_lines     = 0;
	bool             shrink_to_fit = false;
};

constexpr std::array<NamedOverflow, 4> OVERFLOWS {
    NamedOverflow {"Ellipsis, the default", TextOverflow::Ellipsis},
    NamedOverflow {"Clip", TextOverflow::Clip},
    NamedOverflow {"Wrap, at most 2 lines", TextOverflow::Wrap, WRAP_LINES},
    NamedOverflow {"Shrink, then ellipsis", TextOverflow::Ellipsis, 0, true}};

// One style of the ramp, its name, the sample in a hairline box that is its line box, so the room
// around the glyphs shows, and its metrics.
AnyElement RampRow(const ThemeContext& theme, const NamedStyle& named) {
	const TypeStyle&                style = theme.type.*named.style;
	const std::array<AnyElement, 1> sample {
	    Text({.text = SAMPLE,
	          .self = {.styles = {.type = style, .color = theme.colors.text.primary}}}),
	};
	const std::array<AnyElement, 3> cells {
	    CaptionCell(theme, named.name, NAME_WIDTH, theme.colors.text.secondary),
	    Box({.self = {.layout = {.width = SAMPLE_WIDTH}}},
	        std::array<AnyElement, 1> {
	            Box({.self = {.styles = {.border       = theme.colors.border.subtle,
	                                     .border_width = theme.border.hair}}},
	                sample)}),
	    CaptionCell(theme, TypeMetricsText(style), METRICS_WIDTH, theme.colors.text.tertiary),
	};
	return Box({.layout = {.align = Align::Center}}, cells);
}

// The ramp, each style in order, largest first.
AnyElement Ramp(const ThemeContext& theme) {
	std::vector<AnyElement> rows;
	for (const NamedStyle& named: RAMP) {
		rows.push_back(RampRow(theme, named));
	}
	return Box({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s2}}, rows);
}

// The text levels side by side on the raised surface they are judged on, then the text for light
// surfaces on white.
AnyElement TextColours(const ThemeContext& theme) {
	std::vector<AnyElement> dark;
	for (const NamedText& named: TEXT_LEVELS) {
		dark.push_back(Text({.text = named.name,
		                     .self = {.styles = {.type  = theme.type.body,
		                                         .color = theme.colors.text.*named.color}}}));
	}
	const std::array<AnyElement, 2> light {
	    Text({.text = "text.on_light",
	          .self = {.styles = {.type = theme.type.body, .color = theme.colors.text.on_light}}}),
	    Text({.text = "text.secondary_on_light",
	          .self = {.styles = {.type  = theme.type.body,
	                              .color = theme.colors.text.secondary_on_light}}}),
	};
	const std::array<AnyElement, 2> panels {
	    Box({.layout = {.gap = theme.space.s5, .padding = DpInsets(theme.space.s3)},
	         .self = {.styles = {.fill = theme.colors.surface.raised, .radius = theme.radius.sm}}},
	        dark),
	    Box({.layout = {.gap = theme.space.s5, .padding = DpInsets(theme.space.s3)},
	         .self   = {.styles = {.fill = Palette::GRAY_900, .radius = theme.radius.sm}}},
	        light),
	};
	return Box({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s3}}, panels);
}

// One way text gives way, its name and the long sample in a bordered slot too short for it, so
// what it does at the slot's edge shows.
AnyElement SlotRow(const ThemeContext& theme, const NamedOverflow& named) {
	const std::array<AnyElement, 1> text {
	    Text({.text = LONG_SAMPLE,
	          .self = {.layout = {.width = SLOT_WIDTH},
	                   .styles = {.type          = theme.type.body,
	                              .color         = theme.colors.text.primary,
	                              .overflow      = named.overflow,
	                              .max_lines     = named.max_lines,
	                              .shrink_to_fit = named.shrink_to_fit}}}),
	};
	const std::array<AnyElement, 2> cells {
	    CaptionCell(theme, named.name, SLOT_LABEL_WIDTH, theme.colors.text.secondary),
	    Box({.self = {.styles = {.border       = theme.colors.border.standard,
	                             .border_width = theme.border.hair,
	                             .overflow     = Overflow::Clip}}},
	        text),
	};
	return Box({.layout = {.align = Align::Center}}, cells);
}

// Each way text gives way in a slot too short for it.
AnyElement Overflows(const ThemeContext& theme) {
	std::vector<AnyElement> rows;
	for (const NamedOverflow& named: OVERFLOWS) {
		rows.push_back(SlotRow(theme, named));
	}
	return Box({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s3}}, rows);
}

// Longer languages wrapped in a reading width, as translations are longer than English.
AnyElement Languages(const ThemeContext& theme) {
	std::vector<AnyElement> samples;
	for (const std::string_view sample: {GERMAN_SAMPLE, FINNISH_SAMPLE}) {
		samples.push_back(Text({.text = sample,
		                        .self = {.layout = {.width = LANGUAGE_WIDTH},
		                                 .styles = {.type     = theme.type.body,
		                                            .color    = theme.colors.text.primary,
		                                            .overflow = TextOverflow::Wrap}}}));
	}
	return Box({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s2}}, samples);
}

// Numbers aligned to the end of a column, so their digits line up by place.
AnyElement Digits(const ThemeContext& theme) {
	std::vector<AnyElement> numbers;
	for (const std::string_view number: DIGITS) {
		numbers.push_back(Text({.text = number,
		                        .self = {.layout = {.width = DIGITS_WIDTH},
		                                 .styles = {.type  = theme.type.body,
		                                            .color = theme.colors.text.primary,
		                                            .align = TextAlign::End}}}));
	}
	return Box({.layout = {.direction = FlexDirection::Column}}, numbers);
}

// The font files every style is drawn with. A style picks one by its weight.
AnyElement Fonts(const ThemeContext& theme) {
	const std::string               regular = "Regular  " + std::string(theme.fonts.regular);
	const std::string               bold    = "Bold  " + std::string(theme.fonts.bold);
	const std::array<AnyElement, 3> lines {
	    Text({.text = regular,
	          .self = {.styles = {.type = theme.type.body, .color = theme.colors.text.primary}}}),
	    Text({.text = bold,
	          .self = {.styles = {.type = theme.type.label, .color = theme.colors.text.primary}}}),
	    Text({.text = "Two weights and no italic. Text has no shaping and runs left to right.",
	          .self = {.styles = {.type  = theme.type.caption,
	                              .color = theme.colors.text.tertiary}}}),
	};
	return Box({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s1}}, lines);
}

} // namespace

AnyElement BuildTypographyPage() {
	const ThemeContext& theme = CurrentThemeContext();
	return Page({.title = "Typography", .subtitle = RULE},
	            {
	                Section(theme, "Type ramp, each sample in its line box", Ramp(theme)),
	                Section(theme, "Text colours", TextColours(theme)),
	                Section(theme, "In a slot too short", Overflows(theme)),
	                Section(theme, "Longer languages", Languages(theme)),
	                Section(theme, "Digits in a column", Digits(theme)),
	                Section(theme, "Fonts", Fonts(theme)),
	            });
}

} // namespace Libs::Graphics::SystemUi::Gallery
