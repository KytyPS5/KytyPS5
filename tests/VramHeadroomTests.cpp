#include "graphics/host_gpu/memoryHeadroom.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {

using Libs::Graphics::Headroom::CollectorImageFacts;
using Libs::Graphics::Headroom::CollectorVerdict;
using Libs::Graphics::Headroom::ClassifyForCollection;
using Libs::Graphics::Headroom::CollectionAllowed;
using Libs::Graphics::Headroom::EvictBackoffFrames;
using Libs::Graphics::Headroom::EvictMinIdleFrames;
using Libs::Graphics::Headroom::EvictMinIdleFramesCritical;
using Libs::Graphics::Headroom::ReleaseMinIdleFrames;
using Libs::Graphics::Headroom::CollectorCriticalPercent;
using Libs::Graphics::Headroom::CollectorPressurePercent;
using Libs::Graphics::Headroom::CollectorTriggerPercent;
using Libs::Graphics::Headroom::ResolveCollectorThresholds;
using Libs::Graphics::Headroom::HostFallbackAllowedForImageUsage;
using Libs::Graphics::Headroom::ReclaimableWithoutGpuWork;
using Libs::Graphics::Headroom::UsageTransientAttachment;

int g_failures = 0;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "VramHeadroomTests: failed: %s\n", text);
		++g_failures;
	}
}

// A plain live surface: registered, reproducible from guest memory.
constexpr CollectorImageFacts CleanImage() {
	CollectorImageFacts facts {};
	facts.registered = true;
	return facts;
}

// Written by the GPU, nothing newer underneath, with a write-back the tiler can plan.
constexpr CollectorImageFacts TiledGpuSurface() {
	CollectorImageFacts facts {};
	facts.registered       = true;
	facts.gpu_modified     = true;
	facts.safe_to_download = true;
	facts.downloadable     = true;
	return facts;
}

void TestAllocationFallbackTiers() {
	// Lazily-allocated memory has no host-visible equivalent.
	Check(!HostFallbackAllowedForImageUsage(UsageTransientAttachment),
	      "a transient attachment must not be offered a host-visible fallback");
	Check(!HostFallbackAllowedForImageUsage(UsageTransientAttachment | 0x04u),
	      "a transient attachment stays ineligible however it is combined");

	Check(HostFallbackAllowedForImageUsage(0x01u | 0x02u | 0x04u),
	      "a sampled transfer image may fall back to host-visible memory");
	Check(HostFallbackAllowedForImageUsage(0x10u | 0x08u),
	      "a colour attachment with storage may fall back to host-visible memory");
	Check(HostFallbackAllowedForImageUsage(0x20u),
	      "a depth/stencil attachment may fall back to host-visible memory");
	Check(HostFallbackAllowedForImageUsage(0u), "an image with no usage bits may fall back");
}

void TestCollectorKeepsWhatItCannotRestore() {
	auto display = TiledGpuSurface();
	display.video_out = true;
	Check(ClassifyForCollection(display, true) == CollectorVerdict::Keep,
	      "a display buffer is kept even under pressure");
	Check(ClassifyForCollection(display, false) == CollectorVerdict::Keep,
	      "a display buffer is kept when not under pressure");

	auto unwritable = TiledGpuSurface();
	unwritable.safe_to_download = false;
	Check(ClassifyForCollection(unwritable, true) == CollectorVerdict::Keep,
	      "GPU-only pixels that cannot be written back are kept, not dropped");

	auto unplannable = TiledGpuSurface();
	unplannable.downloadable = false;
	Check(ClassifyForCollection(unplannable, true) == CollectorVerdict::Keep,
	      "GPU-only pixels with no download plan are kept");
}

void TestCollectorEvictsTiledGpuSurfaces() {
	Check(ClassifyForCollection(TiledGpuSurface(), true) == CollectorVerdict::Evict,
	      "a tiled GPU-modified surface is evicted under pressure");

	Check(ClassifyForCollection(TiledGpuSurface(), false) == CollectorVerdict::Keep,
	      "an eviction is not worth doing before the cache is under pressure");
}

