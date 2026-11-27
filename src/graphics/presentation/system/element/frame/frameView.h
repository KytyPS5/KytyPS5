#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_VIEW_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_VIEW_H_

#include "graphics/presentation/system/common/props/itemLayout.h"
#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/element/element.h"
#include "graphics/presentation/system/element/tree/nodeTree.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Libs::Graphics::SystemUi {

/// One frame of system UI as components see it. They intern text and bytes in it and build runs of
/// children on its element stack, and never build nodes or begin the frame, which is the
/// FrameBuilder's to do. It is not a read-only view, only the components' side of the frame. Every
/// frame is a FrameBuilder, so the storage lives here where both sides reach it. The frame keeps
/// its capacity from frame to frame, so a steady-state frame allocates nothing. The theme, scale,
/// fonts and clock come from the current context. A frame belongs to the render or ImGui thread and
/// is not thread safe.
class FrameView {
public:
	/// What the frame knows about one element beyond its place in the tree, kept beside the tree
	/// and indexed by the element's node. What layout found is kept apart in ElementLayout. It is
	/// part of the frame because nothing else owns one, and no call gives one to a component, so
	/// components can name it but are never handed one. It is cleared with the tree, so it must
	/// stay trivially destructible.
	struct ElementMetadata {
		/// The decor value of an element with nothing beyond the generic element.
		static constexpr uint32_t NO_DECOR = UINT32_MAX;
		/// The interact value of an element with no interaction record.
		static constexpr uint32_t NO_INTERACT = UINT32_MAX;

		// The id of the element's kind, which is the type of its data. The frame knows the id and
		// not the type, so it never needs to know what kinds there are. Every element has data, so
		// the frame always sets it when it adds the element.
		uint32_t kind = 0;

		// An index into the frame's table of this element's kind, where its data is.
		uint32_t payload = 0;

		// What the element asks of its container. Every element has one, so it lives here and
		// needs no table of its own.
		ItemLayout item;

		// The key the caller gave the element, which names it among its siblings in place of its
		// position. A key is a string, interned in the frame, or a number, and the two never match
		// each other.
		std::string_view key;
		uint32_t         key_number  = 0;
		bool             has_key     = false;
		bool             key_is_text = false;
		// The ImGui id of the element, set by AssignIds from its key or position and its parent's
		// id. Zero until then.
		uint32_t id = 0;

		// An index into the frame's decor records, or NO_DECOR for a plain element.
		uint32_t decor = NO_DECOR;

		// An index into the frame's interaction records, or NO_INTERACT for an element that takes
		// no input.
		uint32_t interact = NO_INTERACT;
	};

	/// What layout found for one element, kept beside the tree and indexed by the element's node.
	/// Only layout reads and writes it, and like ElementMetadata no call hands one to a component.
	/// It is cleared with the tree, so it must stay trivially destructible.
	struct ElementLayout {
		// The size the element wants with no limit on its container's main axis, set by its
		// parent. It stays what it is however much space the parent hands out.
		Vec2 natural;
		// The size with no limit on either axis, which the memo keeps for the whole pass. `natural`
		// is this size, or the size at the room across when the element is wider than that room.
		Vec2 unbounded;
		// A leaf's Measure computes its natural size from its content, and never returns
		// `measured`, which may hold a length assigned in an earlier pass. The size the element
		// got once its parent assigned it a main-axis length, which is what Measure last found. It
		// differs from the natural size only when the element grew or shrank.
		Vec2 measured;
		Rect arranged;
		// The smallest main-axis size the element can take without its content spilling out, what
		// CSS calls min-content. A flex item shrinks no further than this unless it sets its own
		// minimum. For text it depends on overflow, the longest word when wrapped, the ellipsis
		// when truncated and nothing when clipped, and its height is the laid-out height.
		Vec2 min_content;
		// The space Measure was last given for this element, so a parent can tell whether the
		// subtree still holds the layout for the space it is about to assign. Measuring is the only
		// thing that writes the subtree, and any later measure with a different limit overwrites
		// it.
		Vec2 last_limit;
		// Set by Measure, so Arrange can refuse a tree that was never measured.
		bool is_measured = false;
		// Set by the parent's Arrange when its flex solver collapsed this element because the room
		// was short. A collapsed element and its subtree are absent, so they are not arranged
		// again, painted or walked for interaction. It is not decided by the element having no
		// size, since a spacer or a bar of no width is still there. Skipping the walk skips
		// PlayAnimation, so a keyed animation on a collapsed element, or below it, is not played
		// that frame, and the store forgets an animation not played for a frame. An element that
		// collapses and then comes back therefore starts its entrance again, as any element missing
		// for a frame does. Arrange given the element itself clears the flag.
		bool collapsed = false;
		/// The layer the element is drawn on, set by Arrange, the higher of the layer it asks for
		/// and its parent's, so everything inside an element is drawn on its layer or above.
		uint8_t layer = 0;
		// Whether `natural` holds a measurement made in measure pass `natural_generation`, so a
		// parent that measures the element at its natural size again in that pass can reuse it
		// instead of walking its subtree. A later pass may offer different space, so it measures
		// afresh.
		bool     has_natural        = false;
		uint32_t natural_generation = 0;
	};

