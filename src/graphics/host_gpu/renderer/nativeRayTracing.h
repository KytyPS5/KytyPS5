#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_NATIVERAYTRACING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_NATIVERAYTRACING_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

namespace ShaderRecompiler::IR {
struct ShaderInfo;
} // namespace ShaderRecompiler::IR

class BufferCache;
class CommandRecorder;
class CommandScheduler;
struct GraphicContext;

// Host acceleration structures for guest ray tracing (VK_KHR_acceleration_structure).
//
// GTA V builds AMD BVHs with its own compute shaders and traverses them in software with
// IMAGE_BVH_INTERSECT_RAY, one node at a time. Before a dispatch that traces through such a
// BVH, this class mirrors it on the GPU, without the CPU reading what the GPU wrote:
// - every leaf of the guest top-level BVH (an instance) becomes a host TLAS instance with the
//   inverse of the instance's world-to-object matrix and its mask;
// - every guest bottom-level BVH becomes a host BLAS of the triangles of its leaf lists: opaque
//   ones (geometry 0) and alpha tested ones (geometry 1), with a table from the primitive to the
//   guest list entry (its leaf pointer and primitive word).
// A shader then lets the hardware find opaque hits and runs the guest's own leaf code (triangle
// and alpha test) only for the alpha tested triangles a ray hits. BLASes the host has not seen are reported by the top-level walk, counted on the
// GPU and read back asynchronously; the host allocates them and builds them the next time.
//
// The host memory is bounded: the readbacks list the slots each TLAS references, a slot no TLAS
// referenced for EvictFrames frames is evicted (its table entry, acceleration structure and
// buffers are released) and a slot whose BLAS grew releases its old structure. Whatever the GPU
// may still read is destroyed only after the work recorded so far has completed. The memory of
// the structures has a budget too (a share of the VRAM, KYTY_RT_ARENA_MB overrides it): a BLAS
// that would not fit, or is implausibly large, is not allocated, and over the budget the slots
// used least recently are evicted. A nearly empty arena chunk is emptied by moving what is left
// in it (the slots are built again) so that it can be released.
class NativeRayTracing {
public:
	NativeRayTracing(GraphicContext& graphics, CommandScheduler& scheduler,
	                 BufferCache& buffer_cache);
	~NativeRayTracing();
	KYTY_CLASS_NO_COPY(NativeRayTracing);

	// Whether the device supports host ray tracing and it is enabled.
	[[nodiscard]] bool Enabled() const noexcept { return m_enabled; }
	// Whether shader compilation applies the traversal patches (set once at startup).
	[[nodiscard]] static bool PatchShaders() noexcept;
	// The device was created with the ray tracing extensions (before any shader compiles).
	static void SetDeviceSupport(bool supported) noexcept;

	// The guest TLAS of a known tracing shader: its header's address is the 64-bit value at
	// user data pointer (user SGPR pair `user_sgpr`) + `header_offset`; `root` its root node,
	// `layout` how the shader reads its instance records (Decoder::RayTracingLayout::Pack()).
	struct TlasSource {
		uint32_t                user_sgpr     = 0;
		uint32_t                header_offset = 0;
		uint32_t                root          = 0;
		std::array<uint32_t, 4> layout {};
		uint32_t                header_vsharp = 0xffffffffu;
	};
	// The pointer the header address is at `header_offset` from: the user data pointer, or the
	// base of the V# at it + `header_vsharp` (read with `read`, false when unreadable: 0).
	[[nodiscard]] static uint64_t UserPointer(const TlasSource& source, uint64_t pointer,
	                                          const std::function<bool(uint64_t, uint64_t)>& readable);
	// The source of a program that traces through a guest TLAS (its info), or false.
	[[nodiscard]] static bool FindTlasSource(const ShaderRecompiler::IR::ShaderInfo& info,
	                                         TlasSource& source) noexcept;

	// Records, before the dispatch, the walk of the guest TLAS whose header the 64-bit value at
	// `user_pointer + source.header_offset` points to, the builds of pending BLASes and the TLAS
	// build. Outside a render pass.
	void Prepare(const CommandRecorder& command, uint64_t user_pointer, const TlasSource& source,
	             uint64_t frame);

