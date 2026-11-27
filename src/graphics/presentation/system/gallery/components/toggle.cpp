#include "graphics/presentation/system/gallery/components/toggle.h"

#include "graphics/presentation/system/animation.h"
#include "graphics/presentation/system/core.h"

#include <cstdint>
#include <string_view>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

// The switch's track and knob in design units, a track wide enough for the knob to travel a
// knob's width.
constexpr Dp TRACK_WIDTH  = Dp(56.0f);
constexpr Dp TRACK_HEIGHT = Dp(32.0f);
constexpr Dp KNOB_SIZE    = Dp(24.0f);

// How many low bits of the knob's key index count the flips, all of a flip count's, so the toggle's
// own key above them never meets them.
constexpr uint64_t FLIPS_BITS = 32;
// The name the knob's slide is keyed under, with its toggle's key and flips.
constexpr std::string_view KNOB_KEY = "toggle knob";

// The switch's track, grey with a strong edge when off and the accent when on, each with the
// theme's hover and press.
BoxStyles TrackStyle(const ThemeContext& theme, bool on) {
	const AccentColors& accent = theme.colors.accent;
	BoxStyles           style {.fill         = on ? accent.fill : theme.colors.control.track_off,
	                           .border       = on ? accent.line : theme.colors.border.strong,
	                           .border_width = theme.border.hair,
	                           .radius       = RADIUS_FULL};
	if (on) {
		style.states.hover.fill   = accent.fill_hover;
		style.states.pressed.fill = accent.fill_pressed;
	} else {
		style.states.hover.overlay   = theme.colors.state.hover;
		style.states.pressed.overlay = theme.colors.state.pressed;
	}
	return style;
}

// The switch's knob, which slides in from the side it left each time the switch flips, as its key
// changes with every flip. The toggle's own key sits above the flips, so two toggles never share
// a slide. With reduced motion it appears in its place.
AnyElement Knob(const ThemeContext& theme, bool on, uint32_t flips, uint32_t toggle_key) {
	const float      travel = (TRACK_WIDTH - KNOB_SIZE - 2.0f * theme.space.s1).value;
	const Dp         from   = Dp(on ? -travel : travel);
	const AnyElement knob =
	    Box({.self = {.layout = {.width = KNOB_SIZE, .height = KNOB_SIZE},
	                  .styles = {.fill = theme.colors.control.knob, .radius = RADIUS_FULL}}});
	// The first state needs no slide, as nothing flipped it there.
	if (flips == 0) {
		return knob;
	}
	const uint64_t index = (static_cast<uint64_t>(toggle_key) << FLIPS_BITS) | flips;
	return Animate({.slide = Slide {.x = from}, .key = Key(KNOB_KEY, index)}, knob);
}

} // namespace

BoxElement Toggle(const ToggleProps& props) {
	const ThemeContext& theme = CurrentThemeContext();
	const bool          on    = props.on;
	const Color         text  = on ? theme.colors.text.primary : theme.colors.text.secondary;
	const AnyElement    label =
	    Text({.text = props.label,
	          .self = {.layout = {.grow = 1.0f},
	                   .styles = {.type = theme.type.body_small, .color = text}}});
	const AnyElement track = Box({.layout = {.justify = on ? Justify::End : Justify::Start,
	                                         .align   = Align::Center,
	                                         .padding = DpInsets(theme.space.s1)},
	                              .self = {.layout = {.width = TRACK_WIDTH, .height = TRACK_HEIGHT},
	                                       .styles = TrackStyle(theme, on),
	                                       .state  = props.state}},
	                             {Knob(theme, on, props.flips, props.key)});
	BoxStyles        target {.radius = theme.radius.sm};
	target.states.hover.overlay   = theme.colors.state.hover;
	target.states.pressed.overlay = theme.colors.state.pressed;
	return Box(
	    {.layout = {.align   = Align::Center,
	                .gap     = theme.space.s2,
	                .padding = DpInsets(Dp(), theme.space.s4)},
	     .self   = {.layout = {.height = theme.space.s7}, .styles = target, .state = props.state}},
	    {label, track});
}

} // namespace Libs::Graphics::SystemUi::Gallery
