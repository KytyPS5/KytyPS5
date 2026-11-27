#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_ELEMENT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_ELEMENT_H_

#include "graphics/presentation/system/element/tree/nodeHandle.h"

#include <array>
#include <cstddef>
#include <type_traits>

namespace Libs::Graphics::SystemUi {

struct FrameTestHooks;

/// A handle to one element of the frame whose props are a `Props`, or of any kind when `Props` is
/// void. It wraps the handle of the element's node, and is a type of its own so a handle from some
/// other node tree can't be passed where an element is expected. Components pass elements around by
/// value and never see the node itself. Only the frame can make a valid one. Every kind shares this
/// one template, so the typed and untyped handles have a single body between them. The two are
/// unrelated types, so nothing can cast an untyped element to a kind without the frame.
template <class TProps>
class GenericElement {
public:
	/// The element that refers to nothing, which is also what a default-constructed one is.
	constexpr GenericElement() = default;

	/// True for an element that refers to no node.
	[[nodiscard]] constexpr bool IsNone() const { return m_node.IsNone(); }

	/// The same element without its kind, for anything that takes an element of any kind. It is
	/// implicit so a typed element can go straight into a list of children. The untyped element
	/// has none, as it would only convert to itself.
	// NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	constexpr operator GenericElement<void>() const
	requires(!std::is_void_v<TProps>)
	{
		return GenericElement<void>(m_node);
	}

	/// Whether two elements of the same kind refer to the same node made in the same frame serial.
	/// A typed element also compares with an untyped one through the conversion, and elements of
	/// two different kinds don't compare, as they can never be the same element.
	friend constexpr bool operator==(const GenericElement&, const GenericElement&) = default;

private:
	friend class FrameBuilder;
	friend struct FrameTestHooks;
	/// Lets a typed element make its untyped self through the private constructor.
	template <class Other>
	friend class GenericElement;

	/// The element at `node`. Only the frame and the typed elements make one from a raw node.
	constexpr explicit GenericElement(NodeHandle node): m_node(node) {}

	/// The handle of the element's node in the frame's tree.
	[[nodiscard]] constexpr NodeHandle Handle() const { return m_node; }

	NodeHandle m_node;
};

/// An element of any kind, as a list of children, the tree and the element stack hold them.
using AnyElement = GenericElement<void>;

/// An element whose props are a `TProps`, so a function that suits only one kind can take nothing
/// else.
template <class TProps>
using Element = GenericElement<TProps>;

/// A fixed number of elements of any kind, for a list of children whose length is known where it
/// is built, kept on the stack rather than in a vector.
template <size_t TLength>
using ElementList = std::array<AnyElement, TLength>;

static_assert(sizeof(AnyElement) == sizeof(NodeHandle), "an element costs no more than its handle");
static_assert(std::is_trivially_copyable_v<AnyElement>);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_ELEMENT_H_
