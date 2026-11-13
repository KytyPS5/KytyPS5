#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_BDADIRTYREGIONS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_BDADIRTYREGIONS_H_

#include "graphics/host_gpu/regionDefinitions.h"

#include <atomic>
#include <bit>
#include <cstdint>

// One bit per tracker region: set whenever the region's CPU modification epoch may have
// moved, and over a buffer registration's regions and a new BDA region request's. A clear
// bit proves that every BDA region request inside the region still holds its epochs, so a
// bounded BDA preparation only visits the set bits of its (usually 4 GiB) ranges.
namespace Libs::Graphics::BdaDirtyRegions {

inline constexpr uint64_t Regions = TRACKER_ADDRESS_SIZE / TRACKER_REGION_SIZE;
inline constexpr uint64_t Words   = Regions / 64;

inline std::atomic<uint64_t> g_bits[Words] = {};

// A guest thread's write reaches GPU work only through a later submission or a label the GPU waits
// for (as on the console, where a write after the submission, without a label, races the GPU): its
// regions wait here until the translating thread passes one of those points (Publish), instead of
// being synchronized again before every BDA dispatch while the guest keeps writing. The
// translating thread's own writes (CP copies, fills, labels) are visible to its next command.
inline std::atomic<uint64_t> g_pending[Words] = {};
inline std::atomic<uint64_t> g_pending_words[Words / 64] = {};
inline std::atomic<uint64_t> g_pending_groups {0};
inline thread_local bool     g_translating = false;

inline void Mark(uint64_t region) {
	const auto word = region / 64;
	const auto bit  = uint64_t {1} << (region % 64);
	if (g_translating) {
		g_bits[word].fetch_or(bit, std::memory_order_release);
		return;
	}
	// The summary bits after the region's, even when another write set the region's first: a write
	// that returns from here is published by the next Publish that sees the label it writes next.
	g_pending[word].fetch_or(bit, std::memory_order_acq_rel);
	g_pending_words[word / 64].fetch_or(uint64_t {1} << (word % 64), std::memory_order_acq_rel);
	g_pending_groups.fetch_or(uint64_t {1} << (word / 64), std::memory_order_release);
}

// The translating thread when it takes a submission and when a WAIT_REG_MEM is satisfied: the guest
// writes made before the submission or the awaited label become visible to the commands after it.
inline void Publish() {
	if (g_pending_groups.load(std::memory_order_acquire) == 0) return;
	for (auto groups = g_pending_groups.exchange(0, std::memory_order_acq_rel); groups != 0; groups &= groups - 1) {
		const auto group = static_cast<uint64_t>(std::countr_zero(groups));
		for (auto words = g_pending_words[group].exchange(0, std::memory_order_acq_rel); words != 0; words &= words - 1) {
			const auto word = group * 64 + static_cast<uint64_t>(std::countr_zero(words));
			if (const auto bits = g_pending[word].exchange(0, std::memory_order_acq_rel); bits != 0)
				g_bits[word].fetch_or(bits, std::memory_order_release);
		}
	}
}

} // namespace Libs::Graphics::BdaDirtyRegions

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_BDADIRTYREGIONS_H_
