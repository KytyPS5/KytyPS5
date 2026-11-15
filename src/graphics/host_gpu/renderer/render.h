#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/drawTemplates.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/upscaler/upscaler.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

struct GraphicContext;
struct DrawCallInfo;
struct DrawEmitInfo;
struct DrawIndexBufferSource;
struct DynamicState;
class RenderContext;
class CommandScheduler;
struct RenderExecutorTestAccess;
struct IndirectThreadPass;

// An image description derived from a T# and a shader's view of it (see ResolveTexture).
struct TextureDescDerivation {
	TextureCache::ImageDesc desc;
	bool                    shader_conversion = false;
	vk::Format              pixel_format      = vk::Format::eUndefined;
	vk::Format              view_format       = vk::Format::eUndefined;
	uint64_t                size              = 0;
	// Why the host cannot view the T# as the shader reads it (the rest is then unset); empty when
	// it can. See RenderExecutor::ResolveTexture().
	std::string unsupported;
	// The image the last lookup of a sampled texture with this description found, the
	// description it was found with and the view acquired for it. Reused while no image on the
	// pages of the range was registered or unregistered since and the image keeps the state the
	// acquisition left it in.
	struct Found {
		ImageId                 id;
		TextureCache::ImageDesc desc;
		vk::ImageView           view            = nullptr;
		uint64_t                image_set_epoch = 0;
		ImageLookupState        state;
		bool                    valid = false;
	};
	Found found;
};

enum class DrawOffsetSource : uint8_t {
	DrawState,
	IndirectArgs,
};

// Nonzero address: the GPU reads count Vulkan indirect draw records, stride bytes apart. With a
// count address the GPU reads the number of draws there too (count is then the maximum): GPU work
// (culling shaders) writes the records and the count, and reading them on the CPU would wait for
// all the GPU work recorded before the draw.
struct DrawIndirectArgs {
	uint64_t              address = 0;
	uint32_t              count   = 0;
	uint32_t              stride  = 0;
	ShaderIndirectOffsets offsets;
	uint64_t              count_address = 0;
};

struct DrawIndexArgs {
	uint32_t         index_count         = 0;
	const void*      index_addr          = nullptr;
	uint32_t         instance_count      = 0;
	uint32_t         index_type_and_size = 0;
	int32_t          base_vertex         = 0;
	uint32_t         first_instance      = 0;
	DrawOffsetSource offset_source       = DrawOffsetSource::DrawState;
	// With indirect records, index_count is INDEX_BUFFER_SIZE.
	DrawIndirectArgs indirect;
};

struct DrawAutoArgs {
	uint32_t         vertex_count   = 0;
	uint32_t         instance_count = 0;
	uint32_t         first_vertex   = 0;
	uint32_t         first_instance = 0;
	DrawOffsetSource offset_source  = DrawOffsetSource::DrawState;
	DrawIndirectArgs indirect;
};

// The hash of the context and user configuration registers under the snapshot ids it was made for
// (NativeXprStateKeyOf).
struct StateKeyCache {
	uint64_t context_id = 0, user_config_id = 0, hash = 0;
};

struct SubmitInfo {
	static constexpr uint32_t MaxSemaphores = 3;

	std::array<vk::Semaphore, MaxSemaphores>          wait_semaphores {};
	std::array<uint64_t, MaxSemaphores>               wait_ticks {};
	std::array<vk::PipelineStageFlags, MaxSemaphores> wait_stages {};
	std::array<vk::Semaphore, MaxSemaphores>          signal_semaphores {};
	std::array<uint64_t, MaxSemaphores>               signal_ticks {};
	uint32_t                                          num_wait_semaphores   = 0;
	uint32_t                                          num_signal_semaphores = 0;

	void AddWait(vk::Semaphore semaphore, uint64_t tick = 1,
	             vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eAllCommands) {
		EXIT_IF(semaphore == nullptr || num_wait_semaphores >= MaxSemaphores);
		wait_semaphores[num_wait_semaphores] = semaphore;
		wait_ticks[num_wait_semaphores]      = tick;
		wait_stages[num_wait_semaphores++]   = stage;
	}

	void AddSignal(vk::Semaphore semaphore, uint64_t tick = 1) {
		EXIT_IF(semaphore == nullptr || num_signal_semaphores >= MaxSemaphores);
		signal_semaphores[num_signal_semaphores] = semaphore;
		signal_ticks[num_signal_semaphores++]    = tick;
	}
};

class CommandBuffer {
public:
	~CommandBuffer() = default;

	KYTY_CLASS_NO_COPY(CommandBuffer);

	[[nodiscard]] bool IsInvalid() const noexcept {
		return m_stream != nullptr ? !m_open : m_buffer == nullptr;
	}

	void BeginRendering(const RenderState& state) const;
	// Ends the rendering instance, then records the barrier of deferred shader writes, if any.
	void EndRendering() const;
	[[nodiscard]] bool IsRendering() const noexcept { return m_rendering; }
	// A draw whose shaders wrote memory leaves the barrier that orders its writes before later
	// work pending while draws the guest did not order after it go on in the same rendering
	// instance (see RenderExecutor::BeginGuestOperation()). EndRendering() records it: every
	// command other than a draw ends rendering before it records, as does the end of the
	// command buffer.
	void DeferShaderWriteBarrier(vk::PipelineStageFlags source_stages) const noexcept {
		m_pending_shader_writes |= source_stages;
	}
	[[nodiscard]] bool HasPendingShaderWrites() const noexcept {
		return static_cast<bool>(m_pending_shader_writes);
	}

	[[nodiscard]] CommandRecorder Handle() const;
	[[nodiscard]] GraphicContext&   GetGraphics() const noexcept { return m_graphics; }
	[[nodiscard]] RenderContext&    GetContext() const noexcept { return m_context; }
	[[nodiscard]] const HW::Context&    GetRegisters() const noexcept { return *m_registers; }
	[[nodiscard]] const HW::UserConfig& GetUserConfig() const noexcept { return *m_user_config; }
	[[nodiscard]] const HW::Shader&     GetShaders() const noexcept { return *m_shaders; }

	// Pipelines recorded outside the draws (the presentation overlay) replace the dynamic state
	// the draws set.
	void InvalidateDynamicState() const;

private:
	CommandBuffer(CommandScheduler& scheduler, CommandStream* stream);
	void Bind(const HW::Context& registers, const HW::UserConfig& user_config,
	          const HW::Shader& shaders) noexcept {
		m_registers   = &registers;
		m_user_config = &user_config;
		m_shaders     = &shaders;
	}

	void Begin();
	void End() const;

