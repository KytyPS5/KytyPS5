#include "graphics/presentation/system/gallery/pages/parts/format.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

// An opaque channel, and the scale of a percentage.
constexpr float MAX_CHANNEL = 255.0f;
constexpr float PERCENT     = 100.0f;
// Added before flooring, so a value rounds to the nearest with halves up.
constexpr float ROUND_HALF = 0.5f;

// Room for the longest text each formatter writes, with its terminator.
constexpr size_t TEXT_SIZE = 48;

// `fraction` as a whole percentage, rounded to the nearest, halves up.
int WholePercent(float fraction) {
	return static_cast<int>(std::floor(fraction * PERCENT + ROUND_HALF));
}

} // namespace

std::string HexText(Color color) {
	std::array<char, TEXT_SIZE> text {};
	if (color.a == static_cast<uint8_t>(MAX_CHANNEL)) {
		(void)std::snprintf(text.data(), text.size(), "#%02X%02X%02X", color.r, color.g, color.b);
	} else {
		(void)std::snprintf(text.data(), text.size(), "#%02X%02X%02X %d%%", color.r, color.g,
		                    color.b, WholePercent(static_cast<float>(color.a) / MAX_CHANNEL));
	}
	return text.data();
}

std::string LengthText(float length) {
	std::array<char, TEXT_SIZE> text {};
	const bool                  whole = std::floor(length) == length;
	(void)std::snprintf(text.data(), text.size(), whole ? "%.0f" : "%.1f",
	                    static_cast<double>(length));
	return text.data();
}

std::string TypeMetricsText(const TypeStyle& style) {
	std::array<char, TEXT_SIZE> text {};
	(void)std::snprintf(text.data(), text.size(), "%s / %s %s, at least %s",
	                    LengthText(style.size.value).c_str(), LengthText(style.line.value).c_str(),
	                    style.weight == FontWeight::Bold ? "Bold" : "Regular",
	                    LengthText(style.min_size).c_str());
	return text.data();
}

std::string MsText(float milliseconds) {
	return LengthText(milliseconds) + " ms";
}

std::string PercentText(float fraction) {
	return std::to_string(WholePercent(fraction)) + "%";
}

std::string RatioText(float ratio) {
	// Cut down to tenths, as WCAG ratios are never rounded up to a pass.
	constexpr float             TENTHS = 10.0f;
	std::array<char, TEXT_SIZE> text {};
	(void)std::snprintf(text.data(), text.size(), "%.1f:1",
	                    static_cast<double>(std::floor(ratio * TENTHS) / TENTHS));
	return text.data();
}

} // namespace Libs::Graphics::SystemUi::Gallery
