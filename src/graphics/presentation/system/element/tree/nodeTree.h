#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_TREE_NODE_TREE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_TREE_NODE_TREE_H_

#include "graphics/presentation/system/element/tree/node.h"
#include "graphics/presentation/system/element/tree/nodeHandle.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The tree of nodes a frame's elements sit in. Internal to the element layer, so components don't
// include it.
namespace Libs::Graphics::SystemUi {

/// Why the tree refused a handle.
enum class TreeError : uint8_t {
	None,            // the handle refers to nothing
	Stale,           // made before the tree was last cleared
	OutOfRange,      // names a node the tree doesn't have
	AlreadyParented, // a child that already has a parent, or is listed twice
};

/// A tree of nodes built from the leaves up, where a node is added with children that were added
/// before it. It knows only the shape of the tree. Its user keeps whatever the nodes stand for in
/// tables indexed by IndexOf, which run from zero in the order the nodes were added. Clear forgets
/// every node at once and keeps the storage, so a tree rebuilt each frame doesn't allocate again.
class NodeTree {
public:
	/// Forgets every node, so handles made before now are stale, and keeps the storage.
	void Clear();

	/// Adds a node with `children`, which become its children in that order, and returns its
	/// handle. Each child must be a live handle with no parent yet. A refused add returns a none
	/// handle, adds nothing and leaves every child as it was, and `reason`, when given, says why.
	[[nodiscard]] NodeHandle Add(std::span<const NodeHandle> children, TreeError* reason = nullptr);

	/// Why `node` can't be used, or nothing when its serial is this tree's and its index is in
	/// range. Every tree counts serials from the same start, so a handle from another tree can
	/// pass, and a handle space should have one tree.
	[[nodiscard]] std::optional<TreeError> Check(NodeHandle node) const;

	/// Where `node` sits in the tables the user keeps beside the tree, or nothing for a handle that
	/// Check refuses.
	[[nodiscard]] std::optional<uint32_t> IndexOf(NodeHandle node) const;

	/// The node `node` is a child of, or a none handle for a node with no parent or a handle that
	/// Check refuses.
	[[nodiscard]] NodeHandle Parent(NodeHandle node) const;

	/// The children of `node` in the order they were given, or none for a handle that Check
	/// refuses. The span lasts until the next Add or Clear, which may move the storage.
	[[nodiscard]] std::span<const NodeHandle> ChildrenOf(NodeHandle node) const;

	/// How many nodes were added since the last Clear.
	[[nodiscard]] size_t Size() const { return m_nodes.size(); }

	/// The capacity of the tree's storage in bytes, for tests and benchmarks.
	[[nodiscard]] size_t ReservedBytes() const {
		return m_nodes.capacity() * sizeof(Node) + m_children.capacity() * sizeof(NodeHandle);
	}

private:
	/// Claims each child for `parent`, or claims none of them and says why.
	std::optional<TreeError> ClaimChildren(std::span<const NodeHandle> children, uint32_t parent);

	// Starts at 1, so a handle made before anything was added never matches the none serial.
	uint32_t                m_serial = 1;
	std::vector<Node>       m_nodes;
	std::vector<NodeHandle> m_children;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_TREE_NODE_TREE_H_