	// The TLAS formats of the generations of sce::Psr (see hle_tlas.comp): the header and the
	// instance records of GTA V 01.010/01.011 (Base460) and of 01.005 (Base252).
	enum class HleTlasFormat : uint32_t {
		Base460,
		Base252,
	};
	// GTA V's TLAS builds (see CommandProcessor::HleTlasDispatch()): records the build of a TLAS
	// in the game's format at `header_va` from the instance list the game's CPU writes (`count`
	// records of `stride` bytes at `input_va`, see hle_tlas.comp). Outside a render pass.
	void RecordHleTlas(const CommandRecorder& command, uint64_t input_va, uint32_t count,
	                   uint32_t stride, uint64_t header_va, HleTlasFormat format);
	// The instances one HLE TLAS build holds at most.
	static constexpr uint32_t HleTlasMaxInstances = 4096;
	// The first 128 bytes of the header hle_tlas.comp writes for `count` instances (it depends on
	// nothing else); returns the TLAS size.
	static uint32_t HleTlasHeader(uint32_t count, HleTlasFormat format,
	                              std::array<uint32_t, 32>& header) noexcept;

	// The device address of the context table the shaders look their TLAS up in (see
	// ContextEntry in rt_tlas_walk.comp), 0 when disabled.
	[[nodiscard]] uint64_t ContextAddress() const noexcept { return m_context_address; }

	static constexpr uint32_t MaxContexts = 4;

private:
	struct Allocation {
		Buffer*           buffer  = nullptr;
		uint64_t          offset  = 0;
		vk::DeviceAddress address = 0;
		uint64_t          size    = 0;
	};

	// Sub-allocates device memory for acceleration structures and their inputs. Allocations go to
	// the fullest chunk with a free range that fits (best fit inside it), so the other chunks
	// empty out; a chunk with nothing allocated is released (one standard chunk is kept to avoid
	// allocating memory again and again), and an allocation larger than half a chunk gets a
	// chunk of its own that is released with it. A chunk can be drained: new allocations avoid
	// it until the host has moved what is left in it elsewhere. Free() is for memory the GPU no
	// longer reads.
	class Arena {
	public:
		Arena(GraphicContext& graphics, CommandScheduler& scheduler): m_graphics(graphics),
		                                                              m_scheduler(scheduler) {}
		Allocation Allocate(uint64_t size, uint64_t alignment);
		void       Free(const Allocation& allocation);
		// Bytes allocated now, and the bytes of the chunks.
		[[nodiscard]] uint64_t Used() const noexcept { return m_used; }
		[[nodiscard]] uint64_t Reserved() const noexcept { return m_reserved; }
		// Starts draining the standard chunk with the least allocated, if the free space of the
		// others holds what it has; false when there is none.
		bool BeginDrain();
		void EndDrain() noexcept { m_draining = nullptr; }
		// The buffer of the chunk being drained, or null.
		[[nodiscard]] const Buffer* Draining() const noexcept { return m_draining; }

	private:
		static constexpr uint64_t ChunkSize = uint64_t {64} << 20u;
		struct Chunk {
			std::unique_ptr<Buffer>      buffer;
			// Free ranges by offset, and their total.
			std::map<uint64_t, uint64_t> free_ranges;
			uint64_t                     free_bytes = 0;
			// Holds a single large allocation.
			bool                         dedicated  = false;
		};
		GraphicContext&                     m_graphics;
		CommandScheduler&                   m_scheduler;
		std::vector<std::unique_ptr<Chunk>> m_chunks;
		uint64_t                            m_used     = 0;
		uint64_t                            m_reserved = 0;
		const Buffer*                       m_draining = nullptr;
	};

	// The sizes of the memory a slot needs.
	struct SlotSizes {
		uint32_t capacity0 = 0;
		uint32_t capacity1 = 0;
		uint64_t storage   = 0;
		uint64_t vertices0 = 0;
		uint64_t vertices1 = 0;
		uint64_t entries   = 0;
		uint64_t scratch   = 0;
		[[nodiscard]] uint64_t Total() const noexcept { return storage + vertices0 + vertices1 + entries; }
	};

