#include "graphics/presentation/system/element/primitives/box/interactionStates.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace Libs::Graphics::SystemUi {

namespace {

// How strong each state is, from 0 to 1.
struct Weights {
	float selected = 0.0f;
	float hover    = 0.0f;
	float focus    = 0.0f;
	float pressed  = 0.0f;
	float disabled = 0.0f;
};

// How strong each state is for `input`. Selected and disabled are on or off, so they weigh 0 or 1,
// and the highlights weigh as far as they have come, held to 0 through 1 with NaN as 0.
Weights WeightsOf(const StateInput& input) {
	return {input.selected ? 1.0f : 0.0f, ClampUnit(input.hover), ClampUnit(input.focus),
	        ClampUnit(input.pressed), input.disabled ? 1.0f : 0.0f};
}

// The states with their weights in the order they apply.
using Ordered = std::array<std::pair<const StateStyle*, float>, BOX_STATE_COUNT>;

// The states paired with their weights, in the merge order.
Ordered InOrder(const StateStyles& states, const Weights& weights) {
	return {{{&states.selected, weights.selected},
	         {&states.hover, weights.hover},
	         {&states.focus, weights.focus},
	         {&states.pressed, weights.pressed},
	         {&states.disabled, weights.disabled}}};
}

// The value `t` of the way from `from` to `to`, which is `from` at 0 and `to` at 1.
float Mix(float from, float to, float t) {
	return from + (to - from) * t;
}
// The same for colours, blended channel by channel by Lerp.
Color Mix(Color from, Color to, float t) {
	return Lerp(from, to, t);
}
// The same for lengths, so Blend treats a Dp like a float.
Dp Mix(Dp from, Dp to, float t) {
	return Dp(Mix(from.value, to.value, t));
}
// The same for shadows, layer by layer. A layer only one side has is matched with the same layer
// gone clear, so it fades in or out where it lies rather than growing from nothing at the corner,
// and a shadow fades in from none as a state comes on.
Shadow Mix(const Shadow& from, const Shadow& to, float t) {
	// At either end the shadow is that one exactly, so a state at rest adds no clear layers to
	// draw for nothing.
	if (!(t > 0.0f)) {
		return from;
	}
	if (t >= 1.0f) {
		return to;
	}
	const auto layer_or_clear = [](const Shadow& stack, const Shadow& other, size_t index) {
		if (index < stack.Count()) {
			return stack.layers[index];
		}
		ShadowLayer clear = other.layers[index];
		clear.color       = clear.color.WithAlpha(0.0f);
		return clear;
	};
	Shadow mixed;
	mixed.count = static_cast<uint8_t>(std::max(from.Count(), to.Count()));
	for (size_t index = 0; index < mixed.count; ++index) {
		const ShadowLayer start = layer_or_clear(from, to, index);
		const ShadowLayer end   = layer_or_clear(to, from, index);
		mixed.layers[index]     = {.offset_x = Mix(start.offset_x, end.offset_x, t),
		                           .offset_y = Mix(start.offset_y, end.offset_y, t),
		                           .blur     = Mix(start.blur, end.blur, t),
		                           .spread   = Mix(start.spread, end.spread, t),
		                           .color    = Mix(start.color, end.color, t)};
	}
	return mixed;
}

constexpr float DEGREES_PER_TURN = 360.0f;

// The same for gradients, end by end, with the angle turning the short way round, so one at 350
// moving toward one at 10 passes through 0 rather than sweeping back through 180. The angle is
// left as it lands, 370 rather than 10, as wrapping it would also change a style's own angle
// where no state moves it, and every whole turn draws the same.
LinearGradient Mix(const LinearGradient& from, const LinearGradient& to, float t) {
	const float turn = std::remainder(to.angle_degrees - from.angle_degrees, DEGREES_PER_TURN);
	return {Lerp(from.from, to.from, t), Lerp(from.to, to.to, t), from.angle_degrees + turn * t};
}

// Whether a value is a real number. A colour always is, and a shadow is when every length is.
bool IsFinite(float value) {
	return std::isfinite(value);
}
bool IsFinite(Dp length) {
	return std::isfinite(length.value);
}
bool IsFinite(Color /*color*/) {
	return true;
}
bool IsFinite(const LinearGradient& gradient) {
	return IsFinite(gradient.angle_degrees);
}
// A shadow is when every length of every layer it holds is.
bool IsFinite(const Shadow& shadow) {
	for (size_t index = 0; index < shadow.Count(); ++index) {
		const ShadowLayer& layer = shadow.layers[index];
		if (!IsFinite(layer.offset_x) || !IsFinite(layer.offset_y) || !IsFinite(layer.blur) ||
		    !IsFinite(layer.spread)) {
			return false;
		}
	}
	return true;
}

// Whether a state's value can be blended. A non-finite one would make the blend NaN even at a
// weight of 0, and so every state after it, so it is skipped as if the state set nothing.
template <class T>
bool Usable(const std::optional<T>& value) {
	return value.has_value() && IsFinite(*value);
}

// `base` moved toward the value each state sets for `field`, in the merge order. Hover and focus
// that set the same value approach it once, at the larger of their weights, so blending the two
// of them is the same as blending either alone at that weight.
template <class T>
T Blend(T base, const StateStyles& states, std::optional<T> StateStyle::* field,
        const Weights& weights) {
	T          value  = base;
	const auto toward = [&](const StateStyle& state, float weight) {
		if (Usable(state.*field)) {
			value = Mix(value, *(state.*field), weight);
		}
	};
	toward(states.selected, weights.selected);
	const auto& hover = states.hover.*field;
	const auto& focus = states.focus.*field;
	if (Usable(hover) && Usable(focus) && *hover == *focus) {
		value = Mix(value, *hover, std::max(weights.hover, weights.focus));
	} else {
		toward(states.hover, weights.hover);
		toward(states.focus, weights.focus);
	}
	toward(states.pressed, weights.pressed);
	toward(states.disabled, weights.disabled);
	return value;
}

// The fields of a fill or a border that a state can set, its plain colour and its gradient.
struct PaintFields {
	std::optional<Color> StateStyle::* color;
	std::optional<LinearGradient> StateStyle::* gradient;
};

// What `state` moves a gradient toward, its own gradient when it sets one it can use, otherwise
// its plain colour as a gradient of that one colour at `angle`, or nothing when it sets neither.
std::optional<LinearGradient> GradientTarget(const StateStyle& state, const PaintFields& fields,
                                             float angle) {
	if (Usable(state.*fields.gradient)) {
		return *(state.*fields.gradient);
	}
	if (Usable(state.*fields.color)) {
		const Color color = *(state.*fields.color);
		return LinearGradient {color, color, angle};
	}
	return std::nullopt;
}

// The angle a fill or border's gradient starts at, the base gradient's, or else that of the first
// state with any weight that sets one. Nothing when neither gives it a gradient.
std::optional<float> StartingAngle(const std::optional<LinearGradient>& base,
                                   const Ordered& ordered, const PaintFields& fields) {
	if (base) {
		return base->angle_degrees;
	}
	for (const auto& [state, weight]: ordered) {
		if (weight > 0.0f && Usable(state->*fields.gradient)) {
			return (state->*fields.gradient)->angle_degrees;
		}
	}
	return std::nullopt;
}

// A fill or border as a gradient, `base`, or `base_color` as a gradient of that one colour, moved
// toward what each state sets in the merge order, as Blend moves a plain value. Hover and focus
// with the same target approach it once, at the larger of their weights. Nothing when neither the
// style nor a state with any weight gives it a gradient, so it stays a plain colour.
std::optional<LinearGradient> BlendGradient(Color                                base_color,
                                            const std::optional<LinearGradient>& base,
                                            const StateStyles& states, const PaintFields& fields,
                                            const Weights& weights) {
	const std::optional<float> angle = StartingAngle(base, InOrder(states, weights), fields);
	if (!angle) {
		return std::nullopt;
	}
	LinearGradient value  = base.value_or(LinearGradient {base_color, base_color, *angle});
	const auto     toward = [&](const StateStyle& state, float weight) {
        if (const auto target = GradientTarget(state, fields, value.angle_degrees)) {
            value = Mix(value, *target, weight);
        }
	};
	toward(states.selected, weights.selected);
	const auto hover = GradientTarget(states.hover, fields, value.angle_degrees);
	const auto focus = GradientTarget(states.focus, fields, value.angle_degrees);
	if (hover && focus && *hover == *focus) {
		value = Mix(value, *hover, std::max(weights.hover, weights.focus));
	} else {
		toward(states.hover, weights.hover);
		toward(states.focus, weights.focus);
	}
	toward(states.pressed, weights.pressed);
	toward(states.disabled, weights.disabled);
	return value;
}

// The overlays, the focus ring and the selection the states ask for, at their weights.
void CollectDrawing(const Ordered& ordered, ResolvedStyle& resolved) {
	for (const auto& [state, weight]: ordered) {
		if (weight <= 0.0f) {
			continue;
		}
		if (state->overlay.a > 0) {
			resolved.overlays[resolved.overlay_count++] = {state->overlay, weight};
		}
		if (state->focus_ring) {
			resolved.focus_ring = std::max(resolved.focus_ring, weight);
		}
		if (state->selection) {
			resolved.selection = std::max(resolved.selection, weight);
		}
	}
}

} // namespace

