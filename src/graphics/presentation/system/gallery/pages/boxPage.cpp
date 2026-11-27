#include "graphics/presentation/system/gallery/pages/boxPage.h"

#include "graphics/presentation/system/animation.h"
#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/gallery/components/page.h"
#include "graphics/presentation/system/gallery/pages/parts/format.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

constexpr std::string_view RULE =
    "A box is the primitive every component is drawn from. Raise elevation only for things that "
    "float.";

// Each sample's size in design units, wide enough that its corners and border read at a glance.
constexpr Dp SAMPLE_WIDTH  = Dp(96.0f);
constexpr Dp SAMPLE_HEIGHT = Dp(64.0f);

// How long the fade and slide samples stay shown, and then hidden, before they turn again, and
// one whole turn of both.
constexpr std::chrono::milliseconds FADE_HALF_CYCLE {2000};
constexpr std::chrono::milliseconds FADE_CYCLE = 2 * FADE_HALF_CYCLE;
// The name the slide sample is keyed under, with its turn, so each turn arrives anew.
constexpr std::string_view SLIDE_SAMPLE_KEY = "slide sample";

// The tween sample's targets. The low one is faint but still clearly there, so it reads as dimmed
// rather than gone and tells itself apart from the fade beside it.
constexpr float TWEEN_HIGH = 1.0f;
constexpr float TWEEN_LOW  = 0.25f;

// The size every sample takes, so the row lines up.
constexpr ItemLayout SAMPLE_SIZE {.width = SAMPLE_WIDTH, .height = SAMPLE_HEIGHT};

// A state's full weight, so each specimen shows its state at its strongest.
constexpr float FULL_WEIGHT = 1.0f;

// A sample box of the page's size with `style`, captioned.
AnyElement Sample(const ThemeContext& theme, const BoxStyles& style, std::string_view caption) {
	return Captioned(theme, Box({.self = {.layout = SAMPLE_SIZE, .styles = style}}), caption);
}

// A stacked box of the page's size, a filled box with a label laid over it, so the label sits in
// the same rectangle and is drawn on top.
AnyElement StackedSample(const ThemeContext& theme, Color fill) {
	const std::array<AnyElement, 2> layers {
	    Box({.self = {.styles = {.fill = fill, .radius = theme.radius.md}}}),
	    Text({.text = "On top",
	          .self = {.styles = {.type  = theme.type.label,
	                              .color = theme.colors.text.on_accent,
	                              .align = TextAlign::Center}}}),
	};
	return Captioned(theme,
	                 Box({.layout = {.stacked = true}, .self = {.layout = SAMPLE_SIZE}}, layers),
	                 "Stacked");
}

// The one style every state specimen shares, so each shows only what its state does. A neutral
// surface lets the selection's translucent tint show, and its edge keeps it apart from the
// backdrop.
BoxStyles StatesStyle(const ThemeContext& theme) {
	BoxStyles style {.fill         = theme.colors.surface.overlay,
	                 .border       = theme.colors.border.standard,
	                 .border_width = theme.border.hair,
	                 .radius       = theme.radius.md,
	                 .states       = {}};
	style.states.hover.overlay   = theme.colors.state.hover;
	style.states.pressed.overlay = theme.colors.state.pressed;
	// Only the ring, so it is seen on its own and not as a hover with a ring.
	style.states.focus.focus_ring   = true;
	style.states.selected.selection = true;
	// Disabled sets nothing, so the theme's disabled fade is what shows.
	return style;
}

// One specimen of `style` held in `state`, captioned in `caption_color`.
AnyElement StateSample(const ThemeContext& theme, const BoxStyles& style, const StateInput& state,
                       std::string_view caption, Color caption_color) {
	return Captioned(theme, Box({.self = {.layout = SAMPLE_SIZE, .styles = style, .state = state}}),
	                 caption, caption_color);
}

// One specimen of `style` held in `state`, captioned in the theme's secondary text.
AnyElement StateSample(const ThemeContext& theme, const BoxStyles& style, const StateInput& state,
                       std::string_view caption) {
	return StateSample(theme, style, state, caption, theme.colors.text.secondary);
}

