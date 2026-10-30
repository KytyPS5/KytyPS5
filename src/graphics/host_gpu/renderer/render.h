#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/profiler.h"
#include "graphics/host_gpu/pipelineUse.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/renderer/threadDimensionDispatch.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
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
struct DrawRenderState;
struct FrontPipeline;
struct PendingWork;
// Defined with FrontPipeline (renderDraw.cpp).
struct FrontPipelineDeleter {
	void operator()(FrontPipeline* pipeline) const;
};
class RenderContext;
class CommandScheduler;
struct RenderExecutorTestAccess;

enum class CommandBufferDebugOp : uint32_t {
	DispatchDirect,
	DrawIndex,
	DrawIndexAuto,
	DispatchIndirect = 9,
	Unknown,
};

enum class DrawOffsetSource : uint8_t {
	DrawState,
	IndirectArgs,
};

// Nonzero address: the GPU reads count Vulkan indirect draw records, stride bytes apart.
struct DrawIndirectArgs {
	uint64_t              address = 0;
	uint32_t              count   = 0;
	uint32_t              stride  = 0;
	ShaderIndirectOffsets offsets;
};

struct DrawIndexArgs {
	uint32_t         index_count                = 0;
	const void*      index_addr                 = nullptr;
	uint32_t         instance_count             = 0;
	uint32_t         index_type_and_size        = 0;
	int32_t          base_vertex                = 0;
	uint32_t         first_instance             = 0;
	DrawOffsetSource offset_source              = DrawOffsetSource::DrawState;
	uint32_t         render_target_slice_offset = 0;
	// With indirect records, index_count is INDEX_BUFFER_SIZE.
	DrawIndirectArgs indirect;
};

struct DrawAutoArgs {
	uint32_t         vertex_count               = 0;
	uint32_t         instance_count             = 0;
	uint32_t         first_vertex               = 0;
	uint32_t         first_instance             = 0;
	DrawOffsetSource offset_source              = DrawOffsetSource::DrawState;
	uint32_t         render_target_slice_offset = 0;
	DrawIndirectArgs indirect;
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

	[[nodiscard]] bool IsInvalid() const;
	// Any command was recorded since Begin (Handle was taken).
	[[nodiscard]] bool Used() const noexcept { return m_used; }

	void SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0 = 0, uint32_t arg1 = 0,
	                  uint32_t arg2 = 0, uint32_t arg3 = 0, uint64_t arg4 = 0);
	void BeginRendering(const RenderState& state) const;
	void EndRendering() const;

	// Ends an open compute chain first: whatever is recorded waits for the chain's dispatches.
	[[nodiscard]] vk::CommandBuffer Handle() const;
	// For commands that only set state (binds, push constants): an open compute chain stays
	// open.
	[[nodiscard]] vk::CommandBuffer StateHandle() const;
	// Dispatches recorded one after another, with no guest synchronization between them, run
	// together as on the guest GPU. The barrier after a dispatch is left open until anything
	// else is recorded (Handle) or the guest synchronizes (EndComputeChain).
	void ContinueComputeChain() const noexcept { m_compute_chain = true; }
	[[nodiscard]] bool InComputeChain() const noexcept { return m_compute_chain; }
	void EndComputeChain() const;
	[[nodiscard]] GraphicContext&   GetGraphics() const noexcept { return m_graphics; }
	[[nodiscard]] RenderContext&    GetContext() const noexcept { return m_context; }
	[[nodiscard]] HW::Context&      GetRegisters() const noexcept { return *m_registers; }
	[[nodiscard]] HW::UserConfig&   GetUserConfig() const noexcept { return *m_user_config; }
	[[nodiscard]] HW::Shader&       GetShaders() const noexcept { return *m_shaders; }
	// The registers commands are recorded with. A draw recorded after later packets changed them
	// binds a copy of its own for its recording (RenderExecutor::DrawIndex).
	void Bind(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders) noexcept {
		m_registers   = &registers;
		m_user_config = &user_config;
		m_shaders     = &shaders;
	}

private:
	explicit CommandBuffer(CommandScheduler& scheduler);

	void Begin();
	void End() const;

	RenderContext&      m_context;
	GraphicContext&     m_graphics;
	vk::CommandBuffer   m_buffer          = nullptr;
	uint32_t            m_debug_op        = 0;
	uint64_t            m_debug_submit_id = 0;
	uint32_t            m_debug_arg0      = 0;
	uint32_t            m_debug_arg1      = 0;
	uint32_t            m_debug_arg2      = 0;
	uint32_t            m_debug_arg3      = 0;
	uint64_t            m_debug_arg4      = 0;
	mutable RenderState m_render_state;
	mutable bool        m_rendering   = false;
	mutable bool        m_used        = false;
	mutable bool        m_compute_chain = false;
	HW::Context*        m_registers   = nullptr;
	HW::UserConfig*     m_user_config = nullptr;
	HW::Shader*         m_shaders     = nullptr;

	friend class CommandScheduler;
};

