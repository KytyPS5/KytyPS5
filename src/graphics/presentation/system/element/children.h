#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_CHILDREN_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_CHILDREN_H_

#include "graphics/presentation/system/element/element.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <type_traits>
#include <vector>

namespace Libs::Graphics::SystemUi {

/// One entry in a braced list of children, either a single element or a run of them, so a builder
/// can be given Box(props, {title, children}) and spread both into one box. A single element is
/// held by value. A run points at the caller's storage, which need only last for the call. A piece
/// is only ever a braced-list argument and is never stored, as a run built from a temporary would
/// dangle once the statement ends.
class ChildPiece {
public:
	/// A piece of one element, of any kind, so a typed element goes straight into the list.
	template <class TProps>
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	constexpr ChildPiece(GenericElement<TProps> element): m_one(element), m_is_one(true) {}

	/// A piece of every element in `run`, in order, from anything that converts to a span of
	/// AnyElement. A run of typed elements doesn't, so it is gathered as AnyElement first.
	template <class Run>
	requires std::convertible_to<const Run&, std::span<const AnyElement>>
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	constexpr ChildPiece(const Run& run): m_many(run) {}

	/// The piece's elements in order, which point into the piece itself for a single element.
	[[nodiscard]] constexpr std::span<const AnyElement> Elements() const {
		return m_is_one ? std::span<const AnyElement>(&m_one, 1) : m_many;
	}

private:
	AnyElement                  m_one;
	std::span<const AnyElement> m_many;
	bool                        m_is_one = false;
};

/// How many children a braced list spreads without allocating. Lists in place are short, so this
/// covers them, and a longer one still works by moving to the heap.
inline constexpr size_t INLINE_CHILDREN = 16;

/// The children a builder is given, either listed in braces, as in {title, rows}, or as a run the
/// caller keeps, such as a vector. A builder takes it once as `const Children&` and gets both
/// forms. Braces are copied in, so the list may end with the constructor. A run is pointed at, so
/// it must last for the builder's call. The braced form points into itself, so a Children list is
/// never copied, moved or stored. Taken by const reference, a temporary one lasts for the call. A
/// local reference bound to one made from a temporary run dangles once the statement ends, as the
/// run ends with it, and so does a span taken from a temporary list, so pass a list straight to
/// the builder.
class Children {
public:
	/// No children.
	Children() = default;

	/// The elements of `pieces` laid end to end in order, kept inline when they fit.
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	Children(std::initializer_list<ChildPiece> pieces) {
		size_t total = 0;
		for (const ChildPiece& piece: pieces) {
			total += piece.Elements().size();
		}
		if (total > INLINE_CHILDREN) {
			m_spilled.reserve(total);
			for (const ChildPiece& piece: pieces) {
				m_spilled.insert(m_spilled.end(), piece.Elements().begin(), piece.Elements().end());
			}
			m_span = m_spilled;
			return;
		}
		size_t next = 0;
		for (const ChildPiece& piece: pieces) {
			for (const AnyElement element: piece.Elements()) {
				m_inline[next++] = element;
			}
		}
		m_span = std::span<const AnyElement>(m_inline.data(), total);
	}

	/// Every element of `run`, pointed at with no copy, from anything that converts to a span of
	/// AnyElement other than a Children list, which is passed on by reference instead.
	template <class Run>
	requires(!std::same_as<std::remove_cvref_t<Run>, Children> &&
	         std::convertible_to<const Run&, std::span<const AnyElement>>)
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	Children(const Run& run): m_span(run) {}

	Children(const Children&)            = delete;
	Children& operator=(const Children&) = delete;
	Children(Children&&)                 = delete;
	Children& operator=(Children&&)      = delete;

	/// The children as a span, so the list can be a piece of another braced list or reach the
	/// frame. It is valid while this lives.
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	operator std::span<const AnyElement>() const { return m_span; }

private:
	std::array<AnyElement, INLINE_CHILDREN> m_inline {};
	std::vector<AnyElement>                 m_spilled;
	std::span<const AnyElement>             m_span;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_CHILDREN_H_