// The shared style at rest and in each state at full weight. The gap is wider than the row above,
// so the focus ring's glow, which reaches past an s4 gap, stays clear of the neighbours.
AnyElement StatesRow(const ThemeContext& theme) {
	const BoxStyles                 style = StatesStyle(theme);
	const std::array<AnyElement, 6> samples {
	    StateSample(theme, style, {}, "Rest"),
	    StateSample(theme, style, {.hover = FULL_WEIGHT}, "Hover"),
	    StateSample(theme, style, {.focus = FULL_WEIGHT}, "Focus"),
	    StateSample(theme, style, {.pressed = FULL_WEIGHT}, "Pressed"),
	    StateSample(theme, style, {.disabled = true}, "Disabled"),
	    StateSample(theme, style, {.selected = true}, "Selected"),
	};
	return Box({.layout = {.gap = theme.space.s6}}, samples);
}

// Besides the default top to bottom, the gradient specimens run left to right and from the top
// left corner to the bottom right.
constexpr float GRADIENT_ACROSS   = 90.0f;
constexpr float GRADIENT_DIAGONAL = 135.0f;

// Fully clear, the far end of a fade.
constexpr float CLEAR_ALPHA = 0.0f;

// The accent's light and dark steps, far enough apart that a sample's gradient reads at a glance
// without banding, and lighter at one end than the accent fill the samples above share. Run top
// to bottom, and left to right.
constexpr LinearGradient ACCENT_RUN {Palette::BLUE_600, Palette::BLUE_300};
constexpr LinearGradient ACCENT_ACROSS {Palette::BLUE_600, Palette::BLUE_300, GRADIENT_ACROSS};

// A box with medium corners filled with `gradient`.
BoxStyles GradientStyle(const ThemeContext& theme, const LinearGradient& gradient) {
	BoxStyles style {.radius = theme.radius.md};
	style.fill_gradient = gradient;
	return style;
}

// A light accent border fading to clear toward the far corner, on the neutral overlay surface, so
// the border's gradient is the only thing that changes. The thin width, as a hairline is too faint
// to judge the angle by.
BoxStyles BorderGradientStyle(const ThemeContext& theme) {
	BoxStyles style {.fill         = theme.colors.surface.overlay,
	                 .border_width = theme.border.thin,
	                 .radius       = theme.radius.md};
	style.border_gradient = LinearGradient {
	    Palette::BLUE_700, Palette::BLUE_700.WithAlpha(CLEAR_ALPHA), GRADIENT_DIAGONAL};
	return style;
}

// An accent gradient whose hover lightens it and turns it from running top to bottom to running
// left to right, so the hover shows both its colours and its angle blending.
BoxStyles HoverGradientStyle(const ThemeContext& theme) {
	BoxStyles style = GradientStyle(theme, {Palette::BLUE_400, Palette::BLUE_300});
	style.states.hover.fill_gradient =
	    LinearGradient {Palette::BLUE_600, Palette::BLUE_400, GRADIENT_ACROSS};
	return style;
}

// The theme's shine running to clear down the overlay surface, the sheen a lit surface catches,
// captioned in `caption_color`. The sheen is a box stacked over the surface, as a gradient fill
// takes the place of a fill colour. The shine fades through its own alpha, so its middle keeps
// its colour rather than turning grey.
AnyElement SheenSample(const ThemeContext& theme, Color caption_color) {
	const Color                     shine = theme.colors.effect.shine;
	const std::array<AnyElement, 2> layers {
	    Box({.self = {.styles = {.fill   = theme.colors.surface.overlay,
	                             .radius = theme.radius.md}}}),
	    Box({.self = {.styles = GradientStyle(theme, {shine.WithAlpha(Opacity::BORDER_STRONG),
	                                                  shine.WithAlpha(CLEAR_ALPHA)})}}),
	};
	return Captioned(theme,
	                 Box({.layout = {.stacked = true}, .self = {.layout = SAMPLE_SIZE}}, layers),
	                 "Sheen", caption_color);
}

// The gradient specimens, the default direction, an angle, a translucent sheen, a gradient border,
// and a gradient at rest beside it held in its hover.
AnyElement GradientsRow(const ThemeContext& theme) {
	const BoxStyles                 hover = HoverGradientStyle(theme);
	const std::array<AnyElement, 6> samples {
	    Sample(theme, GradientStyle(theme, ACCENT_RUN), "Top to bottom"),
	    Sample(theme, GradientStyle(theme, ACCENT_ACROSS), "Angle 90"),
	    SheenSample(theme, theme.colors.text.secondary),
	    Sample(theme, BorderGradientStyle(theme), "Border"),
	    StateSample(theme, hover, {}, "Rest"),
	    StateSample(theme, hover, {.hover = FULL_WEIGHT}, "Hover"),
	};
	return Box({.layout = {.gap = theme.space.s4}}, samples);
}

