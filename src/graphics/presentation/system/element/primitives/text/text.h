#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_TEXT_TEXT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_TEXT_TEXT_H_

#include "graphics/presentation/system/common/props/itemLayout.h"
#include "graphics/presentation/system/common/props/textStyles.h"
#include "graphics/presentation/system/element/element.h"

#include <string_view>

namespace Libs::Graphics::SystemUi {

struct TextProps;

/// A text's element, what Text returns.
using TextElement = Element<TextProps>;

/// What a Text element shows and how. Text builds its own copy of `text` in the frame, so the
/// caller's string need only last for the call.
struct TextProps {
	/// What a text asks of its container and how it looks, kept apart from what it says as
	/// BoxProps::Self is kept apart from a box's children.
	struct Self {
		// An explicit width is the width the text lays out in, wherever the space on offer ends.
		ItemLayout layout;
		TextStyles styles;
	};

	std::string_view text;
	Self             self;
};

/// A text element showing `props`, built in the current frame. It has no children. Its text is
/// copied into the frame, which keeps it until the next Begin. It needs the current fonts, from the
/// open FrameScope, to measure and draw. A refused text is a none handle, and a RefusalScope open
/// around the call says why. Wrapped text tells its container it can shrink only to its longest
/// run between spaces and line breaks, though ImGui also breaks at tabs, after punctuation and
/// inside a word too long for a line. Text with few spaces, such as Japanese, therefore asks for
/// more room in a row than it needs, until the least width follows ImGui's breaks.
[[nodiscard]] TextElement Text(const TextProps& props);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_TEXT_TEXT_H_
