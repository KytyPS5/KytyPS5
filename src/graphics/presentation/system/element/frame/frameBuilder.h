#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_BUILDER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_BUILDER_H_

#include "graphics/presentation/system/common/props/itemLayout.h"
#include "graphics/presentation/system/element/frame/frameHooks.h"
#include "graphics/presentation/system/element/frame/frameView.h"
#include "graphics/presentation/system/element/refusal.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Libs::Graphics::SystemUi {

/// One frame of system UI as the element layer sees it, the primitives, layout and whoever owns the
/// frame. It is the frame itself, with the building that components don't get on top of their
/// view, so building code calls the frame directly with nothing in between. Only element-layer code
/// includes this header, which is where the line between the two sides is drawn.
class FrameBuilder final: public FrameView {
public:
	FrameBuilder()  = default;
	~FrameBuilder() = default;
	// A frame hands out handles into its own storage, so copying one would share nothing useful.
	FrameBuilder(const FrameBuilder&)            = delete;
	FrameBuilder& operator=(const FrameBuilder&) = delete;
	FrameBuilder(FrameBuilder&&)                 = delete;
	FrameBuilder& operator=(FrameBuilder&&)      = delete;

	/// Starts a new frame. Clears the elements and the interned text, keeping their capacity. Every
	/// element from an earlier frame is then stale.
	void Begin();

	/// Adds an element holding `data` with `children` and what it asks of its container, or refuses
	/// it with a none handle. A refusal goes through RefuseBuild, so a caller that wants the reason
	/// opens a RefusalScope. A refused element adds nothing, to the tree or to any table.
	template <class Data>
	[[nodiscard]] Element<Data> AddNode(Data data, std::span<const AnyElement> children,
	                                    const ItemLayout& item);

	/// The table of `Data` for this frame, made the first time it is asked for and kept from frame
	/// to frame, so a steady frame allocates nothing. Begin empties it.
	template <class Data>
	std::vector<Data>& Table();

	/// The element data of `element`, or null for a handle the tree refuses.
	[[nodiscard]] ElementMetadata* Find(AnyElement element);
	/// The element data of `element` for reading, or null for a handle the tree refuses.
	[[nodiscard]] const ElementMetadata* Find(AnyElement element) const;

	/// The data of `element` in its kind's table, or null for a handle the tree refuses. The
	/// handle's type says the kind, which debug builds check against the element. An untyped
	/// element has no kind to look up, so it is left to the Find above.
	template <class Data>
	requires(!std::is_void_v<Data>)
	[[nodiscard]] Data* Find(Element<Data> element);

	/// The data of `element` when its kind is `Data`, or null for a handle the tree refuses or an
	/// element of another kind. Frame hooks are handed untyped elements, so they find their own
	/// props through this.
	template <class Data>
	[[nodiscard]] Data* FindAs(AnyElement element);
	/// The data of `element` for reading when its kind is `Data`, or null as above. Never null for
	/// a live element of kind `Data`, whose table was made when the element was added.
	template <class Data>
	[[nodiscard]] const Data* FindAs(AnyElement element) const;

	/// What layout found for `element`, or null for a handle the tree refuses.
	[[nodiscard]] ElementLayout* LayoutOf(AnyElement element);
	/// What layout found for `element` for reading, or null for a handle the tree refuses.
	[[nodiscard]] const ElementLayout* LayoutOf(AnyElement element) const;

	/// How many children `element` has, none for a handle the tree refuses.
	[[nodiscard]] size_t ChildCount(AnyElement element) const;

	/// The child of `element` at `index`, in the order the children were given. `index` must be
	/// below ChildCount.
	[[nodiscard]] AnyElement Child(AnyElement element, size_t index) const;

	/// Starts a new measure pass and returns its number. A natural size measured in one pass is
	/// reused only within it, since a later pass may offer different space.
	uint32_t BeginMeasurePass() { return ++m_measure_pass; }

