#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_FONTS_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_FONTS_H_

#include "graphics/presentation/system/core/theme.h"

struct ImFont;

namespace Libs::Graphics::SystemUi {

/// The system UI's fonts, one per weight the theme uses. A missing font is null, and components
/// fall back to the other weight, then to ImGui's current font.
struct FontsContext {
	ImFont* regular = nullptr;
	ImFont* bold    = nullptr;

	/// The font for `weight`, or the other weight when that one is missing. Null only when both
	/// are.
	[[nodiscard]] ImFont* For(FontWeight weight) const {
		ImFont* const preferred = weight == FontWeight::Bold ? bold : regular;
		return preferred != nullptr ? preferred : (weight == FontWeight::Bold ? regular : bold);
	}
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_FONTS_H_
