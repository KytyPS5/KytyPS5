#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_SCALE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_SCALE_H_

#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/core/theme.h"

#include <algorithm>

namespace Libs::Graphics::SystemUi {

/// The user's preferences that change how the UI is sized. Values that are not finite fall back
/// to these defaults.
struct ScaleOptions {
	// Multiplies the scale, within the theme's user scale range.
	float user_scale = 1.0f;
	// The safe area inset as a percentage of the screen, within the theme's range. The caller
	// picks it. The theme suggests ScaleRules::default_safe_area_percent in fullscreen, where a
	// TV may crop the edges, and 0 in a window. Zero still keeps the minimum edge distance.
	float safe_area_percent = 0.0f;
};

/// Converts design units to the output's pixels. Build one whenever the output size or the
/// options change, then use it for the whole frame. The rules must pass ScaleRules::IsValid().
class ScaleContext {
public:
	/// Computes the scale, the safe area and the content frame for an output of `width` by
	/// `height` pixels. An empty, negative or non-finite size gives the minimum scale and empty
	/// areas.
	ScaleContext(const ScaleRules& rules, float width, float height, ScaleOptions options = {});

	/// The factor from design units to output pixels.
	[[nodiscard]] float Scale() const { return m_scale; }

	/// A length, spacing or radius in output pixels, rounded to a whole pixel.
	[[nodiscard]] float Px(float reference) const { return SnapPixel(reference * m_scale); }

	/// A border width in output pixels. It never drops below one pixel, so a border token can't
	/// express "no border". Skip drawing the border instead.
	[[nodiscard]] float Border(float reference) const { return std::max(1.0f, Px(reference)); }

	/// The pixel size of a text style, never smaller than the style's minimum size.
	[[nodiscard]] float FontSize(const TypeStyle& style) const {
		return std::max(Px(style.size.value), style.min_size);
	}

	/// The whole output.
	[[nodiscard]] Rect Screen() const { return m_screen; }

	/// The output minus the inset that keeps content clear of the edges, on whole pixels.
	[[nodiscard]] Rect SafeArea() const { return m_safe_area; }

	/// The safe area, narrowed and centred when it is wider than the theme's content aspect. Menus
	/// and dialogs sit in it so they don't stretch across ultrawide screens. Narrower outputs get
	/// the whole safe area.
	[[nodiscard]] Rect ContentFrame() const { return m_content_frame; }

private:
	float m_scale;
	Rect  m_screen;
	Rect  m_safe_area;
	Rect  m_content_frame;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_SCALE_H_