ResolvedStyle ResolveStyle(const ThemeContext& theme, const BoxStyles& styles,
                           const StateInput& input) {
	const Weights weights = WeightsOf(input);
	// Every field blends from the same states by the same weights, so only base and field vary.
	const auto blend = [&styles, &weights]<class T>(T base, std::optional<T> StateStyle::* field) {
		return Blend(base, styles.states, field, weights);
	};
	ResolvedStyle resolved {.fill         = blend(styles.fill, &StateStyle::fill),
	                        .border       = blend(styles.border, &StateStyle::border),
	                        .border_width = blend(styles.border_width, &StateStyle::border_width),
	                        .radius       = blend(styles.radius, &StateStyle::radius),
	                        .shadow       = blend(styles.shadow, &StateStyle::shadow),
	                        .opacity      = blend(1.0f, &StateStyle::opacity)};
	if (input.disabled && !Usable(styles.states.disabled.opacity)) {
		resolved.opacity *= theme.opacity.disabled;
	}
	resolved.fill_gradient =
	    BlendGradient(styles.fill, styles.fill_gradient, styles.states,
	                  {&StateStyle::fill, &StateStyle::fill_gradient}, weights);
	resolved.border_gradient =
	    BlendGradient(styles.border, styles.border_gradient, styles.states,
	                  {&StateStyle::border, &StateStyle::border_gradient}, weights);
	CollectDrawing(InOrder(styles.states, weights), resolved);
	return resolved;
}

} // namespace Libs::Graphics::SystemUi
