#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RAYTRACING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RAYTRACING_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace ShaderRecompiler::IR {
struct ShaderInfo;
} // namespace ShaderRecompiler::IR

class BufferCache;
class CommandRecorder;
class CommandScheduler;
class RenderContext;
struct GraphicContext;
struct RayTracingTestAccess;


// RayTracing::ReadGuestTlas(): the end of a guest TLAS's memory and its instance count.
// Outside the class so that it is complete where the class stores it in a std::pair: a nested
// class with default member initializers is not default constructible until the enclosing class
// is complete, and libstdc++'s pair constrains its default constructor on that.
struct RayTracingGuestTlas {
	uint64_t end       = 0;
	uint32_t instances = 0;
};


// Mirrors guest BVHs into host acceleration structures (VK_KHR_acceleration_structure), for the
// traversals the ray query pass rewrites (RayQueryTraversal, Prepare(uint64_t, uint64_t)) and
// those of the native ray tracing forms (IR::TraceNativeTraversals, Prepare(const CommandRecorder&,
// ...)). The guest builds and refits its BVHs on the GPU, so GPU jobs read them there; the host
// reads back only what sizes the builds. A guest TLAS instance names a (BLAS, root node) pair,
// mirrored as one host BLAS (a slot).
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
// BLASes whose builds the command processor saw (games that build them with sce::Psr, see
// NoteGuestBlasBuilds()) do not wait for that: their slots are created, sized from the build's
// geometries, as the guest builds them and built at the next preparation, and the walk finds them
// in a table of the host's.
//
// The host memory is bounded: the readbacks list the slots each TLAS references, a slot no TLAS
// referenced for 1800 frames is evicted (its table entry, acceleration structure and
// buffers are released) and a slot whose BLAS grew releases its old structure. Whatever the GPU
// may still read is destroyed only after the work recorded so far has completed. The memory of
// the structures has a budget too (a share of the VRAM, KYTY_RT_ARENA_MB overrides it): a BLAS
// that would not fit, or is implausibly large, is not allocated, and over the budget the slots
// used least recently are evicted. A nearly empty arena chunk is emptied by moving what is left
// in it (the slots are built again) so that it can be released.
class RayTracing {
public:
	explicit RayTracing(RenderContext& context);
	~RayTracing();
	KYTY_CLASS_NO_COPY(RayTracing);

	// Records the mirror of the "PSR_BVHL" TLAS header (format 1, 3, 6 or 10) at `tlas` for a
	// program the ray query pass rewrote, in GPU frame `frame` (what the eviction of unused
	// mirrors counts). Returns the device address of its context entry (ContextEntry in
	// rt_tlas_walk.comp: u32 valid at +36, guest instance count at +32, TLAS address at +8, guest
	// instance array at +40, the slot infos at +24), valid once every visible instance has a host
	// BLAS as the guest holds it at the dispatch; until then the program's own loop walks the
	// guest BVH. New or changed BVHs get mirrors a few readbacks later. Instance i of the TLAS is
	// record i, with the slot of its BLAS as custom index; a hit's prim record is in the slot's
	// entry table. The context and the mirrors serve the commands recorded before the next call.
	// 0 without host ray tracing.
	[[nodiscard]] vk::DeviceAddress Prepare(uint64_t tlas, uint64_t frame);

	// Called at each suspend point: destroys the structures the GPU no longer reads.
	void RunGarbageCollector();

	// Whether the device supports host ray tracing and it is enabled.
	[[nodiscard]] bool Enabled() const noexcept { return m_enabled; }
	// Whether shader compilation rewrites the walks of the native ray tracing forms to trace natively
	// (IR::TraceNativeTraversals; --native-rt, set once at startup).
	[[nodiscard]] static bool NativeTraversals() noexcept;
	// The device was created with the ray tracing extensions (before any shader compiles).
	static void SetDeviceSupport(bool supported) noexcept;