	/// The number of the current measure pass.
	[[nodiscard]] uint32_t MeasurePass() const { return m_measure_pass; }

	/// Gives elements of kind `Data` their frame hooks, which generic layout and paint call through
	/// the element's kind. A primitive sets them on the frame it builds in, so they belong to the
	/// frame and no global table or lock is needed. Setting them again replaces them.
	template <class Data>
	void SetFrameHooks(const FrameHooks& hooks);

	/// The frame hooks of `element`'s kind, or null for a handle the tree refuses or a kind
	/// that set none.
	[[nodiscard]] const FrameHooks* FrameHooksOf(AnyElement element) const;

	/// One `T` for this frame, made the first time it is asked for and kept, with its capacity, for
	/// as long as the frame lives. Begin leaves it alone. Code below the frame keeps its working
	/// storage here, so a steady frame allocates nothing and the frame never names its types. Every
	/// caller asking for the same `T` shares it, a frame hook and the hooks of the children it
	/// measures included, so use it as a stack as ElementStack is used. Note its size, push above
	/// it, use indices and not pointers, as a nested caller may grow it, and resize back after.
	template <class T>
	T& Workspace();

	/// The kept workspace of `T` for reading, or null when nothing has asked for it yet.
	template <class T>
	[[nodiscard]] const T* FindWorkspace() const;

	/// One `T` for the current frame, made the first time it is asked for and emptied by every
	/// Begin. A `T` that owns memory must have a clear() that empties everything a frame wrote and
	/// keeps its capacity, which is checked when it compiles, so a steady frame allocates nothing.
	/// A `T` that is trivially copyable may instead go without, and is remade as a fresh `T`. Use
	/// it for what describes this frame only, such as what was laid out for each element, so
	/// nothing from an earlier frame can be read by mistake. It is separate from the kept Workspace
	/// of the same `T`, and shared by every caller asking for it.
	template <class T>
	T& FrameWorkspace();

	/// The frame workspace of `T` for reading, or null when nothing has asked for it yet.
	template <class T>
	[[nodiscard]] const T* FindFrameWorkspace() const;

private:
	/// Lets the checks read kind ids, which nothing else outside the builder needs.
	friend struct FrameTestHooks;

	/// The table of one kind's rows.
	template <class Data>
	struct TableOf final: TableBase {
		std::vector<Data>    rows;
		void                 Clear() override { rows.clear(); }
		[[nodiscard]] size_t ReservedBytes() const override {
			return rows.capacity() * sizeof(Data);
		}
	};

	/// A new kind id, one more than the last, for KindId.
	static uint32_t NextKindId();

	/// A small id for `Data`, given the first time it is asked for and the same for the rest of the
	/// run, so a table is found by index instead of by a lookup.
	template <class Data>
	static uint32_t KindId() {
		static const uint32_t id = NextKindId();
		return id;
	}

	/// Adds the element itself, whose data will sit at `payload` in the table of kind `kind`. The
	/// part of AddNode that doesn't depend on the kind.
	AnyElement AddElement(uint32_t kind, uint32_t payload, std::span<const AnyElement> children,
	                      const ItemLayout& item);
	/// Whether two different children share a key. A child that isn't live, already has a parent or
	/// is listed twice is left for the tree to refuse, so it gets the same reason it would unkeyed.
	bool HasDuplicateKey(std::span<const AnyElement> children);

	/// What the frame needs of a kept workspace of unknown type, which is only to destroy it.
	struct WorkspaceBase {
		WorkspaceBase()                                = default;
		WorkspaceBase(const WorkspaceBase&)            = delete;
		WorkspaceBase& operator=(const WorkspaceBase&) = delete;
		WorkspaceBase(WorkspaceBase&&)                 = delete;
		WorkspaceBase& operator=(WorkspaceBase&&)      = delete;
		virtual ~WorkspaceBase()                       = default;
	};

