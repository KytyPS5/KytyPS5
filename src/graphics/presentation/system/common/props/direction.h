#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_DIRECTION_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_DIRECTION_H_

#include <cstdint>

namespace Libs::Graphics::SystemUi {

/// The main axis a container lays its items along. A row runs left to right and a column top to
/// bottom. It has a header of its own, as Align does, so a box's props can name it without pulling
/// in the flex solver.
enum class FlexDirection : uint8_t { Row, Column };

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_DIRECTION_H_
