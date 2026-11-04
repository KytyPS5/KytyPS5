#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SKIPPEDDRAWS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SKIPPEDDRAWS_H_

#include <atomic>
#include <cstdint>

namespace Libs::Graphics {

namespace Detail {
inline std::atomic<uint32_t> g_draws_skipped_for_shaders {0};
} // namespace Detail

// With --async-shader-compile, a draw whose shader or pipeline is still being built is skipped.
// The frame it belonged to is then incomplete, and the presenter holds the last complete one.
inline void NoteDrawSkippedForShaders() noexcept {
	Detail::g_draws_skipped_for_shaders.fetch_add(1, std::memory_order_relaxed);
}

// Whether any draw was skipped since the last call. A flip takes this when it records its frame,
// which happens in order with the draws, so the draws before it are the ones it holds.
[[nodiscard]] inline bool TakeDrawsSkippedForShaders() noexcept {
	return Detail::g_draws_skipped_for_shaders.exchange(0, std::memory_order_relaxed) != 0;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SKIPPEDDRAWS_H_
