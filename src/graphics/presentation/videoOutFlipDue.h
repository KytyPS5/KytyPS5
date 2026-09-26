#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_VIDEOOUTFLIPDUE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_VIDEOOUTFLIPDUE_H_

#include <cstdint>
#include <limits>

namespace Libs::VideoOut {

inline constexpr uint64_t kNoPresentedVblank = std::numeric_limits<uint64_t>::max();

// SCE flip_rate N spaces presented frames by at least (N+1) vblanks.
// Pace relative to the last presented vblank so a Ready frame is not starved
// when Flip is sampled on the opposite absolute phase (odd count with rate=1).
// kNoPresentedVblank means no frame has been shown yet; the first is due immediately.
[[nodiscard]] constexpr bool IsFlipDueAtVblank(uint64_t vblank_count, int flip_rate,
                                               uint64_t last_presented_vblank) noexcept {
	if (flip_rate <= 0) {
		return true;
	}
	if (last_presented_vblank == kNoPresentedVblank) {
		return true;
	}
	if (vblank_count < last_presented_vblank) {
		return false;
	}
	const auto interval = static_cast<uint64_t>(flip_rate) + 1u;
	return (vblank_count - last_presented_vblank) >= interval;
}

} // namespace Libs::VideoOut

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_VIDEOOUTFLIPDUE_H_
