#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_JUSTIFY_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_JUSTIFY_H_

#include <cstdint>

namespace Libs::Graphics::SystemUi {

/// Where a container's items sit on its main axis when they don't fill it. It has a header of its
/// own, as Align does, so a box's props can name it without pulling in the flex solver.
enum class Justify : uint8_t { Start, Center, End, SpaceBetween };

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_JUSTIFY_H_
