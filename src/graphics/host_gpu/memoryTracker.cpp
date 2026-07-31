#include "graphics/host_gpu/memoryTracker.h"

#include "common/assert.h"

namespace Libs::Graphics {

static_assert(std::atomic<void*>::is_always_lock_free);

MemoryTracker::MemoryTracker(PageManager& page_manager, PageWatchMode gpu_watch_mode)
    : m_page_manager(page_manager), m_gpu_watch_mode(gpu_watch_mode) {
	switch (m_gpu_watch_mode) {
		case PageWatchMode::Write:
		case PageWatchMode::ReadWrite: break;
		default: EXIT("unsupported memory tracker GPU page-watch mode\n");
	}
	m_regions = std::make_unique<std::atomic<RegionManager*>[]>(REGION_COUNT);
	for (size_t i = 0; i < REGION_COUNT; i++) {
		m_regions[i].store(nullptr, std::memory_order_relaxed);
	}
}

MemoryTracker::~MemoryTracker() = default;

void MemoryTracker::ValidateGpuDirtyPages(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
                                          const char* operation) const noexcept {
	if (vaddr == 0 || size == 0 || size > UINT64_MAX - vaddr ||
	    (vaddr & (TRACKER_PAGE_SIZE - 1)) != 0 || (size & (TRACKER_PAGE_SIZE - 1)) != 0) {
		EXIT("MemoryTracker: invalid dirty-page validation range\n");
	}
	for (auto page = vaddr; page < vaddr + size; page += TRACKER_PAGE_SIZE) {
		bool found = false;
		dirty.ForEachIntersection(page, TRACKER_PAGE_SIZE,
		                          [&found](RangeSet::Range) { found = true; });
		if (!found) {
			EXIT("MemoryTracker: GPU-dirty tracker page has no dirty bytes, operation=%s "
			     "addr=0x%016" PRIx64 "\n",
			     operation, page);
		}
	}
}

void MemoryTracker::ValidateGpuDirtyOwnership(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
                                              const char* operation) {
	ValidateRange(vaddr, size);
	if (vaddr + size > UINT64_MAX - (TRACKER_PAGE_SIZE - 1)) {
		EXIT("MemoryTracker: dirty ownership range alignment overflow\n");
	}
	const auto begin = vaddr & ~(TRACKER_PAGE_SIZE - 1);
	const auto end   = (vaddr + size + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
	for (auto page = begin; page < end; page += TRACKER_PAGE_SIZE) {
		bool has_dirty_bytes = false;
		dirty.ForEachIntersection(page, TRACKER_PAGE_SIZE,
		                          [&has_dirty_bytes](RangeSet::Range) { has_dirty_bytes = true; });
		if (IsRegionGpuModified(page, TRACKER_PAGE_SIZE) != has_dirty_bytes) {
			EXIT("MemoryTracker: tracker and byte ownership disagree, operation=%s "
			     "addr=0x%016" PRIx64 "\n",
			     operation, page);
		}
	}
}

void MemoryTracker::ValidateRange(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("invalid memory tracker range\n");
	}
}

RegionManager* MemoryTracker::GetOrCreateRegion(uint64_t index) {
	if (auto* manager = m_regions[index].load(std::memory_order_acquire); manager != nullptr) {
		return manager;
	}
	std::lock_guard lock(m_region_mutex);
	if (auto* manager = m_regions[index].load(std::memory_order_acquire); manager != nullptr) {
		return manager;
	}
	auto  manager = std::make_unique<RegionManager>(m_page_manager, index * TRACKER_REGION_SIZE);
	auto* ptr     = manager.get();
	m_region_storage.push_back(std::move(manager));
	m_regions[index].store(ptr, std::memory_order_release);
	return ptr;
}

bool MemoryTracker::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	std::lock_guard access(m_access_mutex);
	return Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		return manager->IsModified<DirtySource::Cpu>(offset, bytes);
	});
}

bool MemoryTracker::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	std::lock_guard access(m_access_mutex);
	return Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		return manager->IsModified<DirtySource::Gpu>(offset, bytes);
	});
}

void MemoryTracker::MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	std::lock_guard access(m_access_mutex);
	Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		const auto       changed =
		    manager->ChangeState<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset, bytes);
		manager->ApplyProtection(changed, false);
	});
}

void MemoryTracker::MarkRegionAsGpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	std::lock_guard access(m_access_mutex);
	Iterate<true>(vaddr, size, [this](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		const auto       changed =
		    manager->ChangeState<DirtySource::Gpu, true>(manager->GetCpuAddr() + offset, bytes);
		manager->ApplyGpuProtection(changed, true, m_gpu_watch_mode);
	});
}

void MemoryTracker::UnmarkRegionAsGpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	std::lock_guard access(m_access_mutex);
	Iterate<true>(vaddr, size, [this](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		if (!manager->IsFullyModified<DirtySource::Gpu>(offset, bytes)) {
			EXIT("cannot clear partially GPU-dirty tracking range\n");
		}
		const auto changed =
		    manager->ChangeState<DirtySource::Gpu, false>(manager->GetCpuAddr() + offset, bytes);
		manager->ApplyGpuProtection(changed, false, m_gpu_watch_mode);
	});
}

void MemoryTracker::UntrackMemoryLocked(uint64_t vaddr, uint64_t size) {
	std::vector<RegionManager*> managers;
	managers.reserve((vaddr % TRACKER_REGION_SIZE + size + TRACKER_REGION_SIZE - 1) /
	                 TRACKER_REGION_SIZE);
	Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t, uint64_t) {
		managers.push_back(manager);
	});

	std::vector<std::unique_lock<TrackingSpinLock>> locks;
	locks.reserve(managers.size());
	for (auto* manager: managers) {
		locks.emplace_back(manager->lock);
	}
	if (Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		    return manager->IsModified<DirtySource::Gpu>(offset, bytes);
	    })) {
		EXIT("cannot untrack GPU-dirty memory\n");
	}
	Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		const auto changed =
		    manager->ChangeState<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset, bytes);
		manager->ApplyProtection(changed, false);
	});
	locks.clear();
}

void MemoryTracker::UntrackMemory(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	std::lock_guard access(m_access_mutex);
	UntrackMemoryLocked(vaddr, size);
}

} // namespace Libs::Graphics
