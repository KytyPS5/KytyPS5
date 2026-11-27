#include "graphics/presentation/system/gallery/pages/elevationsPage.h"

#include "graphics/presentation/system/animation.h"
#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/core/pointer.h"
#include "graphics/presentation/system/gallery/components/page.h"
#include "graphics/presentation/system/gallery/pages/parts/format.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

constexpr std::string_view RULE =
    "Raise a surface only when it floats over what is under it, as a toast floats over a game. "
    "Focus lifts a control with a ring and a soft glow, so it reads on bright and dark frames.";

// The size of the shadow and focus samples, in design units.
constexpr Dp         SAMPLE_WIDTH  = Dp(120.0f);
constexpr Dp         SAMPLE_HEIGHT = Dp(80.0f);
constexpr ItemLayout SAMPLE_SIZE {.width = SAMPLE_WIDTH, .height = SAMPLE_HEIGHT};
// A state's full weight, so the focus sample shows its ring at its strongest, and as far as the
// hover sample's hover goes.
constexpr float FULL_WEIGHT = 1.0f;
// The layer samples' cards, and how far the second group of each sample is moved down and right,
// less than a card, so the two groups overlap.
constexpr Dp LAYER_CARD_WIDTH    = Dp(112.0f);
constexpr Dp LAYER_CARD_HEIGHT   = Dp(64.0f);
constexpr Dp LAYER_OFFSET_DOWN   = Dp(32.0f);
constexpr Dp LAYER_OFFSET_ACROSS = Dp(56.0f);
// How many cards each layer sample holds, two in each of its two groups.
constexpr size_t LAYER_CARD_COUNT = 4;

// A layer as a CSS box-shadow entry is read, its offsets, blur and spread and then its colour.
std::string LayerText(const ShadowLayer& layer) {
	return LengthText(layer.offset_x.value) + " " + LengthText(layer.offset_y.value) + " " +
	       LengthText(layer.blur.value) + " " + LengthText(layer.spread.value) + " " +
	       HexText(layer.color);
}

// One of the theme's shadows, called `name` in the code, in the shared well beside `use`, what it
// is for, and each of its layers.
AnyElement ShadowRow(const ThemeContext& theme, const std::string& name, const Shadow& shadow,
                     std::string_view use) {
	std::vector<AnyElement> lines {TokenLine(theme, name, use)};
	for (size_t index = 0; index < shadow.Count(); ++index) {
		lines.push_back(TokenLine(theme, name + ".layers[" + std::to_string(index) + "]",
		                          LayerText(shadow.layers[index])));
	}
	return Box({.layout = {.align = Align::Start, .gap = theme.space.s6}},
	           {ShadowWell(theme, shadow, name, theme.colors.surface.raised, SAMPLE_SIZE),
	            Lines(theme, lines)});
}

// Each of the theme's shadows, the low one first, as it is the one components draw.
AnyElement ShadowRows(const ThemeContext& theme) {
	return Box({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s6}},
	           {ShadowRow(theme, "shadows.low", theme.shadows.low, "cards and toasts"),
	            ShadowRow(theme, "shadows.medium", theme.shadows.medium, "menus"),
	            ShadowRow(theme, "shadows.high", theme.shadows.high, "dialogs")});
}

// The hover sample's element this frame, where it was placed last frame, and its hover, kept
// between frames as the sidebar keeps its rows.
struct HoverSample {
	BoxElement built;
	Rect       rect;
	// Eased from absolute times, so a stalled frame neither freezes nor skips it.
	TweenValue hover;
};
HoverSample g_hover_sample;

// How far the hover sample's hover has come this frame, heading for 1 while the pointer is over
// where it was drawn last frame and for 0 otherwise. It moves at an even pace over the theme's
// hover time in each direction, turning around from wherever it is, so the shadow moves rather
// than jumps.
float HoverWeight(const ThemeContext& theme) {
	const std::optional<Vec2>& pointer = CurrentPointerContext().position;
	const bool                 over    = pointer && g_hover_sample.rect.Contains(*pointer);
	const TimePoint            now     = CurrentClockContext().time;
	const Track even {.duration_ms = theme.interaction.hover_ms, .ease = Ease::Linear};
	g_hover_sample.hover.Retarget(over ? FULL_WEIGHT : 0.0f, {.rise = even, .fall = even}, now);
	return g_hover_sample.hover.Value(now);
}

// A card-like sample resting on the low shadow that lifts to the medium one while the pointer is
// over it, in a well with room below for the medium shadow, so a hover's lift can be tried.
AnyElement HoverLift(const ThemeContext& theme) {
	BoxStyles styles {.fill         = theme.colors.surface.raised,
	                  .border       = theme.colors.border.subtle,
	                  .border_width = theme.border.hair,
	                  .radius       = theme.radius.md,
	                  .shadow       = theme.shadows.low};
	styles.states.hover.shadow = theme.shadows.medium;
	g_hover_sample.built =
	    Box({.self = {
	             .layout = SAMPLE_SIZE, .styles = styles, .state = {.hover = HoverWeight(theme)}}});
	const Dp below = Dp(
	    std::max(theme.space.s6.value, ShadowReach(theme.shadows.medium) + theme.space.s3.value));
	const AnyElement well =
	    Box({.layout = {.padding = DpInsets(theme.space.s6, theme.space.s6, below, theme.space.s6)},
	         .self = {.styles = {.fill = theme.colors.surface.overlay, .radius = theme.radius.md}}},
	        {Captioned(theme, g_hover_sample.built, "Hover me")});
	return Box(
	    {.layout = {.align = Align::Start, .gap = theme.space.s6}},
	    {well, Lines(theme, {TokenLine(theme, "at rest", "shadows.low"),
	                         TokenLine(theme, "states.hover.shadow", "shadows.medium"),
	                         TokenLine(theme, "eased over", MsText(theme.interaction.hover_ms))})});
}