// The state style at rest and focused, and the sheen and gradient border, on a bright frame, so
// each can be judged against a bright game frame rather than the backdrop. The focus ring is judged
// where its light ring has nothing but its glow against what is behind it. The frame stands in
// for a bright game frame, not a surface of the UI, so it takes the palette's white and no surface
// token. Its padding is more than the ring and glow reach, so they stay on the white.
AnyElement BrightRow(const ThemeContext& theme) {
	const BoxStyles style = StatesStyle(theme);
	// Dark text, as the theme's secondary text wouldn't read on the white.
	const Color                     caption = theme.colors.text.secondary_on_light;
	const std::array<AnyElement, 4> samples {
	    StateSample(theme, style, {}, "Rest", caption),
	    StateSample(theme, style, {.focus = FULL_WEIGHT}, "Focus", caption),
	    SheenSample(theme, caption),
	    StateSample(theme, BorderGradientStyle(theme), {}, "Border", caption),
	};
	return Box({.layout = {.gap = theme.space.s6, .padding = DpInsets(theme.space.s6)},
	            .self   = {.styles = {.fill = Palette::GRAY_900, .radius = theme.radius.md}}},
	           samples);
}

// A sample that fades in, stays, fades out and stays out, turning on a steady rhythm read from the
// frame's clock, so the theme's fade can be watched with no input and nothing kept by the page.
// Only the box fades, so its caption stays readable and its space stays put.
AnyElement FadeSample(const ThemeContext& theme, Color fill) {
	const ClockContext::Clock::duration elapsed = CurrentClockContext().time.time_since_epoch();
	const bool                          shown   = (elapsed / FADE_HALF_CYCLE) % 2 == 0;
	return Captioned(theme,
	                 Animate({.fade = Fade {.shown = shown}},
	                         Box({.self = {.layout = SAMPLE_SIZE,
	                                       .styles = {.fill = fill, .radius = theme.radius.md}}})),
	                 "Fade");
}

// A sample that arrives as a toast does, sliding in from the right as it fades in each time the
// fade sample does, then fading away in place on the same rhythm, so the theme's slide can be
// watched with no input. A slide plays only the first time it is built, so each turn builds it
// under a key of its own, which makes it a new arrival, and the old one has already faded out
// when the new one starts. With reduced motion it fades in where it is.
AnyElement SlideSample(const ThemeContext& theme, Color fill) {
	const ClockContext::Clock::duration elapsed = CurrentClockContext().time.time_since_epoch();
	const auto                          turn    = static_cast<uint64_t>(elapsed / FADE_CYCLE);
	const bool                          shown   = (elapsed / FADE_HALF_CYCLE) % 2 == 0;
	return Captioned(theme,
	                 Animate({.fade  = Fade {.shown = shown},
	                          .slide = Slide {.x = theme.space.s6},
	                          .key   = Key(SLIDE_SAMPLE_KEY, turn)},
	                         Box({.self = {.layout = SAMPLE_SIZE,
	                                       .styles = {.fill = fill, .radius = theme.radius.md}}})),
	                 "Slide");
}

// A sample that dims to a low opacity and brightens again on the fade's rhythm, so the theme's
// tween can be watched turning between two targets. It starts at its target, so unlike the fade it
// never shows an entrance.
AnyElement TweenSample(const ThemeContext& theme, Color fill) {
	const ClockContext::Clock::duration elapsed = CurrentClockContext().time.time_since_epoch();
	const float target = (elapsed / FADE_HALF_CYCLE) % 2 == 0 ? TWEEN_HIGH : TWEEN_LOW;
	return Captioned(theme,
	                 Animate({.tween = Tween {.target = target}},
	                         Box({.self = {.layout = SAMPLE_SIZE,
	                                       .styles = {.fill = fill, .radius = theme.radius.md}}})),
	                 "Tween");
}

