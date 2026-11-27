#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_UNITS_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_UNITS_H_

#include "graphics/presentation/system/core/context.h"
#include "graphics/presentation/system/core/dp.h"
#include "graphics/presentation/system/core/geometry.h"

namespace Libs::Graphics::SystemUi {

// The conversions below use the current context's scale, so they need an open ContextScope.

/// `length` in output pixels at the current scale, rounded to a whole pixel.
[[nodiscard]] inline float ToPixels(Dp length) {
	return CurrentScaleContext().Px(length.value);
}

/// The design length that converts back to exactly `pixels` whole pixels at the current scale. For
/// a length worked out in output pixels, such as one measured from text, which would otherwise be
/// rounded a second time. A fractional pixel count is rounded by the conversion, as any length is.
[[nodiscard]] inline Dp FromPixels(float pixels) {
	return Dp(pixels / CurrentScaleContext().Scale());
}

/// A border width in output pixels at the current scale, never below one pixel.
[[nodiscard]] inline float ToBorderPixels(Dp width) {
	return CurrentScaleContext().Border(width.value);
}

/// `insets` in output pixels at the current scale, each side rounded on its own.
[[nodiscard]] inline EdgeInsets ToPixels(const DpInsets& insets) {
	// Looked up once, as each lookup reads the thread's current context.
	const ScaleContext& scale = CurrentScaleContext();
	return EdgeInsets(scale.Px(insets.top.value), scale.Px(insets.right.value),
	                  scale.Px(insets.bottom.value), scale.Px(insets.left.value));
}

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_UNITS_H_
