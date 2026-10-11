#include "graphics/host_gpu/memoryTracker.h"

#include "common/alignment.h"
#include "common/assert.h"

#include <bit>

namespace Libs::Graphics {

static_assert(std::atomic<void*>::is_always_lock_free);

/// Initializes region ownership tracking and the fixed CPU-dirty hint bitmaps.
MemoryTracker::MemoryTracker(PageManager& page_manager): m_page_manager(page_manager) {
	m_regions          = std::make_unique<std::atomic<RegionManager*>[]>(REGION_COUNT);
	m_cpu_dirty_words  = std::make_unique<std::atomic<uint64_t>[]>(DIRTY_WORD_COUNT);
	m_cpu_dirty_groups = std::make_unique<std::atomic<uint64_t>[]>(DIRTY_GROUP_COUNT);
}

MemoryTracker::~MemoryTracker() = default;

/// Publishes a region hint without allocating or changing page ownership.
/// Producers publish leaf bits before summary bits and the pending flag; a racing
/// consumer may perform an extra pass, while late publication remains discoverable.
void MemoryTracker::QueueCpuDirtyRegion(uint64_t index) noexcept {
	const auto word = index / 64;
	m_cpu_dirty_words[word].fetch_or(uint64_t {1} << (index % 64), std::memory_order_release);
	m_cpu_dirty_groups[word / 64].fetch_or(uint64_t {1} << (word % 64), std::memory_order_release);
	m_cpu_dirty_pending.store(true, std::memory_order_release);
}

/// Queues every tracker region intersecting a valid, nonempty guest range.
/// Concurrent producers may use this to request inspection of newly available data.
void MemoryTracker::QueueCpuDirtyRange(uint64_t vaddr, uint64_t size) {
	ValidateRange(vaddr, size);
	const auto end = (vaddr + size - 1) / TRACKER_REGION_SIZE;
	for (auto index = vaddr / TRACKER_REGION_SIZE; index <= end; ++index) {
		QueueCpuDirtyRegion(index);
	}
}

/// Replaces regions with the queued coarse ranges for the single GPU-thread consumer.
/// Exchanges consume hints before uploads; concurrent producers can publish another
/// pass. Locked page ownership checks remain authoritative and are not cleared here.
/// Must not be called from an upload callback or concurrently by multiple consumers.
void MemoryTracker::TakeCpuDirtyRegions(std::vector<GuestRange>& regions) {
	CheckNotInUploadCallback();
	regions.clear();
	if (!m_cpu_dirty_pending.exchange(false, std::memory_order_acquire)) return;
	// Consume before processing: concurrent writes publish a new hint for the next pass.
	// A producer interrupted between levels can cause an extra pass, never a lost write.
	for (size_t group = 0; group < DIRTY_GROUP_COUNT; ++group) {
		auto words = m_cpu_dirty_groups[group].exchange(0, std::memory_order_acquire);
		while (words != 0) {
			const auto word = group * 64 + std::countr_zero(words);
			words &= words - 1;
			auto bits = m_cpu_dirty_words[word].exchange(0, std::memory_order_acquire);
			while (bits != 0) {
				const auto index = word * 64 + std::countr_zero(bits);
				bits &= bits - 1;
				regions.push_back({index * TRACKER_REGION_SIZE, TRACKER_REGION_SIZE});
			}
		}
	}
}

#if KYTY_BUILD == KYTY_BUILD_DEBUG
void MemoryTracker::ValidateGpuDirtyPages(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
                                          const char* operation) const noexcept {
	if (!GuestRange {vaddr, size}.Valid() || (vaddr & (TRACKER_PAGE_SIZE - 1)) != 0 ||
	    (size & (TRACKER_PAGE_SIZE - 1)) != 0) {
		EXIT("MemoryTracker: invalid dirty-page validation range\n");
	}
	for (auto page = vaddr; page < vaddr + size; page += TRACKER_PAGE_SIZE) {
		if (!dirty.Intersects(page, TRACKER_PAGE_SIZE)) {
			EXIT("MemoryTracker: GPU-dirty tracker page has no dirty bytes, operation=%s "
			     "addr=0x%016" PRIx64 "\n",
			     operation, page);
		}
	}
}

void MemoryTracker::ValidateGpuDirtyOwnership(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
                                              const char* operation) {
	ValidateRange(vaddr, size);
	const auto begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	for (auto page = begin; page < end; page += TRACKER_PAGE_SIZE) {
		const bool has_dirty_bytes = dirty.Intersects(page, TRACKER_PAGE_SIZE);
		if (IsRegionGpuModified(page, TRACKER_PAGE_SIZE) != has_dirty_bytes) {
			EXIT("MemoryTracker: tracker and byte ownership disagree, operation=%s "
			     "addr=0x%016" PRIx64 "\n",
			     operation, page);
		}
	}
}
#endif

void MemoryTracker::ValidateRange(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("invalid memory tracker range\n");
	}
}

/// Publishes a new region under the creation mutex and queues its initial CPU data.
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
	QueueCpuDirtyRegion(index);
	return ptr;
}

bool MemoryTracker::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	return Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		return manager->IsModified<DirtySource::Cpu>(offset, bytes);
	});
}

bool MemoryTracker::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	return Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		return manager->IsModified<DirtySource::Gpu>(offset, bytes);
	});
}

/// Marks pages CPU-owned under their region locks and publishes dirty-region hints.
void MemoryTracker::MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	Iterate<true>(vaddr, size, [this](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		manager->ChangeState<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset, bytes);
		QueueCpuDirtyRegion(manager->GetCpuAddr() / TRACKER_REGION_SIZE);
	});
}

void MemoryTracker::MarkRegionAsGpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		manager->ChangeState<DirtySource::Gpu, true>(manager->GetCpuAddr() + offset, bytes);
	});
}

void MemoryTracker::UnmarkRegionAsGpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		manager->ChangeState<DirtySource::Gpu, false>(manager->GetCpuAddr() + offset, bytes);
	});
}

/// Releases tracked page protection after checking that no GPU-owned data remains.
/// Queues the released CPU-owned regions so later BDA preparation can inspect them.
void MemoryTracker::UntrackMemory(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
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
	Iterate<false>(vaddr, size, [this](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		manager->ChangeState<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset, bytes);
		QueueCpuDirtyRegion(manager->GetCpuAddr() / TRACKER_REGION_SIZE);
	});
}

} // namespace Libs::Graphics