namespace Pssr {
struct Request;
}

class RenderExecutor {
public:
	explicit RenderExecutor(RenderContext& context);
	~RenderExecutor();
	KYTY_CLASS_NO_COPY(RenderExecutor);

	void DispatchDirect(uint64_t submit_id, CommandBuffer& buffer, uint32_t thread_group_x,
	                    uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode, bool async_compute);
	// A frame of the game ends: what the next frame draws into is told apart from what it does
	// not (see PlanPipelineUse).
	void NoteFlip();
	// Records the draw or dispatch DrawIndex or DispatchDirect left pending (its program lookups
	// run on the front worker while the work before it is recorded). Before anything else is
	// recorded or guest memory written: the command processor calls it for every packet but
	// register writes, draws and dispatches.
	void FlushPendingWork();
	// Draws or dispatches wait to be recorded (FlushPendingWork records them).
	[[nodiscard]] bool HasPendingWork() const;
	// Queues `action` behind the pending work (HasPendingWork): it runs when they are recorded.
	void QueueDeferred(std::function<void()> action);
	// The newest pending work is a dispatch: a compute chain may be open after it.
	[[nodiscard]] bool LastPendingIsDispatch() const;
	void DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer, uint64_t args_addr,
	                      uint32_t mode, bool async_compute);
	void ProcessPssr(CommandBuffer& buffer, const Pssr::Request& request);
	// Frees the PSSR pipeline (the destructor).
	void DestroyPssr();
	[[nodiscard]] TextureBinding ResolveTexture(const ShaderRecompiler::IR::ImageResource& resource,
	                                            const ShaderRecompiler::IR::DescriptorValue& value);

	void PrepareBindings(const ShaderStageRuntime& runtime, PreparedBindings& prepared);
	void                           FindBuffers(std::span<PreparedBindings* const> stages);
	void                           RebindBuffers(PreparedBindings& bindings);
	void                           RebindImages(PreparedBindings& bindings);
	void CommitBindings(CommandBuffer& buffer, vk::PipelineBindPoint pipeline_bind_point,
	                    const PipelineCache::Pipeline&     pipeline,
	                    std::span<PreparedBindings* const> bindings);