	/// What the frame needs of a frame workspace, also to empty it at Begin. Emptying can't throw,
	/// as Begin has nowhere to report it and a half-emptied workspace would be left behind.
	struct FrameWorkspaceBase: WorkspaceBase {
		/// Empties the workspace and keeps its memory.
		virtual void Clear() noexcept = 0;
	};

	/// The kept workspace of one type.
	template <class T>
	struct WorkspaceOf final: WorkspaceBase {
		/// The base its vector holds, so a holder used with the other vector won't compile.
		using Slot = WorkspaceBase;
		T value {};
	};

	/// The frame workspace of one type. FrameWorkspace checks the type can be emptied cheaply.
	template <class T>
	struct FrameWorkspaceOf final: FrameWorkspaceBase {
		/// As in WorkspaceOf.
		using Slot = FrameWorkspaceBase;
		T value {};
		/// Empties the workspace for a new frame.
		void Clear() noexcept override {
			if constexpr (requires { value.clear(); }) {
				value.clear();
				// A clear() can do something else, as a stream's resets only its error flags.
				if constexpr (requires { value.empty(); }) {
					assert(value.empty() && "a frame workspace's clear() must empty it");
				}
			} else {
				// Remade in place rather than assigned, so a type with a const member works too.
				std::destroy_at(&value);
				std::construct_at(&value);
			}
		}
	};

	/// The workspace of `T` in `slots`, held by a `Holder<T>`, made the first time it is asked for.
	template <template <class> class Holder, class T, class Base>
	static T& WorkspaceIn(std::vector<std::unique_ptr<Base>>& slots);

	/// The workspace of `T` in `slots`, held by a `Holder<T>`, or null when it was never made.
	template <template <class> class Holder, class T, class Base>
	static const T* FindIn(const std::vector<std::unique_ptr<Base>>& slots);

	uint32_t m_measure_pass = 0;
	// The frame hooks of each kind, by kind id. They outlive Begin, as a kind's hooks don't
	// change from frame to frame.
	std::vector<FrameHooks> m_frame_hooks;
	// The kept workspace of each type, by kind id. A type never asked for has none.
	std::vector<std::unique_ptr<WorkspaceBase>> m_workspaces;
	// The frame workspace of each type, by the same ids, which Begin empties.
	std::vector<std::unique_ptr<FrameWorkspaceBase>> m_frame_workspaces;
};

template <class Data>
std::vector<Data>& FrameBuilder::Table() {
	static_assert(std::is_trivially_destructible_v<Data>,
	              "a table is emptied every frame, so its rows must be cheap to drop");
	const uint32_t id = KindId<Data>();
	if (id >= m_tables.size()) {
		m_tables.resize(id + 1);
	}
	std::unique_ptr<TableBase>& table = m_tables[id];
	if (table == nullptr) {
		table = std::make_unique<TableOf<Data>>();
	}
	// The id is Data's alone, so the table at it is always a TableOf<Data>.
	return static_cast<TableOf<Data>&>(*table).rows;
}

template <class Data>
Element<Data> FrameBuilder::AddNode(Data data, std::span<const AnyElement> children,
                                    const ItemLayout& item) {
	std::vector<Data>& table = Table<Data>();
	const AnyElement   element =
	    AddElement(KindId<Data>(), static_cast<uint32_t>(table.size()), children, item);
	// The data goes in only once the element is in, so a refusal leaves the table as it was.
	if (element.IsNone()) {
		return {};
	}
	table.push_back(std::move(data));
	return Element<Data>(element.Handle());
}

template <class Data>
requires(!std::is_void_v<Data>)
Data* FrameBuilder::Find(Element<Data> element) {
	ElementMetadata* const generic = Find(AnyElement(element));
	if (generic == nullptr) {
		return nullptr;
	}
	assert(generic->kind == KindId<Data>() &&
	       "an Element names the kind its element was made with");
	return &Table<Data>()[generic->payload];
}

