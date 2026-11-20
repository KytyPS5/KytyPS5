#include "graphics/host_gpu/renderer/pipeline/strictWatch.h"

#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <algorithm>

namespace Libs::Graphics {

StrictWatch::~StrictWatch() {
	std::scoped_lock lock {m_mutex};
	for (auto& range: m_ranges) {
		Unwatch(*range, 0, range->versions.size());
	}
}

uint64_t StrictWatch::Watch(uint64_t address, uint64_t size) {
	std::scoped_lock lock {m_mutex};
	Arm(Find(address, size));
	return m_version;
}

void StrictWatch::Changed(uint64_t address, uint64_t size, uint64_t version,
                          std::vector<std::pair<uint64_t, uint64_t>>& runs) {
	runs.clear();
	std::scoped_lock lock {m_mutex};
	auto& range = Find(address, size);
	Arm(range);
	const auto end = address + size;
	for (size_t page = 0; page < range.versions.size();) {
		if (range.versions[page] <= version) {
			page++;
			continue;
		}
		const auto first = page;
		while (page < range.versions.size() && range.versions[page] > version) {
			page++;
		}
		const auto begin = std::max(address, range.base + first * TRACKER_PAGE_SIZE);
		const auto stop  = std::min(end, range.base + page * TRACKER_PAGE_SIZE);
		runs.emplace_back(begin - address, stop - begin);
	}
}

void StrictWatch::UnmapMemory(uint64_t address, uint64_t size) {
	std::scoped_lock lock {m_mutex};
	std::erase_if(m_ranges, [&](const std::unique_ptr<Range>& range) {
		if (address >= range->end || range->base >= address + size) {
			return false;
		}
		Unwatch(*range, 0, range->versions.size());
		return true;
	});
	UpdateBounds();
}

StrictWatch::Range& StrictWatch::Find(uint64_t address, uint64_t size) {
	const auto base = address & ~(TRACKER_PAGE_SIZE - 1);
	const auto end  = (address + size + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
	for (auto& range: m_ranges) {
		if (range->base == base && range->end == end) {
			range->used = ++m_uses;
			return *range;
		}
	}
	if (m_ranges.size() >= MaxRanges) {
		// The least recently used range goes: the reads of it compare whole again.
		const auto oldest = std::ranges::min_element(
		    m_ranges, [](const auto& a, const auto& b) { return a->used < b->used; });
		Unwatch(**oldest, 0, (*oldest)->versions.size());
		m_ranges.erase(oldest);
	}
	auto range  = std::make_unique<Range>();
	range->base = base;
	range->end  = end;
	range->used = ++m_uses;
	range->versions.assign((end - base) / TRACKER_PAGE_SIZE, 0);
	m_ranges.push_back(std::move(range));
	UpdateBounds();
	return *m_ranges.back();
}

void StrictWatch::Arm(Range& range) {
	bool bumped = false;
	for (size_t page = 0; page < range.versions.size();) {
		if (range.versions[page] != 0) {
			page++;
			continue;
		}
		if (!bumped) {
			m_version++;
			bumped = true;
		}
		const auto first = page;
		for (; page < range.versions.size() && range.versions[page] == 0; page++) {
			range.versions[page] = m_version;
		}
		// Watched before the bytes are read: a write during the read faults.
		m_page_manager.UpdatePageWatchers<true>(range.base + first * TRACKER_PAGE_SIZE,
		                                        (page - first) * TRACKER_PAGE_SIZE);
	}
}

void StrictWatch::Unwatch(Range& range, size_t first, size_t last) {
	for (size_t page = first; page < last;) {
		if (range.versions[page] == 0) {
			page++;
			continue;
		}
		const auto begin = page;
		for (; page < last && range.versions[page] != 0; page++) {
			range.versions[page] = 0;
		}
		m_page_manager.UpdatePageWatchers<false>(range.base + begin * TRACKER_PAGE_SIZE,
		                                         (page - begin) * TRACKER_PAGE_SIZE);
	}
}

void StrictWatch::Invalidate(uint64_t address, uint64_t size) {
	std::scoped_lock lock {m_mutex};
	for (auto& range: m_ranges) {
		const auto begin = std::max(address, range->base);
		const auto end   = std::min(address + size, range->end);
		if (begin >= end) {
			continue;
		}
		Unwatch(*range, (begin - range->base) / TRACKER_PAGE_SIZE,
		        (end - 1 - range->base) / TRACKER_PAGE_SIZE + 1);
	}
}

void StrictWatch::UpdateBounds() {
	uint64_t begin = UINT64_MAX;
	uint64_t end   = 0;
	for (const auto& range: m_ranges) {
		begin = std::min(begin, range->base);
		end   = std::max(end, range->end);
	}
	m_begin.store(begin, std::memory_order_release);
	m_end.store(end, std::memory_order_release);
}

} // namespace Libs::Graphics