	// The guest TLAS of a known tracing shader: its header's address is the 64-bit value at
	// user data pointer (user SGPR pair `user_sgpr`) + `header_offset` (or that pointer itself, see
	// HeaderInUserData()); `root` its root node,
	// `layout` how the shader reads its instance records (IR::RayTracingLayout::Pack());
	// `header_slots` the flattened SRT dwords the shader reads the header's address from, when it
	// loads it from its user data (ShaderInfo::ray_query_header_slots); `header_vsharp` (not
	// NoRayQuerySlot) the offset from the user data pointer of a V# whose base takes the place of
	// the pointer (ShaderInfo::ray_query_header_vsharp, see UserPointer()).
	struct TlasSource {
		uint32_t                user_sgpr     = 0;
		uint32_t                header_offset = 0;
		uint32_t                root          = 0;
		std::array<uint32_t, 4> layout {};
		std::array<uint32_t, 2> header_slots {};
		uint32_t                header_vsharp = 0xffffffffu;
		// The header's address itself, when the caller has it (not 0: the fields above that
		// locate it do not count).
		uint64_t                header        = 0;
	};
	// TlasSource::layout word 3 of the traversals the ray query pass rewrites (see LayoutPsrHeader
	// in rt_common.inc): Psr headers of format 1, 3, 6 or 10 as the header says.
	static constexpr uint32_t PsrHeaderLayout = 1u << 30u;
	// The pointer the header address is at `header_offset` from: the user data pointer, or the
	// base of the V# at it + `header_vsharp` (read with `read`, false when unreadable: 0).
	[[nodiscard]] static uint64_t UserPointer(const TlasSource& source, uint64_t pointer,
	                                          const std::function<bool(uint64_t, uint64_t)>& readable);
	// Whether the header's address is the user data pointer itself, of which the shader takes the
	// low 48 bits (IR::ShaderInfo::UserDataRayQueryHeader): no memory holds it.
	[[nodiscard]] static bool HeaderInUserData(const TlasSource& source) noexcept;
	[[nodiscard]] static constexpr uint64_t UserDataHeader(uint64_t pointer) noexcept {
		return pointer & 0xffffffffffffull;
	}
	// The source of a program that traces through a guest TLAS (its info), or false.
	[[nodiscard]] static bool FindTlasSource(const ShaderRecompiler::IR::ShaderInfo& info,
	                                         TlasSource& source) noexcept;
	// The source of the guest TLAS of a program whose BVH walk a native form recognizes, traced
	// natively or not (without header slots when the guest walks it), or false.
	[[nodiscard]] static bool FindGuestTlasSource(const ShaderRecompiler::IR::ShaderInfo& info,
	                                              TlasSource& source) noexcept;

	// The guest TLAS at `header` as the CPU reads it, where `readable(address, size)` holds (memory
	// the CPU wrote last), before the dispatch that traces it: the end of its memory (its box
	// nodes and the instance records of the leaves they reach), 0 when it cannot be read, and its
	// instance count. When it reads the TLAS (not one of the last it read: execution thread), it
	// adds to `blases` the memory ([first, second)) of the BLASes its instances reference that it
	// can read as well.
	using GuestTlas = RayTracingGuestTlas;
	[[nodiscard]] GuestTlas ReadGuestTlas(uint64_t header, const TlasSource& source,
	                                      const std::function<bool(uint64_t, uint64_t)>& readable,
	                                      std::vector<std::pair<uint64_t, uint64_t>>& blases);
	// The guest BLASes that the next Prepare() may walk or gather to build them (appended).
	void PendingWalks(std::vector<uint64_t>& blases);
	// The caller uploaded the CPU writes of the memory of `blases` only (not of all cached
	// memory): the next Prepare() builds no other BLAS; the others wait for the one after.
	void AnnounceWalks(std::span<const uint64_t> blases);
	// The instance count of the TLAS that the next Prepare() walks, read on the CPU (0: not known):
	// the host TLAS of a TLAS the walks have not seen yet has room for all its instances.
	void NoteTlasInstances(uint32_t count) noexcept { m_next_instances = count; }

	// Records, before the dispatch, the walk of the guest TLAS whose header the 64-bit value at
	// `user_pointer + source.header_offset` points to (the dwords at `source.header_slots` of
	// `flattened_srt`, the dispatch's flattened SRT, when it has them), the builds of pending
	// BLASes and the TLAS build. Outside a render pass. `shader_hash`: the dispatch's program (for
	// the "[RT]" summaries).
	void Prepare(const CommandRecorder& command, uint64_t user_pointer, const TlasSource& source,
	             const vk::DescriptorBufferInfo& flattened_srt, uint64_t frame, uint64_t shader_hash);

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

	// GTA V's BLAS refits (see CommandProcessor::HleBlasDispatch()): a geometry of a refit BLAS
	// with its vertex buffer V# and object matrix (column-major), as the game's CPU wrote them.
	struct HleRefitGeometry {
		uint64_t                 blas     = 0;
		uint32_t                 geometry = 0;
		uint32_t                 pad      = 0;
		std::array<uint32_t, 4>  vsharp {};
		std::array<float, 16>    matrix {};
	};
	// Records the refits of `blases` in the game's format: the vertices of the triangle nodes of
	// `geometries`, then the boxes of the box nodes (see hle_blas_refit.comp). `header_layout` is
	// the BLAS header of the library: 0 for generation Base460, 1 for Base252, 2 for Base515.
	// `arguments` (Base515): instead of refitting, writes the dispatch arguments of the game's
	// vertex passes of `blases` (see hle_blas_refit.comp). Outside a render pass.
	void RecordHleRefit(const CommandRecorder& command, std::span<const uint64_t> blases,
	                    std::span<const HleRefitGeometry> geometries, uint32_t header_layout,
	                    bool arguments = false);

