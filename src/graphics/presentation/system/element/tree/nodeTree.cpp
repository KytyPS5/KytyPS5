#include "graphics/presentation/system/element/tree/nodeTree.h"

namespace Libs::Graphics::SystemUi {

void NodeTree::Clear() {
	m_nodes.clear();
	m_children.clear();
	// Skipping the none serial when the count wraps keeps every live handle distinct from none.
	if (++m_serial == NodeHandle::NONE_SERIAL) {
		++m_serial;
	}
}

std::optional<TreeError> NodeTree::ClaimChildren(std::span<const NodeHandle> children,
                                                 uint32_t                    parent) {
	// Claim as we go and undo on failure, so a refused node leaves nothing half built. Claiming as
	// we go also catches a child listed twice, whose second look finds it taken.
	size_t                   claimed = 0;
	std::optional<TreeError> failure;
	for (const NodeHandle child: children) {
		failure = Check(child);
		if (failure.has_value()) {
			break;
		}
		Node& child_node = m_nodes[child.m_index];
		if (child_node.parent != Node::NO_PARENT) {
			failure = TreeError::AlreadyParented;
			break;
		}
		child_node.parent = parent;
		++claimed;
	}
	if (failure.has_value()) {
		for (size_t undo = 0; undo < claimed; ++undo) {
			m_nodes[children[undo].m_index].parent = Node::NO_PARENT;
		}
	}
	return failure;
}

NodeHandle NodeTree::Add(std::span<const NodeHandle> children, TreeError* reason) {
	// Indexes are 32 bits and the last one marks no parent, which a UI tree never comes near, as it
	// would take tens of gigabytes of nodes.
	const auto index = static_cast<uint32_t>(m_nodes.size());
	if (const auto failure = ClaimChildren(children, index); failure.has_value()) {
		if (reason != nullptr) {
			*reason = *failure;
		}
		return {};
	}
	Node node;
	node.first_child = static_cast<uint32_t>(m_children.size());
	node.child_count = static_cast<uint32_t>(children.size());
	// Inserting from the tree's own child storage would be undefined once it grows, and only the
	// one-parent check above rules that out, since every stored child already has a parent.
	m_children.insert(m_children.end(), children.begin(), children.end());
	m_nodes.push_back(node);
	return {index, m_serial};
}

std::optional<TreeError> NodeTree::Check(NodeHandle node) const {
	if (node.IsNone()) {
		return TreeError::None;
	}
	if (node.m_serial != m_serial) {
		return TreeError::Stale;
	}
	if (node.m_index >= m_nodes.size()) {
		return TreeError::OutOfRange;
	}
	return std::nullopt;
}

std::optional<uint32_t> NodeTree::IndexOf(NodeHandle node) const {
	if (Check(node).has_value()) {
		return std::nullopt;
	}
	return node.m_index;
}

NodeHandle NodeTree::Parent(NodeHandle node) const {
	if (Check(node).has_value()) {
		return {};
	}
	const uint32_t parent = m_nodes[node.m_index].parent;
	return parent == Node::NO_PARENT ? NodeHandle() : NodeHandle(parent, m_serial);
}

std::span<const NodeHandle> NodeTree::ChildrenOf(NodeHandle node) const {
	if (Check(node).has_value()) {
		return {};
	}
	const Node& tree_node = m_nodes[node.m_index];
	return std::span<const NodeHandle>(m_children)
	    .subspan(tree_node.first_child, tree_node.child_count);
}

} // namespace Libs::Graphics::SystemUi
