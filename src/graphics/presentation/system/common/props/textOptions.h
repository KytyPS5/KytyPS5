#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_TEXT_OPTIONS_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_TEXT_OPTIONS_H_

#include <cstdint>

// How text sits in its space and gives way when it doesn't fit. Both the drawing layer and the
// element layer's Text name these, so they live here and neither layer includes the other for them.
namespace Libs::Graphics::SystemUi {

/// Where each line sits across its space.
enum class TextAlign : uint8_t { Start, Center, End };

/// What happens to text wider than its space.
enum class TextOverflow : uint8_t {
	Ellipsis, // one line, cut short with an ellipsis
	Clip,     // one line, cut off at the edge
	Wrap,     // as many lines as needed, up to max_lines, the last cut short with an ellipsis
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_TEXT_OPTIONS_H_
