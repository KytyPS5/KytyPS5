#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_

#include "common/assert.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/regionManager.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace Libs::Graphics {

// Hint publication runs inside the guest write-fault path: it must be one lock-free atomic RMW,
// never a hidden mutex.
static_assert(std::atomic<uint64_t>::is_always_lock_free);

class MemoryTracker final {
public:
	explicit MemoryTracker(PageManager& page_manager);
	~MemoryTracker();

	KYTY_CLASS_NO_COPY(MemoryTracker);

	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	void               MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size);
	void               MarkRegionAsGpuModified(uint64_t vaddr, uint64_t size);
	void               UnmarkRegionAsGpuModified(uint64_t vaddr, uint64_t size);
	void               UntrackMemory(uint64_t vaddr, uint64_t size);

	// --- Selective BDA discovery ------------------------------------------------------------
	// One hint bit per 4 MiB region meaning "this region MAY hold CPU-dirty data that
	// PrepareBda must examine". It is a conservative index over RegionManager's CPU-dirty
	// bitmap, which remains the only truth: a false positive costs one empty snapshot, a false
	// negative is a coherency bug.
	//
	// Contract, at every completed publication boundary: if a CPU-dirty page lies inside a
	// mapped registered buffer and legacy PrepareBda would have to synchronise it before the
	// current BDA consumer, then its region's hint is pending, or responsibility for that
	// region has been consumed by the PrepareBda pass executing right now (it exchanged the
	// hint and has not finished the region). Completion: a write ordered before a BDA consumer
	// is examined and synchronised before that consumer proceeds; leaving it pending "for next
	// time" is not enough. Genuinely racing writes keep the legacy semantics.
	//
	// Publications:
	//   P1  RegionManager::ChangeState<Cpu, true>: under the region lock, every execution
	//   P2  GetOrCreateRegion: the hint before the manager pointer (a new region is all-dirty)
	//   P3  BufferCache registration: every region of the actual registered span
	//   P4  GpuResourceManager::MapMemory: under the exclusive mapped-range lock
	// Consumption exchanges each word once per pass and never clears a completed region again.
	static constexpr size_t BDA_HINT_WORDS = TRACKER_ADDRESS_SIZE / TRACKER_REGION_SIZE / 64;

	// P3/P4: flag every region intersecting the half-open range [vaddr, vaddr + size).
	void PublishBdaHints(uint64_t vaddr, uint64_t size) noexcept;
	// Takes ownership of one word's pending regions for the running pass.
	[[nodiscard]] uint64_t ConsumeBdaHintWord(size_t word) noexcept;
	// Makes regions a pass owned but did not complete pending again (fail-closed paths).
	void RestoreBdaHints(size_t word, uint64_t bits) noexcept;
	// Legacy walk: drops every pending hint before a full scan answers them.
	void                         DiscardBdaHints() noexcept;
	[[nodiscard]] bool           IsBdaHintPending(uint64_t region) const noexcept;
	[[nodiscard]] RegionManager* FindRegion(uint64_t region) const noexcept {
		return m_regions[region].load(std::memory_order_acquire);
	}
	// Copy of the region's CPU-dirty bitmap taken under its lock. Discovery input only.
	[[nodiscard]] RegionBits SnapshotCpuDirty(RegionManager& manager);

	// Invariant checker (tests, --bda-sync SelectiveChecked). In every region of the range, a
	// CPU-dirty page, or a missing manager (all-dirty by construction), requires a pending
	// hint unless `owned(region)` says the running pass owns the region. The hint is read
	// under the region lock, which P1 holds while it publishes, so the check cannot race into
	// a false failure.
	template <typename Owned>
	[[nodiscard]] bool BdaHintsCoverCpuDirty(uint64_t vaddr, uint64_t size, Owned&& owned) {
		CheckNotInUploadCallback();
		ValidateRange(vaddr, size);
		uint64_t remaining = size;
		uint64_t index     = vaddr / TRACKER_REGION_SIZE;
		uint64_t offset    = vaddr % TRACKER_REGION_SIZE;
		while (remaining != 0) {
			const auto bytes = std::min(TRACKER_REGION_SIZE - offset, remaining);
			if (!owned(index)) {
				auto* manager = m_regions[index].load(std::memory_order_acquire);
				if (manager == nullptr) {
					if (!IsBdaHintPending(index)) {
						return false;
					}
				} else {
					std::scoped_lock lock(manager->lock);
					if (manager->IsModified<DirtySource::Cpu>(offset, bytes) &&
					    !IsBdaHintPending(index)) {
						return false;
					}
				}
			}
			remaining -= bytes;
			offset = 0;
			index++;
		}
		return true;
	}
	// Removes protection from a range and flushes GPU-owned data when required.
	template <typename Flush>
	void InvalidateRegion(uint64_t vaddr, uint64_t size, Flush&& on_flush) noexcept {
		static_assert(std::is_invocable_v<Flush&>);
		CheckNotInUploadCallback();

		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			const bool should_flush = [&] {
				// Perform both the GPU modification check and CPU state change with the lock in
				// case the GPU thread is racing to mark the page modified. If a flush is needed,
				// on_flush performs the CPU state change.
				std::scoped_lock lock(manager->lock);
				if (manager->IsModified<DirtySource::Gpu>(offset, bytes)) {
					return true;
				}
				manager->ChangeState<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset, bytes);
				return false;
			}();
			if (should_flush) {
				on_flush();
			}
		});
	}