	// A BLAS the game built with sce::Psr (see CommandProcessor::PsrBlasDispatch()): its address
	// and, per geometry in the build's order, its triangle count and whether it is opaque, as the
	// game's CPU wrote them in the build's geometry table.
	struct GuestBlasBuild {
		uint64_t                               blas_va = 0;
		std::vector<std::pair<uint32_t, bool>> geometries;
		// Where its header holds the fields the layout build reads: 0 in generation Base460, 1 in
		// Base252 (GTA V 01.005, see rt_blas_walk.comp).
		uint32_t                               header_layout = 0;
	};
	// In operation order after the guest built the BLASes of `builds`: gives each a slot laid out
	// by primitive (each guest triangle at a position given by its geometry and triangle index, see
	// rt_blas_walk.comp) and builds it at the next preparation, from the guest BLAS as usual. A BLAS
	// whose build does not match the layout is given up (its slot is evicted) and from then on
	// found and counted by the walks.
	void NoteGuestBlasBuilds(std::span<const GuestBlasBuild> builds);

	// In operation order after sce::Psr decompressed a BLAS into the `memory_size` bytes at
	// `blas_va` (see CommandProcessor::PsrBlasDispatch()): gives it a slot sized from its memory and
	// builds it at the next preparation by walking the guest BLAS, instead of waiting for a walk of a
	// TLAS to find it.
	void NoteGuestBlasDecompressed(uint64_t blas_va, uint64_t memory_size);

	// The dispatches of programs that walk a guest BVH without tracing natively, for the "[RT]"
	// summaries of the log (every 10 s, only when something did not trace natively): the program
	// with the guest's own walks of a dispatch without a TLAS, and a program whose walk no native
	// form recognizes. On the execution thread, before the dispatch.
	enum class GuestDispatch : uint32_t { NullTlas, Unrecognized, Count };
	static void NoteGuestDispatch(GuestDispatch kind, uint64_t shader_hash) noexcept;
	// GPU counts for instrumented ray query sites; a hash of zero denotes a shared entry.
	struct RayCounts {
		uint64_t native;
		uint64_t guest;
	};
	[[nodiscard]] static RayCounts GetRayCounts() noexcept;
	static void NoteRayCounts(uint64_t shader_hash, uint32_t native, uint32_t guest);

	// The device address of the context table the shaders look their TLAS up in (see
	// ContextEntry in rt_tlas_walk.comp), 0 when disabled.
	[[nodiscard]] uint64_t ContextAddress() const noexcept { return m_context_address; }

	static constexpr uint32_t MaxContexts = 4;
	// A host BLAS no TLAS referenced for this many frames is evicted (its BLAS left the scene or
	// its memory was unmapped); one that comes back is counted and built again as a new BLAS, so
	// objects the camera turns away from for a while keep their shadows when it turns back.
	static constexpr uint64_t EvictFrames = 1800;

private:
	friend struct RayTracingTestAccess;


	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	BufferCache&                                      m_buffer_cache;
	RenderContext&                                    m_context;

