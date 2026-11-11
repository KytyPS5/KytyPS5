#ifndef EMULATOR_SRC_COMMON_BITARRAY_H_
#define EMULATOR_SRC_COMMON_BITARRAY_H_

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

namespace Common {

// Words are accessed atomically, so an array whose writers hold a lock can be read without it.
template <size_t N>
class BitArray final {
	static_assert(N != 0, "BitArray size must be nonzero");
	static_assert(N % 64 == 0, "BitArray size must be a multiple of 64 bits");

	static constexpr size_t BITS_PER_WORD = 64;
	static constexpr size_t WORD_COUNT    = N / BITS_PER_WORD;

public:
	using Range = std::pair<size_t, size_t>;

	class Iterator final {
	public:
		using iterator_category = std::forward_iterator_tag;
		using value_type        = Range;
		using difference_type   = std::ptrdiff_t;
		using pointer           = const Range*;
		using reference         = const Range&;

		Iterator(const BitArray& bits, size_t start)
		    : m_bits(bits), m_range(bits.FirstRangeFrom(start)) {}

		Iterator& operator++() {
			m_range = m_bits.FirstRangeFrom(m_range.second);
			return *this;
		}

		[[nodiscard]] bool operator==(const Iterator& other) const {
			return &m_bits == &other.m_bits && m_range == other.m_range;
		}

		[[nodiscard]] bool operator!=(const Iterator& other) const { return !(*this == other); }

		[[nodiscard]] reference operator*() const { return m_range; }
		[[nodiscard]] pointer   operator->() const { return &m_range; }

	private:
		const BitArray& m_bits;
		Range           m_range;
	};

	using const_iterator = Iterator;

	constexpr BitArray() = default;
	BitArray(const BitArray& other): BitArray(other, 0, N) {}
	BitArray& operator=(const BitArray&) = delete;

	BitArray(const BitArray& other, size_t start, size_t end) {
		ForEachWord(start, end, [&](size_t word, uint64_t mask) {
			Store(word, other.Load(word) & mask);
			return false;
		});
	}

	[[nodiscard]] bool Get(size_t index) const {
		return (Load(index / BITS_PER_WORD) & (uint64_t {1} << (index % BITS_PER_WORD))) != 0;
	}

	void Set(size_t index) {
		const auto word = index / BITS_PER_WORD;
		Store(word, Load(word) | (uint64_t {1} << (index % BITS_PER_WORD)));
	}

	void Unset(size_t index) {
		const auto word = index / BITS_PER_WORD;
		Store(word, Load(word) & ~(uint64_t {1} << (index % BITS_PER_WORD)));
	}

	void SetRange(size_t start, size_t end) {
		ForEachWord(start, end, [this](size_t word, uint64_t mask) {
			Store(word, Load(word) | mask);
			return false;
		});
	}

	void UnsetRange(size_t start, size_t end) {
		ForEachWord(start, end, [this](size_t word, uint64_t mask) {
			Store(word, Load(word) & ~mask);
			return false;
		});
	}

	void Clear() {
		for (size_t word = 0; word < WORD_COUNT; word++) {
			Store(word, 0);
		}
	}

	void Fill() {
		for (size_t word = 0; word < WORD_COUNT; word++) {
			Store(word, ~uint64_t {0});
		}
	}

	[[nodiscard]] bool None() const {
		uint64_t combined = 0;
		for (size_t word = 0; word < WORD_COUNT; word++) {
			combined |= Load(word);
		}
		return combined == 0;
	}

	[[nodiscard]] bool Any(size_t start, size_t end) const {
		return ForEachWord(start, end,
		                   [this](size_t word, uint64_t mask) { return (Load(word) & mask) != 0; });
	}