#if KYTY_BUILD == KYTY_BUILD_DEBUG
	void ValidateGpuDirtyPages(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
	                           const char* operation) const noexcept;
	void ValidateGpuDirtyOwnership(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
	                               const char* operation);
#else
	void ValidateGpuDirtyPages(const RangeSet&, uint64_t, uint64_t, const char*) const noexcept {}
	void ValidateGpuDirtyOwnership(const RangeSet&, uint64_t, uint64_t, const char*) {}
#endif

	template <bool clear, typename Preflight, typename Func>
	void ForEachDownloadRange(uint64_t vaddr, uint64_t size, Preflight&& preflight, Func&& func) {
		static_assert(std::is_nothrow_invocable_v<Preflight&, uint64_t, uint64_t>);
		static_assert(std::is_nothrow_invocable_v<Func&, uint64_t, uint64_t>);
		CheckNotInUploadCallback();
		std::vector<RegionManager*> managers;
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t, uint64_t) {
			managers.push_back(manager);
		});
		std::vector<std::unique_lock<TrackingSpinLock>> locks;
		locks.reserve(managers.size());
		for (auto* manager: managers) {
			locks.emplace_back(manager->lock);
		}
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			const auto address = manager->GetCpuAddr() + offset;
			manager->template ForEachModifiedRange<DirtySource::Gpu, false>(address, bytes,
			                                                                preflight);
		});
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			manager->template ForEachModifiedRange<DirtySource::Gpu, false>(
			    manager->GetCpuAddr() + offset, bytes, func);
		});
		if constexpr (clear) {
			Iterate<false>(vaddr, size,
			               [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				               const auto address = manager->GetCpuAddr() + offset;
				               manager->template ForEachModifiedRange<DirtySource::Gpu, true>(
				                   address, bytes, [](uint64_t, uint64_t) noexcept {});
			               });
		}
	}

	template <bool clear, typename Func>
	void ForEachDownloadRange(uint64_t vaddr, uint64_t size, Func&& func) {
		ForEachDownloadRange<clear>(
		    vaddr, size, [](uint64_t, uint64_t) noexcept {}, std::forward<Func>(func));
	}

	template <typename RangeFunc, typename UploadFunc>
	void ForEachUploadRange(uint64_t vaddr, uint64_t size, bool is_written, RangeFunc&& range_func,
	                        UploadFunc&& upload_func) {
		static_assert(std::is_nothrow_invocable_v<RangeFunc&, uint64_t, uint64_t>);
		static_assert(std::is_nothrow_invocable_v<UploadFunc&>);
		CheckNotInUploadCallback();
		Iterate<true>(vaddr, size, [](RegionManager*, uint64_t, uint64_t) {});
		const auto* previous_upload_owner = std::exchange(s_upload_owner, this);
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			manager->lock.lock();
			manager->ForEachModifiedRange<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset,
			                                                      bytes, range_func);
			if (!is_written) {
				manager->lock.unlock();
			}
		});
		upload_func();
		if (is_written) {
			Iterate<false>(vaddr, size,
			               [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				               manager->template ChangeState<DirtySource::Gpu, true>(
				                   manager->GetCpuAddr() + offset, bytes);
				               manager->lock.unlock();
			               });
		}
		s_upload_owner = previous_upload_owner;
	}

private:
	static constexpr size_t REGION_COUNT = TRACKER_ADDRESS_SIZE / TRACKER_REGION_SIZE;
	inline static thread_local const MemoryTracker* s_upload_owner = nullptr;

	void CheckNotInUploadCallback() const noexcept {
		if (s_upload_owner == this) {
			EXIT("memory tracker re-entered from upload callback\n");
		}
	}

	template <bool create, typename Func>
	bool Iterate(uint64_t vaddr, uint64_t size, Func&& func) {
		ValidateRange(vaddr, size);
		using Result = std::invoke_result_t<Func, RegionManager*, uint64_t, uint64_t>;
		constexpr bool returns_bool = std::is_same_v<Result, bool>;
		uint64_t       remaining    = size;
		uint64_t       index        = vaddr / TRACKER_REGION_SIZE;
		uint64_t       offset       = vaddr % TRACKER_REGION_SIZE;
		while (remaining != 0) {
			const auto bytes   = std::min(TRACKER_REGION_SIZE - offset, remaining);
			auto*      manager = m_regions[index].load(std::memory_order_acquire);
			if (manager == nullptr && create) {
				manager = GetOrCreateRegion(index);
			}
			if (manager != nullptr) {
				if constexpr (returns_bool) {
					if (func(manager, offset, bytes)) {
						return true;
					}
				} else {
					func(manager, offset, bytes);
				}
			}
			remaining -= bytes;
			offset = 0;
			index++;
		}
		return false;
	}

	static void    ValidateRange(uint64_t vaddr, uint64_t size);
	RegionManager* GetOrCreateRegion(uint64_t index);

	std::unique_ptr<std::atomic<RegionManager*>[]> m_regions;
	std::vector<std::unique_ptr<RegionManager>>    m_region_storage;
	std::mutex                                     m_region_mutex;
	PageManager&                                   m_page_manager;
	std::unique_ptr<std::atomic<uint64_t>[]>       m_bda_hints;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_