	// The host structures of the traversals of the native ray tracing forms
	// (rayTracingSlots.cpp), set up and released by the constructor and the destructor.
	void InitializeSlots();
	void DestroySlots();

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
		// The last HLE refit pass that refit the guest BLAS, and the one before the last host
		// build.
		uint64_t                     refit_seq       = 0;
		uint64_t                     built_refit_seq = 0;
		// Built for a fast build (see FastBlasFlags), gathered builds since the last walk.
		bool                         fast_built      = false;
		uint32_t                     gathers         = 0;
		// The last build allows updates (see UpdatableBlasFlags): a gathered build refits it.
		bool                         updatable       = false;
		// The last frame a TLAS referenced the slot (from the readbacks), or its creation.
		uint64_t                     last_use_frame  = 0;
		// Released: kept in m_retired's shadow until the readbacks that may name it are gone.
		bool                         evicted         = false;
		// A slot of a BLAS whose build the host saw: per guest geometry, its first host primitive,
		// triangle count and opacity (see NoteGuestBlasBuilds()), and its entry in the host table
		// (UINT32_MAX: none; table_index is then UINT32_MAX too).
		std::vector<std::array<uint32_t, 4>> layout;
		// The header layout of the BLAS of the layout (GuestBlasBuild::header_layout).
		uint32_t                     header_layout    = 0;
		uint32_t                     host_table_index = UINT32_MAX;
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
		// The program of the dispatch.
		uint64_t                shader_hash = 0;
	};

	void CreatePipelines();
	void CreateBuffers();
	void CreateTlas(uint32_t capacity);
	// Creates the slot of a counted BLAS; false when no slot is free.
	bool CreateSlot(const PendingSlot& pending);
	// A slot in the host table for the BLAS at `blas_va` (root GuestBlasRoot), queued for a build;
	// its index, or UINT32_MAX when the table or the slots are full (m_mutex held).
	uint32_t CreateHostSlot(uint64_t blas_va, const SlotSizes& sizes);
	// The slot of the BLAS at `blas_va` with root GuestBlasRoot, or UINT32_MAX (m_mutex held).
	[[nodiscard]] uint32_t FindHostSlotIndex(uint64_t blas_va) const;
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
	// Whether it gathered slots (their boxes are taken by the check of this preparation).
	bool RecordBuilds(const CommandRecorder& command);
	void ProcessReadback(Readback& readback);

	bool              m_enabled = false;
	bool              m_cleared = false;

	vk::DescriptorSetLayout m_walk_descriptor_layout = nullptr;
	vk::PipelineLayout      m_tlas_layout       = nullptr;
	vk::PipelineLayout      m_blas_layout       = nullptr;
	vk::Pipeline            m_tlas_pipeline     = nullptr;
	vk::Pipeline            m_blas_pipeline     = nullptr;
	vk::PipelineLayout      m_hle_tlas_layout   = nullptr;
	vk::Pipeline            m_hle_tlas_pipeline = nullptr;
	std::unique_ptr<Buffer> m_hle_tlas_scratch;
	vk::PipelineLayout      m_hle_refit_layout   = nullptr;
	vk::Pipeline            m_hle_refit_pipeline = nullptr;
	std::unique_ptr<Buffer> m_hle_jobs;
	std::unique_ptr<Buffer> m_hle_pool;
	std::unique_ptr<Buffer> m_hle_state;

	std::unique_ptr<Buffer> m_walk_state;
	std::unique_ptr<Buffer> m_queue;
	std::unique_ptr<Buffer> m_instance_nodes;
	std::unique_ptr<Buffer> m_instances;
	std::unique_ptr<Buffer> m_table;
	std::unique_ptr<Buffer> m_slots;
	std::unique_ptr<Buffer> m_missing;
	std::unique_ptr<Buffer> m_context_table;
	std::unique_ptr<Buffer> m_pool;
	std::unique_ptr<Buffer> m_verify_list;
	std::unique_ptr<Buffer> m_changed;
	std::unique_ptr<Buffer> m_jobs;
	// The layouts of the builds of a preparation (see Slot::layout).
	std::unique_ptr<Buffer> m_layouts;
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
	// The slots of each guest BLAS address, and the HLE refit passes so far.
	std::unordered_map<uint64_t, std::vector<uint32_t>> m_slots_by_blas;
	uint64_t m_refit_seq = 0;
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
	// The host TLAS size below which the walks of a source do not start (the count of its overflows).
	std::unordered_map<uint32_t, uint32_t> m_instance_floors;
	// The sources whose TLAS the CPU has counted (NoteTlasInstances()).
	std::unordered_set<uint32_t>           m_cpu_counted_sources;
	// ReadGuestTlas(): the last TLASes it read (by header), replaced in turn; NoteTlasInstances().
	std::array<std::pair<uint64_t, GuestTlas>, 8> m_guest_tlases {};
	uint32_t                                      m_guest_tlas_next = 0;
	uint32_t                                      m_next_instances  = 0;
	uint32_t                  m_prepare_id          = 0;
	// The TLASes whose header is the user data pair that a preparation checked the BLASes of
	// (header, frame), replaced in turn (see Prepare()).
	std::array<std::pair<uint64_t, uint64_t>, 8> m_checked_headers {};
	uint32_t                                     m_checked_next = 0;
	// BlasPush::flags of the last preparation's layout.
	uint32_t                  m_blas_flags          = 0;
	uint64_t                  m_walk_frame               = 0;
	// The host table (see FindHostSlot in rt_tlas_walk.comp): the key of each entry, and the
	// entries to write at the next preparation (index, key, slot + 1).
	std::vector<uint64_t>                                 m_host_keys;
	std::vector<std::tuple<uint32_t, uint64_t, uint32_t>> m_host_table_writes;
	// Slots that got new memory outside a preparation: their infos are written at the next one.
	std::vector<uint32_t>     m_moved_slots;
	// Slots whose layout build did not match: evicted at the next preparation.
	std::vector<uint32_t> m_give_up;
	// AnnounceWalks(): the BLASes (sorted) the next preparation may build, when announced.
	std::vector<uint64_t> m_announced_walks;
	bool                  m_walks_announced = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RAYTRACING_H_