private:
	// False only for a draw with indirect_args_address that the host GPU cannot read the
	// arguments of (see HostReadsIndirectArgs): nothing was drawn, and the caller reads them.
	bool DrawIndex(uint64_t submit_id, CommandBuffer& buffer, const DrawIndexArgs& args);
	bool DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args);

	struct GraphicsBindings {
		std::array<PreparedBindings, 3> vertex;
		std::optional<PreparedBindings> pixel;
	};

	// Remembered in m_decoded_textures.
	[[nodiscard]] DecodedTexture DecodeTexture(const ShaderRecompiler::IR::ImageResource& resource,
	                                           const ShaderRecompiler::IR::DescriptorValue& value) const;
	void PrepareGraphicsBindings(std::span<PreparedBindings* const> stages,
	                             std::span<RenderColorInfo> colors);
	void ResolveRenderColorTarget(CommandBuffer& buffer, RenderColorInfo& target,
	                              uint32_t render_target_slice_offset, uint32_t render_target_slot,
	                              bool ignore_target_mask = false, bool exact_format = false);
	void ResolveRenderDepthTarget(CommandBuffer& buffer, RenderDepthInfo& target);
	[[nodiscard]] bool DepthStencilCopy(CommandBuffer& buffer);
	[[nodiscard]] bool CopyDepthToColor(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                   const DrawEmitInfo& emit, DrawRenderState& state);
	[[nodiscard]] bool PrepareDrawRenderState(CommandBuffer& buffer,
	                                          const DrawCallInfo& draw,
	                                          uint32_t            render_target_slice_offset,
	                                          DrawRenderState& state);
	// The part of PrepareDrawRenderState after the program lookups.
	[[nodiscard]] bool PrepareDrawTargets(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                      uint32_t render_target_slice_offset,
	                                      DrawRenderState& state);
	// DrawIndex from its prepared state on: targets, bindings, the recording.
	void FinishDrawIndex(uint64_t submit_id, CommandBuffer& buffer, const DrawIndexArgs& args,
	                     const DrawCallInfo& draw, vk::PrimitiveTopology topology,
	                     const DrawIndexBufferSource& index_source, bool primitive_restart,
	                     DrawRenderState& state, bool prepare_targets);
	// DrawAuto from its prepared state on (targets included): the recording.
	void FinishDrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args,
	                    const DrawCallInfo& draw, vk::PrimitiveTopology topology,
	                    DrawRenderState& state);
	// KYTY_FRONT_WORKER=0 keeps the program lookups on the GPU thread.
	[[nodiscard]] static bool FrontWorkerEnabled();
	// DrawIndex of an ordinary draw with a front worker: see FlushPendingWork.
	void PipelineDrawIndex(uint64_t submit_id, CommandBuffer& buffer, const DrawIndexArgs& args,
	                       const DrawCallInfo& draw, vk::PrimitiveTopology topology,
	                       const DrawIndexBufferSource& index_source, bool primitive_restart);
	// DrawAuto of an ordinary draw with a front worker.
	void PipelineDrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args,
	                      const DrawCallInfo& draw, vk::PrimitiveTopology topology);
	// DispatchDirect with a front worker, from its input before the program lookup.
	void PipelineDispatchDirect(uint64_t submit_id, CommandBuffer& buffer,
	                            const std::array<uint32_t, 3>& groups, uint32_t mode,
	                            const ShaderComputeInputInfo& input_info);
	// DispatchDirect after its program lookup.
	void FinishDispatchDirect(uint64_t submit_id, CommandBuffer& buffer,
	                          std::array<uint32_t, 3> groups, uint32_t mode,
	                          ShaderComputeInputInfo& input_info,
	                          const ShaderModule&  compute_program);
	// The free slot for the next pending work, with a copy of the buffer's registers.
	PendingWork& NextPendingWork(CommandBuffer& buffer, uint64_t submit_id);
	// Queues the lookups of `next` on the front worker, and records the oldest pending work
	// while more than FrontDepth wait.
	void QueuePendingWork(CommandBuffer& buffer, PendingWork& next);
	void TrimPendingWork(CommandBuffer& buffer);
	// Settles and records the oldest pending work.
	void RecordOldestPendingWork(CommandBuffer& buffer);
	// The front worker's job for the pending work is done and still holds, or redone here.
	void SettlePendingLookups(PendingWork& pending);
	void RecordPendingWork(PendingWork& pending, CommandBuffer& buffer);
	void RecordPendingDrawIndex(PendingWork& pending, CommandBuffer& buffer);
	void RecordPendingDrawAuto(PendingWork& pending, CommandBuffer& buffer);
	void ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer, const DrawCallInfo& draw,
	                         DrawRenderState& state, const DrawEmitInfo& emit,
	                         const DrawIndexBufferSource& index_source);
	void ApplyDepthBoundsByShader(CommandBuffer& buffer, DrawRenderState& state,
	                              const PipelineCache::Pipeline& pipeline);
	[[nodiscard]] RenderState AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
	                                               uint32_t color_count, RenderDepthInfo& depth,
	                                               vk::ImageAspectFlags& feedback_aspects,
	                                               std::span<PreparedBindings* const> stages = {});
	[[nodiscard]] bool        ResolveColorTargets(CommandBuffer& buffer,
	                                              uint32_t render_target_slice_offset);
	[[nodiscard]] bool        ConsumeMetadataColorOperation(CommandBuffer& buffer,
	                                                       uint32_t render_target_slice_offset);
	void                      BindImage(ImageId id, bool storage);
	void                      BindRenderTarget(ImageId id);
	void                      ResetBindings();
	[[nodiscard]] bool        TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
	                                                     const CommandBuffer&          buffer);
	[[nodiscard]] bool TryConsumeComputeFill(const ShaderComputeInputInfo& input,
	                                        CommandBuffer& command, uint32_t group_x,
	                                        uint32_t group_y, uint32_t group_z, uint32_t mode);

	RenderContext&                        m_context;
	std::unique_ptr<ThreadDimensionDispatch> m_thread_dimensions; // created on first use
	GraphicsBindings                     m_graphics_bindings;
	PreparedBindings                     m_compute_bindings;
	// Created on the first pipelined draw or dispatch (see FrontWorkerEnabled).
	std::unique_ptr<FrontPipeline, FrontPipelineDeleter> m_front_pipeline;
	// The resources the program lookups of the current draw and dispatch materialized.
	ShaderRecompiler::IR::ResourceSnapshot m_dispatch_resources;
	// The last color target each slot resolved (colorRenderTarget.cpp).
	struct ColorTargetMemos;
	std::unique_ptr<ColorTargetMemos>    m_color_target_memos;
	// The open compute chain has waited for the work recorded before it (see DispatchDirect).
	bool                                 m_compute_chain_ordered = false;
	std::vector<ImageId>                  m_bound_images;
	// The frame in which each render target was last drawn into. A target is an image slot with
	// its generation (so a slot given to a new image does not inherit the frame of the old one)
	// and the mip level and array layer (a face of a cube map, a layer of an atlas or a cascade of
	// a shadow map drawn once is not covered by draws into the others).
	// One target of a draw: an image (slot and generation), a mip and a layer. Exact, no hash.
	struct TargetId {
		uint64_t image = 0;
		uint32_t mip   = 0;
		uint32_t layer = 0;
		bool     operator==(const TargetId&) const = default;
	};
	struct TargetIdHash {
		size_t operator()(const TargetId& id) const noexcept {
			return std::hash<uint64_t> {}(id.image ^ ((uint64_t {id.mip} << 40) | (uint64_t {id.layer} << 8)));
		}
	};
	// TODO(upstream merge stage 2): slots have no generation since the merge, so a slot given to a
	// new image inherits the frame of the old one; Image::serial would tell them apart.
	static uint64_t ImageIdentity(ImageId image) {
		return uint64_t {image.index} << 32;
	}
	static TargetId TargetKey(ImageId image, uint32_t mip = 0, uint32_t layer = 0) {
		return {ImageIdentity(image), mip, layer};
	}
	std::unordered_map<TargetId, TargetHistory, TargetIdHash> m_target_frames;
	// Buffers a draw uses on the GPU alone, within one submission (a depth snapshot, the records
	// of an emulated mesh draw). Kept and used again once the GPU has passed the submission that
	// used them: creating and destroying one per draw cost 0.45 ms each on MoltenVK.
	struct ScratchBuffer {
		std::unique_ptr<Buffer> buffer;
		uint64_t                tick = 0;
	};
	std::vector<ScratchBuffer> m_scratch;
	uint64_t                   m_scratch_bytes = 0;
	// The depth snapshot the last depth-bounds draw read (see depthSnapshotPlan.h). Held out
	// of the scratch pool while it may be read again; returned to it when replaced.
	struct DepthSnapshot {
		std::unique_ptr<Buffer> buffer;
		uint64_t                tick       = 0;
		uint64_t                image      = 0;
		uint32_t                layer      = 0;
		uint64_t                generation = 0;
		bool                    load_clear = false;
		float                   clear      = 0.0f;
	};
	DepthSnapshot              m_depth_snapshot;
	Buffer&                    AcquireScratch(uint64_t bytes);
	static TargetId TargetKey(const RenderColorInfo& color);
	static TargetId TargetKey(const RenderDepthInfo& depth);
	uint64_t                               m_frame = 0;
	std::vector<vk::DescriptorImageInfo>  m_descriptor_images;
	std::vector<vk::WriteDescriptorSet>   m_descriptor_writes;
	// CommitBindings: the (address, size) ranges a draw or dispatch writes.
	std::vector<std::pair<uint64_t, uint64_t>> m_written_image_ranges;
	std::vector<std::pair<uint64_t, uint64_t>> m_written_buffer_ranges;
	// Dropped whole at the end of a frame when full (bindings point into it): a frame of Crash
	// Bandicoot 4 decodes about 1,500 different ones, and one image heap of Marvel's Spider-Man 2
	// holds about 9,000 (a limit below that decoded the whole heap again in every frame).
	static constexpr size_t                MaxDecodedTextures = 16384;
	std::unordered_map<TextureDecodeKey, DecodedTexture, TextureDecodeKeyHash, TextureDecodeKeyEqual>
	    m_decoded_textures;
	// The entry each shader image slot found last, by the slot's address: a draw that binds the
	// same descriptor to the slot again compares instead of hashing. The resource is kept to be
	// compared too, since a freed program's address can be reused. Void when the table is cleared.
	struct DecodedSlot {
		const ShaderRecompiler::IR::ImageResource* slot = nullptr;
		ShaderRecompiler::IR::ImageResource        resource;
		ShaderRecompiler::IR::DescriptorValue      value;
		const DecodedTexture*                      texture = nullptr;
		uint64_t                                   epoch   = 0;
	};
	static constexpr uint32_t              DecodedSlotBits = 15;
	// On the heap: inline, its 5 MB made every RenderContext on a stack overflow an 8 MB stack.
	std::vector<DecodedSlot> m_decoded_slots = std::vector<DecodedSlot>(size_t {1} << DecodedSlotBits);
	uint64_t                      m_decoded_epoch = 1;
	vk::DescriptorSetLayout               m_pssr_descriptor_layout = nullptr;
	vk::PipelineLayout                    m_pssr_pipeline_layout   = nullptr;
	vk::Pipeline                          m_pssr_pipeline          = nullptr;

	friend class CommandProcessor;
	friend struct RenderExecutorTestAccess;
};

[[nodiscard]] bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                                            uint32_t group_y, uint32_t group_z, uint32_t mode,
                                            ShaderBufferResource& descriptor,
                                            uint32_t& packed_clear, uint64_t& size);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_ */
