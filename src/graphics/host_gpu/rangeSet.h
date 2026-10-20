#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_

#include "common/assert.h"

#include <algorithm>
#include <cstdint>
#include <map>

namespace Libs::Graphics {

class RangeSet final {
public:
	void Add(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		auto       it  = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second >= address) {
			it = std::prev(it);
		}
		uint64_t begin = address;
		uint64_t last  = end;
		while (it != m_ranges.end() && it->first <= last) {
			begin = std::min(begin, it->first);
			last  = std::max(last, it->second);
			it    = m_ranges.erase(it);
		}
		m_ranges.emplace(begin, last);
	}

	void Subtract(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		auto       it  = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second > address) {
			it = std::prev(it);
		}
		while (it != m_ranges.end() && it->first < end) {
			const auto begin = it->first;
			const auto last  = it->second;
			it               = m_ranges.erase(it);
			if (begin < address) {
				m_ranges.emplace(begin, address);
			}
			if (last > end) {
				m_ranges.emplace(end, last);
				break;
			}
		}
	}

	void Clear() { m_ranges.clear(); }

	template <typename Func>
	void ForEach(Func&& func) const {
		for (const auto& [begin, end]: m_ranges) {
			func(begin, end);
		}
	}

	[[nodiscard]] bool Intersects(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		auto       it  = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second > address) {
			return true;
		}
		return it != m_ranges.end() && it->first < end;
	}

	[[nodiscard]] bool Contains(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		auto       it  = m_ranges.upper_bound(address);
		if (it == m_ranges.begin()) {
			return false;
		}
		--it;
		return it->first <= address && it->second >= end;
	}

	template <typename Func>
	void ForEachInRange(uint64_t address, uint64_t size, Func&& func) const {
		const auto end = End(address, size);
		auto       it  = m_ranges.upper_bound(address);
		if (it != m_ranges.begin()) {
			--it;
		}
		for (; it != m_ranges.end() && it->first < end; ++it) {
			const auto begin = std::max(address, it->first);
			const auto last  = std::min(end, it->second);
			if (begin < last) {
				func(begin, last);
			}
		}
	}

	// Finds the mapped range containing address, or the closest one by gap distance.
	// Used to describe unhandled guest faults; never aborts.
	[[nodiscard]] bool Nearest(uint64_t address, uint64_t* begin, uint64_t* end) const {
		if (m_ranges.empty() || begin == nullptr || end == nullptr) {
			return false;
		}
		auto it = m_ranges.upper_bound(address);
		if (it != m_ranges.begin()) {
			const auto prev = std::prev(it);
			if (prev->first <= address && address < prev->second) {
				*begin = prev->first;
				*end   = prev->second;
				return true;
			}
		}
		const bool has_prev = it != m_ranges.begin();
		const bool has_next = it != m_ranges.end();
		if (has_prev && has_next) {
			const auto prev = std::prev(it);
			// Both gaps are strictly positive here (containment handled above).
			if (address - prev->second <= it->first - address) {
				*begin = prev->first;
				*end   = prev->second;
			} else {
				*begin = it->first;
				*end   = it->second;
			}
		} else if (has_prev) {
			const auto prev = std::prev(it);
			*begin          = prev->first;
			*end            = prev->second;
		} else {
			*begin = it->first;
			*end   = it->second;
		}
		return true;
	}

	[[nodiscard]] bool Empty() const { return m_ranges.empty(); }

private:
	static uint64_t End(uint64_t address, uint64_t size) {
		if (size == 0 || size > UINT64_MAX - address) {
			EXIT("invalid range-set address or size\n");
		}
		return address + size;
	}

	std::map<uint64_t, uint64_t> m_ranges;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
