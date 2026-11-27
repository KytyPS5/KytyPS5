#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_TREE_NODE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_TREE_NODE_H_

#include <cstdint>
#include <type_traits>

// One node of the element tree. Internal to the element layer, so components don't include it.
namespace Libs::Graphics::SystemUi {

/// One node of a NodeTree. It records only where the node sits in the tree. Whatever a node stands
/// for is kept by its user in tables indexed by the node, so the tree stays generic. The tree is
/// reset by clearing vectors, so a node must stay trivially destructible.
struct Node {
	/// The parent value of a node that has none yet.
	static constexpr uint32_t NO_PARENT = UINT32_MAX;

	// The index of the node this one is a child of. A node has at most one parent, since the tree
	// refuses to add a node under a second one.
	uint32_t parent = NO_PARENT;

	// The children as a run in the tree's child storage.
	uint32_t first_child = 0;
	uint32_t child_count = 0;
};

static_assert(std::is_trivially_destructible_v<Node>,
              "the tree is reset without running destructors");

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_TREE_NODE_H_