	CommandScheduler&   m_scheduler;
	RenderContext&      m_context;
	GraphicContext&     m_graphics;
	// Direct recording writes m_buffer; deferred recording appends to m_stream while m_open.
	vk::CommandBuffer   m_buffer = nullptr;
	CommandStream*      m_stream = nullptr;
	bool                m_open   = false;
	mutable RenderState m_render_state;
	mutable bool        m_rendering   = false;
	// The source stages of the shader writes whose barrier is still to record.
	mutable vk::PipelineStageFlags m_pending_shader_writes;
	const HW::Context*    m_registers   = nullptr;
	const HW::UserConfig* m_user_config = nullptr;
	const HW::Shader*     m_shaders     = nullptr;

	friend class CommandScheduler;
};

namespace Pssr {
struct Request;
}

class RenderExecutor {
public:
	explicit RenderExecutor(RenderContext& context): m_context(context) {}
	~RenderExecutor();
	KYTY_CLASS_NO_COPY(RenderExecutor);

	void DispatchDirect(CommandBuffer& buffer, uint32_t thread_group_x, uint32_t thread_group_y,
	                    uint32_t thread_group_z, uint32_t mode, bool async_compute);
	void DispatchIndirect(CommandBuffer& buffer, uint64_t args_addr, uint32_t mode,
	                      bool async_compute);
	void ProcessPssr(CommandBuffer& buffer, const Pssr::Request& request);
	// DISPATCH_INDIRECT with USE_THREAD_DIMENSIONS: converts the thread counts, which GPU work
	// writes, on the GPU instead of reading them back.
	void DispatchIndirectThreads(CommandBuffer& buffer, uint64_t args_addr, uint32_t mode,
	                             bool async_compute);

	// Shader programs and resources of a draw resolved ahead of its execution.
	struct ResolvedDraw;
	// Resolve thread: resolves the shader programs of a draw with the given registers ahead of
	// its execution; null when the draw resolves them itself (tessellation, programs that are not
	// compiled yet, guest memory that cannot be read ahead). Waits while every resolution is in
	// use: the execution thread releases them in order.
	[[nodiscard]] ResolvedDraw* ResolveDrawAhead(const HW::Context&    context,
	                                             const HW::UserConfig& user_config,
	                                             const HW::Shader&     shaders,
	                                             const ShaderIndirectOffsets& indirect_offsets = {},
	                                             bool indirect_draw = false, uint64_t context_id = 0,
	                                             uint64_t user_config_id = 0);
	// Resolve thread: as ResolveDrawAhead(), for a resolution another thread makes
	// (ResolveDrawInto()): the next resolution of the ring, in use and not ready yet; null when the
	// draw resolves its programs itself. The execution thread waits for it to be ready.
	[[nodiscard]] ResolvedDraw* ReserveResolvedDraw(const HW::Shader& shaders);
	// Any thread that resolves ahead: resolves the draw into `draw` (from ReserveResolvedDraw())
	// and makes it ready, or marks it failed (the draw resolves its programs itself).
	void ResolveDrawInto(ResolvedDraw& draw, const HW::Context& context, const HW::UserConfig& user_config,
	                     const HW::Shader& shaders, const ShaderIndirectOffsets& indirect_offsets,
	                     bool indirect_draw, uint64_t context_id, uint64_t user_config_id);
	// Resolve thread: the compute program of a DISPATCH_DIRECT with the given registers and
	// initiator; null when the dispatch resolves it itself. Shares the ring of draw resolutions.
	[[nodiscard]] ResolvedDraw* ResolveDispatchAhead(const HW::Context& context,
	                                                 const HW::Shader& shaders,
	                                                 const uint32_t (&groups)[3], uint32_t mode,
	                                                 bool async_compute);
	// Resolve thread: the compute program of a DISPATCH_INDIRECT with the given registers and
	// initiator, which does not depend on the arguments the dispatch reads when it executes.
	[[nodiscard]] ResolvedDraw* ResolveIndirectDispatchAhead(const HW::Context& context,
	                                                         const HW::Shader&  shaders,
	                                                         uint32_t           mode, uint64_t args_addr,
	                                                         bool               async_compute);
	// Execution thread: the program a dispatch resolved ahead, when the guest memory the
	// resolution read still holds the same bytes.
	bool TakeResolvedDispatch(ShaderComputeInputInfo& input_info, const ShaderModule*& module);
	// Execution thread: the next draw uses `resolved` when the guest memory it read still holds
	// the same bytes.
	void UseResolvedDraw(ResolvedDraw* resolved) noexcept {
		m_resolved_draw = resolved != nullptr ? WaitResolvedDraw(resolved) : nullptr;
	}
	// Execution thread: `resolved` once ready; null when its resolution failed.
	static ResolvedDraw* WaitResolvedDraw(ResolvedDraw* resolved) noexcept;
	// Whether a stage of the resolved draw writes buffers.
	[[nodiscard]] static bool ResolvedDrawWritesBuffers(const ResolvedDraw& resolved);
	// Execution thread: the operation that carried `resolved` executed.
	static void ReleaseResolvedDraw(ResolvedDraw* resolved) noexcept;
	// Execution thread: the context register snapshot of the operation about to execute (0 for
	// live registers); operations with the same id see the same context registers.
	void UseContextId(uint64_t context_id, uint64_t user_config_id = 0) noexcept {
		m_context_id     = context_id;
		m_user_config_id = user_config_id;
	}
	// Execution thread: an operation of guest queue `queue` is about to execute. The barrier
	// after a dispatch (direct or indirect) is deferred to the next operation, unless that is a
	// dispatch of the same queue the guest did not order after it (see DispatchDirect()).
	void BeginGuestOperation(uint32_t queue, bool dispatch, bool draw, bool guest_sync);
	// Records the barrier a dispatch deferred, if any.
	void FlushDeferredDispatchBarrier();
	// Execution thread: an operation other than a draw is about to execute (see
	// RenderTargetMemo).
	void InvalidateRenderTargetMemo() noexcept;
	// Advances whenever an operation other than a draw executes (see
	// InvalidateRenderTargetMemo()): within one value only draws executed, so no guest-visible
	// effect let the guest write the memory they read.
	[[nodiscard]] uint64_t DrawWindow() const noexcept { return m_draw_window; }

	void PrepareBindings(const ShaderStageRuntime& runtime, PreparedBindings& prepared);
	void FindBuffers(std::span<PreparedBindings* const> stages, bool find_buffers = true);
	void                           RebindBuffers(PreparedBindings& bindings);
	void                           RebindImages(PreparedBindings& bindings);
	void CommitBindings(CommandBuffer& buffer, vk::PipelineBindPoint pipeline_bind_point,
	                    const PipelineCache::Pipeline&     pipeline,
	                    std::span<PreparedBindings* const> bindings);
	// table_candidate (bindless table elements) binds a null image for a texture whose guest
	// memory cannot be read, as tables keep the T#s of freed textures, and for one whose image the
	// texture cache declines to recreate or cannot view as the T# reads it; a depth T# over a
	// color image binds a depth copy of it. examined receives the guest range before the lookup
	// depends on the texture cache: a null image without it depends only on the T# and the guest
	// backing.
	[[nodiscard]] TextureBinding ResolveTexture(const ShaderRecompiler::IR::ImageResource&   resource,
	                                            const ShaderRecompiler::IR::DescriptorValue& value,
	                                            bool        table_candidate = false,
	                                            GuestRange* examined        = nullptr);

