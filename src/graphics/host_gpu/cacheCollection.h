#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_CACHECOLLECTION_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_CACHECOLLECTION_H_

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

// What a garbage-collection pass of the buffer and texture caches frees.
//
// The caches collect when the device memory in use reaches a trigger derived from the memory
// budget. A GPU with memory of its own reaches it. On a GPU that uses the system memory the budget
// is most of that memory (32 GB on an M4 Max with 36 GB), and a game never reaches the trigger: the
// caches keep every buffer and image they ever made.
//
// That costs more than memory: MoltenVK attaches every live resource to every command buffer. In
// Crash Bandicoot 4 the GPU time of a frame grew from about 20 ms to 75 ms in five minutes, and the
// game fell from 27 to 7 fps. With idle passes in the texture cache it stayed at 15 to 16 fps.
//
// So on system memory a texture-cache pass also frees, below the trigger, images that were unused
// for the idle age. An idle pass keeps GPU-written images: images the GPU wrote and the CPU has not
// read back. It marks them as used now, so that they do not stop the pass from reaching the images
// behind them.
//
// The buffer cache has no idle passes. Shaders reach buffers through device addresses, and that
// access neither marks a buffer as used nor records what it writes. A buffer that looks idle can
// hold GPU results the guest has not read yet, and freeing it clears its entries in the
// device-address page table. Idle passes of the buffer cache froze Crash Bandicoot 4 after about
// three minutes, in two of five runs.
enum class CollectionKind : uint8_t {
	// Nothing to free.
	None,
	// Free images that were unused for the idle age, except GPU-written images. Only the texture
	// cache asks for these passes.
	Idle,
	// Memory is short: free with the cache's own ages and limits.
	Memory,
};

struct CollectionState {
	// Device memory in use, and the use at which the cache collects for memory.
	uint64_t used_memory    = 0;
	uint64_t trigger_memory = 0;
	// The GPU uses the system memory, not memory of its own.
	bool system_memory = false;
	// Collection passes run so far.
	uint64_t passes = 0;
};

// The idle age, in collection passes. A pass runs once for each completed guest submission, so the
// time this takes depends on the game and its frame rate: Crash Bandicoot 4 runs about 28 passes in
// a frame, so 1,000 passes are about 36 frames, one to two seconds.
inline constexpr uint64_t IDLE_COLLECT_AGE = 1000;

[[nodiscard]] constexpr CollectionKind PlanCollection(const CollectionState& state) noexcept {
	if (state.used_memory >= state.trigger_memory) {
		return CollectionKind::Memory;
	}
	// Before the idle age has passed once, no item can have been unused for it.
	if (state.system_memory && state.passes >= IDLE_COLLECT_AGE) {
		return CollectionKind::Idle;
	}
	return CollectionKind::None;
}

// The minimum age, in passes, of an item a pass may free. `memory_age` is the cache's own age for
// a memory pass; `passes` is the number of passes run so far.
[[nodiscard]] constexpr uint64_t CollectionAge(CollectionKind kind, uint64_t memory_age,
                                               uint64_t passes) noexcept {
	return kind == CollectionKind::Idle ? IDLE_COLLECT_AGE : std::min(memory_age, passes);
}

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_CACHECOLLECTION_H_ */
