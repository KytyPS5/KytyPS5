#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SKIPPEDDRAWS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SKIPPEDDRAWS_H_

#include <atomic>
#include <cstdint>

namespace Libs::Graphics {

namespace Detail {
inline std::atomic<uint32_t> g_draws_skipped_for_shaders {0};
inline std::atomic<uint64_t> g_total_draws_skipped {0};
inline std::atomic<uint64_t> g_frame {0};
} // namespace Detail

// With --async-shader-compile, a draw whose shader or pipeline is still being built is skipped.
// The frame it belonged to is then incomplete, and the presenter holds the last complete one.
inline void NoteDrawSkippedForShaders() noexcept {
	Detail::g_draws_skipped_for_shaders.fetch_add(1, std::memory_order_relaxed);
	Detail::g_total_draws_skipped.fetch_add(1, std::memory_order_relaxed);
}

// Whether any draw was skipped since the last call. A flip takes this when it records its frame,
// which happens in order with the draws, so the draws before it are the ones it holds.
[[nodiscard]] inline bool TakeDrawsSkippedForShaders() noexcept {
	return Detail::g_draws_skipped_for_shaders.exchange(0, std::memory_order_relaxed) != 0;
}

// Every draw skipped for shaders since the start, for the status line.
[[nodiscard]] inline uint64_t TotalDrawsSkippedForShaders() noexcept {
	return Detail::g_total_draws_skipped.load(std::memory_order_relaxed);
}

// A flip was recorded: the draws after it belong to the next frame. Called in order with the
// draws, so the draws of one frame share a number.
inline void NoteFrameFlipped() noexcept {
	Detail::g_frame.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline uint64_t CurrentFrame() noexcept {
	return Detail::g_frame.load(std::memory_order_relaxed);
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SKIPPEDDRAWS_H_