	// Native XPR draws (nativeXpr.inc; KYTY_NATIVE_XPR=0 turns them off): an indexed indirect
	// draw whose arguments the host GPU reads, drawn from a record of its object when the record
	// is proven current. False when the normal path draws it (and, when that helps, stores a
	// record).
	[[nodiscard]] bool NativeXprTry(CommandBuffer& buffer, uint64_t args_addr, uint64_t index_base,
	                                uint64_t index_bytes, vk::IndexType index_type);
	// The same for a direct indexed draw.
	[[nodiscard]] bool NativeXprTryDirect(CommandBuffer& buffer, const DrawIndexArgs& args);
	// The draw a try offered is done (drawn or skipped by the normal path).
	void NativeXprEndDraw() noexcept {
		m_native_xpr_store = false;
		m_table_store      = false;
	}
	// Table draws (tableDraw.inc; KYTY_TABLE_XPR=1, =2 also continues runs): an indexed draw of one object (an
	// indirect one whose arguments the host GPU reads at `args_addr`, or `direct`), drawn from its shader pair's table
	// programs and its image set; `clean`: no graphics register but user data changed since the draw before (the
	// command processor's DrawStateObserver). False when the normal path draws it.
	// (`count` records of a run `stride` bytes apart from `args_addr`: his native XPR runs; direct: count 1.)
	// The last try asked the normal path to draw and store the pair's table variant (TableStore): nothing may draw the
	// draw before that (a native XPR record would, and the pair would never get its variant).
	[[nodiscard]] bool TableStoreRequested() const noexcept { return m_table_store; }
	// The last table try of the draw's shader pair drew it: the resolve thread skips its resolution ahead, which the
	// table draw does not use (a draw the table then refuses resolves itself, as one without a resolution ahead).
	[[nodiscard]] bool TableLikely(const HW::Shader& shaders) const noexcept;
	// Bit 0: the pair's last table try drew it; bit 1: a try of the pair was refused lately (its draws keep their
	// resolution ahead).
	[[nodiscard]] uint32_t TableHint(const HW::Shader& shaders) const noexcept;
	// Whether a table try ever drew a draw (any thread).
	[[nodiscard]] bool TableDrewDraws() const noexcept { return m_table_drew_draws.load(std::memory_order_relaxed); }
	// The executing draw's table evaluation, made ahead by the resolve thread (KYTY_SCOUT=3; null: none).
	void SetTableTicket(const void* ticket) noexcept { m_table_ticket = ticket; }
	// The resolve thread: whether a plain dispatch of `cs` most likely goes through the table (tableXpr.inc).
	[[nodiscard]] bool TableDispatchHint(const HW::ComputeShaderInfo& cs) const noexcept;
	void               TableNoteDispatched(const HW::ComputeShaderInfo& cs, bool tabled) noexcept;
	[[nodiscard]] bool TableXprTry(CommandBuffer& buffer, uint64_t args_addr, uint32_t count, uint32_t stride,
	                               uint64_t index_base, uint64_t index_bytes, vk::IndexType index_type,
	                               const DrawIndexArgs* direct, bool clean,
	                               const ShaderIndirectOffsets& offsets = {}, const uint32_t* run_offsets = nullptr);
	// Resolve thread: whether the last native XPR try of the draw's shader pair drew it. Its
	// resolution ahead is then likely wasted (KYTY_XPR_RESOLVE_SKIP=0 resolves them all the same).
	[[nodiscard]] bool NativeXprLikely(const HW::Shader& shaders) const noexcept;

private:
	struct NativeXprCache;
	struct NativeXprRecord;
	struct NativeXprDrawState;
	NativeXprCache&   NativeXprState();
	vk::DescriptorSet NativeXprAllocateSet(vk::DescriptorSetLayout layout, vk::DescriptorPool& pool_out);
	void              NativeXprRetireSet(vk::DescriptorPool pool, vk::DescriptorSet set);
	void              NativeXprRetire(std::unique_ptr<NativeXprRecord> record);
	void              NativeXprCollect();
	bool              NativeXprSweepStates(NativeXprCache& cache, uint64_t frame);
	bool              NativeXprLearn(const NativeXprRecord& stale, NativeXprRecord& fresh);
	const char*       NativeXprBindResources(NativeXprRecord& record, std::span<PreparedBindings* const> stages,
	                                         const PipelineCache::Pipeline& pipeline);
	void NativeXprStore(CommandBuffer& buffer, const DrawRenderState& state, vk::PrimitiveTopology topology,
	                    bool primitive_restart_enable, const RenderState& rendering,
	                    std::span<PreparedBindings* const> stages, const DynamicState& dynamic_state);
	bool NativeXprValidateResources(NativeXprRecord& record, uint64_t frame, bool mapping_moved);
	bool NativeXprValidate(NativeXprRecord& record, uint64_t frame, bool& broken);
	bool NativeXprRebindBuffers(NativeXprRecord& record, std::span<const std::pair<uint32_t, uint32_t>> slots);
	NativeXprRecord* NativeXprRelocated(uint64_t frame, bool& store);
	void             NativeXprRefuseStore(CommandBuffer& buffer);
	void             NativeXprDropRelocated(NativeXprRecord& record);
	void NativeXprStoreRecord(std::unique_ptr<NativeXprRecord> record, CommandBuffer& buffer,
	                          const DrawRenderState& state, vk::PrimitiveTopology topology,
	                          bool primitive_restart_enable, const RenderState& rendering,
	                          std::span<PreparedBindings* const> stages, const DynamicState& dynamic_state,
	                          const GuestReadLog* reads);
	void NativeXprCaptureState(const DrawRenderState& state, const RenderState& rendering, uint64_t state_key,
	                           const DynamicState& dynamic_state);
	struct TableImageSet;
	bool NativeXprEmitTargets(const NativeXprDrawState& draw_state, CommandRecorder vk_buffer,
	                          const TableImageSet* reading = nullptr);
	bool NativeXprTargetsCurrent(const NativeXprDrawState& draw_state);
	void NativeXprEmitDynamic(NativeXprCache& cache, CommandRecorder vk_buffer, const NativeXprDrawState& draw_state,
	                          bool same_command);
	bool NativeXprEmit(CommandBuffer& buffer, NativeXprRecord& record, NativeXprDrawState& draw_state,
	                   const PipelineCache::Pipeline& pipeline, uint64_t args_addr, uint64_t index_base,
	                   uint64_t index_bytes, vk::IndexType index_type);

	// False, with nothing drawn, when the draw's indirect records need CPU counts.
	bool DrawIndex(CommandBuffer& buffer, const DrawIndexArgs& args);
	bool DrawAuto(CommandBuffer& buffer, const DrawAutoArgs& args);