	// A frame hands out handles into its own storage, so copying one would share nothing useful.
	FrameView(const FrameView&)            = delete;
	FrameView& operator=(const FrameView&) = delete;
	FrameView(FrameView&&)                 = delete;
	FrameView& operator=(FrameView&&)      = delete;

	/// How many times Begin has restarted the frame, which changes with every Begin and is zero
	/// before the first. A caller that wants to know the frame was begun again since it last looked
	/// compares it with what it saw.
	[[nodiscard]] uint32_t BeginCount() const { return m_begin_count; }

	/// Copies `text` into frame storage and returns a view of the copy. The copy stays at the same
	/// address until the next Begin, so a pointer taken from the view may be kept that long, and
	/// views from earlier calls stay valid however much is interned after them.
	[[nodiscard]] std::string_view Intern(std::string_view text);

	/// Copies `text` into frame storage with a terminating NUL, for an ImGui call that wants a C
	/// string. Like Intern, the copy stays where it is until the next Begin.
	[[nodiscard]] const char* InternCString(std::string_view text);

	/// Copies `size` bytes at `bytes` into frame storage and returns the copy, or null for none.
	/// Like text from Intern the copy stays where it is until the next Begin, however much is
	/// stored after it, and the caller may write to it. It is not aligned for any type, so objects
	/// are read and written with memcpy. A component uses it to give a trivially copyable callable
	/// something variable-sized to point at, such as a table of handles or a callback's bytes.
	[[nodiscard]] std::byte* StoreBytes(const void* bytes, size_t size);

	/// A stack of elements a component can build a run of children on, so it doesn't allocate a
	/// vector for them each frame. It keeps its capacity from frame to frame, and Begin empties it.
	/// A component notes its size, pushes its children, uses them, and resizes it back to the noted
	/// size, so a child that builds a run of its own in between does so above the first's. Use
	/// indices and not pointers into it, as pushing may move it.
	[[nodiscard]] std::vector<AnyElement>& ElementStack() { return m_element_stack; }

	/// The capacity of the frame's storage in bytes, for tests and benchmarks. The table objects
	/// themselves aren't counted, only their rows, so it is a measure of growth more than an exact
	/// total. A builder's workspaces aren't counted either, as the frame can't see what they hold.
	[[nodiscard]] size_t ReservedBytes() const;

protected:
	/// Lets the checks look at the storage, which nothing else outside the frame reads.
	friend struct FrameTestHooks;

	// Only a FrameBuilder is ever made, so a component can't make a frame it couldn't begin.
	FrameView()  = default;
	~FrameView() = default;

	/// A run of interned text. Chunks are never resized, so earlier views stay put.
	struct TextChunk {
		std::unique_ptr<char[]> data;
		size_t                  capacity = 0;
		size_t                  used     = 0;
	};

	/// What the frame needs of a table whose row type it doesn't know, so it can hold the tables of
	/// every kind of element without naming any of them.
	struct TableBase {
		TableBase()                            = default;
		TableBase(const TableBase&)            = delete;
		TableBase& operator=(const TableBase&) = delete;
		TableBase(TableBase&&)                 = delete;
		TableBase& operator=(TableBase&&)      = delete;
		virtual ~TableBase()                   = default;
		/// Empties the table and keeps its capacity.
		virtual void Clear() = 0;
		/// The capacity of the table in bytes.
		[[nodiscard]] virtual size_t ReservedBytes() const = 0;
	};

	// The storage both sides of the frame use. It is protected rather than private because the
	// FrameBuilder, the only subclass, builds into it directly, and accessors for every member
	// would only add a layer between the two halves of one object.
	// NOLINTBEGIN(misc-non-private-member-variables-in-classes,cppcoreguidelines-non-private-member-variables-in-classes)
	uint32_t m_begin_count = 0;
	NodeTree m_tree;

	// What each element keeps beside the tree, indexed by its node.
	std::vector<ElementMetadata> m_elements;
	std::vector<ElementLayout>   m_layouts;

	// The keyed children of the node being added, sorted to find a repeated key. Kept at capacity.
	std::vector<const ElementMetadata*> m_key_scratch;
	// The node handles of the children being added, as the tree takes them. Kept at capacity.
	std::vector<NodeHandle> m_child_scratch;
	std::vector<AnyElement> m_element_stack;

	std::vector<TextChunk> m_chunks;
	size_t                 m_current_chunk = 0;

	// The table of each kind, by kind id. A kind this frame never used has no table.
	std::vector<std::unique_ptr<TableBase>> m_tables;
	// NOLINTEND(misc-non-private-member-variables-in-classes,cppcoreguidelines-non-private-member-variables-in-classes)

private:
	/// Room for `size` bytes of interned text, which stay put until the next Begin.
	char* ReserveText(size_t size);
};

static_assert(std::is_trivially_destructible_v<FrameView::ElementMetadata>,
              "the element metadata is cleared with the tree, without running destructors");
static_assert(std::is_trivially_destructible_v<FrameView::ElementLayout>,
              "the element layouts are cleared with the tree, without running destructors");

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_VIEW_H_
