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
#include <vector>

namespace Libs::Graphics {

class MemoryTracker final {
public:
	explicit MemoryTracker(PageManager& page_manager);
	~MemoryTracker();

	KYTY_CLASS_NO_COPY(MemoryTracker);

	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	void               MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size);
	void               UnmarkRegionAsGpuModified(uint64_t vaddr, uint64_t size);
	// Removes protection from a range and flushes GPU-owned data when required.
	template <typename Flush>
	void InvalidateRegion(uint64_t vaddr, uint64_t size, Flush&& on_flush) noexcept {
		static_assert(std::is_invocable_v<Flush&>);
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			const bool should_flush = [&] {
				// Perform both the GPU modification check and CPU state change with the lock in
				// case the GPU thread is racing to mark the page modified. If a flush is needed,
				// on_flush performs the CPU state change.
				std::scoped_lock lock(manager->lock);
				if (manager->IsModified<DirtySource::Gpu>(offset, bytes)) {
					return true;
				}
				manager->ChangeState<DirtySource::Cpu, true>(offset, bytes);
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

	template <bool clear, typename Func>
	void ForEachDownloadRange(uint64_t vaddr, uint64_t size, Func&& func) {
		static_assert(std::is_nothrow_invocable_v<Func&, uint64_t, uint64_t>);
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			std::scoped_lock lock(manager->lock);
			manager->ForEachModifiedRange<DirtySource::Gpu, false>(offset, bytes, func);
			if constexpr (clear) {
				manager->ChangeState<DirtySource::Gpu, false>(offset, bytes);
			}
		});
	}

	// The CPU-dirty epoch of the tracking region `index` (address / TRACKER_REGION_SIZE), which
	// advances whenever its pages become CPU dirty; 0 while the region is untracked.
	[[nodiscard]] uint64_t RegionCpuDirtyEpoch(uint64_t index) const {
		if (index >= REGION_COUNT) {
			EXIT("invalid memory tracker region\n");
		}
		const auto* manager = m_regions[index].load(std::memory_order_acquire);
		return manager != nullptr ? manager->RegionCpuDirtyEpoch() : 0;
	}

	// Calls func(address, size) for maximal runs of pages that may hold CPU writes not yet
	// uploaded, after releasing the region locks. Untracked regions count as dirty, as a new
	// region starts dirty. Runs at most max_gap bytes apart come as one, with the clean pages
	// between them.
	template <typename Func>
	void ForEachCpuDirtySpan(uint64_t vaddr, uint64_t size, uint64_t max_gap, Func&& func) {
		ValidateRange(vaddr, size);
		const auto                                  end = vaddr + size;
		std::vector<std::pair<uint64_t, uint64_t>> spans; // [begin, end)
		const auto add = [&](uint64_t begin, uint64_t finish) {
			begin  = std::max(begin, vaddr);
			finish = std::min(finish, end);
			if (begin >= finish) {
				return;
			}
			if (!spans.empty() && begin <= spans.back().second + max_gap) {
				spans.back().second = finish;
			} else {
				spans.emplace_back(begin, finish);
			}
		};
		for (uint64_t index = vaddr / TRACKER_REGION_SIZE; index * TRACKER_REGION_SIZE < end;
		     index++) {
			auto*      manager = m_regions[index].load(std::memory_order_acquire);
			const auto start   = std::max(index * TRACKER_REGION_SIZE, vaddr);
			const auto finish  = std::min((index + 1) * TRACKER_REGION_SIZE, end);
			if (manager == nullptr) {
				add(start, finish);
				continue;
			}
			if (!manager->HasCpuDirty()) {
				continue;
			}
			std::scoped_lock lock(manager->lock);
			manager->ForEachModifiedRange<DirtySource::Cpu, false>(
			    start - index * TRACKER_REGION_SIZE, finish - start,
			    [&](uint64_t address, uint64_t bytes) { add(address, address + bytes); });
		}
		for (const auto& [begin, finish]: spans) {
			func(begin, finish - begin);
		}
	}

	// Calls func(address, size) for the runs of pages of [vaddr, vaddr + size) that the CPU may
	// have written since the previous call over them: pages that became CPU dirty since then, or
	// are still CPU dirty. Untracked pages start tracking as written. A single consumer owns the
	// written state.
	template <typename Func>
	void ConsumeCpuWrites(uint64_t vaddr, uint64_t size, Func&& func) {
		std::vector<std::pair<uint64_t, uint64_t>> runs;
		Iterate<true>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			std::scoped_lock lock(manager->lock);
			manager->ConsumeCpuWritten(offset, bytes,
			                           [&](uint64_t address, uint64_t length) {
				                           if (!runs.empty() &&
				                               runs.back().first + runs.back().second == address) {
					                           runs.back().second += length;
				                           } else {
					                           runs.emplace_back(address, length);
				                           }
			                           });
		});
		const auto end = vaddr + size;
		for (const auto& [address, length]: runs) {
			const auto begin  = std::max(address, vaddr);
			const auto finish = std::min(address + length, end);
			if (begin < finish) {
				func(begin, finish - begin);
			}
		}
	}

	// Marks the pages of [vaddr, vaddr + size) GPU modified, except those that are CPU dirty, and
	// calls func(address, size) for the page runs it marked.
	template <typename Func>
	void MarkRegionAsGpuModifiedUnlessCpuDirty(uint64_t vaddr, uint64_t size, Func&& func) {
		std::vector<std::pair<uint64_t, uint64_t>> runs;
		Iterate<true>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			std::scoped_lock lock(manager->lock);
			manager->MarkGpuDirtyUnlessCpuDirty(
			    offset, bytes,
			    [&](uint64_t address, uint64_t length) { runs.emplace_back(address, length); });
		});
		for (const auto& [address, length]: runs) {
			func(address, length);
		}
	}

	template <typename RangeFunc, typename UploadFunc>
	void ForEachUploadRange(uint64_t vaddr, uint64_t size, bool is_written, RangeFunc&& range_func,
	                        UploadFunc&& upload_func) {
		static_assert(std::is_nothrow_invocable_v<RangeFunc&, uint64_t, uint64_t>);
		static_assert(std::is_nothrow_invocable_v<UploadFunc&>);
		Iterate<true>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			if (!is_written && !manager->IsModified<DirtySource::Cpu>(offset, bytes)) {
				return;
			}
			manager->lock.lock();
			manager->ForEachModifiedRange<DirtySource::Cpu, true>(offset, bytes, range_func);
			if (!is_written) {
				manager->lock.unlock();
			}
		});
		upload_func();
		if (is_written) {
			Iterate<false>(vaddr, size,
			               [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				               manager->ChangeState<DirtySource::Gpu, true>(offset, bytes);
				               manager->lock.unlock();
			               });
		}
	}

private:
	static constexpr size_t REGION_COUNT = TRACKER_ADDRESS_SIZE / TRACKER_REGION_SIZE;

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
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_