	struct GraphicsBindings {
		std::array<PreparedBindings, 3> vertex;
		PreparedBindings                pixel;
	};

	// Bindless table elements resolved for one root image, by T#. Most of a table is unchanged
	// from dispatch to dispatch, and resolving an element repeats the same texture-cache lookup
	// while no image in its range was registered or unregistered and the guest backing did not
	// change. Elements with DCC metadata are resolved again on every use: their lookup
	// materializes fast clears.
	struct TableElementResolution {
		// What every preparation of a table reads comes first, on one cache line: a table names
		// thousands of elements.
		// The PrepareBindings() call that last bound the element, and its index there.
		uint64_t       prepared_stamp = 0;
		uint32_t       prepared_index = 0;
		// The element's counter in m_table_element_versions (kept while the element lives).
		uint32_t       version_slot  = UINT32_MAX;
		bool           permanent     = false; // Null for the root's view, whatever the memory.
		bool           volatile_dcc  = false;
		// A resolved element depends on the guest backing only through whether its data and
		// metadata are readable: when the backing epoch moves (any map or unmap anywhere,
		// constantly while a game streams), the element stays valid while that answer holds.
		bool           backing_checkable = false;
		bool           backing_readable  = false;
		uint64_t       image_epoch   = 0;
		uint64_t       backing_epoch = 0;
		// The global image-set epoch at which the element was last known valid: while it holds,
		// no image anywhere changed and the range needs no check.
		uint64_t       checked_epoch = 0;
		GuestRange     range; // Guest memory whose images decide the lookup, if any.
		GuestRange     metadata_range;
		TableViewMemo  view;
		TextureBinding texture;
	};
	struct TableDescriptorHash {
		size_t operator()(const std::array<uint32_t, 8>& dwords) const noexcept;
	};
	using TableElements =
	    std::unordered_map<std::array<uint32_t, 8>, TableElementResolution, TableDescriptorHash>;
	// The element the last preparation of a root found for one slot: a slot whose T# did not
	// change skips the lookup by T#.
	struct TableSlot {
		std::array<uint32_t, 8> dwords {};
		TableElementResolution* element = nullptr;
		// The element's version_slot, read without reading the element.
		uint32_t                version_slot = UINT32_MAX;
	};
	struct TableResolution {
		ShaderRecompiler::IR::ImageResource root;
		uint64_t                            last_use = 0;
		TableElements                       elements;
		std::vector<TableSlot>              slots;
	};

	// `stamp` identifies the PrepareBindings() call: an element it already resolved is not
	// resolved again for another slot. `known` is the element of `value`, when the caller has it.
	[[nodiscard]] TableElementResolution&
	ResolveTableElement(TableResolution& table, const ShaderRecompiler::IR::DescriptorValue& value,
	                    uint64_t stamp, TableElementResolution* known = nullptr);
	[[nodiscard]] TableResolution& FindTableResolution(const ShaderRecompiler::IR::ImageResource& root);
	[[nodiscard]] bool TableElementBackingReadable(const TableElementResolution& element);
	// Advances the element's counter: its texture, view or layout changed (see
	// TableBindingState::Checked).
	void TableElementChanged(const TableElementResolution& element) {
		if (element.version_slot < m_table_element_versions.size()) {
			m_table_element_versions[element.version_slot]++;
		}
	}

	// The bindless tables a program bound last. GTA V's ray tracing dispatches bind ~16k slots
	// naming ~8k textures each, and almost all of them stay the same from frame to frame: while
	// the T#s of every slot are unchanged, a binding of the program revisits only the elements
	// that an image registration or a guest backing change may have affected, and the elements
	// whose image is no longer bound as before (see ReuseTableBindings()). The images of the
	// elements stay pinned in the texture cache meanwhile: they are not touched every binding.
	struct TableBindingState {
		struct Range {
			ShaderRecompiler::IR::ImageResource root;
			TableResolution*                    resolution = nullptr;
			uint32_t                            table      = 0;
			uint32_t                            capacity   = 0;
			// The ImageTableSnapshot::slots_id the range's values were taken from (0: unknown).
			uint64_t                            slots_id   = 0;
		};
		struct Element {
			TableElementResolution*               resolution = nullptr;
			TableResolution*                      table      = nullptr;
			ShaderRecompiler::IR::DescriptorValue source;
			uint32_t                              root = 0;
			// Image the element is pinned to.
			ImageId                               pinned;
			// Visited by every binding (DCC metadata), or whenever the guest backing changes
			// (the element's resolution does not record whether its backing is readable).
			bool always            = false;
			bool unchecked_backing = false;
			// The ranges `spans` holds for the element.
			GuestRange span_range;
			GuestRange span_metadata;
		};
		// The guest pages (in texture-cache page granularity) of an element's data or metadata.
		struct PageSpan {
			uint64_t first   = 0;
			uint64_t last    = 0;
			uint32_t element = 0;
		};
		std::vector<Range>                   ranges;
		// The T# of every slot of every range, in binding order.
		std::vector<ShaderRecompiler::IR::DescriptorValue> values;
		std::vector<Element>                 elements;
		// The element of every slot.
		std::vector<uint32_t>                slots;
		// The descriptor of every element, and of every slot.
		std::vector<vk::DescriptorImageInfo> infos;
		std::vector<vk::DescriptorImageInfo> slot_infos;
		// The slots of each element: element_slots[element_slot_begin[e] .. [e + 1]).
		std::vector<uint32_t>                element_slot_begin;
		std::vector<uint32_t>                element_slots;
		// The element of each element resolution, by its version_slot (UINT32_MAX: none). The
		// elements of a state that applies are alive, so their version slots are theirs alone
		// (see ResetBindings()).
		std::vector<uint32_t>                element_of_version;
		// Element spans by first page; `max_span` is the longest (last - first).
		std::vector<PageSpan>                spans;
		uint64_t                             max_span = 0;
		// Elements visited by every binding, and whenever the guest backing changes.
		std::vector<uint32_t>                always;
		std::vector<uint32_t>                unchecked_backing;
		// The state of a compute program: no draw samples its depth elements in an attachment
		// layout (see DepthTableElement()).
		bool                                 compute = false;
		// Elements marked for a visit by this binding.
		std::vector<uint64_t>                marks;
		uint64_t                             mark_stamp = 0;
		// What RebindReusedTables() last found an element unchanged with: the version of its
		// image (TextureCache::ImageVersion()) and of its resolution (TableElementChanged()).
		// While both hold and the operation does not bind the image itself, the element is still
		// unchanged: the check reads neither the resolution nor the image (GTA V's ray tracing
		// tables have ~16 000 elements, scattered in memory, and almost none change).
		struct Checked {
			static constexpr uint32_t None      = UINT32_MAX; // Check the element.
			static constexpr uint32_t NullImage = UINT32_MAX - 1;
			uint32_t                  image           = None; // ImageId index, or NullImage.
			uint32_t                  image_version   = 0;
			uint32_t                  element         = 0; // TableElementResolution::version_slot.
			uint32_t                  element_version = 0;
		};
		std::vector<Checked>                 checked;
		uint64_t                             generation = 0;
		uint64_t                             set_epoch  = 0;
		uint64_t                             backing_epoch = 0;
		bool                                 valid = false;
		// The descriptors of the table slots (`slot_infos`) in a set that is never bound
		// (DescriptorHeap::CommitPersistent()): a binding that reuses the state copies its table
		// ranges from it and writes only the slots whose descriptors changed into it.
		vk::DescriptorSet                    slot_set;
		bool                                 slot_set_current = false;
	};
	// Prepares the tables of `prepared` from the program's state when it still applies; false
	// when the tables must be resolved slot by slot.
	[[nodiscard]] bool ReuseTableBindings(const ShaderRecompiler::IR::CompiledShaderInfo& program,
	                                      const ShaderRecompiler::IR::ResourceSnapshot&   snapshot,
	                                      TableBindingState& state, uint64_t stamp,
	                                      PreparedBindings& prepared);
	// Marks the elements a change of the image set or of the guest backing since the state's
	// epochs may affect, and moves the epochs to now.
	void MarkTableChanges(TableBindingState& state);
	void MarkTableElement(TableBindingState& state, uint32_t element);
	void MarkTablePages(TableBindingState& state, uint64_t address, uint64_t size);
	void RebuildTableSpans(TableBindingState& state);
	// Adds the spans of an element that joined the state after the others (unsorted).
	static void AddTableSpans(TableBindingState& state, uint32_t element);
	// Sorts the spans AddTableSpans() added after the first `sorted` into place.
	void MergeTableSpans(TableBindingState& state, size_t sorted);
	// The slots of each element (element_slot_begin / element_slots) from `slots`.
	static void IndexTableSlots(TableBindingState& state);
	// ReuseTableBindings() of tables whose T#s changed in some slots: binds those slots to the
	// elements of their new T#s. New elements join the state and are marked; a slot that names
	// an element the state has goes to m_table_moved_slots.
	void PatchTableSlots(const ShaderRecompiler::IR::CompiledShaderInfo& program,
	                     const ShaderRecompiler::IR::ResourceSnapshot&   snapshot,
	                     TableBindingState& state, uint64_t stamp);
	// Drops the elements no slot names any more (and unpins their images); the binding's marks
	// follow the elements it keeps.
	void DropUnusedTableElements(TableBindingState& state);
	// After CommitBindings() bound the tables: records the descriptors and pins the images.
	void FinishTableBindings(TableBindingState& state, const PreparedBindings& prepared);
	void UnpinTableBindings(TableBindingState& state);
	// RebindImages() of reused tables: visits the elements PrepareBindings() marked and those
	// whose image is no longer bound as the state records.
	void RebindReusedTables(PreparedBindings& prepared);

