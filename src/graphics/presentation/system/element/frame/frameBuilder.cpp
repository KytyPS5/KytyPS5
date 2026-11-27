#include "graphics/presentation/system/element/frame/frameBuilder.h"

#include <algorithm>
#include <atomic>
#include <cassert>

namespace Libs::Graphics::SystemUi {

namespace {

// The element layer's reason for a refusal the tree made.
ElementError ToElementError(TreeError why) {
	switch (why) {
		case TreeError::None: return ElementError::None;
		case TreeError::Stale: return ElementError::Stale;
		case TreeError::OutOfRange: return ElementError::OutOfRange;
		case TreeError::AlreadyParented: return ElementError::AlreadyParented;
	}
	// A reason added to the tree must be mapped here, or its refusals would go unlogged as None.
	assert(false && "a TreeError has no ElementError");
	return ElementError::None;
}

// Orders keys so equal ones sit together. Number keys come before text keys.
bool KeyLess(const FrameView::ElementMetadata* left, const FrameView::ElementMetadata* right) {
	if (left->key_is_text != right->key_is_text) {
		return !left->key_is_text;
	}
	return left->key_is_text ? left->key < right->key : left->key_number < right->key_number;
}

// Whether two different keyed elements have the same key. A child listed twice is one element,
// which the tree refuses as already parented.
bool KeyEqual(const FrameView::ElementMetadata* left, const FrameView::ElementMetadata* right) {
	if (left == right || left->key_is_text != right->key_is_text) {
		return false;
	}
	return left->key_is_text ? left->key == right->key : left->key_number == right->key_number;
}

} // namespace

void FrameBuilder::Begin() {
	// Zero means never begun, so the count skips it when it wraps.
	if (++m_begin_count == 0) {
		++m_begin_count;
	}
	m_tree.Clear();
	m_elements.clear();
	m_layouts.clear();
	m_element_stack.clear();
	for (TextChunk& chunk: m_chunks) {
		chunk.used = 0;
	}
	m_current_chunk = 0;
	for (const std::unique_ptr<TableBase>& table: m_tables) {
		if (table != nullptr) {
			table->Clear();
		}
	}
	// Kept workspaces are left alone, as they outlive frames by design.
	for (const std::unique_ptr<FrameWorkspaceBase>& workspace: m_frame_workspaces) {
		if (workspace != nullptr) {
			workspace->Clear();
		}
	}
}

uint32_t FrameBuilder::NextKindId() {
	// Atomic, as kinds can first be used on more than one thread.
	static std::atomic<uint32_t> next {0};
	return next.fetch_add(1);
}

const FrameView::ElementMetadata* FrameBuilder::Find(AnyElement element) const {
	const auto index = m_tree.IndexOf(element.Handle());
	return index.has_value() ? &m_elements[*index] : nullptr;
}

FrameView::ElementMetadata* FrameBuilder::Find(AnyElement element) {
	// The const lookup holds the logic, and the frame itself isn't const here, so casting it away
	// is safe.
	return const_cast<ElementMetadata*>(std::as_const(*this).Find(element));
}

const FrameView::ElementLayout* FrameBuilder::LayoutOf(AnyElement element) const {
	const auto index = m_tree.IndexOf(element.Handle());
	return index.has_value() ? &m_layouts[*index] : nullptr;
}

FrameView::ElementLayout* FrameBuilder::LayoutOf(AnyElement element) {
	// As Find above.
	return const_cast<ElementLayout*>(std::as_const(*this).LayoutOf(element));
}

size_t FrameBuilder::ChildCount(AnyElement element) const {
	return m_tree.ChildrenOf(element.Handle()).size();
}

AnyElement FrameBuilder::Child(AnyElement element, size_t index) const {
	const std::span<const NodeHandle> children = m_tree.ChildrenOf(element.Handle());
	assert(index < children.size() && "Child needs an index below ChildCount");
	return index < children.size() ? AnyElement(children[index]) : AnyElement();
}

const FrameHooks* FrameBuilder::FrameHooksOf(AnyElement element) const {
	const ElementMetadata* const metadata = Find(element);
	if (metadata == nullptr || metadata->kind >= m_frame_hooks.size()) {
		return nullptr;
	}
	// A kind below a later kind's id has a slot whether or not it set hooks, so an empty slot is
	// treated as none.
	const FrameHooks& hooks = m_frame_hooks[metadata->kind];
	const bool        empty = hooks.measure == nullptr && hooks.arrange == nullptr &&
	                   hooks.paint == nullptr && hooks.inherit == nullptr;
	return empty ? nullptr : &hooks;
}

bool FrameBuilder::HasDuplicateKey(std::span<const AnyElement> children) {
	m_key_scratch.clear();
	for (const AnyElement child: children) {
		const FrameView::ElementMetadata* data = Find(child);
		if (data != nullptr && data->has_key && m_tree.Parent(child.Handle()).IsNone()) {
			m_key_scratch.push_back(data);
		}
	}
	// Sorting finds a repeat in n log n, which keeps a long list of keyed rows cheap.
	std::sort(m_key_scratch.begin(), m_key_scratch.end(), KeyLess);
	return std::adjacent_find(m_key_scratch.begin(), m_key_scratch.end(), KeyEqual) !=
	       m_key_scratch.end();
}

AnyElement FrameBuilder::AddElement(uint32_t kind, uint32_t payload,
                                    std::span<const AnyElement> children, const ItemLayout& item) {
	if (m_begin_count == 0) {
		RefuseBuild(ElementError::NotBegun);
		return {};
	}
	// Keys are checked before the tree claims the children, so a refusal leaves nothing to undo.
	if (HasDuplicateKey(children)) {
		RefuseBuild(ElementError::DuplicateKey);
		return {};
	}
	m_child_scratch.clear();
	for (const AnyElement child: children) {
		m_child_scratch.push_back(child.Handle());
	}
	TreeError        why  = TreeError::None;
	const NodeHandle node = m_tree.Add(m_child_scratch, &why);
	if (node.IsNone()) {
		RefuseBuild(ToElementError(why));
		return {};
	}
	FrameView::ElementMetadata data;
	data.kind    = kind;
	data.payload = payload;
	data.item    = item;
	m_elements.push_back(data);
	m_layouts.emplace_back();
	// The tree numbers nodes in the order they are added, which is the order of these tables.
	assert(m_elements.size() == m_tree.Size() && m_layouts.size() == m_tree.Size());
	return AnyElement(node);
}

} // namespace Libs::Graphics::SystemUi
