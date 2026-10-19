#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUWRITETICKS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUWRITETICKS_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>

namespace Libs::Graphics {

// The submission (tick) that last wrote each range of guest memory on the GPU, for buffers that
// are guest memory itself (guestMemoryImport.h). The command processor reads such memory in
// place and waits only when a write of the bytes it reads is still to complete; a buffer's own
// last write would make it wait for writes anywhere in a buffer of many megabytes.
//
// Ranges do not overlap; a newer write of part of a range splits it. Entries of completed
// submissions are forgotten (ForgetCompleted).
class GpuWriteTicks final {
public:
	void Add(uint64_t address, uint64_t size, uint64_t tick) {
		if (size == 0) {
			return;
		}
		const auto end = address + size;
		Cut(address);
		Cut(end);
		m_ranges.erase(m_ranges.lower_bound(address), m_ranges.lower_bound(end));
		m_ranges.emplace(address, Range {end, tick});
	}

	// The newest tick of the writes of [address, address + size), or 0.
	[[nodiscard]] uint64_t LastWrite(uint64_t address, uint64_t size) const {
		const auto end   = address + size;
		uint64_t   tick  = 0;
		auto       range = m_ranges.upper_bound(address);
		if (range != m_ranges.begin()) {
			--range;
		}
		for (; range != m_ranges.end() && range->first < end; ++range) {
			if (range->second.end > address) {
				tick = std::max(tick, range->second.tick);
			}
		}
		return tick;
	}

	// Forgets the writes of the submissions up to `completed_tick`.
	void ForgetCompleted(uint64_t completed_tick) {
		std::erase_if(m_ranges, [&](const auto& entry) { return entry.second.tick <= completed_tick; });
	}

	[[nodiscard]] size_t Size() const noexcept { return m_ranges.size(); }

private:
	struct Range {
		uint64_t end  = 0;
		uint64_t tick = 0;
	};

	// Splits the range that contains `address` there.
	void Cut(uint64_t address) {
		auto range = m_ranges.upper_bound(address);
		if (range == m_ranges.begin()) {
			return;
		}
		--range;
		if (range->first < address && range->second.end > address) {
			m_ranges.emplace(address, Range {range->second.end, range->second.tick});
			range->second.end = address;
		}
	}

	std::map<uint64_t, Range> m_ranges;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUWRITETICKS_H_ */
