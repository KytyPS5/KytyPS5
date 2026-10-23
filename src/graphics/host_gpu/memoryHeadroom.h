#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYHEADROOM_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYHEADROOM_H_

#include <cstdint>

namespace Libs::Graphics::Headroom {

// VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, spelled numerically to keep Vulkan headers out.
inline constexpr uint32_t UsageTransientAttachment = 0x00000040u;

enum class AllocationTier : uint32_t {
	DeviceLocal,
	HostFallback,
};

// A transient attachment needs lazily allocated memory, which no host-visible heap offers.
[[nodiscard]] constexpr bool HostFallbackAllowedForImageUsage(uint32_t usage) noexcept {
	return (usage & UsageTransientAttachment) == 0;
}

struct CollectorImageFacts {
	bool registered       = false;
	bool depth_associated = false;
	bool video_out        = false;
	bool gpu_modified     = false;
	bool safe_to_download = false;
	// No stencil upload or download path exists, so a written stencil plane cannot be restored.
	bool stencil_plane = false;
	bool downloadable  = false;
};

enum class CollectorVerdict : uint32_t {
	Skip,
	// Nothing could restore the pixels.
	Keep,
	// Guest memory already reproduces the image.
	Free,
	// Write the pixels back, then free once the download has reached guest memory.
	Evict,
};

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

// The emergency pass runs mid-draw, so it may only release images guest memory reproduces.
[[nodiscard]] constexpr bool ReclaimableWithoutGpuWork(const CollectorImageFacts& facts,
                                                       bool frame_abandoned) noexcept {
	return frame_abandoned && ClassifyForCollection(facts, false) == CollectorVerdict::Free;
}

// Without VK_EXT_memory_budget `reported` is false, `budget` is the heap size, `usage` unknown.
struct MemoryBudget {
	uint64_t usage     = 0;
	uint64_t budget    = 0;
	uint64_t heap_size = 0;
	bool     reported  = false;
};

// The collector's three levels, as fractions of the working ceiling.
inline constexpr uint64_t CollectorTriggerPercent  = 15; // below this the collector does not run
inline constexpr uint64_t CollectorPressurePercent = 55; // at this, eviction becomes worth its copy
inline constexpr uint64_t CollectorCriticalPercent = 85; // at this, collect aggressively

inline constexpr uint64_t CollectorMinPressureBytes = 768ull * 1024 * 1024;
inline constexpr uint64_t CollectorMinCriticalBytes = 1024ull * 1024 * 1024;

struct CollectorThresholds {
	uint64_t trigger  = 0;
	uint64_t pressure = 0;
	uint64_t critical = 0;
};

// `budget` is GraphicContext::GetTotalMemoryBudget(); zero keeps the caller's defaults.
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
	// On a tiny ceiling the two floors can cross; critical must never precede pressure.
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