	[[nodiscard]] Range FirstRangeFrom(size_t start) const {
		if (start >= N) {
			return {N, N};
		}

		auto word_index = start / BITS_PER_WORD;
		auto word       = Load(word_index) & StartMask(start);
		while (word == 0) {
			word_index++;
			if (word_index == WORD_COUNT) {
				return {N, N};
			}
			word = Load(word_index);
		}

		const auto first      = word_index * BITS_PER_WORD + std::countr_zero(word);
		const auto first_bit  = first % BITS_PER_WORD;
		const auto first_ones = static_cast<size_t>(std::countr_one(word >> first_bit));
		if (first_bit + first_ones < BITS_PER_WORD) {
			return {first, first + first_ones};
		}

		for (word_index++; word_index < WORD_COUNT; word_index++) {
			word = Load(word_index);
			if (word != ~uint64_t {0}) {
				return {first, word_index * BITS_PER_WORD + std::countr_one(word)};
			}
		}
		return {first, N};
	}

	[[nodiscard]] Range FirstRange() const { return FirstRangeFrom(0); }

	[[nodiscard]] Range LastRangeFrom(size_t end) const {
		if (end == 0) {
			return {0, 0};
		}
		if (end > N) {
			end = N;
		}

		auto word_index = (end - 1) / BITS_PER_WORD;
		auto word       = Load(word_index) & EndMask(end);
		while (word == 0) {
			if (word_index == 0) {
				return {0, 0};
			}
			word = Load(--word_index);
		}

		const auto empty_bits = static_cast<size_t>(std::countl_zero(word));
		const auto ones       = static_cast<size_t>(std::countl_one(word << empty_bits));
		const auto last       = (word_index + 1) * BITS_PER_WORD - empty_bits;
		if (empty_bits + ones < BITS_PER_WORD) {
			return {last - ones, last};
		}

		while (word_index != 0) {
			word = Load(--word_index);
			if (word != ~uint64_t {0}) {
				return {(word_index + 1) * BITS_PER_WORD - std::countl_one(word), last};
			}
		}
		return {0, last};
	}

	[[nodiscard]] Range LastRange() const { return LastRangeFrom(N); }

	[[nodiscard]] const_iterator begin() const { return Iterator(*this, 0); }
	[[nodiscard]] const_iterator end() const { return Iterator(*this, N); }

	[[nodiscard]] BitArray operator~() const {
		BitArray result;
		for (size_t word = 0; word < WORD_COUNT; word++) {
			result.Store(word, ~Load(word));
		}
		return result;
	}

private:
	static constexpr uint64_t StartMask(size_t start) {
		return ~uint64_t {0} << (start % BITS_PER_WORD);
	}

	static constexpr uint64_t EndMask(size_t end) {
		const auto end_bit = (end - 1) % BITS_PER_WORD;
		return end_bit == BITS_PER_WORD - 1 ? ~uint64_t {0} : (uint64_t {1} << (end_bit + 1)) - 1;
	}

	// Calls func(word, mask) for each word of [start, end), with mask selecting the bits in range,
	// until func returns true; returns whether it did.
	template <typename Func>
	static bool ForEachWord(size_t start, size_t end, Func&& func) {
		if (start >= end || end > N) {
			return false;
		}
		const auto first_word = start / BITS_PER_WORD;
		const auto last_word  = (end - 1) / BITS_PER_WORD;
		for (auto word = first_word; word <= last_word; word++) {
			const auto mask = (word == first_word ? StartMask(start) : ~uint64_t {0}) &
			                  (word == last_word ? EndMask(end) : ~uint64_t {0});
			if (func(word, mask)) {
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] uint64_t Load(size_t word) const {
		// A load never writes, so it may reference the word through a const member.
		return std::atomic_ref(const_cast<uint64_t&>(m_data[word])).load(std::memory_order_relaxed);
	}

	void Store(size_t word, uint64_t value) {
		std::atomic_ref(m_data[word]).store(value, std::memory_order_relaxed);
	}

	std::array<uint64_t, WORD_COUNT> m_data {};
};

} // namespace Common

#endif // EMULATOR_SRC_COMMON_BITARRAY_H_
