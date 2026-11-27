#include "graphics/presentation/system/element/primitives/box/paint.h"

#include "graphics/presentation/system/core/context.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/frame/framePaint.h"
#include "graphics/presentation/system/element/primitives/box/box.h"
#include "graphics/presentation/system/element/primitives/box/interactionStates.h"
#include "graphics/presentation/system/element/primitives/box/scroll.h"
#include "graphics/presentation/system/imgui/draw/box.h"
#include "graphics/presentation/system/imgui/draw/edge.h"
#include "graphics/presentation/system/imgui/draw/highlight.h"
#include "graphics/presentation/system/imgui/draw/shadow.h"

#include <algorithm>
#include <cmath>

namespace Libs::Graphics::SystemUi {

namespace {

// DrawBox takes design units and converts them itself, so a box's design units pass through as
// they are and round as every other drawing does.
DrawBoxStyle ToDrawBoxStyle(const ResolvedStyle& resolved) {
	return {.fill            = resolved.fill,
	        .radius          = resolved.radius.value,
	        .border          = resolved.border,
	        .border_width    = resolved.border_width.value,
	        .fill_gradient   = resolved.fill_gradient,
	        .border_gradient = resolved.border_gradient};
}

// The box's fill and border, with its edge band between them when it has one, so the border still
// draws the outline over the band. Without a band it is one DrawBox, as before.
void DrawBody(ImDrawList& list, const Rect& rect, const ResolvedStyle& resolved,
              const EdgeBand& edge, float opacity) {
	const DrawBoxStyle body = ToDrawBoxStyle(resolved);
	if (edge.width.value <= 0.0f || edge.color.a == 0) {
		DrawBox(list, rect, body, opacity);
		return;
	}
	DrawBox(list, rect,
	        {.fill = body.fill, .radius = body.radius, .fill_gradient = body.fill_gradient},
	        opacity);
	DrawEdgeBand(list, rect, body.radius, edge.width.value, edge.color, opacity);
	DrawBox(list, rect,
	        {.fill            = {},
	         .radius          = body.radius,
	         .border          = body.border,
	         .border_width    = body.border_width,
	         .border_gradient = body.border_gradient},
	        opacity);
}

// The overlays the box's states ask for, over its fill and border, each faded by its state's
// weight and rounded as the box is.
void DrawOverlays(ImDrawList& list, const Rect& rect, const ResolvedStyle& resolved,
                  float opacity) {
	for (size_t index = 0; index < resolved.overlay_count; ++index) {
		const StateOverlay& overlay = resolved.overlays[index];
		// An overlay tints what is under it and has no border of its own.
		DrawBox(list, rect,
		        {.fill   = overlay.color.ScaleAlpha(overlay.weight),
		         .radius = resolved.radius.value,
		         .border = {}},
		        opacity);
	}
}

// The theme's focus ring. Its lengths pass through as design units, as a box's do.
FocusRingStyle FocusRingOf(const ThemeContext& theme) {
	return {.ring          = theme.colors.focus.ring,
	        .keyline       = theme.colors.focus.keyline,
	        .glow          = theme.colors.focus.glow,
	        .ring_width    = theme.border.thick.value,
	        .keyline_width = theme.border.hair.value,
	        .glow_radius   = theme.glow.focus.radius.value,
	        .glow_peak     = theme.glow.focus.peak_alpha};
}

// The theme's selection.
SelectionStyle SelectionOf(const ThemeContext& theme) {
	return {.tint      = theme.colors.accent.tint,
	        .bar       = theme.colors.accent.line,
	        .bar_width = theme.border.bar.value};
}

// Draws `style` under `rect` as CSS draws a blur. A CSS blur is a Gaussian centred on the edge,
// rising from about `blur` inside the box to `blur` outside it and half strength at the edge, so
// the halo starts `blur` inside the box and spreads twice that, where the smoothstep falloff
// follows the Gaussian's curve closely. On a box smaller than twice the blur the inset stops at
// the middle, so the shadow reaches a little further than CSS and is full under the centre, which
// a box that small never shows.
void DrawBlurredShadow(ImDrawList& list, const Rect& rect, float radius, ShadowStyle style,
                       float opacity) {
	const float blur = std::max(0.0f, style.blur);
	// The inset can't pass the box's middle, or the inner rectangle would turn inside out.
	const float inset =
	    std::min(CurrentScaleContext().Px(blur), std::min(rect.width, rect.height) / 2.0f);
	const Rect inner {rect.x + inset, rect.y + inset, rect.width - 2.0f * inset,
	                  rect.height - 2.0f * inset};
	style.blur = 2.0f * blur;
	DrawShadow(list, inner, std::max(0.0f, radius - blur), style, opacity);
}

// Draws the layers of `shadow` under a box, the last first, so the first listed is on top as in
// CSS. Each layer's colour carries its own strength.
void DrawBoxShadow(ImDrawList& list, const Rect& rect, float radius, const Shadow& shadow,
                   float opacity) {
	const ScaleContext& scale = CurrentScaleContext();
	for (size_t index = shadow.Count(); index > 0; --index) {
		const ShadowLayer& layer = shadow.layers[index - 1];
		// A length that isn't a number would put every vertex of the layer at NaN, so it is left
		// out.
		if (!std::isfinite(layer.spread.value) || !std::isfinite(layer.blur.value) ||
		    !std::isfinite(layer.offset_x.value) || !std::isfinite(layer.offset_y.value)) {
			continue;
		}
		// The spread grows the shape on every side, its corners with it, before the blur. A
		// negative one can't shrink it past its middle.
		const float grow =
		    std::max(scale.Px(layer.spread.value), -std::min(rect.width, rect.height) / 2.0f);
		const Rect spread {rect.x - grow, rect.y - grow, rect.width + 2.0f * grow,
		                   rect.height + 2.0f * grow};
		DrawBlurredShadow(list, spread, std::max(0.0f, radius + layer.spread.value),
		                  {.color      = layer.color,
		                   .blur       = layer.blur.value,
		                   .offset_x   = layer.offset_x.value,
		                   .offset_y   = layer.offset_y.value,
		                   .peak_alpha = 1.0f},
		                  opacity);
	}
}

// How far inside the box its focus ring starts, the width of the border it draws, so the ring
// takes the border's place rather than sitting outside it as a second edge. A border DrawBox
// doesn't draw, as it is clear or has no width, is nothing to cover.
float FocusCover(const ResolvedStyle& resolved) {
	const bool seen =
	    resolved.border_gradient ? resolved.border_gradient->Visible() : resolved.border.a > 0;
	const bool drawn = seen && resolved.border_width.value > 0.0f;
	return drawn ? resolved.border_width.value : 0.0f;
}

// How wide in pixels the border DrawBox draws is, nothing for a border it doesn't draw.
float BorderPixels(const ResolvedStyle& resolved) {
	const float cover = FocusCover(resolved);
	return cover > 0.0f ? CurrentScaleContext().Border(cover) : 0.0f;
}

// The theme's scroll indicator in pixels for a box at `rect` with `resolved` style and a border
// `border` pixels wide. Each end of its track keeps clear of the curve inside the box's rounded
// corners, as far as ScrollIndicatorRect finds room.
IndicatorSizes IndicatorSizesOf(const ThemeContext& theme, const Rect& rect,
                                const ResolvedStyle& resolved, float border) {
	const ScaleContext& scale  = CurrentScaleContext();
	const float         inset  = scale.Px(theme.space.hair.value);
	const float         radius = BoxRounding(rect, resolved.radius.value);
	return {.thickness  = scale.Px(theme.space.s1.value),
	        .inset      = inset,
	        .min_length = scale.Px(theme.space.s6.value),
	        .corner     = std::max(0.0f, radius - border - inset)};
}

} // namespace

void PaintBox(const FrameBuilder& frame, AnyElement box, ImDrawList& list,
              const PaintState& state) {
	const BoxProps*                       props  = frame.FindAs<BoxProps>(box);
	const FrameView::ElementLayout* const layout = frame.LayoutOf(box);
	if (props == nullptr || layout == nullptr) {
		return;
	}
	const ThemeContext& theme    = CurrentThemeContext();
	const ResolvedStyle resolved = ResolveStyle(theme, props->self.styles, props->self.state);
	const Rect&         rect     = layout->arranged;
	const float         radius   = resolved.radius.value;
	// The shadow goes first, under the body. The selection sits on the body under the overlays,
	// and the ring goes on top, so nothing the box draws covers it. The ring lies over the border's
	// band when the box draws a border, and otherwise just outside the box.
	DrawBoxShadow(list, rect, radius, resolved.shadow, state.opacity);
	// The glow goes over the shadow, so the shadow can't darken it, and under the body.
	const BoxGlow& glow = props->self.styles.glow;
	DrawGlow(
	    list, rect, radius,
	    {.color = glow.color, .radius = glow.glow.radius.value, .peak_alpha = glow.glow.peak_alpha},
	    state.opacity);
	DrawBody(list, rect, resolved, props->self.styles.edge, state.opacity);
	if (resolved.selection > 0.0f) {
		DrawSelection(list, rect, radius, SelectionOf(theme), resolved.selection * state.opacity);
	}
	DrawOverlays(list, rect, resolved, state.opacity);
	if (resolved.focus_ring > 0.0f) {
		DrawFocusRing(list, rect, radius, FocusRingOf(theme), resolved.focus_ring * state.opacity,
		              FocusCover(resolved));
	}
}

PaintState InheritBox(const FrameBuilder& frame, AnyElement box, const PaintState& parent) {
	const BoxProps* props = frame.FindAs<BoxProps>(box);
	if (props == nullptr) {
		return parent;
	}
	// Resolved again rather than kept from paint, as the walk asks before it paints and the merge
	// is a few blends. A state can ask for more than full opacity, which must not brighten the
	// subtree, so it is held to 0 through 1 before it multiplies.
	const ResolvedStyle resolved =
	    ResolveStyle(CurrentThemeContext(), props->self.styles, props->self.state);
	PaintState own = parent;
	own.opacity *= ClampUnit(resolved.opacity);
	return own;
}

bool ClipBox(const FrameBuilder& frame, AnyElement box, Rect& out) {
	const BoxProps*                       props  = frame.FindAs<BoxProps>(box);
	const FrameView::ElementLayout* const layout = frame.LayoutOf(box);
	if (props == nullptr || layout == nullptr || props->self.styles.overflow == Overflow::Visible) {
		return false;
	}
	out = layout->arranged;
	if (props->self.styles.overflow == Overflow::Scroll) {
		// Inside the border, so scrolled content passes under the border rather than over it.
		const ResolvedStyle resolved =
		    ResolveStyle(CurrentThemeContext(), props->self.styles, props->self.state);
		out = out.Inset(EdgeInsets(BorderPixels(resolved)));
	}
	return true;
}

void PaintBoxOver(const FrameBuilder& frame, AnyElement box, ImDrawList& list,
                  const PaintState& state) {
	const BoxProps* props = frame.FindAs<BoxProps>(box);
	// Only a box that scrolls has an indicator, so others skip the search for its metrics.
	if (props == nullptr || props->self.styles.overflow != Overflow::Scroll) {
		return;
	}
	const FrameView::ElementLayout* const layout  = frame.LayoutOf(box);
	const ScrollMetrics*                  metrics = FindScrollMetrics(frame, box);
	if (layout == nullptr || metrics == nullptr) {
		return;
	}
	const ThemeContext&       theme    = CurrentThemeContext();
	const ResolvedStyle       resolved = ResolveStyle(theme, props->self.styles, props->self.state);
	const Rect&               rect     = layout->arranged;
	const float               border   = BorderPixels(resolved);
	const std::optional<Rect> indicator = ScrollIndicatorRect(
	    rect.Inset(EdgeInsets(border)), *metrics, IndicatorSizesOf(theme, rect, resolved, border));
	if (indicator.has_value()) {
		DrawBox(list, *indicator, {.fill = theme.colors.border.strong, .radius = RADIUS_FULL.value},
		        state.opacity);
	}
}

} // namespace Libs::Graphics::SystemUi
