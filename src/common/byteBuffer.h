#ifndef KYTY_COMMON_BYTEBUFFER_H_
#define KYTY_COMMON_BYTEBUFFER_H_

#include "common/assert.h"
#include "common/common.h"

#include <algorithm>
#include <cstddef> // IWYU pragma: export
#include <utility>
#include <vector>

namespace Common {

using Byte = std::byte;

class ByteBuffer {
public:
	using DataType = std::vector<Byte>;

	static constexpr uint32_t INVALID_INDEX = static_cast<uint32_t>(-1);

	using iterator       = DataType::iterator;
	using const_iterator = DataType::const_iterator;

	ByteBuffer()                  = default;
	ByteBuffer(const ByteBuffer&) = default;
	ByteBuffer(ByteBuffer&&)      = default;

	ByteBuffer& operator=(const ByteBuffer&) = default;
	ByteBuffer& operator=(ByteBuffer&&)      = default;

	explicit ByteBuffer(uint32_t size): m_data(size) {}

	ByteBuffer(std::initializer_list<uint8_t> list) {
		for (const uint8_t& e: list) {
			m_data.emplace_back(static_cast<Byte>(e));
		}
	}

	explicit ByteBuffer(const void* buf, uint32_t size)
	    : ByteBuffer(size) // @suppress("Ambiguous problem")
	{
		EXIT_IF(buf == nullptr && size != 0);

		if (size != 0) {
			std::memcpy(m_data.data(), buf, size);
		}
	}

	[[nodiscard]] uint32_t Size() const { return static_cast<uint32_t>(m_data.size()); }
	[[nodiscard]] bool     IsEmpty() const { return m_data.empty(); }

	void Clear() { m_data.clear(); }

	void Add(const Byte& val) { m_data.push_back(val); }
	void Add(Byte&& val) { m_data.push_back(std::move(val)); }

	void Add(const Byte* val, uint32_t num) {
		EXIT_IF(val == nullptr && num != 0);
		m_data.insert(m_data.end(), val, val + num);
	}

	void Add(const ByteBuffer& v) {
		if (this == &v) {
			const ByteBuffer copy(v);
			Add(copy.GetDataConst(), copy.Size());
			return;
		}
		Add(v.GetDataConst(), v.Size());
	}

	void InsertAt(uint32_t index, const Byte& val) {
		EXIT_IF(index > Size());
		m_data.insert(m_data.begin() + index, val);
	}

	void InsertAt(uint32_t index, Byte&& val) {
		EXIT_IF(index > Size());
		m_data.insert(m_data.begin() + index, std::move(val));
	}

	[[nodiscard]] uint32_t Find(const Byte& t) const {
		const auto it = std::find(m_data.begin(), m_data.end(), t);
		return (it == m_data.end() ? INVALID_INDEX
		                           : static_cast<uint32_t>(std::distance(m_data.begin(), it)));
	}

	[[nodiscard]] bool Contains(const Byte& t) const { return Find(t) != INVALID_INDEX; }

	bool Remove(const Byte& t) {
		const auto it = std::find(m_data.begin(), m_data.end(), t);
		if (it == m_data.end()) {
			return false;
		}
		m_data.erase(it);
		return true;
	}

	bool RemoveAt(uint32_t index, uint32_t count = 1) {
		if (index >= Size() || count == 0) {
			return false;
		}

		const uint32_t erase_count = std::min(count, Size() - index);
		m_data.erase(m_data.begin() + index, m_data.begin() + index + erase_count);
		return true;
	}

	[[nodiscard]] bool IndexValid(uint32_t index) const { return index < Size(); }

	Byte& operator[](uint32_t index) {
		EXIT_IF(index >= Size());
		return m_data[index];
	}

	const Byte& operator[](uint32_t index) const {
		EXIT_IF(index >= Size());
		return m_data[index];
	}

	[[nodiscard]] const Byte& At(uint32_t index) const {
		EXIT_IF(index >= Size());
		return m_data[index];
	}

	Byte*                     GetData() { return m_data.data(); }
	[[nodiscard]] const Byte* GetData() const { return m_data.data(); }
	[[nodiscard]] const Byte* GetDataConst() const { return m_data.data(); }

	void Sort() {
		std::sort(m_data.begin(), m_data.end(), [](Byte a, Byte b) {
			return std::to_integer<uint8_t>(a) < std::to_integer<uint8_t>(b);
		});
	}

	template <typename OP>
	void Sort(OP&& comp_func) {
		std::sort(m_data.begin(), m_data.end(), std::forward<OP>(comp_func));
	}

	bool operator==(const ByteBuffer& s) const { return m_data == s.m_data; }
	bool operator!=(const ByteBuffer& s) const { return !(*this == s); }

	iterator begin() { return m_data.begin(); } // NOLINT(readability-identifier-naming)
	iterator end() { return m_data.end(); }     // NOLINT(readability-identifier-naming)
	[[nodiscard]] const_iterator begin() const {
		return m_data.begin();
	} // NOLINT(readability-identifier-naming)
	[[nodiscard]] const_iterator end() const {
		return m_data.end();
	} // NOLINT(readability-identifier-naming)
	[[nodiscard]] const_iterator cbegin() const {
		return m_data.cbegin();
	} // NOLINT(readability-identifier-naming)
	[[nodiscard]] const_iterator cend() const {
		return m_data.cend();
	} // NOLINT(readability-identifier-naming)

private:
	DataType m_data;
};

} // namespace Common

#endif /* KYTY_COMMON_BYTEBUFFER_H_ */