	struct Slot {
		uint64_t                     blas_va     = 0;
		uint32_t                     root        = 0;
		uint32_t                     table_index = 0;
		uint32_t                     capacity0   = 0;
		uint32_t                     capacity1   = 0;
		// The leaves of the guest BLAS when it was counted (0: not known): its walk needs a queue
		// of about as many nodes in the pool.
		uint32_t                     leaf_count  = 0;
		Allocation                   vertices0;
		Allocation                   vertices1;
		Allocation                   entries;
		Allocation                   storage;
		vk::AccelerationStructureKHR handle      = nullptr;
		vk::DeviceAddress            as_address  = 0;
		uint64_t                     build_scratch = 0;
		bool                         built       = false;
		bool                         needs_build = false;
		// Until this preparation the slot is rebuilt every time (the guest changed it lately).
		uint32_t                     dynamic_until = 0;
		uint64_t                     built_frame   = UINT64_MAX;
		// The last frame a TLAS referenced the slot (from the readbacks), or its creation.
		uint64_t                     last_use_frame  = 0;
		// Released: kept in m_retired's shadow until the readbacks that may name it are gone.
		bool                         evicted         = false;
	};

	// Acceleration structures and buffers the GPU may still read, destroyed once the work
	// recorded when they were released has completed. A released slot index is reused only after
	// every readback recorded before the release was processed.
	struct Retired {
		uint64_t                     tick       = 0;
		uint64_t                     prepare_id = 0;
		vk::AccelerationStructureKHR handle     = nullptr;
		std::array<Allocation, 4>    allocations {};
		uint32_t                     slot       = UINT32_MAX;
	};

	struct PendingSlot {
		uint64_t blas_va     = 0;
		uint32_t root        = 0;
		uint32_t table_index = 0;
		uint32_t leaf_count  = 0;
		uint32_t opaque      = 0;
		uint32_t alpha       = 0;
	};

	struct Readback {
		std::unique_ptr<Buffer> buffer;
		uint64_t                tick  = 0;
		uint32_t                build_count = 0;
		std::vector<uint32_t>   build_slots;
		// The TLAS source the preparation walked (user SGPR << 16 | header offset).
		uint32_t                source_key  = 0;
		// The frame of the preparation: the slots its TLAS referenced were used then.
		uint64_t                frame       = 0;
	};

	void CreatePipelines();
	void CreateBuffers();
	void CreateTlas(uint32_t capacity);
	// Creates the slot of a counted BLAS; false when no slot is free.
	bool CreateSlot(const PendingSlot& pending);
	// The sizes of a slot holding `opaque` and `alpha` tested triangles (plus room to grow, none
	// when `exact`: they are the capacities);
	// false when it is implausibly large, its build would not fit in the scratch buffer or it
	// would not fit in the budget (m_budget).
	bool ComputeSlotSizes(uint32_t opaque, uint32_t alpha, SlotSizes& sizes, bool exact = false);
	// Creates the acceleration structure of `slot` and allocates its memory (a grown or moved
	// slot has released its old memory, see RetireResources).
	void AllocateSlot(Slot& slot, const SlotSizes& sizes);
	[[nodiscard]] static uint64_t SlotBytes(const Slot& slot) noexcept;
	// A BLAS that could not be counted or given a slot: its table entry is forgotten so the walk
	// reports it again, a few times (m_mutex held).
	void ReportAgain(uint64_t blas_va, uint32_t root, uint32_t table_index);
	// Moves the acceleration structure and the buffers of `slot` to m_retired.
	// `slot_index` is the index of a slot released for good, UINT32_MAX for a grown slot.
	void RetireResources(Slot& slot, uint32_t slot_index);
	// Destroys what the GPU has finished with and recycles released slot indices.
	void DestroyRetired();
	// Evicts the slots no TLAS referenced lately (a bounded number per call), appending their
	// indices to `evicted`; the GPU side is written by the caller.
	void EvictUnused(std::vector<uint32_t>& evicted);
	// Over the budget, evicts the slots used least recently (same contract as EvictUnused).
	void EvictOverBudget(std::vector<uint32_t>& evicted);
	void EvictSlot(uint32_t index);
	// When the arena holds much more memory than it uses, moves the slots with memory in the
	// least used chunk to new memory (like a grown slot: they are built again), appending their
	// indices to `moved`.
	void Defragment(std::vector<uint32_t>& moved);
	void RecordBuilds(const CommandRecorder& command);
	void ProcessReadback(Readback& readback);