// The scroll specimens' sizes in design units. Each scroller is about three times shorter than
// its content, so its indicator is about a third of its track and plainly moves.
constexpr Dp     SCROLLER_WIDTH  = Dp(280.0f);
constexpr Dp     COLUMN_HEIGHT   = Dp(160.0f);
constexpr Dp     LIST_ROW_HEIGHT = Dp(32.0f);
constexpr size_t LIST_ROWS       = 12;
constexpr Dp     CHIPS_HEIGHT    = Dp(40.0f);
constexpr Dp     CHIP_WIDTH      = Dp(72.0f);
constexpr size_t CHIPS           = 10;
// Every third row is tinted, so the list's movement shows even where its labels look alike.
constexpr size_t TINT_EVERY = 3;
// Room for a row's or chip's label.
constexpr size_t SCROLL_LABEL_SIZE = 16;

// A labelled row of the scrolling list, tinted when `tinted`.
AnyElement ListRow(const ThemeContext& theme, size_t number, bool tinted) {
	std::array<char, SCROLL_LABEL_SIZE> label {};
	std::snprintf(label.data(), label.size(), "Row %zu", number);
	const std::array<AnyElement, 1> text {Text(
	    {.text = label.data(),
	     .self = {.styles = {.type = theme.type.body_small, .color = theme.colors.text.primary}}})};
	return Box({.layout = {.align = Align::Center, .padding = DpInsets(Dp(0.0f), theme.space.s2)},
	            .self   = {.layout = {.height = LIST_ROW_HEIGHT},
	                       .styles = {.fill   = tinted ? theme.colors.accent.tint : Color {},
	                                  .radius = theme.radius.sm}}},
	           text);
}

// A column of rows taller than it is, which scrolls with the wheel. The rows stretch across it,
// so the tinted ones read as stripes, and its right padding keeps them clear of the indicator.
AnyElement ScrollingList(const ThemeContext& theme) {
	std::array<AnyElement, LIST_ROWS> rows;
	for (size_t index = 0; index < rows.size(); ++index) {
		rows[index] = ListRow(theme, index + 1, index % TINT_EVERY == TINT_EVERY - 1);
	}
	return Box({.layout = {.direction = FlexDirection::Column,
	                       .align     = Align::Stretch,
	                       .gap       = theme.space.s1,
	                       .padding   = DpInsets(theme.space.s2, theme.space.s4, theme.space.s2,
	                                             theme.space.s2)},
	            .self   = {.layout = {.width = SCROLLER_WIDTH, .height = COLUMN_HEIGHT},
	                       .styles = {.fill         = theme.colors.surface.overlay,
	                                  .border       = theme.colors.border.subtle,
	                                  .border_width = theme.border.thin,
	                                  .radius       = theme.radius.md,
	                                  .overflow     = Overflow::Scroll}}},
	           rows);
}

// A labelled chip of the scrolling row, alternating between a surface and an accent tint so its
// place along the row reads at a glance.
AnyElement Chip(const ThemeContext& theme, size_t number) {
	std::array<char, SCROLL_LABEL_SIZE> label {};
	std::snprintf(label.data(), label.size(), "Chip %zu", number);
	const std::array<AnyElement, 1> text {Text(
	    {.text = label.data(),
	     .self = {.styles = {.type = theme.type.caption, .color = theme.colors.text.primary}}})};
	return Box({.layout = {.justify = Justify::Center, .align = Align::Center},
	            .self   = {.layout = {.width = CHIP_WIDTH, .align_self = Align::Stretch},
	                       .styles = {.fill   = number % 2 == 0 ? theme.colors.accent.tint
	                                                            : theme.colors.surface.raised,
	                                  .radius = RADIUS_FULL}}},
	           text);
}

// A row of chips wider than it is, which scrolls with the horizontal wheel, or the wheel with
// Shift held.
AnyElement ScrollingChips(const ThemeContext& theme) {
	std::array<AnyElement, CHIPS> chips;
	for (size_t index = 0; index < chips.size(); ++index) {
		chips[index] = Chip(theme, index + 1);
	}
	return Box({.layout = {.gap = theme.space.s2, .padding = DpInsets(theme.space.s2)},
	            .self   = {.layout = {.width = SCROLLER_WIDTH, .height = CHIPS_HEIGHT},
	                       .styles = {.fill         = theme.colors.surface.overlay,
	                                  .border       = theme.colors.border.subtle,
	                                  .border_width = theme.border.thin,
	                                  .radius       = theme.radius.md,
	                                  .overflow     = Overflow::Scroll}}},
	           chips);
}

