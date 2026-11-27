#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_ALIGN_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_ALIGN_H_

#include <cstdint>

namespace Libs::Graphics::SystemUi {

/// Where an item sits on the cross axis. Stretch makes it as long as the container's cross axis.
/// An item longer than the cross axis starts at its start, as on the main axis. It has a header of
/// its own so an element can override its alignment, and the box's flex solver can align items,
/// without either pulling in what the other needs.
enum class Align : uint8_t { Start, Center, End, Stretch };

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMMON_PROPS_ALIGN_H_