	GraphicContext&   m_graphics;
	CommandScheduler& m_scheduler;
	BufferCache&      m_buffer_cache;
	bool              m_enabled = false;
	bool              m_cleared = false;

	vk::DescriptorSetLayout m_descriptor_layout = nullptr;
	vk::PipelineLayout      m_tlas_layout       = nullptr;
	vk::PipelineLayout      m_blas_layout       = nullptr;
	vk::Pipeline            m_tlas_pipeline     = nullptr;
	vk::Pipeline            m_blas_pipeline     = nullptr;
	vk::PipelineLayout      m_hle_tlas_layout   = nullptr;
	vk::Pipeline            m_hle_tlas_pipeline = nullptr;
	std::unique_ptr<Buffer> m_hle_tlas_scratch;

	std::unique_ptr<Buffer> m_state;
	std::unique_ptr<Buffer> m_queue;
	std::unique_ptr<Buffer> m_instance_nodes;
	std::unique_ptr<Buffer> m_instances;
	std::unique_ptr<Buffer> m_table;
	std::unique_ptr<Buffer> m_slots;
	std::unique_ptr<Buffer> m_missing;
	std::unique_ptr<Buffer> m_context;
	std::unique_ptr<Buffer> m_pool;
	std::unique_ptr<Buffer> m_verify_list;
	std::unique_ptr<Buffer> m_changed;
	std::unique_ptr<Buffer> m_jobs;
	std::unique_ptr<Buffer> m_build_scratch;
	std::unique_ptr<Buffer> m_tlas_storage;
	vk::AccelerationStructureKHR m_tlas         = nullptr;
	vk::DeviceAddress            m_tlas_address = 0;
	uint64_t                     m_tlas_scratch_size = 0;
	uint32_t                     m_tlas_capacity     = 0;
	uint64_t                     m_context_address   = 0;
	Arena                        m_arena;

	std::array<Readback, 32> m_readbacks;
	uint32_t                m_readback_index = 0;

	// Host state the readbacks (deferred host operations) update.
	Common::Mutex             m_mutex;
	std::vector<PendingSlot>  m_new_slots;
	std::vector<std::unique_ptr<Slot>> m_slot_list;
	std::vector<uint32_t>     m_build_queue;
	std::vector<uint32_t>     m_dynamic;
	// Slots whose builds overflowed: slot, opaque and alpha tested triangles.
	std::vector<std::array<uint32_t, 3>> m_grow;
	// Table entries of BLASes that could not be counted, cleared so the walk reports them again.
	std::vector<uint32_t>     m_retry;
	// Reports of each BLAS (address and root) that could not be counted or given a slot, and the
	// table entry still holding the failed one. Bounded (see MaxRetryKeys).
	struct RetryState {
		uint32_t count       = 0;
		uint32_t table_index = 0;
	};
	std::unordered_map<uint64_t, RetryState> m_retries;
	// Table entries of BLASes whose reports failed MaxRetries times, with the preparation they are
	// forgotten at (so the walk reports them again): a guest BLAS that cannot be counted now (the
	// pool was full, its memory was being written) may be countable later.
	std::vector<std::pair<uint64_t, uint32_t>> m_late_retries;
	// Slot indices free for reuse, released slots and resources waiting for the GPU.
	std::vector<uint32_t>     m_free_slots;
	std::deque<Retired>       m_retired;
	// Bytes of the memory in m_retired.
	uint64_t                  m_retired_bytes     = 0;
	// The most the arena allocates, 0 for no limit (KYTY_RT_ARENA_MB, else a share of the VRAM).
	uint64_t                  m_budget            = 0;
	// No defragmentation before this preparation / since this preparation.
	uint64_t                  m_defrag_after      = 0;
	uint64_t                  m_defrag_started    = 0;
	uint64_t                  m_last_scan_prepare = 0;
	// The instance count of the last walk of each TLAS source (shaders trace different TLASes).
	std::unordered_map<uint32_t, uint32_t> m_last_instance_counts;
	uint32_t                  m_prepare_id          = 0;
	// BlasPush::flags of the last preparation's layout.
	uint32_t                  m_blas_flags          = 0;
	uint64_t                  m_frame               = 0;

};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_NATIVERAYTRACING_H_
