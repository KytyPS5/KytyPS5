#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_RECENTGPUWRITES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_RECENTGPUWRITES_H_

#include "graphics/host_gpu/regionDefinitions.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace Libs::Graphics {

// Writes larger than this never enter the ring. Measured LEGO Batman data shows the many small
// finished writes are cheap to co-download, while a few huge ones carry most of the bytes and would
// make every readback pay for data that may never fault.
inline constexpr uint64_t RecentWriteMaxBytes = 64ull * 1024;
// About 58 small writes finish per readback and a frame holds several readbacks, so this spans a
// few frames. It is a starting value to tune by measurement.
inline constexpr size_t RecentGpuWriteCapacity = 1024;

/// Remembers the spans of recent small GPU writes with the tick that submitted each one. It is
/// only a hint of where the GPU wrote lately. Its users check the memory tracker before they
/// download, so a stale entry costs a lookup and never causes a wrong copy. Entries hold no buffer
/// reference. Used on the GPU thread only.
class RecentGpuWriteRing {
public:
	/// Records a write. Empty writes and writes over RecentWriteMaxBytes are ignored. It overwrites
	/// the oldest entry once the ring is full. A write with the same span as the newest entry only
	/// refreshes that entry's tick, so a buffer rewritten by many draws in a row does not flush
	/// older entries out.
	void Note(uint64_t address, uint64_t size, uint64_t tick) {
		if (size == 0 || size > RecentWriteMaxBytes) {
			return;
		}
		if (m_count > 0) {
			auto& newest = m_entries[NewestIndex()];
			if (newest.address == address && newest.size == size) {
				newest.tick = tick;
				return;
			}
		}
		m_entries[m_next] = {address, size, tick};
		m_next            = (m_next + 1) % RecentGpuWriteCapacity;
		m_count           = std::min(m_count + 1, RecentGpuWriteCapacity);
	}

	/// Walks the entries newest first and calls visit(address, size, finished) for each one. The
	/// entry is finished when its tick is at most known_tick, meaning the GPU has completed it. The
	/// span is rounded out to whole tracker pages, because the tracker works in pages. Overlapping
	/// entries all come out, so the caller drops spans it has already claimed. Pending entries are
	/// still visited so the caller can keep an older finished entry under a newer pending write
	/// out of a download. The walk stops early when visit returns false.
	template <class Visit>
	void ForEachNewest(uint64_t known_tick, Visit&& visit) const {
		for (size_t age = 0; age < m_count; ++age) {
			const auto& entry =
			    m_entries[(NewestIndex() + RecentGpuWriteCapacity - age) % RecentGpuWriteCapacity];
			// Mask rounding works because the tracker page size is a power of two.
			const uint64_t begin = entry.address & ~(TRACKER_PAGE_SIZE - 1);
			const uint64_t end =
			    (entry.address + entry.size + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
			if (!visit(begin, end - begin, entry.tick <= known_tick)) {
				return;
			}
		}
	}

	/// Returns how many entries the ring holds, at most RecentGpuWriteCapacity.
	[[nodiscard]] size_t Size() const { return m_count; }

private:
	struct Entry {
		uint64_t address = 0;
		uint64_t size    = 0;
		uint64_t tick    = 0;
	};
	// m_next is the slot the next write takes, so the newest entry sits just before it.
	[[nodiscard]] size_t NewestIndex() const {
		return (m_next + RecentGpuWriteCapacity - 1) % RecentGpuWriteCapacity;
	}
	std::array<Entry, RecentGpuWriteCapacity> m_entries{};
	size_t                                    m_next  = 0;
	size_t                                    m_count = 0;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_RECENTGPUWRITES_H_ */