	// ResolveTexture's image descriptions by T# and shader view (see DeriveTextureDesc).
	struct TextureDescKey {
		std::array<uint32_t, 8> dwords {};
		std::array<uint32_t, 5> view {};
		bool                    operator==(const TextureDescKey&) const = default;
	};
	struct TextureDescKeyHash {
		size_t operator()(const TextureDescKey& key) const noexcept;
	};
	[[nodiscard]] static TextureDescKey
	MakeTextureDescKey(const ShaderRecompiler::IR::ImageResource&   resource,
	                   const ShaderRecompiler::IR::DescriptorValue& value);
	// Writes the image and view the last lookup of a description found into `binding`, when
	// they still hold (see TextureDescDerivation::Found).
	[[nodiscard]] bool ReuseTextureLookup(TextureDescDerivation& derived, TextureBinding& binding);
	static constexpr size_t MaxTextureDescs = size_t {1} << 16u;
	std::unordered_map<TextureDescKey, TextureDescDerivation, TextureDescKeyHash> m_texture_descs;
	// Bindings point into m_texture_descs until their views are acquired: a full map is cleared
	// once the bindings of the operation are reset.
	bool m_clear_texture_descs = false;
	// Advances when m_texture_descs is cleared (bindings keep pointers into it).
	uint64_t m_texture_descs_generation = 1;

