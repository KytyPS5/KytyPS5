#include "graphics/presentation/system/gallery/pages/animationsPage.h"

#include "graphics/presentation/system/animation.h"
#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/gallery/components/page.h"
#include "graphics/presentation/system/gallery/pages/parts/format.h"
#include "graphics/presentation/system/gallery/pages/parts/motionTrack.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

constexpr std::string_view RULE =
    "Animate fades, slides, tweens and shines what it holds, keeping its space so nothing around "
    "it moves. Motion explains a change and is never the only sign of it, and with reduced motion "
    "a slide stays put, a shine doesn't sweep and a fade is cut short.";

// A specimen's size and how far a slide enters from, in design units.
constexpr Dp SAMPLE_WIDTH   = Dp(120.0f);
constexpr Dp SAMPLE_HEIGHT  = Dp(80.0f);
constexpr Dp SLIDE_DISTANCE = Dp(48.0f);
// How faint the tween falls to when its loop is on the way back, so it never vanishes.
constexpr float TWEEN_LOW = 0.3f;

// Which of Animate's animations a specimen shows.
enum class Kind : uint8_t {
	Fade,
	Slide,
	Tween,
	FadeAndSlide,
	Shine,
};

// The bits of the index a specimen's Animate is keyed by under SPECIMEN_KEY.
// The lowest bit is whether it runs under reduced motion and the next three its kind, so the
// loop's count can sit above them without the specimens' keys meeting.
constexpr uint64_t REDUCED_BIT = 1;
constexpr uint64_t KIND_SHIFT  = 1;
constexpr uint64_t LOOP_SHIFT  = 4;
// How many flips of the loop one shine takes, a sweep and then a rest, so each sweep reads as the
// passing moment it is in a toast.
constexpr uint64_t SHINE_FLIPS = 2;
// Where the held shines stand, across a sample from left to right.
constexpr std::array<float, 3> HELD_SHINES {0.25f, 0.5f, 0.75f};
// Every shine token is a float or a length held in one, so a field added to the shine and not
// listed in the token lines fails to build.
constexpr size_t SHINE_FIELDS = 5;
static_assert(sizeof(ShineTokens) == SHINE_FIELDS * sizeof(float));
// The name every specimen's Animate is keyed under, with the bits above.
constexpr std::string_view SPECIMEN_KEY = "specimen";

// The interaction tokens and how each is written.
enum class Unit : uint8_t {
	Ms,
	Length,
	Percent,
};

struct NamedInteraction {
	std::string_view name;
	float InteractionTokens::* value;
	Unit                       unit = Unit::Ms;
};

constexpr std::array<NamedInteraction, 11> INTERACTIONS {
    NamedInteraction {"interaction.focus_in_ms", &InteractionTokens::focus_in_ms},
    NamedInteraction {"interaction.focus_out_ms", &InteractionTokens::focus_out_ms},
    NamedInteraction {"interaction.hover_ms", &InteractionTokens::hover_ms},
    NamedInteraction {"interaction.press_ms", &InteractionTokens::press_ms},
    NamedInteraction {"interaction.press_scale", &InteractionTokens::press_scale, Unit::Percent},
    NamedInteraction {"interaction.knob_ms", &InteractionTokens::knob_ms},
    NamedInteraction {"interaction.indicator_ms", &InteractionTokens::indicator_ms},
    NamedInteraction {"interaction.repeat_delay_ms", &InteractionTokens::repeat_delay_ms},
    NamedInteraction {"interaction.repeat_rate_ms", &InteractionTokens::repeat_rate_ms},
    NamedInteraction {"interaction.min_gamepad_target", &InteractionTokens::min_gamepad_target,
                      Unit::Length},
    NamedInteraction {"interaction.min_pointer_target", &InteractionTokens::min_pointer_target,
                      Unit::Length}};
// The interaction tokens are all floats, so a field added to them and not shown here fails to
// build.
static_assert(sizeof(InteractionTokens) == INTERACTIONS.size() * sizeof(float));

