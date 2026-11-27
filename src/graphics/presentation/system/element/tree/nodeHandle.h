#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_TREE_NODE_HANDLE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_TREE_NODE_HANDLE_H_

#include <cstdint>
#include <type_traits>

// A handle to a node of the element tree. Internal to the element layer, so components don't
// include it.
namespace Libs::Graphics::SystemUi {

class NodeTree;

/// A handle to one node of a NodeTree. It holds the node's index and the serial of the tree when it
/// made the node, so a handle kept past Clear is recognised as stale instead of pointing at
/// whatever node now sits at that index. Only the tree can make a valid one.
class NodeHandle {
public:
	/// The handle that refers to nothing, which is also what a default-constructed one is.
	constexpr NodeHandle() = default;

	/// True for a handle that refers to no node.
	[[nodiscard]] constexpr bool IsNone() const { return m_serial == NONE_SERIAL; }

	/// Whether two handles refer to the same node made by the same tree serial.
	friend constexpr bool operator==(const NodeHandle&, const NodeHandle&) = default;

private:
	friend class NodeTree;

	/// The tree serial starts above this, so a default handle never matches a live tree.
	static constexpr uint32_t NONE_SERIAL = 0;

	/// Node `index` of the tree whose serial is `serial`. Only the tree hands these out.
	constexpr NodeHandle(uint32_t index, uint32_t serial): m_index(index), m_serial(serial) {}

	uint32_t m_index  = 0;
	uint32_t m_serial = NONE_SERIAL;
};

static_assert(sizeof(NodeHandle) == 8, "a handle is passed by value, so it stays two 32-bit words");
static_assert(std::is_trivially_copyable_v<NodeHandle>);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_TREE_NODE_HANDLE_H_