	// Draw templates (see DrawTemplate): the resource key of the executing draw's resolution
	// (0 without one), the templates, and the template steps of ExecutePreparedDraw().
	uint64_t                           m_draw_resource_key = 0;
	std::unique_ptr<DrawTemplateTable> m_draw_templates;
	// The read-only storage buffer views the stages of the draw being bound obtained, while it binds
	// them (see RebindBuffers()): the stages of a draw often bind the same ranges, and nothing runs
	// between their lookups that changes what one finds.
	struct DrawBufferView {
		uint64_t                 address = 0;
		uint64_t                 size    = 0;
		uint32_t                 flags   = 0;
		uint32_t                 offset  = 0;
		vk::DescriptorBufferInfo view;
	};
	std::array<DrawBufferView, 16> m_draw_buffer_views {};
	uint32_t                       m_draw_buffer_view_count = 0;
	bool                           m_draw_buffer_views_on   = false;
	// KYTY_DRAW_BUFFERS=1: the small read-only ranges of the draw being bound, copied to the stream
	// buffer before its stages bind them (see StreamDrawReads()), with their stream offsets
	// (UINT64_MAX: the range binds as before).
	struct DrawStreamRead {
		uint64_t address = 0;
		uint64_t size    = 0;
		uint64_t offset  = UINT64_MAX;
	};
	std::array<DrawStreamRead, 32> m_draw_stream_reads {};
	uint32_t                       m_draw_stream_read_count = 0;
	void                           StreamDrawReads(std::span<PreparedBindings* const> stages);
	// Resolves the programs of a draw into `draw`; false when the draw resolves them itself.
	bool ResolveDrawPrograms(ResolvedDraw& draw, const HW::Context& context, const HW::UserConfig& user_config,
	                         const HW::Shader& shaders, const ShaderIndirectOffsets& indirect_offsets,
	                         bool indirect_draw, uint64_t context_id, uint64_t user_config_id);
	// The next resolution of the ring, once the execution thread released it.
	ResolvedDraw& NextResolvedDraw();
	// Whether a template may stand for the image and sampler bindings of these stages.
	[[nodiscard]] static bool DrawTemplateEligible(std::span<const ShaderStageRuntime* const> stages);
	// The template for `key` when its stages run the same programs with the same T#s and S#s.
	[[nodiscard]] DrawTemplate* FindDrawTemplate(uint64_t key,
	                                             std::span<const ShaderStageRuntime* const> stages);
	// Whether a draw failed to store the template for `key` in this tick.
	[[nodiscard]] bool DrawTemplateRefused(uint64_t key) const;
	// PrepareBindings() of a stage whose image and sampler bindings come from a template.
	void PrepareBindingsFromTemplate(const ShaderStageRuntime& runtime,
	                                 const DrawTemplateStage& stage, PreparedBindings& prepared);
	// Whether the images a stage took from its template still hold; those that do not are looked
	// up again (and are then acquired by RebindImages()).
	[[nodiscard]] bool CheckTemplateImages(PreparedBindings& prepared);
	// Records the committed image and sampler bindings of the stages as the template for `key`.
	void StoreDrawTemplate(uint64_t key, std::span<PreparedBindings* const> stages);
	// Whether the template's stages run the programs of `stages` with the same T#s and S#s.
	[[nodiscard]] static bool DrawTemplateStagesMatch(const DrawTemplate&                        found,
	                                                  std::span<const ShaderStageRuntime* const> stages);
	// State templates (DrawStateTemplatesEnabled()): whether a draw of `found`, whose stages bind
	// the template's images, acquires its targets as the template recorded (see
	// DrawTemplate::targets_valid), leaving them as that acquisition would; and recording them.
	[[nodiscard]] bool TakeTemplateTargets(const DrawTemplate& found);
	void               StoreTemplateTargets(DrawTemplate& stored, const RenderState& rendering,
	                                        vk::ImageAspectFlags feedback_aspects);
	// Template commits (see DrawTemplate::commit_valid): CommitBindings() captures the descriptor
	// writes of m_commit_capture's draw (the binding kind and stage of each), and draws of a
	// template with a commit push its data with their own buffer views.
	DrawTemplate*                                              m_commit_capture    = nullptr;
	bool                                                       m_commit_capture_ok = false;
	std::vector<std::pair<uint8_t, uint8_t>>                   m_commit_capture_kinds;
	std::unordered_map<uint64_t, vk::DescriptorUpdateTemplate> m_commit_templates;
	std::vector<uint8_t>                                       m_commit_data;
	[[nodiscard]] static bool TemplateCommitsSupported();
	void StoreTemplateCommit(DrawTemplate& stored, const PipelineCache::Pipeline& pipeline);
	void CommitTemplateBindings(CommandBuffer& buffer, const PipelineCache::Pipeline& pipeline,
	                            std::span<PreparedBindings* const> stages, const DrawTemplate& found);
	// The image set epoch of the last registration or unregistration of an image over the range,
	// or a later one (as TextureCache::ImageEpochInRegion(), without its lock): the templates
	// keep their own copy of the epochs, by page, brought up to date from the texture cache's
	// history of changes once per draw that finds the set changed.
	[[nodiscard]] uint64_t TemplateRangeEpoch(uint64_t address, uint64_t size);
	static constexpr uint32_t TemplatePageBuckets = 1u << 16u;
	std::vector<uint64_t>     m_template_page_epochs;
	uint64_t                  m_template_pages_epoch = 0;
	uint64_t                  m_template_pages_floor = 0;

	// The host sampler for a sampler binding; see NativeSampler().
	[[nodiscard]] vk::Sampler NativeSampler(const ShaderRecompiler::IR::CompiledShaderInfo& program,
	                                        uint32_t                                        index,
	                                        const ShaderRecompiler::IR::DescriptorValue&    value);
	struct SamplerMemoEntry {
		std::array<uint32_t, 4> fields {};
		bool                    integer_border = false;
		vk::Sampler             sampler        = nullptr;
	};
	std::array<SamplerMemoEntry, 256> m_sampler_memo {};