void TestCollectorKeepsStencilPlanes() {
	auto stencil_target = CleanImage();
	stencil_target.stencil_plane = true;
	Check(ClassifyForCollection(stencil_target, false) == CollectorVerdict::Keep,
	      "a depth/stencil target is kept even when nothing marks it GPU-modified");
	Check(ClassifyForCollection(stencil_target, true) == CollectorVerdict::Keep,
	      "a depth/stencil target is kept under pressure too");

	for (const bool gpu_modified: {false, true}) {
		for (const bool safe: {false, true}) {
			for (const bool downloadable: {false, true}) {
				for (const bool pressured: {false, true}) {
					auto facts             = CleanImage();
					facts.stencil_plane    = true;
					facts.gpu_modified     = gpu_modified;
					facts.safe_to_download = safe;
					facts.downloadable     = downloadable;
					Check(ClassifyForCollection(facts, pressured) == CollectorVerdict::Keep,
					      "no combination of dirty flags lets a stencil plane be collected");
				}
			}
		}
	}

	auto abandoned_target = CleanImage();
	abandoned_target.stencil_plane = true;
	Check(!ReclaimableWithoutGpuWork(abandoned_target, true),
	      "the emergency reclaim never drops a stencil plane, however abandoned the image looks");

	auto proxy = CleanImage();
	proxy.depth_associated = true;
	proxy.stencil_plane    = true;
	Check(ClassifyForCollection(proxy, true) == CollectorVerdict::Skip,
	      "a stencil association is still skipped, not kept");

	auto depth_only = CleanImage();
	Check(ClassifyForCollection(depth_only, false) == CollectorVerdict::Free,
	      "a depth target with no stencil plane is unaffected");

	auto untouched_pair = CleanImage();
	Check(ClassifyForCollection(untouched_pair, false) == CollectorVerdict::Free,
	      "a depth/stencil image the GPU never wrote is still reclaimable");
}

void TestCollectorFreesReproducibleImages() {
	Check(ClassifyForCollection(CleanImage(), false) == CollectorVerdict::Free,
	      "an image guest memory reproduces is freed at any pressure level");
	Check(ClassifyForCollection(CleanImage(), true) == CollectorVerdict::Free,
	      "an image guest memory reproduces is freed under pressure too");

	auto unregistered = CleanImage();
	unregistered.registered = false;
	Check(ClassifyForCollection(unregistered, true) == CollectorVerdict::Skip,
	      "an unregistered slot is not a candidate");

	auto stencil = CleanImage();
	stencil.depth_associated = true;
	Check(ClassifyForCollection(stencil, true) == CollectorVerdict::Skip,
	      "a stencil association is only freed with its depth owner");
}

void TestEmergencyReclaimNeverRecordsGpuWork() {
	Check(ReclaimableWithoutGpuWork(CleanImage(), true),
	      "a clean, frame-abandoned image is reclaimable mid-allocation");
	Check(!ReclaimableWithoutGpuWork(CleanImage(), false),
	      "an image the frame in flight still touches is never reclaimed mid-allocation");
	Check(!ReclaimableWithoutGpuWork(TiledGpuSurface(), true),
	      "an eviction is never attempted from inside a failed allocation");

	auto display = CleanImage();
	display.video_out = true;
	Check(!ReclaimableWithoutGpuWork(display, true),
	      "a display buffer is never reclaimed mid-allocation");

	for (const bool abandoned: {false, true}) {
		for (const auto& facts: {CleanImage(), TiledGpuSurface()}) {
			if (ReclaimableWithoutGpuWork(facts, abandoned)) {
				Check(ClassifyForCollection(facts, false) == CollectorVerdict::Free,
				      "the emergency pass only ever drops what the collector calls Free");
			}
		}
	}
}

void TestCollectorThresholds() {
	constexpr uint64_t MiB = 1024ull * 1024;
	constexpr uint64_t GiB = 1024ull * MiB;

	const auto laptop = ResolveCollectorThresholds(11 * GiB);
	Check(laptop.pressure > 6 * GiB,
	      "eviction must not engage inside the 4-6 GiB an ordinary menu occupies");
	Check(laptop.pressure < 7 * GiB,
	      "eviction must engage with real headroom left, not at the ceiling");
	Check(laptop.critical > laptop.pressure && laptop.critical < 10 * GiB,
	      "aggressive collection sits above eviction and below the ceiling");
	Check(laptop.trigger < laptop.pressure && laptop.trigger > GiB,
	      "the collector starts running well before eviction becomes worth its copy");

	for (const uint64_t ceiling: {4 * GiB, 8 * GiB, 11 * GiB, 16 * GiB, 24 * GiB}) {
		const auto thresholds = ResolveCollectorThresholds(ceiling);
		Check(thresholds.pressure == ceiling / 100 * CollectorPressurePercent,
		      "pressure is exactly its fraction of the ceiling above the small-device floors");
		Check(thresholds.critical == ceiling / 100 * CollectorCriticalPercent,
		      "critical is exactly its fraction of the ceiling above the small-device floors");
		Check(thresholds.trigger == ceiling / 100 * CollectorTriggerPercent,
		      "trigger is exactly its fraction of the ceiling above the small-device floors");
		Check(thresholds.trigger <= thresholds.pressure &&
		          thresholds.pressure <= thresholds.critical && thresholds.critical <= ceiling,
		      "the three levels stay ordered and inside the ceiling at every capacity");
	}

	for (const uint64_t ceiling: {64 * MiB, 256 * MiB, 768 * MiB, GiB, 2 * GiB}) {
		const auto thresholds = ResolveCollectorThresholds(ceiling);
		Check(thresholds.trigger <= thresholds.pressure &&
		          thresholds.pressure <= thresholds.critical && thresholds.critical <= ceiling,
		      "a small-VRAM ceiling keeps the levels ordered and reachable");
	}

	const auto unknown = ResolveCollectorThresholds(0);
	Check(unknown.trigger == 0 && unknown.pressure == 0 && unknown.critical == 0,
	      "an unknown ceiling resolves to nothing, not to a ceiling of zero");
}

