#include "graphics/host_gpu/memoryTracker.h"

#include "common/assert.h"
#include "graphics/host_gpu/bdaTestHooks.h"

namespace Libs::Graphics {

static_assert(std::atomic<void*>::is_always_lock_free);

MemoryTracker::MemoryTracker(PageManager& page_manager): m_page_manager(page_manager) {
	m_regions = std::make_unique<std::atomic<RegionManager*>[]>(REGION_COUNT);
	for (size_t i = 0; i < REGION_COUNT; i++) {
		m_regions[i].store(nullptr, std::memory_order_relaxed);
	}
	m_bda_hints = std::make_unique<std::atomic<uint64_t>[]>(BDA_HINT_WORDS);
	for (size_t i = 0; i < BDA_HINT_WORDS; i++) {
		m_bda_hints[i].store(0, std::memory_order_relaxed);
	}
}

MemoryTracker::~MemoryTracker() = default;

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
	const auto begin = vaddr & ~(TRACKER_PAGE_SIZE - 1);
	const auto end   = (vaddr + size + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
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

RegionManager* MemoryTracker::GetOrCreateRegion(uint64_t index) {
	if (auto* manager = m_regions[index].load(std::memory_order_acquire); manager != nullptr) {
		return manager;
	}
	std::lock_guard lock(m_region_mutex);
	if (auto* manager = m_regions[index].load(std::memory_order_acquire); manager != nullptr) {
		return manager;
	}
	auto  manager = std::make_unique<RegionManager>(m_page_manager, index * TRACKER_REGION_SIZE,
	                                                m_bda_hints[index / 64]);
	auto* ptr     = manager.get();
	m_region_storage.push_back(std::move(manager));
	// P2: a new region starts entirely CPU-dirty. Initialise, publish the hint, then publish
	// the pointer. A consumer that sees the hint while the pointer is still null synchronises
	// the region's mapped part through the legacy walk at once; that walk's Iterate<true>
	// waits on m_region_mutex here and receives this manager.
	ptr->PublishBdaHint();
	(void)BdaTestHooks::Fire(BdaTestHooks::Point::RegionHintPublished, index);
	m_regions[index].store(ptr, std::memory_order_release);
	return ptr;
}

void MemoryTracker::PublishBdaHints(uint64_t vaddr, uint64_t size) noexcept {
	if (size == 0 || vaddr >= TRACKER_ADDRESS_SIZE) {
		return;
	}
	const auto end   = size > TRACKER_ADDRESS_SIZE - vaddr ? TRACKER_ADDRESS_SIZE : vaddr + size;
	const auto first = vaddr / TRACKER_REGION_SIZE;
	const auto last  = (end - 1) / TRACKER_REGION_SIZE;
	for (auto region = first; region <= last; ++region) {
		m_bda_hints[region / 64].fetch_or(uint64_t {1} << (region % 64), std::memory_order_release);
	}
}

uint64_t MemoryTracker::ConsumeBdaHintWord(size_t word) noexcept {
	auto& hint = m_bda_hints[word];
	// A publication that happens-before this pass is visible to this relaxed load by
	// coherence, so the precheck cannot skip anything the current consumer must see. A
	// concurrent publication it misses stays pending for the next pass.
	if (hint.load(std::memory_order_relaxed) == 0) {
		return 0;
	}
	return hint.exchange(0, std::memory_order_acquire);
}

void MemoryTracker::RestoreBdaHints(size_t word, uint64_t bits) noexcept {
	if (bits != 0) {
		m_bda_hints[word].fetch_or(bits, std::memory_order_release);
	}
}

void MemoryTracker::DiscardBdaHints() noexcept {
	for (size_t word = 0; word < BDA_HINT_WORDS; word++) {
		(void)ConsumeBdaHintWord(word);
	}
}

bool MemoryTracker::IsBdaHintPending(uint64_t region) const noexcept {
	return region < REGION_COUNT &&
	       ((m_bda_hints[region / 64].load(std::memory_order_acquire) >> (region % 64)) & 1u) != 0;
}

RegionBits MemoryTracker::SnapshotCpuDirty(RegionManager& manager) {
	CheckNotInUploadCallback();
	std::scoped_lock lock(manager.lock);
	return manager.CpuDirtyBits();
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

void MemoryTracker::MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		manager->ChangeState<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset, bytes);
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
	Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		manager->ChangeState<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset, bytes);
	});
}

} // namespace Libs::Graphics
