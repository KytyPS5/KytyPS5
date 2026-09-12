#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYHEADROOM_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYHEADROOM_H_

// What the emulator does when device memory runs out: which allocation tiers may be tried before
// giving up, and which cached image the collector may release. Kept as constexpr functions over
// plain facts so none of it needs a Vulkan device to test.
//
// Releasing an image means either dropping it, when guest memory already holds the pixels, or
// evicting it - writing the pixels back first. Eviction is what separates "cannot write it back"
// from "has not written it back yet"; without it the collector keeps almost everything, because
// most of this title's surfaces are tiled and GPU-modified.

#include <cstdint>

namespace Libs::Graphics::Headroom {

// VkImageUsageFlagBits, spelled numerically so this header pulls in no Vulkan headers. Only the
// bit the fallback rule turns on is named.
inline constexpr uint32_t UsageTransientAttachment = 0x00000040u;

// Memory tiers an image allocation may be placed in, in the order they are tried.
enum class AllocationTier : uint32_t {
	// VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT required. What every allocation asks for first.
	DeviceLocal = 0,
	// Device-local again, after the caller has forced a collection to make room.
	DeviceLocalAfterReclaim = 1,
	// Device-local merely preferred, so VMA may place the resource in system memory. Slow, and
	// correct; an image that reads at PCIe speed beats an emulator that aborts.
	HostFallback = 2,
};

// A transient attachment needs VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT memory, which no
// host-visible heap offers, so it has no fallback tier. Every other usage may fall back.
[[nodiscard]] constexpr bool HostFallbackAllowedForImageUsage(uint32_t usage) noexcept {
	return (usage & UsageTransientAttachment) == 0;
}

// What the collector knows about one cached image at the moment it considers it.
struct CollectorImageFacts {
	// Still answering for a range of guest memory. An unregistered slot is not a candidate.
	bool registered = false;
	// Holds a stencil association: freed with its depth owner, never on its own.
	bool depth_associated = false;
	// A display buffer the guest is about to flip. Freeing one presents a whole black frame.
	bool video_out = false;
	// Pixels were produced on the GPU and guest memory does not hold them.
	bool gpu_modified = false;
	// Nothing newer has landed under the image, so writing the pixels back cannot clobber a guest
	// write. False means the pixels may not be written back at all.
	bool safe_to_download = false;
	// Stored in a GPU tiling. Recorded but deliberately not part of the decision; a test pins that.
	bool tiled = false;
	// Set when the GPU has written a stencil plane. Nothing in this tree uploads or downloads
	// stencil, so no replacement image can be given one back.
	bool stencil_plane = false;
	// A download transfer could be planned for this image (BuildDownload().valid). False for
	// multisampled images, compressed metadata, and depth formats with no readback path.
	bool downloadable = false;
};

enum class CollectorVerdict : uint32_t {
	// Not a collection candidate at all.
	Skip,
	// A candidate whose pixels nothing could restore. Leave it alone.
	Keep,
	// Guest memory already reproduces this image. Drop it; no GPU work needed.
	Free,
	// Pixels exist only on the GPU but can be written back. Download them, then free once the
	// download has been published to guest memory.
	Evict,
};

// `pressured` is the collector's own signal that device memory is past its pressure threshold.
// Eviction costs a copy and a submit, so it is only worth doing under pressure; dropping a clean
// image is free and happens at any level.
[[nodiscard]] constexpr CollectorVerdict ClassifyForCollection(const CollectorImageFacts& facts,
                                                               bool pressured) noexcept {
	if (!facts.registered || facts.depth_associated) {
		return CollectorVerdict::Skip;
	}
	if (facts.video_out) {
		return CollectorVerdict::Keep;
	}
	if (facts.stencil_plane) {
		return CollectorVerdict::Keep;
	}
	if (!facts.gpu_modified) {
		return CollectorVerdict::Free;
	}
	if (!facts.safe_to_download) {
		return CollectorVerdict::Keep;
	}
	if (!pressured || !facts.downloadable) {
		return CollectorVerdict::Keep;
	}
	return CollectorVerdict::Evict;
}

inline constexpr uint64_t ReleaseMinIdleFrames       = 8;
inline constexpr uint64_t ReleaseMinIdleFramesOver   = 2;
inline constexpr uint64_t EvictMinIdleFrames         = 120;
inline constexpr uint64_t EvictMinIdleFramesCritical = 30;
inline constexpr uint64_t EvictBackoffFrames         = 600;
inline constexpr uint32_t MaxEvictionsPerCollection  = 2;

[[nodiscard]] constexpr bool CollectionAllowed(CollectorVerdict verdict, uint64_t current_frame,
                                               uint64_t last_used_frame, uint64_t backoff_until,
                                               bool critical, bool over_budget = false) noexcept {
	const uint64_t idle =
	    current_frame - (last_used_frame < current_frame ? last_used_frame : current_frame);
	if (verdict == CollectorVerdict::Free) {
		return idle >= (over_budget ? ReleaseMinIdleFramesOver : ReleaseMinIdleFrames);
	}
	if (verdict != CollectorVerdict::Evict || current_frame < backoff_until) {
		return false;
	}
	return idle >= (critical ? EvictMinIdleFramesCritical : EvictMinIdleFrames);
}

// The emergency pass runs part-way through building a draw, so it may only release images guest
// memory can already reproduce - never an eviction. `frame_abandoned` is the interlock that keeps
// it from freeing an image whose id its own caller is still holding.
[[nodiscard]] constexpr bool ReclaimableWithoutGpuWork(const CollectorImageFacts& facts,
                                                       bool frame_abandoned) noexcept {
	return frame_abandoned && ClassifyForCollection(facts, false) == CollectorVerdict::Free;
}

// What the device says about its own headroom. `reported` is false when VK_EXT_memory_budget is
// absent, in which case `budget` is the raw heap size and `usage` is unknown.
struct MemoryBudget {
	uint64_t usage     = 0;
	uint64_t budget    = 0;
	uint64_t heap_size = 0;
	bool     reported  = false;
};

[[nodiscard]] constexpr uint64_t HeadroomBytes(const MemoryBudget& budget) noexcept {
	return budget.budget > budget.usage ? budget.budget - budget.usage : uint64_t {0};
}

// Only ever answers true when the device actually reports its usage. Without VK_EXT_memory_budget
// the emulator does not know its own headroom and must not pretend it is exhausted.
[[nodiscard]] constexpr bool WouldExceedBudget(const MemoryBudget& budget, uint64_t bytes) noexcept {
	return budget.reported && bytes > HeadroomBytes(budget);
}

// The collector's three levels, as fractions of the working ceiling rather than fixed
// subtractions from it, so one tuning holds across capacities. These three percentages are the
// judgement call; nothing else in the file needs to move to retune it.
inline constexpr uint64_t CollectorTriggerPercent  = 15; // below this the collector does not run
inline constexpr uint64_t CollectorPressurePercent = 55; // at this, eviction becomes worth its copy
inline constexpr uint64_t CollectorCriticalPercent = 85; // at this, collect aggressively

// A device with very little memory must not put its first level so low that the collector runs
// without pause, nor its last so high that it never turns aggressive before the driver refuses.
inline constexpr uint64_t CollectorMinPressureBytes = 768ull * 1024 * 1024;
inline constexpr uint64_t CollectorMinCriticalBytes = 1024ull * 1024 * 1024;

struct CollectorThresholds {
	uint64_t trigger  = 0;
	uint64_t pressure = 0;
	uint64_t critical = 0;
};

// `budget` is the working ceiling - GraphicContext::GetTotalMemoryBudget(), which already holds a
// reserve back from the device's reported budget. A zero ceiling means the device never told us
// anything, and the caller keeps its own defaults rather than deriving nonsense from it.
[[nodiscard]] constexpr CollectorThresholds ResolveCollectorThresholds(uint64_t budget) noexcept {
	if (budget == 0) {
		return {};
	}
	const uint64_t floor_pressure =
	    CollectorMinPressureBytes < budget ? CollectorMinPressureBytes : budget;
	const uint64_t floor_critical =
	    CollectorMinCriticalBytes < budget ? CollectorMinCriticalBytes : budget;

	CollectorThresholds thresholds {};
	thresholds.pressure = budget / 100 * CollectorPressurePercent;
	if (thresholds.pressure < floor_pressure) {
		thresholds.pressure = floor_pressure;
	}
	thresholds.critical = budget / 100 * CollectorCriticalPercent;
	if (thresholds.critical < floor_critical) {
		thresholds.critical = floor_critical;
	}
	// On a tiny ceiling the two floors can cross. Ordering matters more than either floor: an
	// inverted pair would make `critical` reachable before `pressure` and skip eviction entirely.
	if (thresholds.critical < thresholds.pressure) {
		thresholds.critical = thresholds.pressure;
	}
	thresholds.trigger = budget / 100 * CollectorTriggerPercent;
	if (thresholds.trigger > thresholds.pressure) {
		thresholds.trigger = thresholds.pressure;
	}
	return thresholds;
}

} // namespace Libs::Graphics::Headroom

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYHEADROOM_H_