	[[nodiscard]] TextureBinding
	ResolveTableTexture(const ShaderRecompiler::IR::ImageResource&   root,
	                    const ShaderRecompiler::IR::DescriptorValue& value,
	                    GuestRange*                                  examined = nullptr);
	void PrepareGraphicsBindings(std::span<PreparedBindings* const> stages,
	                             std::span<RenderColorInfo> colors);
	void ResolveRenderColorTarget(CommandBuffer& buffer, RenderColorInfo& target,
	                              uint32_t render_target_slot, bool ignore_target_mask = false,
	                              bool exact_format = false);
	void ResolveRenderDepthTarget(CommandBuffer& buffer, RenderDepthInfo& target);
	[[nodiscard]] bool DepthStencilCopy(CommandBuffer& buffer);
	[[nodiscard]] bool CopyDepthToColor(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                    const DrawEmitInfo& emit, const DrawRenderState& state);
	struct DrawRenderStorage;
	struct ColorTargetMemo;
	struct DepthTargetMemo;
	struct RenderTargetMemo;
	struct ResolvedDrawRing;
	// See RenderTargetMemo.
	void               CaptureRenderTargetKey(const HW::Context& registers, uint32_t mrt_mask);
	[[nodiscard]] bool RenderTargetMemoCurrent();
	[[nodiscard]] bool TryReuseRenderTargets(const HW::Context& registers, uint32_t mrt_mask);
	void StoreRenderTargetMemo(const RenderColorInfo* colors, uint32_t color_count,
	                           const RenderDepthInfo& depth, const vk::ImageView* views,
	                           vk::ImageView depth_view, ImageId stencil);
	// The draw state of the executing draw: the one its resolution prepared when that is still
	// valid (shaders_resolved), otherwise the reused state.
	[[nodiscard]] DrawRenderState& AcquireDrawRenderState(bool  tessellation,
	                                                      bool& shaders_resolved,
	                                                      const ShaderIndirectOffsets& indirect_offsets,
	                                                      bool indirect_draw);
	[[nodiscard]] bool PrepareDrawRenderState(CommandBuffer& buffer, bool shaders_resolved,
	                                          const DrawCallInfo& draw, DrawRenderState& state);
	void               ExecutePreparedDraw(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                       const DrawRenderState& state, const DrawEmitInfo& emit,
	                                       const DrawIndexBufferSource& index_source);
	[[nodiscard]] RenderState AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
	                                               uint32_t color_count, RenderDepthInfo& depth,
	                                               vk::ImageAspectFlags& feedback_aspects,
	                                               std::span<PreparedBindings* const> stages = {});
	[[nodiscard]] bool        ResolveColorTargets(CommandBuffer& buffer);
	[[nodiscard]] bool        ConsumeMetadataColorOperation(CommandBuffer& buffer);
	void                      BindImage(ImageId id, bool storage);
	void                      BindRenderTarget(ImageId id);
	void                      ResetBindings();
	[[nodiscard]] bool        TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
	                                                     const CommandBuffer&          buffer);
	[[nodiscard]] bool TryConsumeComputeFill(const ShaderComputeInputInfo& input,
	                                        CommandBuffer& command, uint32_t group_x,
	                                        uint32_t group_y, uint32_t group_z, uint32_t mode);
	void CreatePssrPipeline();
	struct UpscalerFeature;
	// The feature the upscaler evaluates the request with: the context's, or a new one (and `reset`
	// set) when that one does not take the request; null when the upscaler can't take it.
	[[nodiscard]] UpscalerFeature* UpscalerFeatureFor(const Pssr::Request& request,
	                                                  const Dlss::Inputs& inputs, bool& reset);
	// Where the command buffer is recorded: creates the feature the first time and evaluates it.
	// False when the upscaler refused the feature or the evaluation failed.
	[[nodiscard]] static bool EvaluateUpscaler(vk::CommandBuffer command, UpscalerFeature& feature,
	                                           const Pssr::Request& request, Dlss::Inputs& inputs,
	                                           bool reset);
	static void ReleaseUpscalerFeature(UpscalerFeature* feature);

	// Upscaler history of a PSSR context.
	struct UpscalerContext {
		uint64_t         context;
		Dlss::Creation   creation;
		UpscalerFeature* feature;
	};

	RenderContext&                        m_context;
	GraphicsBindings                     m_graphics_bindings;
	// The current draw's targets; PrepareDrawRenderState resolves them again for each draw.
	DrawTargets                           m_targets;
	// The targets of the executing draw: m_targets, or the remembered ones of the render target
	// memo, used in place, when the draw reuses the targets of the draw before.
	DrawTargets*                          m_draw_targets = &m_targets;
	PreparedBindings                     m_compute_bindings;
	std::vector<ImageId>                  m_bound_images;
	std::vector<vk::DescriptorImageInfo>  m_descriptor_images;
	std::vector<vk::WriteDescriptorSet>   m_descriptor_writes;
	// Table ranges copied from the slot sets of the table states (see CommitBindings()), and the
	// slots written into those sets first.
	std::vector<vk::CopyDescriptorSet>    m_descriptor_copies;
	std::vector<vk::WriteDescriptorSet>   m_slot_set_writes;
	vk::DescriptorSetLayout               m_pssr_descriptor_layout = nullptr;
	vk::PipelineLayout                    m_pssr_pipeline_layout   = nullptr;
	vk::Pipeline                          m_pssr_pipeline          = nullptr;
	std::vector<UpscalerContext>          m_upscaler_contexts;
	// Prepared bindings and slot memos point into the elements: a deque never relocates its
	// entries (std::unordered_map has no noexcept move, so a growing vector copied the elements
	// and left those pointers dangling).
	std::deque<TableResolution>           m_table_resolutions;
	// Elements a table resolution dropped while prepared bindings may still point into them;
	// freed by ResetBindings().
	std::deque<TableElements>             m_retired_table_elements;
	uint64_t                              m_table_resolution_tick = 0;
	uint64_t                              m_table_prepare_stamp   = 0;
	std::vector<vk::DescriptorImageInfo>  m_table_image_infos;
	// Advances whenever a table resolution drops its elements: table binding states pointing
	// into them no longer apply.
	uint64_t                              m_table_generation = 0;
	std::unordered_map<const void*, TableBindingState> m_table_states;
	std::vector<std::pair<uint64_t, uint64_t>>         m_table_changes;
	std::vector<uint32_t>                              m_table_marked;
	// MergeTableSpans(): the merged spans (swapped with the state's).
	std::vector<TableBindingState::PageSpan>           m_table_spans;
	// DropUnusedTableElements(): the new index of every element (UINT32_MAX: dropped).
	std::vector<uint32_t>                              m_table_remap;
	// ReuseTableBindings(): the slots whose T# changed (slot, range, slot in its range).
	struct ChangedTableSlot {
		uint32_t slot  = 0;
		uint32_t range = 0;
		uint32_t index = 0;
	};
	std::vector<ChangedTableSlot>                      m_table_changed_slots;
	// Per element of a table state: whether RebindReusedTables() checks it.
	std::vector<uint8_t>                               m_table_element_flags;
	// PatchTableSlots(): the changed slots that name an element the state already had.
	std::vector<uint32_t>                              m_table_moved_slots;
	// RebindReusedTables(): the elements whose versions moved, and whether their binding changed
	// (one byte per element checked).
	std::vector<uint32_t>                              m_table_checks;
	std::vector<uint8_t>                               m_table_flags;
	// A counter per table element resolution (TableElementResolution::version_slot); freed
	// counters are reused.
	std::vector<uint32_t>                              m_table_element_versions;
	std::vector<uint32_t>                              m_free_table_element_versions;
	// RebindReusedTables(): the images the operation binds so far (m_bound_images), by ImageId
	// index: their binding state is the operation's.
	std::vector<uint64_t>                              m_bound_image_marks;
	uint64_t                                           m_bound_image_stamp = 0;
	// See BeginGuestOperation().
	uint32_t                                           m_shader_write_queue      = 0;
	bool                                               m_shader_write_deferred   = false;
	uint32_t                                           m_guest_queue             = 0;
	bool                                               m_deferred_barrier        = false;
	uint32_t                                           m_deferred_barrier_queue  = 0;
	bool                                               m_dispatch_chained        = false;
	// Reused by every draw; see AcquireDrawRenderState().
	std::unique_ptr<DrawRenderStorage, void (*)(DrawRenderStorage*)> m_draw_state {nullptr,
	                                                                               nullptr};
	// Resolutions ahead of execution; see ResolveDrawAhead().
	std::unique_ptr<ResolvedDrawRing, void (*)(ResolvedDrawRing*)> m_resolved_ring {nullptr,
	                                                                                nullptr};
	ResolvedDraw*                                                  m_resolved_draw = nullptr;
	uint64_t                                                       m_context_id    = 0;
	uint64_t                                                       m_user_config_id = 0;
	StateKeyCache                                                  m_state_key_cache;
	// The context snapshot whose registers hw_check() last accepted.
	uint64_t m_checked_context_id = 0;
	// The last color-target descriptions per slot; see ResolveRenderColorTarget().
	std::unique_ptr<ColorTargetMemo, void (*)(ColorTargetMemo*)> m_color_target_memo {nullptr,
	                                                                                  nullptr};
	// The last depth-target descriptions; see ResolveRenderDepthTarget().
	std::unique_ptr<DepthTargetMemo, void (*)(DepthTargetMemo*)> m_depth_target_memo {nullptr,
	                                                                                  nullptr};
	// The render targets of the last draw; see RenderTargetMemo.
	std::unique_ptr<RenderTargetMemo, void (*)(RenderTargetMemo*)> m_render_target_memo {nullptr,
	                                                                                     nullptr};
	// Whether the executing draw reused the render targets of the draw before.
	bool     m_render_targets_reused = false;
	// The last AcquireRenderTargets() took the targets of the render target memo.
	bool     m_targets_from_memo     = false;
	uint64_t m_draw_window           = 0;
	std::unique_ptr<IndirectThreadPass, void (*)(IndirectThreadPass*)> m_indirect_thread_pass {
	    nullptr, nullptr};
	std::unique_ptr<NativeXprCache, void (*)(NativeXprCache*)> m_native_xpr {nullptr, nullptr};
	// Table draws (tableXpr.inc, chenxiao07's GPU-driven XPR draws; KYTY_TABLE_XPR).
	struct TableXpr;
	struct TablePair;
	struct TableVariant;
	struct TableImageSet;
	struct TableFlatStage;
	// Deferred: the pair's table variant is on its way (TableTry): the normal path draws, storing no native record.
	enum class TableResult { Drawn, Native, Store, Deferred };
	std::unique_ptr<TableXpr, void (*)(TableXpr*)> m_table_xpr {nullptr, nullptr};
	// The draw's try asked the normal path to store its table variant (TableStore).
	bool m_table_store = false;
	const void* m_table_ticket = nullptr;
	TableXpr&   OwnTable();
	// (The speculation's catalog of chenxiao07's tree is not ported: this executor's table is the only one.)
	[[nodiscard]] static constexpr uint32_t Role() noexcept { return 0; }
	static constexpr uint32_t               Roles = 1;
	[[nodiscard]] RenderExecutor&           Catalog() noexcept { return *this; }
	[[nodiscard]] TableXpr*                 CatalogTable() noexcept { return m_table_xpr.get(); }
	struct CatalogLock {
		CatalogLock() noexcept {}
		~CatalogLock() {}
	};
	[[nodiscard]] static CatalogLock CatalogWrite() noexcept { return {}; }
	[[nodiscard]] static CatalogLock CatalogRead() noexcept { return {}; }
	void TableMemoRetired(); // KYTY_TABLE_MEMO: a set left service (memo_epoch moves)
	void DropStaleCatalog() {}
	void ForgetStaleCatalog() {}
	void TableForgetStates();
	void TableSweepPairs();
	const DrawIndexArgs* m_native_xpr_direct = nullptr; // the direct draw a try is for
