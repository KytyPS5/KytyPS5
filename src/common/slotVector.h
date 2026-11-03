#ifndef EMULATOR_SRC_COMMON_SLOTVECTOR_H_
#define EMULATOR_SRC_COMMON_SLOTVECTOR_H_

#include "common/common.h"

#include <compare>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <utility>
#include <vector>

namespace Common {

struct SlotId {
	static constexpr uint32_t INVALID_INDEX = std::numeric_limits<uint32_t>::max();

	constexpr SlotId() noexcept = default;
	constexpr SlotId(uint32_t index) noexcept: index(index) {}

	[[nodiscard]] constexpr explicit operator bool() const noexcept {
		return index != INVALID_INDEX;
	}
	constexpr auto operator<=>(const SlotId&) const noexcept = default;

	uint32_t index = INVALID_INDEX;
};

// Fixed-size chunks keep the stored, non-movable resources in place. Occupancy is a byte per
// slot, not a bitset: a deferred erase runs while fault threads read other slots.
template <typename T>
class SlotVector {
	static constexpr uint32_t ChunkSize = 256;

public:
	SlotVector() = default;
	KYTY_CLASS_NO_COPY(SlotVector);

	~SlotVector() noexcept {
		ForEach([](SlotId, T& value) { value.~T(); });
	}

	[[nodiscard]] T& operator[](SlotId id) noexcept {
		return m_chunks[id.index / ChunkSize][id.index % ChunkSize].object;
	}

	[[nodiscard]] const T& operator[](SlotId id) const noexcept {
		return m_chunks[id.index / ChunkSize][id.index % ChunkSize].object;
	}

	[[nodiscard]] T* try_get(SlotId id) noexcept {
		return is_allocated(id) ? &(*this)[id] : nullptr;
	}

	[[nodiscard]] const T* try_get(SlotId id) const noexcept {
		return is_allocated(id) ? &(*this)[id] : nullptr;
	}

	[[nodiscard]] bool is_allocated(SlotId id) const noexcept {
		return id.index < m_stored.size() && m_stored[id.index] != 0;
	}

	template <typename... Args>
	[[nodiscard]] SlotId insert(Args&&... args) {
		if (m_free_list.empty()) {
			Reserve();
		}
		const uint32_t index = m_free_list.back();
		m_free_list.pop_back();
		new (&(*this)[index]) T(std::forward<Args>(args)...);
		m_stored[index] = 1;
		return SlotId {index};
	}

	void erase(SlotId id) noexcept {
		(*this)[id].~T();
		m_stored[id.index] = 0;
		m_free_list.push_back(id.index);
	}

	template <typename F>
	void ForEach(F&& fn) {
		for (uint32_t index = 0; index < m_stored.size(); ++index) {
			if (m_stored[index] != 0) {
				fn(SlotId {index}, (*this)[index]);
			}
		}
	}

	template <typename F>
	void ForEach(F&& fn) const {
		for (uint32_t index = 0; index < m_stored.size(); ++index) {
			if (m_stored[index] != 0) {
				fn(SlotId {index}, (*this)[index]);
			}
		}
	}

private:
	union Slot {
		Slot() noexcept {}
		~Slot() noexcept {}

		T object;
	};

	void Reserve() {
		const auto first = static_cast<uint32_t>(m_stored.size());
		m_chunks.push_back(std::make_unique<Slot[]>(ChunkSize));
		m_stored.resize(first + ChunkSize);
		m_free_list.resize(ChunkSize);
		std::iota(m_free_list.rbegin(), m_free_list.rend(), first);
	}

	std::vector<std::unique_ptr<Slot[]>> m_chunks;
	std::vector<uint8_t>                 m_stored;
	std::vector<uint32_t>                m_free_list;
};

} // namespace Common

template <>
struct std::hash<Common::SlotId> {
	[[nodiscard]] size_t operator()(Common::SlotId id) const noexcept {
		return std::hash<uint32_t> {}(id.index);
	}
};

#endif // EMULATOR_SRC_COMMON_SLOTVECTOR_H_