void TestCollectorKeepsTheWorkingSet() {
	constexpr uint64_t frame = 10000;
	for (const bool critical: {false, true}) {
		Check(!CollectionAllowed(CollectorVerdict::Evict, frame, frame, 0, critical),
		      "an image used this frame is never evicted");
		Check(!CollectionAllowed(CollectorVerdict::Evict, frame, frame - 1, 0, critical),
		      "an image used last frame is never evicted");
		Check(!CollectionAllowed(CollectorVerdict::Free, frame, frame - 1, 0, critical),
		      "a clean image used last frame is not dropped to be uploaded again");
		Check(!CollectionAllowed(CollectorVerdict::Keep, frame, 0, 0, critical) &&
		          !CollectionAllowed(CollectorVerdict::Skip, frame, 0, 0, critical),
		      "Keep and Skip are never released");
	}
	Check(CollectionAllowed(CollectorVerdict::Free, frame, frame - ReleaseMinIdleFrames, 0, false),
	      "a clean image idle for a few frames may be dropped");
	Check(!CollectionAllowed(CollectorVerdict::Evict, frame, frame - EvictMinIdleFramesCritical, 0,
	                         false),
	      "outside critical pressure an eviction waits for the long idle period");
	Check(CollectionAllowed(CollectorVerdict::Evict, frame, frame - EvictMinIdleFramesCritical, 0,
	                        true),
	      "critical pressure evicts sooner, but still only idle images");
	Check(CollectionAllowed(CollectorVerdict::Evict, frame, frame - EvictMinIdleFrames, 0, false),
	      "an image idle for the long period may be evicted");
	Check(!CollectionAllowed(CollectorVerdict::Evict, frame, frame - EvictMinIdleFrames,
	                         frame + 1, true),
	      "an image that outran its last eviction is left alone until its backoff ends");
	Check(CollectionAllowed(CollectorVerdict::Evict, frame + EvictBackoffFrames,
	                        frame - EvictMinIdleFrames, frame + EvictBackoffFrames, false),
	      "the backoff ends");
	Check(!CollectionAllowed(CollectorVerdict::Evict, 5, 9, 0, true),
	      "a touch stamped after the collector read the frame counts as current");
	Check(!CollectionAllowed(CollectorVerdict::Free, frame, frame - 1, 0, false, true) &&
	          CollectionAllowed(CollectorVerdict::Free, frame, frame - 2, 0, false, true),
	      "past the device budget a clean image goes after two idle frames, never sooner");
	Check(!CollectionAllowed(CollectorVerdict::Evict, frame, frame - 1, 0, true, true),
	      "being over budget never evicts an image in use");
}

} // namespace

int main() {
	TestAllocationFallbackTiers();
	TestCollectorKeepsWhatItCannotRestore();
	TestCollectorEvictsTiledGpuSurfaces();
	TestCollectorKeepsStencilPlanes();
	TestCollectorFreesReproducibleImages();
	TestEmergencyReclaimNeverRecordsGpuWork();
	TestCollectorThresholds();
	TestCollectorKeepsTheWorkingSet();

	if (g_failures != 0) {
		std::fprintf(stderr, "VramHeadroomTests: %d check(s) failed\n", g_failures);
		return EXIT_FAILURE;
	}
	std::printf("VramHeadroomTests: all checks passed\n");
	return EXIT_SUCCESS;
}
