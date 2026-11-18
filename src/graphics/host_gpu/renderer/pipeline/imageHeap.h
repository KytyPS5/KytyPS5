#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEHEAP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEHEAP_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class Buffer;
class CommandBuffer;
class PageManager;
class RenderContext;
class RenderExecutor;
struct ImageHeapTestAccess;

// Guest image tables that shaders index by GPU-computed offsets, kept resident in one bindless
// sampled-image array. A table is keyed by its base and follows its descriptor's size. Each view
// of a table owns device slot words, one per table dword, that select an array element; one
// element serves a descriptor and profile in every view. A probe has its element while draws
// can select it: a draw that names the offsets it can select keeps those until a frame that
// uses the view names them no more; a draw that cannot keeps every probe, and a view no draw
// uses keeps its own, for the frames an idle table stays. CPU
// writes to a table fault in its snapshot, which each draw refreshes once; a view takes the
// changes when a draw first acquires it. Images behind elements report their changes through
// Image::heap_changes. A smaller table leaves at a suspend point if the frame did not acquire
// it, a larger one after LargeTableIdleFrames such frames.
class ImageHeap final: public ShaderRecompiler::IR::ImageHeapSource {
public:
	ImageHeap(RenderContext& context, PageManager& page_manager);
	~ImageHeap();
	KYTY_CLASS_NO_COPY(ImageHeap);

	// Starts a draw or dispatch, before its programs acquire views.
	void BeginDraw() { ++m_draw; }
	bool Acquire(const ShaderRecompiler::IR::ImageHeapRequest& request,
	             ShaderRecompiler::IR::ImageHeapView&          result) override;
	// Image discovery: resolves new elements and rediscovers changed images.
	void Rebind(RenderExecutor& executor);
	// Recording: uploads slots, moves resident images to their read layout and binds the array.
	void Commit(CommandBuffer& buffer, vk::PipelineBindPoint bind_point, vk::PipelineLayout layout);
	// CPU write fault, on any thread.
	void InvalidateMemory(uint64_t address, uint64_t size);
	void UnmapMemory(uint64_t address, uint64_t size);
	// At a suspend point.
	void RunGarbageCollector();

private:
	using DescriptorValue = ShaderRecompiler::IR::DescriptorValue;

	struct Key {
		DescriptorValue value;
		uint32_t        profile = 0; // Index into m_profiles.

		bool operator==(const Key&) const = default;
	};

	struct KeyHash {
		size_t operator()(const Key& key) const noexcept;
	};

	// A stale element's slots move to a new element of its descriptor, a buried one's to null.
	enum class State : uint8_t { Live, Stale, Buried };

	struct Element {
		Key      key;
		ImageId  image;
		uint64_t serial = 0; // Image::serial of image.
		uint32_t kind   = 0;
		uint32_t refs   = 0; // Slots of all views that select it.
		State    state  = State::Live;
	};

	struct Memory;

	struct View {
		Memory&                      memory;
		uint64_t                     step    = 0;
		uint64_t                     first   = 0;
		uint64_t                     count   = 0;
		uint64_t                     limit   = 0; // Probes a key domain allows, if fewer.
		uint32_t                     profile = 0;
		std::vector<uint32_t>        slots; // Plus a trailing null slot.
		std::vector<uint32_t>        kind_keys;
		std::vector<DescriptorValue> kinds; // One representative per kind_keys entry.
		std::unique_ptr<Buffer>      buffer;
		size_t                       dirty_begin   = 0; // Slots to upload.
		size_t                       dirty_end     = 0;
		size_t                       changed_begin = 0; // Table bytes changed since it was probed.
		size_t                       changed_end   = 0;
		// One bit per probe: it has its element, not only its kind.
		std::vector<uint64_t> populated;
		uint64_t              populated_count = 0;
		std::vector<uint64_t> demanded;       // Named by a draw since the last suspend point.
		uint32_t              idle       = 0; // Suspend points since a draw used the view.
		uint32_t              whole_idle = UINT32_MAX; // Since a draw could select any probe.
	};

	struct Memory {
		uint64_t                           base = 0;
		uint64_t                           size = 0;
		std::vector<uint32_t>              words;
		std::vector<uint8_t>               dirty_pages; // Unwatched, so unread since a write.
		std::unordered_map<uint32_t, uint64_t> dead;   // Buried words: their descriptor hashes.
		uint64_t                           draw = 0;    // Last draw that refreshed it.
		uint32_t                           idle = 0;    // Suspend points since a frame acquired it.
		std::vector<std::unique_ptr<View>> views;
	};

	[[nodiscard]] bool Refresh(Memory& memory);
	void               MarkDirty(Memory& memory, size_t begin, size_t end);
	template <bool track>
	void     Watch(const Memory& memory, size_t begin, size_t end);
	void     Unwatch(const Memory& memory);
	void     Drop(Memory& memory);
	void     UpdateProbes(View& view, uint64_t begin, uint64_t end);
	void     UpdateProbe(View& view, uint64_t probe);
	void     Demand(View& view, const std::vector<uint32_t>* offsets);
	void     ReleaseIdle(View& view);
	void     Assign(View& view, size_t slot, uint32_t entry);
	uint32_t Arm(View& view, uint32_t kind, const DescriptorValue& value);
	uint32_t Intern(View& view, uint32_t kind, const Key& key);
	void     Release(uint32_t entry);
	void     Retire();
	void     Mark(uint32_t index, State state);
	template <typename Fn>
	bool     ForEachChanged(Fn&& fn);
	void     Repoint();
	void     TakeChanges();
	ImageId  CurrentImage(const Image::HeapChange& change);
	uint32_t Allocate();
	void     Resize(Memory& memory, uint64_t base, uint64_t size);
	void     WriteDescriptors();

	RenderContext&                       m_context;
	PageManager&                         m_page_manager;
	std::mutex                           m_mutex; // Fault-visible Memory state and m_memories.
	std::vector<std::unique_ptr<Memory>> m_memories;
	std::vector<View*>                   m_active; // Acquired since the last commit.
	uint64_t                             m_draw         = 1;
	uint64_t                             m_suspend_draw = 0; // m_draw at the last suspend point.
	std::vector<ShaderRecompiler::IR::ImageResource>  m_profiles;
	std::vector<Element>                              m_elements;
	std::unordered_map<Key, uint32_t, KeyHash>        m_index;
	std::vector<uint32_t>                             m_free;
	std::vector<uint32_t>                             m_pending;  // Not yet resolved.
	std::vector<uint32_t>                             m_released; // Unselected after the upload.
	std::vector<uint32_t>                             m_held; // Another extent holds the address.
	std::vector<uint32_t>                             m_dcc;  // Metadata clears apply per draw.
	std::vector<std::pair<uint64_t, ImageId>>         m_away; // Serial, image: not read layout.
	// Serial, image: targets whose newest pixels are in an EQAA shadow.
	std::vector<std::pair<uint64_t, ImageId>>         m_unsettled;
	uint64_t                                          m_held_serial = 0; // Image::serials.
	std::deque<std::pair<uint64_t, uint32_t>>         m_retired; // Tick, element.
	std::vector<std::pair<size_t, size_t>>            m_runs;
	std::vector<uint32_t>                             m_scratch;
	std::vector<Image::HeapChange>                    m_changes;
	std::vector<std::pair<uint32_t, vk::DescriptorImageInfo>> m_writes;
	vk::DescriptorSetLayout                           m_layout = nullptr;
	vk::DescriptorPool                                m_pool   = nullptr;
	vk::DescriptorSet                                 m_set    = nullptr;

	friend struct ImageHeapTestAccess;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEHEAP_H_
