#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_TEXT_STYLES_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_TEXT_STYLES_H_

#include "graphics/presentation/system/common/props/textOptions.h"
#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/theme.h"

namespace Libs::Graphics::SystemUi {

/// How text looks and gives way in its space, apart from what it says. The drawing layer and the
/// Text element both take it whole, so the two can't drift apart.
struct TextStyles {
	TypeStyle    type;
	Color        color;
	TextAlign    align    = TextAlign::Start;
	TextOverflow overflow = TextOverflow::Ellipsis;
	// For wrapped text, the most lines to show. Zero means no limit. When lines are dropped, the
	// last one shown ends with an ellipsis.
	int max_lines = 0;
	// For one-line text, shrink the font down to 85% before cutting it short, as a title in a
	// fixed-width slot does.
	bool shrink_to_fit = false;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_TEXT_STYLES_H_
