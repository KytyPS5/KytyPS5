#ifndef KYTY_COMMON_INLINEFUNCTION_H_
#define KYTY_COMMON_INLINEFUNCTION_H_

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace Common {

// A move-only void() callable stored in place when it fits in Capacity bytes, so queuing it
// does not allocate; larger callables fall back to the heap.
template <size_t Capacity>
class InlineFunction {
public:
	InlineFunction() noexcept = default;

	template <typename F, typename Callable = std::decay_t<F>,
	          typename = std::enable_if_t<!std::is_same_v<Callable, InlineFunction>>>
	InlineFunction(F&& function) { // NOLINT(google-explicit-constructor)
		if constexpr (Fits<Callable>) {
			::new (static_cast<void*>(m_storage)) Callable(std::forward<F>(function));
			m_invoke = [](void* storage) { (*std::launder(static_cast<Callable*>(storage)))(); };
			m_manage = [](void* target, void* source) {
				auto* callable = std::launder(static_cast<Callable*>(source));
				if (target != nullptr) {
					::new (target) Callable(std::move(*callable));
				}
				callable->~Callable();
			};
		} else {
			auto* callable = new Callable(std::forward<F>(function));
			::new (static_cast<void*>(m_storage)) Callable*(callable);
			m_invoke = [](void* storage) { (**std::launder(static_cast<Callable**>(storage)))(); };
			m_manage = [](void* target, void* source) {
				auto* pointer = *std::launder(static_cast<Callable**>(source));
				if (target != nullptr) {
					::new (target) Callable*(pointer);
				} else {
					delete pointer;
				}
			};
		}
	}

	InlineFunction(InlineFunction&& other) noexcept { MoveFrom(other); }
	InlineFunction& operator=(InlineFunction&& other) noexcept {
		if (this != &other) {
			Reset();
			MoveFrom(other);
		}
		return *this;
	}
	InlineFunction(const InlineFunction&)            = delete;
	InlineFunction& operator=(const InlineFunction&) = delete;
	~InlineFunction() { Reset(); }

	void operator()() { m_invoke(m_storage); }

	explicit operator bool() const noexcept { return m_invoke != nullptr; }

	void Reset() noexcept {
		if (m_manage != nullptr) {
			m_manage(nullptr, m_storage);
		}
		m_invoke = nullptr;
		m_manage = nullptr;
	}

private:
	template <typename Callable>
	static constexpr bool Fits = sizeof(Callable) <= Capacity &&
	                             alignof(Callable) <= alignof(std::max_align_t) &&
	                             std::is_nothrow_move_constructible_v<Callable>;

	void MoveFrom(InlineFunction& other) noexcept {
		if (other.m_manage != nullptr) {
			other.m_manage(m_storage, other.m_storage);
		}
		m_invoke       = other.m_invoke;
		m_manage       = other.m_manage;
		other.m_invoke = nullptr;
		other.m_manage = nullptr;
	}

	alignas(std::max_align_t) std::byte m_storage[Capacity];
	void (*m_invoke)(void* storage)             = nullptr;
	void (*m_manage)(void* target, void* source) = nullptr;
};

} // namespace Common

#endif // KYTY_COMMON_INLINEFUNCTION_H_
