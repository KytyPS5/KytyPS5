#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_IDENTITY_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_IDENTITY_H_

#include <cstdint>

// Which animation a kept record belongs to. The tree is built again every frame, so an animation's
// progress, or a box's scroll position, is found again by the element's key, or without one, by
// its build order among the unkeyed elements of its kind.
namespace Libs::Graphics::SystemUi {

/// Which animation a record belongs to, the same every frame the same animation is built.
struct AnimationIdentity {
	uint64_t value = 0;

	constexpr bool operator==(const AnimationIdentity&) const = default;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_IDENTITY_H_