public:
	// A direct indexed draw (DRAW_INDEX_2 / DRAW_INDEX_OFFSET_2): its indices, count and instances; or a direct draw
	// of vertices (not `indexed`: the count is of vertices; table draws only). `indirect` (not `indexed`): a draw of
	// vertices whose arguments the GPU reads (his KYTY_SPEC_DRAW_INDIRECT; not used here).
	struct NativeXprDirectDraw {
		uint64_t index_address  = 0;
		uint32_t index_count    = 0;
		uint32_t instance_count = 1;
		int32_t  base_vertex    = 0;
		bool     indexed        = true;
		bool     indirect       = false;
	};
private:
	TableResult TableTry(CommandBuffer& buffer, std::span<const uint64_t> commands, uint64_t index_base,
	                     uint64_t index_bytes, vk::IndexType index_type, const NativeXprDirectDraw* direct);
	bool        TableContinue(CommandBuffer& buffer, std::span<const uint64_t> commands, uint64_t index_base,
	                          uint64_t index_bytes, vk::IndexType index_type, const NativeXprDirectDraw* direct);
	TableResult TableDraw(CommandBuffer& buffer, TablePair& pair, TableVariant& variant,
	                      const NativeXprDrawState& draw_state, std::span<const uint64_t> commands,
	                      uint64_t index_base, uint64_t index_bytes, vk::IndexType index_type,
	                      const NativeXprDirectDraw* direct);
	void        TableReset(TablePair& pair);
	void        TableRuns(TablePair& pair);
	void        TablePack(TablePair& pair, const std::array<TableFlatStage, 2>& flat_forms);
	bool        TableEvaluate(TablePair& pair, std::array<std::span<const uint32_t>, 2> user_data);
	void        TableGather(const TablePair& pair, std::array<std::span<const uint32_t>, 2> user_data);
	TableImageSet* TableSet(CommandBuffer& buffer, TablePair& pair, const TableVariant* variant,
	                        const NativeXprDrawState* draw_state);
	TableImageSet* TableResolveSet(CommandBuffer& buffer, TablePair& pair, const TableVariant* variant,
	                               const NativeXprDrawState* draw_state, std::span<const uint32_t> words);
	bool        TableValidateSet(TableImageSet& set);
	bool        TableTransit(const TableImageSet& set, CommandRecorder vk_buffer, const NativeXprDrawState* draw_state);
	void        TablePrepareBda(const TablePair& pair, std::array<std::span<const uint32_t>, 2> user_data);
	bool        TableBlocks(TablePair& pair, std::array<std::span<const uint32_t>, 2> user_data,
	                        std::array<vk::DeviceAddress, 2>& blocks, bool* writes);
	// After a normal draw a table try asked to store (`storable`: one a table variant can draw): the pair's table
	// programs (compiled by a worker) and this draw state's pipeline, and the draw state.
	void TableStore(CommandBuffer& buffer, const DrawRenderState& state, vk::PrimitiveTopology topology,
	                bool primitive_restart_enable, const RenderState& rendering, const DynamicState& dynamic_state,
	                bool storable);
	// Table dispatches (KYTY_TABLE_DISPATCH): the normal path's plain dispatch teaches the shader pair its table
	// program (TableDispatchLearn); later dispatches of the pair go through the table (TableDispatch).
	void TableDispatchLearn(const HW::ComputeShaderInfo& cs, const ShaderModule& module, bool async_compute);
	bool TableDispatch(CommandBuffer& buffer, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z, uint32_t mode,
	                   bool async_compute);
	// The shader access a table image set adds to target `id` (KYTY_TABLE_TARGET_READ; none: empty).
	[[nodiscard]] static vk::AccessFlags2 TableTargetRead(const TableImageSet& set, ImageId id);
	// The try of the executing draw asked the normal path to store a record.
	bool m_native_xpr_store = false;
	// Per hashed object key: its last native XPR try drew it from its record (NativeXprLikely()).
	std::array<std::atomic<uint64_t>, 65536> m_native_xpr_drawn {};
	// Per hashed shader pair (the stages' code addresses): its last table try drew it (TableLikely()), and the frame
	// until which its draws are resolved ahead all the same (a try of the pair was refused lately: some of its draws
	// take the normal path, which is several times slower without the resolution ahead).
	std::array<std::atomic<uint64_t>, 16384> m_table_drawn {};
	std::atomic<bool>                        m_table_drew_draws {false};
	// The compute stages whose last plain dispatch went through the table (TableDispatchHint).
	std::array<std::atomic<uint64_t>, 4096> m_table_dispatched {};
	std::array<std::atomic<uint64_t>, 16384> m_table_mixed_until {};
	// Per hashed shader pair and start instance: its table try was refused for good (TableTry: the pair refused) under
	// that shader map generation. Its draws take the normal path without the try (TableXprTry).
	std::array<uint64_t, 4096> m_table_refused {};
	std::array<uint64_t, 4096> m_table_refused_generation {};
	bool                       m_table_try_refused = false;

	friend class CommandProcessor;
	friend struct RenderExecutorTestAccess;
};

[[nodiscard]] bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                                            uint32_t group_y, uint32_t group_z, uint32_t mode,
                                            ShaderBufferResource& descriptor,
                                            uint32_t& packed_clear, uint64_t& size);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_ */
