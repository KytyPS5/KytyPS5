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

class MemoryTracker final {
public:
	explicit MemoryTracker(PageManager&  page_manager,
	                       PageWatchMode gpu_watch_mode = PageWatchMode::ReadWrite);
	~MemoryTracker();

	KYTY_CLASS_NO_COPY(MemoryTracker);

	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	void               MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size);
	void               MarkRegionAsGpuModified(uint64_t vaddr, uint64_t size);
	void               UnmarkRegionAsGpuModified(uint64_t vaddr, uint64_t size);
	void               UntrackMemory(uint64_t vaddr, uint64_t size);
	template <typename Flush>
	void InvalidateRegion(uint64_t vaddr, uint64_t size, Flush&& on_flush) {
		static_assert(std::is_invocable_v<Flush&>);
		CheckNotInUploadCallback();
		ValidateRange(vaddr, size);

		const auto update_cpu_state = [this, vaddr, size] {
			std::lock_guard             access(m_access_mutex);
			std::vector<RegionManager*> managers;
			Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t, uint64_t) {
				managers.push_back(manager);
			});
			std::vector<std::unique_lock<TrackingSpinLock>> locks;
			locks.reserve(managers.size());
			for (auto* manager: managers) {
				locks.emplace_back(manager->lock);
			}
			const bool gpu_modified = Iterate<false>(
			    vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				    return manager->IsModified<DirtySource::Gpu>(offset, bytes);
			    });
			if (gpu_modified) {
				return true;
			}
			Iterate<false>(vaddr, size,
			               [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				               const auto changed = manager->ChangeState<DirtySource::Cpu, true>(
				                   manager->GetCpuAddr() + offset, bytes);
				               manager->ApplyProtection(changed, false);
			               });
			return false;
		};

		if (!update_cpu_state()) {
			return;
		}
		std::forward<Flush>(on_flush)();
		if (update_cpu_state()) {
			EXIT("memory invalidation retained GPU-owned pages\n");
		}
	}
	void ValidateGpuDirtyPages(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
	                           const char* operation) const noexcept;
	void ValidateGpuDirtyOwnership(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
	                               const char* operation);

	template <bool clear, typename Preflight, typename Func>
	void ForEachDownloadRange(uint64_t vaddr, uint64_t size, Preflight&& preflight, Func&& func) {
		static_assert(std::is_nothrow_invocable_v<Preflight&, uint64_t, uint64_t>);
		static_assert(std::is_nothrow_invocable_v<Func&, uint64_t, uint64_t>);
		CheckNotInUploadCallback();
		std::lock_guard             access(m_access_mutex);
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
				               const auto changed =
				                   manager->template ForEachModifiedRange<DirtySource::Gpu, true>(
				                       address, bytes, [](uint64_t, uint64_t) noexcept {});
				               manager->ApplyGpuProtection(changed, false, m_gpu_watch_mode);
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
		std::unique_lock access(m_access_mutex);
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
			Iterate<false>(
			    vaddr, size, [this](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				    const auto changed = manager->template ChangeState<DirtySource::Gpu, true>(
				        manager->GetCpuAddr() + offset, bytes);
				    manager->ApplyGpuProtection(changed, true, m_gpu_watch_mode);
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
	void           UntrackMemoryLocked(uint64_t vaddr, uint64_t size);
	RegionManager* GetOrCreateRegion(uint64_t index);

	std::unique_ptr<std::atomic<RegionManager*>[]> m_regions;
	std::vector<std::unique_ptr<RegionManager>>    m_region_storage;
	std::mutex                                     m_region_mutex;
	std::mutex                                     m_access_mutex;
	PageManager&                                   m_page_manager;
	PageWatchMode                                  m_gpu_watch_mode = PageWatchMode::ReadWrite;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_