// The props `kind` animates with now, by the loop over motion.slower. A fade or tween keeps one
// key, so flipping its target moves it from wherever it is. A slide only enters, so its key
// changes at every flip to enter again, and a fade with a slide changes key each time it comes
// back, so it arrives as a toast does and leaves in place. A shine also only plays once, so its key
// changes every SHINE_FLIPS flips to sweep again after a rest.
AnimateProps PropsOf(Kind kind, float loop_ms, bool reduced) {
	const bool     out   = LoopIsOut(loop_ms);
	const uint64_t flips = LoopFlips(loop_ms);
	const uint64_t base = (static_cast<uint64_t>(kind) << KIND_SHIFT) | (reduced ? REDUCED_BIT : 0);
	const Slide    slide {.x = SLIDE_DISTANCE};
	switch (kind) {
		case Kind::Fade: return {.fade = Fade {.shown = out}, .key = Key(SPECIMEN_KEY, base)};
		case Kind::Slide:
			return {.slide = slide, .key = Key(SPECIMEN_KEY, base | (flips << LOOP_SHIFT))};
		case Kind::Tween:
			return {.tween = Tween {.target = out ? 1.0f : TWEEN_LOW},
			        .key   = Key(SPECIMEN_KEY, base)};
		case Kind::Shine:
			return {.shine = Shine {},
			        .key   = Key(SPECIMEN_KEY, base | ((flips / SHINE_FLIPS) << LOOP_SHIFT))};
		case Kind::FadeAndSlide: break;
	}
	// Two flips make one round, so the key changes as the specimen comes back in.
	return {.fade  = Fade {.shown = out},
	        .slide = slide,
	        .key   = Key(SPECIMEN_KEY, base | ((flips / 2) << LOOP_SHIFT))};
}

// The sample every animation on the page plays on, one accent box, so the samples differ only in
// how they move.
AnyElement AccentSample(const ThemeContext& theme) {
	return Box({.self = {.layout = {.width = SAMPLE_WIDTH, .height = SAMPLE_HEIGHT},
	                     .styles = {.fill = theme.colors.accent.fill, .radius = theme.radius.md}}});
}

// One specimen of `kind`, the accent sample animating as the loop says, with or without reduced
// motion, which it opens a scope for so the rest of the page keeps the player's.
AnyElement Specimen(const ThemeContext& theme, Kind kind, bool reduced) {
	const ContextContainer& context = CurrentContext();
	const ContextScope      scope(context.theme, context.scale, context.fonts, context.clock,
	                              {.reduced = reduced || context.motion.reduced}, context.pointer);
	return Animate(PropsOf(kind, theme.motion.slower, reduced), AccentSample(theme));
}

// `kind` as set beside the same under reduced motion, each captioned, with the lines that say how
// it moves.
AnyElement Pair(const ThemeContext& theme, Kind kind, const Children& lines) {
	return Box({.layout = {.align = Align::Center, .gap = theme.space.s8}},
	           {Captioned(theme, Specimen(theme, kind, false), "As set"),
	            Captioned(theme, Specimen(theme, kind, true), "Reduced motion"),
	            Lines(theme, lines)});
}

// The fade's lines, its props and the theme's timings.
AnyElement FadeRow(const ThemeContext& theme) {
	return Pair(theme, Kind::Fade,
	            {TokenLine(theme, "props", "fade = Fade {.shown}"),
	             TokenLine(theme, "in, Ease::Enter", MsText(theme.motion.base)),
	             TokenLine(theme, "out, Ease::Exit", MsText(theme.motion.exit_fade)),
	             TokenLine(theme, "reduced, at most", MsText(theme.motion.reduced_fade_max))});
}

// The slide's lines. It only enters, so the loop builds it anew to show it again.
AnyElement SlideRow(const ThemeContext& theme) {
	return Pair(theme, Kind::Slide,
	            {TokenLine(theme, "props", "slide = Slide {.x}"),
	             TokenLine(theme, "from", LengthText(SLIDE_DISTANCE.value) + " to the right"),
	             TokenLine(theme, "in, Ease::Enter", MsText(theme.motion.base)),
	             TokenLine(theme, "reduced", "stays in place")});
}

