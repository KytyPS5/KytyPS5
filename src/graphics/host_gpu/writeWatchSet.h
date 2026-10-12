#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_WRITEWATCHSET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_WRITEWATCHSET_H_

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

// A reader watches a guest address range. Every write that touches the range gives the watch a new
// generation. The reader keeps the generation it last acted on; an equal generation proves nothing
// wrote to the range since. Generations come from one counter, so a value is never seen twice.
//
// Writers (the GPU thread and the page-fault thread) call NotifyWrite; the lock is short.

namespace Libs::Graphics {

class WriteWatchSet {
public:
	using Id = uint32_t;

	// Watching a range that is already watched returns the same id.
	[[nodiscard]] Id Watch(uint64_t address, uint64_t size) {
		std::scoped_lock lock {m_mutex};
		const auto       range = m_by_start.equal_range(address);
		for (auto it = range.first; it != range.second; ++it) {
			if (it->second.size == size) {
				return it->second.id;
			}
		}
		Id id = 0;
		if (m_free_ids.empty()) {
			id = static_cast<Id>(m_generations.size());
			m_generations.push_back(m_clock);
		} else {
			id = m_free_ids.back();
			m_free_ids.pop_back();
			// A reused id never repeats a generation its previous owner saw.
			m_generations[id] = ++m_clock;
		}
		m_by_start.emplace(address, Entry {size, id});
		m_longest = std::max(m_longest, size);
		return id;
	}

	// Ends a watch. The id may be given to a later Watch.
	void Unwatch(Id id) {
		std::scoped_lock lock {m_mutex};
		const auto       entry = std::ranges::find_if(
		    m_by_start, [id](const auto& watched) { return watched.second.id == id; });
		if (entry != m_by_start.end()) {
			m_by_start.erase(entry);
			m_free_ids.push_back(id);
		}
	}

	void NotifyWrite(uint64_t address, uint64_t size) {
		if (size == 0) {
			return;
		}
		std::scoped_lock lock {m_mutex};
		if (m_by_start.empty()) {
			return;
		}
		const auto end   = address + size;
		const auto first = address > m_longest ? address - m_longest : 0;
		// A watch that starts before `first` is longer than any watch, so it cannot reach
		// `address`.
		for (auto it = m_by_start.lower_bound(first); it != m_by_start.end() && it->first < end;
		     ++it) {
			if (it->first + it->second.size > address) {
				m_generations[it->second.id] = ++m_clock;
			}
		}
	}

	[[nodiscard]] uint64_t Generation(Id id) const {
		std::scoped_lock lock {m_mutex};
		return m_generations[id];
	}

private:
	struct Entry {
		uint64_t size = 0;
		Id       id   = 0;
	};

	mutable std::mutex             m_mutex;
	std::multimap<uint64_t, Entry> m_by_start;
	std::vector<uint64_t>          m_generations;
	std::vector<Id>                m_free_ids;
	uint64_t                       m_longest = 0;
	uint64_t                       m_clock   = 1;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_WRITEWATCHSET_H_ */
