#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_VIDEOOUTFLIPDUE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_VIDEOOUTFLIPDUE_H_

#include <cstdint>
#include <limits>
#include <atomic>

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

// Present-thread pacing credit from fast frames must not grow without bound:
// sleeping the full credit freezes Flip while Ready frames wait.
[[nodiscard]] constexpr int64_t ClampPresentPacingWait(int64_t total_wait,
                                                       uint64_t period_ticks) noexcept {
	if (period_ticks == 0) {
		return 0;
	}
	const auto period = static_cast<int64_t>(period_ticks);
	return total_wait > period ? period : total_wait;
}

// Soft-stall diagnosis: which PresentThread/Presenter step is running.
inline constexpr uint32_t kPresentStageSleep          = 1;
inline constexpr uint32_t kPresentStageVblankBegin    = 2;
inline constexpr uint32_t kPresentStageFlipEnter      = 3;
inline constexpr uint32_t kPresentStageFlipNotReady   = 4;
inline constexpr uint32_t kPresentStageFlipNotDue     = 5;
inline constexpr uint32_t kPresentStagePresentMutex   = 6;
inline constexpr uint32_t kPresentStagePresentAcquire = 7;
inline constexpr uint32_t kPresentStagePresentSubmit  = 8;
inline constexpr uint32_t kPresentStagePresentQueue   = 9;
inline constexpr uint32_t kPresentStageVblankEnd      = 10;
inline constexpr uint32_t kPresentStagePresentDone    = 11;
inline constexpr uint32_t kPresentStageFlipPublish    = 12;
inline constexpr uint32_t kPresentStageFlipPublishWait = 13;

inline std::atomic<uint32_t>& PresentStageFlag() noexcept {
	static std::atomic<uint32_t> stage {0};
	return stage;
}

inline void SetPresentStage(uint32_t stage) noexcept {
	PresentStageFlag().store(stage, std::memory_order_relaxed);
}

[[nodiscard]] inline uint32_t GetPresentStage() noexcept {
	return PresentStageFlag().load(std::memory_order_relaxed);
}

} // namespace Libs::VideoOut

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_VIDEOOUTFLIPDUE_H_