// One card of the layer samples, named by `name`, on `layer`, with a band in `band` along its top
// so the four cards are told apart at a glance.
AnyElement LayerCard(const ThemeContext& theme, std::string_view name, uint8_t layer, Color band) {
	const std::string label = std::string(name) + ", layer " + std::to_string(layer);
	return Box({.layout = {.justify = Justify::Center, .align = Align::Center},
	            .self   = {.layout = {.width = LAYER_CARD_WIDTH, .height = LAYER_CARD_HEIGHT},
	                       .styles = {.fill         = theme.colors.surface.raised,
	                                  .border       = theme.colors.border.subtle,
	                                  .border_width = theme.border.hair,
	                                  .radius       = theme.radius.md,
	                                  .layer        = layer,
	                                  .shadow       = theme.shadows.low,
	                                  .edge         = {.color = band, .width = theme.border.bar}}}},
	           {Text({.text = label,
	                  .self = {.styles = {.type  = theme.type.body_small,
	                                      .color = theme.colors.text.primary}}})});
}

// Two groups of two cards in one stack, the second offset down and right so it overlaps the
// first, with the cards on `layers`, A and B in the first group and C and D in the second. Each
// group is its own box, so a card rising over the other group shows layers reach across parents.
AnyElement LayerPanel(const ThemeContext&                          theme,
                      const std::array<uint8_t, LAYER_CARD_COUNT>& layers) {
	const SemanticColors& colors = theme.colors;
	const auto            group  = [&](DpInsets offset, const Children& cards) {
        return Box({.layout = {.padding = offset, .gap = theme.space.s2}}, cards);
	};
	const AnyElement first =
	    group(DpInsets {}, {LayerCard(theme, "A", layers[0], colors.status.info),
	                        LayerCard(theme, "B", layers[1], colors.status.success)});
	const AnyElement second = group(DpInsets(LAYER_OFFSET_DOWN, Dp(), Dp(), LAYER_OFFSET_ACROSS),
	                                {LayerCard(theme, "C", layers[2], colors.status.warning),
	                                 LayerCard(theme, "D", layers[3], colors.status.error)});
	// Room below for the cards' shadow, so it doesn't reach under the caption.
	const Dp below = Dp(ShadowReach(theme.shadows.low));
	return Box({.layout = {.padding = DpInsets(Dp(), Dp(), below, Dp()), .stacked = true}},
	           {first, second});
}

// The same cards on one layer, where the second group covers the first as it is built after it,
// beside them on layers, where B rises out of the first group over the whole second group and C
// on layer 1 still covers A on layer 0.
AnyElement LayerSamples(const ThemeContext& theme) {
	const AnyElement samples = Box(
	    {.layout = {.align = Align::Start, .gap = theme.space.s8}},
	    {Captioned(theme, LayerPanel(theme, {0, 0, 0, 0}), "All on layer 0, built A B C D"),
	     Captioned(theme, LayerPanel(theme, {0, 2, 1, 0}), "B on 2 and C on 1, drawn A D C B")});
	return BesideTokens(
	    theme, samples,
	    Lines(theme, {TokenLine(theme, "styles.layer", "0 to 255, 0 the lowest"),
	                  TokenLine(theme, "a child", "drawn on its box's layer or above"),
	                  TokenLine(theme, "one layer", "drawn in tree order")}));
}

// A sample on the raised surface held in focus, its ring, keyline and glow showing, with their
// tokens.
AnyElement FocusGlow(const ThemeContext& theme) {
	BoxStyles style {.fill = theme.colors.surface.raised, .radius = theme.radius.md};
	style.states.focus.focus_ring = true;
	const AnyElement sample =
	    Box({.self = {.layout = SAMPLE_SIZE, .styles = style, .state = {.focus = FULL_WEIGHT}}});
	return Box(
	    {.layout = {.align = Align::Center, .gap = theme.space.s6}},
	    {Box({.layout = {.padding = DpInsets(theme.space.s6)}}, {sample}),
	     Lines(
	         theme,
	         {TokenLine(theme, "glow.focus.radius", LengthText(theme.glow.focus.radius.value)),
	          TokenLine(theme, "glow.focus.peak_alpha", PercentText(theme.glow.focus.peak_alpha)),
	          TokenLine(theme, "border.thick, the ring", LengthText(theme.border.thick.value)),
	          TokenLine(theme, "border.hair, the keyline", LengthText(theme.border.hair.value))})});
}

} // namespace

void RememberElevationsHover(const FrameBuilder& frame) {
	// A handle from a frame the page wasn't built in has no layout, so the last place is kept. The
	// hover goes back to rest, so a sample left lifted when the page was left starts low on the
	// way back and eases up again rather than showing already lifted.
	if (const auto* layout = frame.LayoutOf(g_hover_sample.built)) {
		g_hover_sample.rect = layout->arranged;
	} else {
		g_hover_sample.hover = TweenValue {};
	}
}

AnyElement BuildElevationsPage() {
	const ThemeContext& theme = CurrentThemeContext();
	return Page({.title = "Elevations", .subtitle = RULE},
	            {Section(theme, "Shadows", ShadowRows(theme)),
	             Section(theme, "Lifting on hover", HoverLift(theme)),
	             Section(theme, "Layers", LayerSamples(theme)),
	             Section(theme, "Focus", FocusGlow(theme))});
}

} // namespace Libs::Graphics::SystemUi::Gallery
