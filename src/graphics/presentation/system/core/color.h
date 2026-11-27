#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_COLOR_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_COLOR_H_

#include <cstdint>

namespace Libs::Graphics::SystemUi {

/// `value` within 0..1, with NaN as 0. Used for alphas, opacities and progress.
[[nodiscard]] constexpr float ClampUnit(float value) {
	return value > 0.0f ? (value < 1.0f ? value : 1.0f) : 0.0f;
}

/// A colour with 8-bit channels and straight (not premultiplied) alpha.
struct Color {
	uint8_t r = 0;
	uint8_t g = 0;
	uint8_t b = 0;
	uint8_t a = 0;

	/// The same colour with its alpha replaced by `alpha` in 0..1. Values outside that range are
	/// clamped, and NaN gives zero, so the byte conversion is always defined.
	[[nodiscard]] constexpr Color WithAlpha(float alpha) const {
		constexpr float MAX_CHANNEL = 255.0f;
		constexpr float ROUND_HALF  = 0.5f;
		return {r, g, b, static_cast<uint8_t>(ClampUnit(alpha) * MAX_CHANNEL + ROUND_HALF)};
	}

	/// The same colour with its alpha multiplied by `factor` in 0..1, as an animation's opacity
	/// fades a whole element. A factor outside that range is clamped, and NaN gives zero.
	[[nodiscard]] constexpr Color ScaleAlpha(float factor) const {
		constexpr float MAX_CHANNEL = 255.0f;
		return WithAlpha(static_cast<float>(a) / MAX_CHANNEL * ClampUnit(factor));
	}

	constexpr bool operator==(const Color&) const = default;
};

/// An opaque colour from 0xRRGGBB.
[[nodiscard]] constexpr Color Rgb(uint32_t rgb) {
	constexpr uint32_t RED_SHIFT   = 16u;
	constexpr uint32_t GREEN_SHIFT = 8u;
	constexpr uint32_t CHANNEL     = 0xffu;
	return {static_cast<uint8_t>((rgb >> RED_SHIFT) & CHANNEL),
	        static_cast<uint8_t>((rgb >> GREEN_SHIFT) & CHANNEL),
	        static_cast<uint8_t>(rgb & CHANNEL), static_cast<uint8_t>(CHANNEL)};
}

/// The colour `t` of the way from `from` to `to`, channel by channel, alpha included. `t` is
/// clamped to 0..1, and NaN gives `from`.
[[nodiscard]] constexpr Color Lerp(Color from, Color to, float t) {
	constexpr float ROUND_HALF = 0.5f;
	const float     clamped    = ClampUnit(t);
	const auto      channel    = [clamped](uint8_t start, uint8_t end) {
        return static_cast<uint8_t>(
            static_cast<float>(start) +
            (static_cast<float>(end) - static_cast<float>(start)) * clamped + ROUND_HALF);
	};
	return {channel(from.r, to.r), channel(from.g, to.g), channel(from.b, to.b),
	        channel(from.a, to.a)};
}

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_COLOR_H_