// The scrolling list and chips side by side, each captioned with how to scroll it.
AnyElement ScrollRow(const ThemeContext& theme) {
	const std::array<AnyElement, 2> specimens {
	    Captioned(theme, ScrollingList(theme), "Scroll with the mouse wheel"),
	    Captioned(theme, ScrollingChips(theme), "Hold Shift and scroll the wheel"),
	};
	return Box({.layout = {.gap = theme.space.s6}}, specimens);
}

// A glow's radius and strength in one value.
std::string GlowText(const Glow& glow) {
	return LengthText(glow.radius.value) + ", " + PercentText(glow.peak_alpha);
}

// A sample glowing with `glow` in `color` on a `fill` surface, in a well wide enough for the whole
// glow, captioned with `caption` under the well rather than under the sample, so the glow doesn't
// reach over the caption.
AnyElement GlowSample(const ThemeContext& theme, const Glow& glow, Color color, Color fill,
                      std::string_view caption) {
	const AnyElement sample = Box({.self = {.layout = SAMPLE_SIZE,
	                                        .styles = {.fill   = fill,
	                                                   .radius = theme.radius.md,
	                                                   .glow   = {.glow = glow, .color = color}}}});
	const AnyElement well =
	    Box({.layout = {.padding = DpInsets(glow.radius)},
	         .self = {.styles = {.fill = theme.colors.surface.overlay, .radius = theme.radius.md}}},
	        {sample});
	return Captioned(theme, well, caption);
}

// The theme's glows on samples, the trophy glow in gold on the tile a trophy toast shows without
// an icon and the emphasis glow in the success colour, beside their tokens. They sit in a row of
// their own, as a glow reaches past its box over whatever is beside it.
AnyElement GlowRow(const ThemeContext& theme) {
	const AnyElement samples =
	    Box({.layout = {.align = Align::Start, .gap = theme.space.s6}},
	        {GlowSample(theme, theme.glow.trophy, theme.colors.trophy.gold,
	                    theme.colors.surface.inset, "glow.trophy in gold"),
	         GlowSample(theme, theme.glow.emphasis, theme.colors.status.success,
	                    theme.colors.surface.raised, "glow.emphasis in success")});
	return BesideTokens(
	    theme, samples,
	    Lines(theme, {TokenLine(theme, "styles.glow", "BoxGlow {.glow, .color}"),
	                  TokenLine(theme, "glow.trophy", GlowText(theme.glow.trophy)),
	                  TokenLine(theme, "glow.emphasis", GlowText(theme.glow.emphasis))}));
}

} // namespace

AnyElement BuildBoxPage() {
	const ThemeContext& theme = CurrentThemeContext();
	// The samples share one fill, so the only difference between them is the property each shows.
	const Color                     sample_fill = theme.colors.accent.fill;
	const std::array<AnyElement, 8> samples {
	    Sample(theme, {.fill = sample_fill}, "Fill"),
	    Sample(theme, {.fill = sample_fill, .radius = theme.radius.md}, "Radius"),
	    Sample(theme,
	           {.fill         = sample_fill,
	            .border       = theme.colors.border.strong,
	            .border_width = theme.border.thin},
	           "Border"),
	    StackedSample(theme, sample_fill),
	    FadeSample(theme, sample_fill),
	    SlideSample(theme, sample_fill),
	    TweenSample(theme, sample_fill),
	    // Last, so no sample is painted after it for its shadow to cover.
	    ShadowWell(theme, theme.shadows.low, "shadows.low", sample_fill, SAMPLE_SIZE),
	};
	return Page({.title = "Box", .subtitle = RULE},
	            {
	                Section(theme, "Fill, radius, border, stacking and motion",
	                        Box({.layout = {.gap = theme.space.s4}}, samples)),
	                Section(theme, "Glow", GlowRow(theme)),
	                Section(theme, "Gradients", GradientsRow(theme)),
	                Section(theme, "States", StatesRow(theme)),
	                Section(theme, "On a bright frame", BrightRow(theme)),
	                Section(theme, "Scroll", ScrollRow(theme)),
	            });
}

} // namespace Libs::Graphics::SystemUi::Gallery