template <class Data>
Data* FrameBuilder::FindAs(AnyElement element) {
	ElementMetadata* const generic = Find(element);
	if (generic == nullptr || generic->kind != KindId<Data>()) {
		return nullptr;
	}
	return &Table<Data>()[generic->payload];
}

template <class Data>
const Data* FrameBuilder::FindAs(AnyElement element) const {
	const ElementMetadata* const generic = Find(element);
	const uint32_t               id      = KindId<Data>();
	if (generic == nullptr || generic->kind != id) {
		return nullptr;
	}
	// AddNode makes a kind's table before its first element and Begin only empties tables, so an
	// element of this kind always has one. A const frame can't make it, so release builds give
	// null.
	assert(id < m_tables.size() && m_tables[id] != nullptr &&
	       "an element of a kind exists only after its table");
	if (id >= m_tables.size() || m_tables[id] == nullptr) {
		return nullptr;
	}
	// The id is Data's alone, so the table at it is always a TableOf<Data>.
	return &static_cast<const TableOf<Data>&>(*m_tables[id]).rows[generic->payload];
}

template <class Data>
void FrameBuilder::SetFrameHooks(const FrameHooks& hooks) {
	const uint32_t id = KindId<Data>();
	if (id >= m_frame_hooks.size()) {
		m_frame_hooks.resize(id + 1);
	}
	m_frame_hooks[id] = hooks;
}

template <template <class> class Holder, class T, class Base>
T& FrameBuilder::WorkspaceIn(std::vector<std::unique_ptr<Base>>& slots) {
	static_assert(std::is_same_v<typename Holder<T>::Slot, Base>, "a holder from the other vector");
	// Workspaces share the kind ids, which are per type, so a workspace type and a kind type never
	// clash, and the vector stays as short as the ids handed out.
	const uint32_t id = KindId<T>();
	if (id >= slots.size()) {
		slots.resize(id + 1);
	}
	std::unique_ptr<Base>& workspace = slots[id];
	if (workspace == nullptr) {
		workspace = std::make_unique<Holder<T>>();
	}
	// The id is T's alone and each vector has one holder, so the workspace at it is a Holder<T>.
	return static_cast<Holder<T>&>(*workspace).value;
}

template <template <class> class Holder, class T, class Base>
const T* FrameBuilder::FindIn(const std::vector<std::unique_ptr<Base>>& slots) {
	static_assert(std::is_same_v<typename Holder<T>::Slot, Base>, "a holder from the other vector");
	const uint32_t id = KindId<T>();
	if (id >= slots.size() || slots[id] == nullptr) {
		return nullptr;
	}
	// As in WorkspaceIn, the workspace at T's id is always a Holder<T>.
	return &static_cast<const Holder<T>&>(*slots[id]).value;
}

template <class T>
T& FrameBuilder::Workspace() {
	return WorkspaceIn<WorkspaceOf, T>(m_workspaces);
}

template <class T>
const T* FrameBuilder::FindWorkspace() const {
	return FindIn<WorkspaceOf, T>(m_workspaces);
}

template <class T>
T& FrameBuilder::FrameWorkspace() {
	static_assert(
	    requires(T& value) { value.clear(); } || std::is_trivially_copyable_v<T>,
	    "a frame workspace that owns memory needs a clear() that keeps its capacity, "
	    "or it would be remade and allocate every frame");
	return WorkspaceIn<FrameWorkspaceOf, T>(m_frame_workspaces);
}

template <class T>
const T* FrameBuilder::FindFrameWorkspace() const {
	return FindIn<FrameWorkspaceOf, T>(m_frame_workspaces);
}

/// The frame of the innermost open FrameScope on this thread, as the element layer sees it. It is
/// the same object CurrentFrame gives components, and declared here so only code that includes the
/// builder can reach it. With no scope open it asserts, and a release build gets the same never
/// begun frame CurrentFrame falls back to. Defined in frameScope.cpp, beside the scope.
[[nodiscard]] FrameBuilder& CurrentBuilder();

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_BUILDER_H_
