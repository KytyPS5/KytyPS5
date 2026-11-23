#ifndef EMULATOR_SRC_COMMON_SMALLVECTOR_H_
#define EMULATOR_SRC_COMMON_SMALLVECTOR_H_

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <type_traits>

namespace Common {

// A vector of trivially copyable elements that keeps its first N inline.
template <class T, std::size_t N, typename SizeType = std::size_t>
class SmallVector {
	static_assert(N > 0, "SmallVector inline capacity must be > 0");
	static_assert(std::is_trivially_copyable_v<T>);

public:
	using size_type      = SizeType;
	using const_iterator = const T*;

	SmallVector() noexcept                     = default;
	SmallVector(const SmallVector&)            = delete;
	SmallVector& operator=(const SmallVector&) = delete;
	~SmallVector() {
		if (!is_inline()) {
			std::allocator<T> {}.deallocate(m_data, m_capacity);
		}
	}

	[[nodiscard]] const T&       front() const noexcept { return m_data[0]; }
	[[nodiscard]] const_iterator begin() const noexcept { return m_data; }
	[[nodiscard]] const_iterator end() const noexcept { return m_data + m_size; }
	[[nodiscard]] size_type      size() const noexcept { return m_size; }

	void push_back(const T& v) {
		if (m_size == m_capacity) [[unlikely]] {
			grow_to(m_capacity + m_capacity / 2 + 1);
		}
		std::construct_at(m_data + m_size, v);
		++m_size;
	}

	void erase(const_iterator pos) noexcept {
		const auto i = static_cast<size_type>(pos - m_data);
		std::copy(m_data + i + 1, m_data + m_size, m_data + i);
		--m_size;
	}

private:
	[[nodiscard]] bool is_inline() const noexcept { return m_data == m_inline.buf; }

	void grow_to(size_type new_cap) {
		T* nb = std::allocator<T> {}.allocate(new_cap);
		std::memcpy(nb, m_data, m_size * sizeof(T));
		if (!is_inline()) {
			std::allocator<T> {}.deallocate(m_data, m_capacity);
		}
		m_data     = nb;
		m_capacity = new_cap;
	}

	union Storage {
		Storage() noexcept {}
		T buf[N];
	};

	T*        m_data     = m_inline.buf;
	size_type m_size     = 0;
	size_type m_capacity = N;
	Storage   m_inline;
};

} // namespace Common

#endif // EMULATOR_SRC_COMMON_SMALLVECTOR_H_
