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
	using iterator       = T*;
	using const_iterator = const T*;

	SmallVector() noexcept = default;
	SmallVector(const SmallVector& other) { *this = other; }
	SmallVector(SmallVector&& other) noexcept { *this = std::move(other); }
	SmallVector& operator=(const SmallVector& other) {
		if (this == &other) {
			return *this;
		}
		clear();
		if (other.m_size > m_capacity) {
			grow_to(other.m_size);
		}
		if (other.m_size != 0) {
			std::memcpy(m_data, other.m_data, other.m_size * sizeof(T));
		}
		m_size = other.m_size;
		return *this;
	}
	SmallVector& operator=(SmallVector&& other) noexcept {
		if (this == &other) {
			return *this;
		}
		if (!is_inline()) {
			std::allocator<T> {}.deallocate(m_data, m_capacity);
		}
		if (other.is_inline()) {
			m_data     = m_inline.buf;
			m_capacity = N;
			if (other.m_size != 0) {
				std::memcpy(m_data, other.m_data, other.m_size * sizeof(T));
			}
		} else {
			m_data           = other.m_data;
			m_capacity       = other.m_capacity;
			other.m_data     = other.m_inline.buf;
			other.m_capacity = N;
		}
		m_size       = other.m_size;
		other.m_size = 0;
		return *this;
	}
	~SmallVector() {
		if (!is_inline()) {
			std::allocator<T> {}.deallocate(m_data, m_capacity);
		}
	}

	[[nodiscard]] T&             operator[](size_type index) noexcept { return m_data[index]; }
	[[nodiscard]] const T&       operator[](size_type index) const noexcept { return m_data[index]; }
	[[nodiscard]] T&             front() noexcept { return m_data[0]; }
	[[nodiscard]] const T&       front() const noexcept { return m_data[0]; }
	[[nodiscard]] T*             data() noexcept { return m_data; }
	[[nodiscard]] const T*       data() const noexcept { return m_data; }
	[[nodiscard]] iterator       begin() noexcept { return m_data; }
	[[nodiscard]] const_iterator begin() const noexcept { return m_data; }
	[[nodiscard]] iterator       end() noexcept { return m_data + m_size; }
	[[nodiscard]] const_iterator end() const noexcept { return m_data + m_size; }
	[[nodiscard]] size_type      size() const noexcept { return m_size; }
	[[nodiscard]] bool           empty() const noexcept { return m_size == 0; }

	void clear() noexcept { m_size = 0; }

	void push_back(const T& v) {
		if (m_size == m_capacity) [[unlikely]] {
			grow_to(m_capacity + m_capacity / 2 + 1);
		}
		std::construct_at(m_data + m_size, v);
		++m_size;
	}

	template <typename... Args>
	T& emplace_back(Args&&... args) {
		if (m_size == m_capacity) [[unlikely]] {
			grow_to(m_capacity + m_capacity / 2 + 1);
		}
		T* ptr = std::construct_at(m_data + m_size, std::forward<Args>(args)...);
		++m_size;
		return *ptr;
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