// The tween's lines, an opacity eased toward a target and back.
AnyElement TweenRow(const ThemeContext& theme) {
	return Pair(theme, Kind::Tween,
	            {TokenLine(theme, "props", "tween = Tween {.target}"),
	             TokenLine(theme, "targets", PercentText(1.0f) + " and " + PercentText(TWEEN_LOW)),
	             TokenLine(theme, "rising, Ease::Enter", MsText(theme.motion.base)),
	             TokenLine(theme, "falling, Ease::Exit", MsText(theme.motion.exit_fade))});
}

// A fade and a slide on one element, arriving together as a toast does and leaving in place.
AnyElement FadeAndSlideRow(const ThemeContext& theme) {
	return Pair(theme, Kind::FadeAndSlide,
	            {TokenLine(theme, "props", "fade and slide"),
	             TokenLine(theme, "arrives", "fading in as it slides"),
	             TokenLine(theme, "leaves", "fading out in place")});
}

// A shine sweeping across the sample once every SHINE_FLIPS flips of the loop, beside the same
// under reduced motion, where it doesn't sweep.
AnyElement ShineRow(const ThemeContext& theme) {
	return Pair(theme, Kind::Shine,
	            {TokenLine(theme, "props", "shine = Shine {}"),
	             TokenLine(theme, "across, Ease::Standard", MsText(theme.motion.shine)),
	             TokenLine(theme, "reduced", "doesn't sweep")});
}

// The shine held still at a few places across a sample, so its band can be looked at closely,
// also with reduced motion, beside the shine's tokens.
AnyElement HeldShineRow(const ThemeContext& theme) {
	std::vector<AnyElement> samples;
	for (const float position: HELD_SHINES) {
		samples.push_back(
		    Captioned(theme, Animate({.shine = Shine {.position = position}}, AccentSample(theme)),
		              "held at " + PercentText(position)));
	}
	const ShineTokens& shine = theme.shine;
	return BesideTokens(
	    theme, Box({.layout = {.align = Align::Start, .gap = theme.space.s6}}, samples),
	    Lines(
	        theme,
	        {TokenLine(theme, "props", "shine = Shine {.position}"),
	         TokenLine(theme, "shine.angle_degrees", LengthText(shine.angle_degrees) + " degrees"),
	         TokenLine(theme, "shine.width_fraction", PercentText(shine.width_fraction)),
	         TokenLine(theme, "shine.min_width", LengthText(shine.min_width.value)),
	         TokenLine(theme, "shine.peak_alpha", PercentText(shine.peak_alpha)),
	         TokenLine(theme, "shine.plateau_fraction", PercentText(shine.plateau_fraction))}));
}

// `value` written in `unit`.
std::string UnitText(Unit unit, float value) {
	switch (unit) {
		case Unit::Length: return LengthText(value);
		case Unit::Percent: return PercentText(value);
		case Unit::Ms: break;
	}
	return MsText(value);
}

// How controls are meant to respond to focus, the pointer, presses and a held direction. No
// component reads these yet, so the page lists them rather than showing them.
AnyElement Interaction(const ThemeContext& theme) {
	std::vector<AnyElement> lines;
	for (const NamedInteraction& named: INTERACTIONS) {
		lines.push_back(
		    TokenLine(theme, named.name, UnitText(named.unit, theme.interaction.*named.value)));
	}
	return Lines(theme, lines);
}

} // namespace

AnyElement BuildAnimationsPage() {
	const ThemeContext& theme = CurrentThemeContext();
	return Page({.title = "Animations", .subtitle = RULE},
	            {Section(theme, "Fade", FadeRow(theme)), Section(theme, "Slide", SlideRow(theme)),
	             Section(theme, "Tween", TweenRow(theme)),
	             Section(theme, "Fade and slide, as a toast arrives", FadeAndSlideRow(theme)),
	             Section(theme, "Shine", ShineRow(theme)),
	             Section(theme, "Shine, held still", HeldShineRow(theme)),
	             Section(theme, "Interaction, not used by any component yet", Interaction(theme))});
}

} // namespace Libs::Graphics::SystemUi::Gallery
