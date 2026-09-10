#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_BDATESTHOOKS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_BDATESTHOOKS_H_

#include <cstdint>

#if defined(KYTY_BDA_TEST_HOOKS)
#include <atomic>
#endif

// Interleaving points for the selective BDA synchronisation tests. They compile to nothing
// unless KYTY_BDA_TEST_HOOKS is defined, which only test targets do.
namespace Libs::Graphics::BdaTestHooks {

enum class Point : uint32_t {
	AfterHintExchange,     // value: hint word; the pass now owns that word's regions
	NullRegionManager,     // value: region; a consumed region has no RegionManager yet
	AfterDirtySnapshot,    // value: region; CPU-dirty snapshot taken, owners not yet visited
	RegionHintPublished,   // value: region; P2 published the hint, not yet the manager pointer
	BeforeUploadCopy,      // value: buffer address; protection re-armed, staging copy not made
	AfterUploadCopy,       // value: buffer address; staging copy made
	ForceSelectiveFailure, // value: guest address; returning true simulates an owner-index failure
};

#if defined(KYTY_BDA_TEST_HOOKS)
using Callback = bool (*)(Point point, uint64_t value);
inline std::atomic<Callback> g_callback {nullptr};

inline bool Fire(Point point, uint64_t value) {
	const auto callback = g_callback.load(std::memory_order_acquire);
	return callback != nullptr && callback(point, value);
}
#else
constexpr bool Fire(Point /*point*/, uint64_t /*value*/) noexcept {
	return false;
}
#endif

} // namespace Libs::Graphics::BdaTestHooks

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_BDATESTHOOKS_H_
