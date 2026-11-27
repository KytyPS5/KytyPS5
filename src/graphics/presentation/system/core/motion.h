#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_MOTION_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_MOTION_H_

namespace Libs::Graphics::SystemUi {

/// How much the system UI may move, from the player's settings. With reduced motion, elements
/// appear in their place rather than sliding there, and fades are no longer than the theme's
/// reduced_fade_max, for players who find movement uncomfortable. A fade's delay and a tween's
/// easing are left as they are, as neither moves anything.
struct MotionContext {
	bool reduced = false;

	constexpr bool operator==(const MotionContext&) const = default;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_MOTION_H_
