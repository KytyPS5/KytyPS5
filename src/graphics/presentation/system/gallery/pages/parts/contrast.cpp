#include "graphics/presentation/system/gallery/pages/parts/contrast.h"

#include <algorithm>
#include <cmath>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

// An opaque channel, and the half added before truncating so a blend rounds to the nearest.
constexpr float MAX_CHANNEL = 255.0f;
constexpr float ROUND_HALF  = 0.5f;

// The sRGB transfer curve and the luminance weights, from the WCAG definition of relative
// luminance.
constexpr float SRGB_LINEAR_END = 0.04045f;
constexpr float SRGB_LINEAR_DIV = 12.92f;
constexpr float SRGB_OFFSET     = 0.055f;
constexpr float SRGB_DIV        = 1.055f;
constexpr float SRGB_POWER      = 2.4f;
constexpr float RED_WEIGHT      = 0.2126f;
constexpr float GREEN_WEIGHT    = 0.7152f;
constexpr float BLUE_WEIGHT     = 0.0722f;
// Added to both luminances, so a ratio against black stays finite.
constexpr float RATIO_OFFSET = 0.05f;

// `channel` from 0 to 255 in sRGB, as linear light from 0 to 1.
float Linear(uint8_t channel) {
	const float value = static_cast<float>(channel) / MAX_CHANNEL;
	return value <= SRGB_LINEAR_END ? value / SRGB_LINEAR_DIV
	                                : std::pow((value + SRGB_OFFSET) / SRGB_DIV, SRGB_POWER);
}

// How bright `color` is to the eye, from 0 to 1.
float Luminance(Color color) {
	return RED_WEIGHT * Linear(color.r) + GREEN_WEIGHT * Linear(color.g) +
	       BLUE_WEIGHT * Linear(color.b);
}

// The text levels on each surface they are read on. The HUD rows use the secondary level, the
// least a HUD should show.
void AddTextRows(const SemanticColors& colors, std::vector<ContrastRow>& rows) {
	const Color raised = colors.surface.raised;
	rows.push_back({"Primary text on raised", colors.text.primary, raised});
	rows.push_back({"Secondary text on raised", colors.text.secondary, raised});
	rows.push_back({"Tertiary text on raised", colors.text.tertiary, raised});
	rows.push_back({"Primary text on base", colors.text.primary, colors.surface.base});
	rows.push_back({"Secondary text on base", colors.text.secondary, colors.surface.base});
	rows.push_back({"Primary text on overlay", colors.text.primary, colors.surface.overlay});
	rows.push_back({"Secondary text on HUD", colors.text.secondary, colors.surface.hud});
	rows.push_back(
	    {"Secondary text on strong HUD", colors.text.secondary, colors.surface.hud_strong});
	rows.push_back({"Text on light, on white", colors.text.on_light, Palette::GRAY_900});
	rows.push_back(
	    {"Secondary text on light, on white", colors.text.secondary_on_light, Palette::GRAY_900});
}

// The status colours as text on a raised surface, and on their own tints laid on one, as a
// toast's type bar and badges are.
void AddStatusRows(const SemanticColors& colors, std::vector<ContrastRow>& rows) {
	const Color         raised = colors.surface.raised;
	const StatusColors& status = colors.status;
	rows.push_back({"Success text on raised", status.success, raised});
	rows.push_back({"Warning text on raised", status.warning, raised});
	rows.push_back({"Error text on raised", status.error, raised});
	rows.push_back({"Info text on raised", status.info, raised});
	rows.push_back({"Success text on its tint, on raised", status.success, status.success_tint,
	                CONTRAST_TEXT, raised});
	rows.push_back({"Warning text on its tint, on raised", status.warning, status.warning_tint,
	                CONTRAST_TEXT, raised});
	rows.push_back({"Error text on its tint, on raised", status.error, status.error_tint,
	                CONTRAST_TEXT, raised});
	rows.push_back(
	    {"Info text on its tint, on raised", status.info, status.info_tint, CONTRAST_TEXT, raised});
}

// The parts that aren't text, judged against the lower target.
void AddPartRows(const SemanticColors& colors, std::vector<ContrastRow>& rows) {
	const Color raised = colors.surface.raised;
	rows.push_back({"Accent line on raised, not text", colors.accent.line, raised, CONTRAST_LARGE});
	rows.push_back({"Focus ring on raised, not text", colors.focus.ring, raised, CONTRAST_LARGE});
	rows.push_back(
	    {"Strong border on raised, not text", colors.border.strong, raised, CONTRAST_LARGE});
}

} // namespace

Color Composite(Color top, Color under) {
	const float alpha = static_cast<float>(top.a) / MAX_CHANNEL;
	const auto  blend = [alpha](uint8_t over, uint8_t below) {
        return static_cast<uint8_t>(static_cast<float>(over) * alpha +
		                             static_cast<float>(below) * (1.0f - alpha) + ROUND_HALF);
	};
	return {blend(top.r, under.r), blend(top.g, under.g), blend(top.b, under.b),
	        static_cast<uint8_t>(MAX_CHANNEL)};
}

float ContrastRatio(Color first, Color second) {
	const float one   = Luminance(first);
	const float other = Luminance(second);
	return (std::max(one, other) + RATIO_OFFSET) / (std::min(one, other) + RATIO_OFFSET);
}

float ContrastOver(const ContrastRow& row, Color frame) {
	const Color floor      = row.under.a > 0 ? Composite(row.under, frame) : frame;
	const Color background = Composite(row.background, floor);
	return ContrastRatio(Composite(row.foreground, background), background);
}

bool ContrastPasses(const ContrastRow& row) {
	return std::min(ContrastOver(row, CONTRAST_BLACK_FRAME),
	                ContrastOver(row, CONTRAST_WHITE_FRAME)) >= row.target;
}

std::vector<ContrastRow> ContrastRows(const ThemeContext& theme) {
	std::vector<ContrastRow> rows;
	AddTextRows(theme.colors, rows);
	AddStatusRows(theme.colors, rows);
	AddPartRows(theme.colors, rows);
	return rows;
}

} // namespace Libs::Graphics::SystemUi::Gallery
