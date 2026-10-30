#include "common/assert.h"
#include "common/common.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/perfCounters.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/gpuBreadcrumbs.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {
static bool FillSourcesDisjoint(std::span<const ShaderRecompiler::IR::DescriptorValue> sources,
                                 GuestRange destination, uint32_t output_buffer = UINT32_MAX) {
	for (uint32_t i = 0; i < sources.size(); ++i) {
		if (i == output_buffer) continue;
		const auto source = DecodeNativeDescriptor<ShaderBufferResource>(sources[i]);
		const auto bytes  = source.GetSize();
		if (source.Base48() < destination.End() && destination.address < source.Base48() + bytes)
			return false;
	}
	return true;
}

bool RenderExecutor::TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
                                                const CommandBuffer&          buffer) {
	const auto& program   = input.stage.module->info;
	const auto& resources = *input.stage.resources;
	if (resources.buffers.size() != program.info.buffers.size()) {
		EXIT("compute runtime buffer count does not match shader metadata\n");
	}
	auto& cache = buffer.GetContext().GetTextureCache();
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& resource   = program.info.buffers[i];
		const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		// A metadata resource that is also read is not proven to be a full overwrite. Execute it
		// conservatively instead of replacing the dispatch with a coarse full-surface clear.
		if ((!resource.written || resource.read) && cache.IsMeta(descriptor.Base48())) {
			return false;
		}
	}

	if (!program.info.has_bitwise_xor) {
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& resource = program.info.buffers[i];
			if (resource.written) {
				const auto descriptor =
				    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
				if (cache.ClearMeta(descriptor.Base48())) {
					return true;
				}
			}
		}
	}
	return false;
}

bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                              uint32_t group_y, uint32_t group_z, uint32_t mode,
                              ShaderBufferResource& resolved_descriptor, uint32_t& resolved_clear,
                              uint64_t& resolved_size) {
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	if (fill.kind != ShaderRecompiler::IR::UniformFillKind::Buffer) {
		return false;
	}
	const auto element_size = fill.words * sizeof(uint32_t);
	const auto descriptor =
	    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[fill.resource]);
	constexpr std::array formats {
	    Prospero::BufferFormat::k32UInt, Prospero::BufferFormat::k32_32UInt,
	    Prospero::BufferFormat::k32_32_32UInt, Prospero::BufferFormat::k32_32_32_32UInt};
	// Offset-or-swizzle bounds do not describe the element range proved by the shader.
	if (descriptor.Stride() != element_size || descriptor.Format() != formats[fill.words - 1] ||
	    descriptor.SwizzleEnabled() || descriptor.IndexStride() != 0 || descriptor.AddTid() ||
	    descriptor.OutOfBounds() == 3 || descriptor.Base48() == 0) {
		return false;
	}
	if (input.threads_num[0] == 0 || input.threads_num[0] != fill.group_stride[0] ||
	    input.threads_num[1] != 1 || input.threads_num[2] != 1 || group_x == 0 || group_y != 1 ||
	    group_z != 1 || mode != (input.dispatch_thread_dimensions ? 0x61u : 0x41u)) {
		return false;
	}
	const uint64_t invocations = input.dispatch_thread_dimensions
	                                 ? group_x
	                                 : static_cast<uint64_t>(group_x) * input.threads_num[0];
	const auto     size        = descriptor.GetSize();
	const auto elements = descriptor.NumRecords();
	const bool covered = fill.bounded_elements != 0
	                         ? fill.bounded_elements == elements && invocations >= elements
	                         : invocations == elements;
	if (!covered || invocations > uint64_t {UINT32_MAX} + 1 || size == 0 || size > UINT32_MAX ||
	    (input.dispatch_thread_dimensions &&
	     (group_x % input.threads_num[0] != 0 || input.dispatch_threads_num[0] != group_x ||
	      input.dispatch_threads_num[1] != 1 || input.dispatch_threads_num[2] != 1))) {
		return false;
	}
	if (!FillSourcesDisjoint(resources.buffers, {descriptor.Base48(), size}, fill.resource))
		return false;
	resolved_descriptor = descriptor;
	resolved_clear      = fill.value;
	resolved_size       = size;
	return true;
}

bool RenderExecutor::TryConsumeComputeFill(const ShaderComputeInputInfo& input,
                                          CommandBuffer& command, uint32_t group_x,
                                          uint32_t group_y, uint32_t group_z, uint32_t mode) {
	const auto& program   = input.stage.module->info;
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	auto&       cache     = command.GetContext().GetTextureCache();
	if (fill.kind == ShaderRecompiler::IR::UniformFillKind::Image) {
		if (mode != 0x41u || input.dispatch_thread_dimensions || fill.value > 255 ||
		    input.threads_num[2] != 1)
			return false;
		const auto  descriptor = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[0]);
		const auto& resource   = program.info.images[0];
		if (descriptor.IsNull() || descriptor.Format() != Prospero::BufferFormat::k8UInt ||
		    descriptor.Type() != Prospero::ImageType::kColor2DArray || descriptor.MetaCompress() ||
		    descriptor.WriteCompress() || descriptor.BaseLevel() > descriptor.LastLevel() ||
		    descriptor.BaseLevel() > descriptor.MaxMip() ||
		    descriptor.BaseArray5() > descriptor.Depth() || descriptor.DstSelX() != 4)
			return false;
		const std::array extents {
		    std::max(1u, (descriptor.Width5() + 1u) >> descriptor.BaseLevel()),
		    std::max(1u, (descriptor.Height5() + 1u) >> descriptor.BaseLevel()),
		    descriptor.Depth() - descriptor.BaseArray5() + 1u};
		const std::array groups {group_x, group_y, group_z};
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const uint64_t threads = input.threads_num[axis];
			// Guest image writes outside the descriptor dimensions are discarded. Only the
			// final workgroup may extend beyond the selected image view.
			if (threads == 0 || threads != fill.group_stride[axis] ||
			    groups[axis] != (extents[axis] + threads - 1) / threads ||
			    groups[axis] * threads > UINT32_MAX) return false;
		}
		// The plane's depth owner, through the stencil association over the plane's memory. The
		// storage binding itself resolves to a colour image of its own (a D32S8 image cannot be a
		// storage image), and a clear of that image never reaches the stencil the draws test:
		// Crash Bandicoot 4 clears its stencil this way each frame, and its shadows blinked.
		const auto decoded = DecodeTexture(resource, resources.images[0]);
		if (decoded.null_descriptor) return false;
		const auto& destination = decoded.desc.info.data;
		if (!FillSourcesDisjoint(resources.buffers, destination)) return false;
		std::scoped_lock lock {cache.m_lock};
		ImageId          depth_id {};
		for (const auto id: cache.FindImagesInRegion(destination.address, destination.size, false)) {
			const auto* plane = cache.m_slot_images.try_get(id);
			if (plane != nullptr && plane->depth_id && plane->info.data == destination) {
				depth_id = plane->depth_id;
				break;
			}
		}
		if (!depth_id) return false;
		const auto& image = cache.GetImage(depth_id);
		const auto& view  = decoded.desc.view_info;
		if (image.backing.format != vk::Format::eD32SfloatS8Uint || image.info.samples != 1 ||
		    image.info.stencil != destination || view.base_level >= image.backing.mip_levels ||
		    view.base_layer >= image.backing.layers || view.layer_count != extents[2] ||
		    view.layer_count > image.backing.layers - view.base_layer ||
		    std::max(1u, image.info.extent.width >> view.base_level) != extents[0] ||
		    std::max(1u, image.info.extent.height >> view.base_level) != extents[1]) return false;
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eStencil, view.base_level,
		                                       1, view.base_layer, view.layer_count};
		vk::ClearValue clear {};
		clear.depthStencil = vk::ClearDepthStencilValue {0.0f, fill.value};
		cache.ClearImage(command, depth_id, image.backing.format, range, clear);
		return true;
	}
	ShaderBufferResource descriptor;
	uint32_t             packed_clear = 0;
	uint64_t             size         = 0;
	if (!ResolveComputeBufferFill(input, group_x, group_y, group_z, mode, descriptor, packed_clear,
	                              size)) {
		return false;
	}
	// A fill without an image of exactly its memory runs as the dispatch, in GPU order: shaders
	// read guest memory itself, so a CPU fill now would reach GPU work queued before it. Its
	// value is recorded instead (RecordColorMetadataFill), which spares the read-back.
	if (!cache.ClearImageFromBuffer(command, descriptor.Base48(), size, packed_clear)) {
		return false;
	}
	static std::atomic<uint32_t> logged_clears {0};
	if (logged_clears.fetch_add(1, std::memory_order_relaxed) < 32) {
		LOGF("GraphicsRenderDispatchDirect: compute image clear shader=0x%016" PRIx64
		     " addr=0x%016" PRIx64 " size=0x%016" PRIx64 " value=0x%08" PRIx32 "\n",
		     input.stage.module->info.shader_hash, descriptor.Base48(), size, packed_clear);
	}
	return true;
}


namespace {

// The compute command a breadcrumb names: its shader, size and buffers.
std::string DescribeDispatch(const char* kind, uint64_t submit_id, const ShaderRecompiler::IR::CompiledShaderInfo& program,
                             const ShaderRecompiler::IR::ResourceSnapshot& resources, uint32_t x, uint32_t y, uint32_t z) {
	auto text = fmt::format("{} cs={:016x} submit={} groups={}x{}x{} buffers:", kind, program.shader_hash, submit_id, x, y, z);
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto r = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		text += fmt::format(" {}{:x}+{:x}", program.info.buffers[i].written ? "W" : "r", r.Base48(),
		                    uint64_t {r.NumRecords()} * std::max(1u, static_cast<uint32_t>(r.Stride())));
	}
	return text + fmt::format(" images={} dma={}", program.info.images.size(), program.info.uses_dma ? 1 : 0);
}

} // namespace

static void BindSharedMemory(RenderContext& context, ShaderComputeInputInfo& input,
                             PreparedBindings& bindings, uint64_t indirect_args = 0) {
	if (input.stage.module->info.bindings.descriptor_counts[
	        static_cast<size_t>(ShaderRecompiler::IR::DescriptorBindingKind::SharedMemory)] == 0) {
		return;
	}
	auto& cache = context.GetBufferCache();
	if (indirect_args != 0) {
		cache.ReadMemory(indirect_args, sizeof(vk::DispatchIndirectCommand));
		std::memcpy(input.workgroup_counts, reinterpret_cast<const void*>(indirect_args),
		            sizeof(input.workgroup_counts));
	}
	// LDS has no contents to preserve between dispatches. The existing shader hazard
	// barriers also order other users of this GPU-only utility buffer.
	auto& storage = cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
	const auto limit = std::min<uint64_t>(storage.Size(),
	    context.GetGraphics().GetPhysicalDeviceProperties().limits.maxStorageBufferRange);
	uint64_t size = sizeof(uint32_t);
	if (std::ranges::find(input.workgroup_counts, 0u) == std::end(input.workgroup_counts)) {
		size = uint64_t {input.lds_size_dwords} * sizeof(uint32_t);
		EXIT_IF(size == 0 || size > limit);
		for (const auto count: input.workgroup_counts) {
			EXIT_IF(size > limit / count);
			size *= count;
		}
	}
	bindings.shared_memory = {storage.Handle(), 0, size};
}

void RenderExecutor::DispatchDirect(uint64_t submit_id, CommandBuffer& buffer,
                                    uint32_t thread_group_x, uint32_t thread_group_y,
                                    uint32_t thread_group_z, uint32_t mode, bool async_compute) {
	PerfCounters::Add(PerfCounters::Counter::Dispatches);
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid());
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchDirect), submit_id,
	                    thread_group_x, thread_group_y, thread_group_z, mode,
	                    sh_ctx.GetCs().cs_regs.data_addr);

	if (thread_group_x == 0 || thread_group_y == 0 || thread_group_z == 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: skipping zero-sized dispatch groups=%ux%ux%u "
			     "mode=0x%08" PRIx32 " shader=0x%016" PRIx64 "\n",
			     thread_group_x, thread_group_y, thread_group_z, mode,
			     sh_ctx.GetCs().cs_regs.data_addr);
		}
		return;
	}

	Common::LockGuard lock(m_context.GetMutex());
	if (sh_ctx.GetCs().cs_regs.data_addr == 0) {
		LOGF("GraphicsRenderDispatchDirect: temporary: ignoring dispatch with null CS shader, "
		     "groups=%ux%ux%u mode=%u\n",
		     thread_group_x, thread_group_y, thread_group_z, mode);
		return;
	}

	constexpr uint32_t DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS = 1u << 5u;
	constexpr uint32_t DISPATCH_INITIATOR_BASE_BITS             = 0x41u;
	constexpr uint32_t DISPATCH_INITIATOR_MODIFIER_BITS         = 0xa038u;
	constexpr uint32_t DISPATCH_INITIATOR_KNOWN_MASK =
	    DISPATCH_INITIATOR_BASE_BITS | DISPATCH_INITIATOR_MODIFIER_BITS;

	const uint32_t unknown_mode_bits = mode & ~DISPATCH_INITIATOR_KNOWN_MASK;
	if (unknown_mode_bits != 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: unknown dispatch initiator bits "
			     "mode=0x%08" PRIx32 " unknown=0x%08" PRIx32 " shader=0x%016" PRIx64
			     " groups=%ux%ux%u\n",
			     mode, unknown_mode_bits, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x,
			     thread_group_y, thread_group_z);
		}
	}

	const auto& cs_regs = sh_ctx.GetCs();

	ShaderComputeInputInfo input_info {};
	input_info.async_compute = async_compute;
	const bool use_thread_dimensions = (mode & DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	input_info.dispatch_thread_dimensions = use_thread_dimensions;
	input_info.workgroup_counts[0] = thread_group_x;
	input_info.workgroup_counts[1] = thread_group_y;
	input_info.workgroup_counts[2] = thread_group_z;
	if (use_thread_dimensions) {
		const uint32_t group_sizes[] = {cs_regs.cs_regs.num_thread_x, cs_regs.cs_regs.num_thread_y,
		                                cs_regs.cs_regs.num_thread_z};
		for (uint32_t axis = 0; axis < 3u; ++axis) {
			const auto size = std::max(group_sizes[axis], 1u);
			const auto threads = input_info.workgroup_counts[axis];
			input_info.workgroup_counts[axis] = threads / size + (threads % size != 0u);
		}
	}
	const std::array<uint32_t, 3> groups {thread_group_x, thread_group_y, thread_group_z};
	m_context.GetImageHeap().BeginDraw();
	const auto* compute_program = m_context.GetPipelineCache().GetComputeProgram(cs_regs, input_info);
	if (compute_program == nullptr) return;
	FinishDispatchDirect(submit_id, buffer, groups, mode, input_info, *compute_program);
}

void RenderExecutor::FinishDispatchDirect(uint64_t submit_id, CommandBuffer& buffer,
                                          std::array<uint32_t, 3> groups, uint32_t mode,
                                          ShaderComputeInputInfo& input_info,
                                          const ShaderModule&     compute_program) {
	auto& sh_ctx         = buffer.GetShaders();
	const auto& cs_regs  = sh_ctx.GetCs();
	auto thread_group_x  = groups[0];
	auto thread_group_y  = groups[1];
	auto thread_group_z  = groups[2];
	const bool use_thread_dimensions = input_info.dispatch_thread_dimensions;
	if (use_thread_dimensions) {
		input_info.dispatch_threads_num[0]    = thread_group_x;
		input_info.dispatch_threads_num[1]    = thread_group_y;
		input_info.dispatch_threads_num[2]    = thread_group_z;
	}

	const auto& program   = input_info.stage.module->info;
	const auto& resources = *input_info.stage.resources;
	if (resources.specialization_reads.empty() &&
	    (TryConsumeComputeMetaClear(input_info, buffer) ||
	     TryConsumeComputeFill(input_info, buffer, thread_group_x, thread_group_y,
	                           thread_group_z, mode))) {
		ResetBindings();
		return;
	}

	// The counts as the guest gave them: threads in thread dimensions, as the fill check reads them.
	const uint32_t guest_x = thread_group_x;
	const uint32_t guest_y = thread_group_y;
	const uint32_t guest_z = thread_group_z;
	if (use_thread_dimensions) {
		const uint32_t old_x = thread_group_x;
		const uint32_t old_y = thread_group_y;
		const uint32_t old_z = thread_group_z;
		thread_group_x       = input_info.workgroup_counts[0];
		thread_group_y       = input_info.workgroup_counts[1];
		thread_group_z       = input_info.workgroup_counts[2];

		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: use-thread-dimensions %ux%ux%u / %ux%ux%u -> "
			     "groups %ux%ux%u\n",
			     old_x, old_y, old_z, std::max(cs_regs.cs_regs.num_thread_x, 1u),
			     std::max(cs_regs.cs_regs.num_thread_y, 1u),
			     std::max(cs_regs.cs_regs.num_thread_z, 1u), thread_group_x, thread_group_y,
			     thread_group_z);
		}
	}

	buffer.EndRendering();
	auto& pipeline = m_context.GetPipelineCache().GetComputePipeline(input_info);
	auto& bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	if (program.bindings.dispatch_thread_dword != ShaderRecompiler::IR::PushData::NoStart) {
		std::copy(std::begin(input_info.dispatch_threads_num), std::end(input_info.dispatch_threads_num),
		          bindings.shader_data.begin() + program.bindings.dispatch_thread_dword);
	}
	PreparedBindings* descriptor_stage = &bindings;
	FindBuffers(std::span {&descriptor_stage, 1u});
	if (program.info.uses_dma) {
		m_context.PrepareBda(program.has_address_writes);
	}
	if (program.info.uses_image_heap) {
		m_context.GetImageHeap().Rebind(*this);
	}
	RebindImages(bindings);
	BindSharedMemory(m_context, input_info, bindings);
	RebindBuffers(bindings);

	// The dispatch joins the chain of the dispatches before it when nothing was recorded or
	// synchronized since (see CommandBuffer::ContinueComputeChain). Emulated LDS and GDS are one
	// buffer that dispatches cannot share at the same time.
	if (bindings.shared_memory.buffer != nullptr || program.info.uses_gds) {
		buffer.EndComputeChain();
	}
	const bool chained   = buffer.InComputeChain();
	auto       vk_buffer = buffer.StateHandle();
	if (!chained) {
		m_compute_chain_ordered = false;
	}
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	bool has_storage_writes = bindings.shared_memory.buffer != nullptr ||
	    HasShaderBufferWrites(input_info.stage);
	has_storage_writes =
	    std::any_of(program.info.images.begin(), program.info.images.end(),
	                [](const auto& image) {
		                return image.written &&
		                       image.resource_class ==
		                           ShaderRecompiler::IR::ImageResourceClass::Storage;
	                }) ||
	    has_storage_writes;
	if (has_storage_writes && !m_compute_chain_ordered) {
		// Writes wait for everything recorded before the chain; once per chain, since its
		// dispatches run together anyway.
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
		m_compute_chain_ordered = true;
	}
	if (Config::GraphicsDebugDumpEnabled()) {
		const auto sampled_images = std::count_if(
		    program.info.images.begin(), program.info.images.end(), [](const auto& image) {
			    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
		    });
		const uint32_t frame_num = static_cast<uint32_t>(m_context.GetGpu().GetFrameNum());
		LOGF("GraphicsRenderDispatchDirect: frame=%u shader=0x%016" PRIx64
		     " hash=0x%016" PRIx64 " tick=%" PRIu64 " completed_tick=%" PRIu64
		     " groups=%ux%ux%u mode=0x%08" PRIx32 " local=%ux%ux%u "
		     "buffers=%zu textures=%zu sampled=%zu storage=%zu samplers=%zu push=%u\n",
		     frame_num, sh_ctx.GetCs().cs_regs.data_addr, program.shader_hash,
		     m_context.GetCommandScheduler().CurrentTick(),
		     m_context.GetCommandScheduler().GetMasterSemaphore().KnownGpuTick(),
		     thread_group_x, thread_group_y, thread_group_z, mode,
		     input_info.threads_num[0], input_info.threads_num[1],
		     input_info.threads_num[2], program.info.buffers.size(), program.info.images.size(),
		     sampled_images, program.info.images.size() - sampled_images,
		     program.info.samplers.size(),
		     program.bindings.UsesPushData()
		         ? static_cast<uint32_t>(sizeof(ShaderRecompiler::IR::PushData))
		         : 0u);
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& buffer = program.info.buffers[i];
			const auto  r      = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
			LOGF("  CS buffer[%u]: source=%u usage=%s addr=0x%012" PRIx64
			     " stride=%u records=%u format=%u\n",
			     i, buffer.source, buffer.written ? "read-write" : "read-only", r.Base48(),
			     r.Stride(), r.NumRecords(), r.RawFormat());
		}
		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			const auto& image = program.info.images[i];
			const auto  r     = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
			LOGF("  CS texture[%u]: source=%u usage=%s sampled=%s addr=0x%010" PRIx64
			     " type=%u fmt=%u extent=%ux%u depth=%u levels=%u tile=%u\n",
			     i, image.source, image.written ? "read-write" : "read-only",
			     image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled
			         ? "true"
			         : "false",
			     r.Base40(), static_cast<uint32_t>(r.Type()), static_cast<uint32_t>(r.Format()),
			     static_cast<uint32_t>(r.Width5()) + 1u, static_cast<uint32_t>(r.Height5()) + 1u,
			     static_cast<uint32_t>(r.Depth()) + 1u,
			     r.Type() == Prospero::ImageType::kColor2DMsaa ||
			             r.Type() == Prospero::ImageType::kColor2DMsaaArray
			         ? 1u
			         : static_cast<uint32_t>(image.r128 ? r.LastLevel() : r.MaxMip()) + 1u,
			     static_cast<uint32_t>(r.TileMode()));
		}
		for (uint32_t i = 0; i < program.info.samplers.size(); i++) {
			const auto r = DecodeNativeDescriptor<ShaderSamplerResource>(
			    resources.samplers[program.info.samplers[i].snapshot_index]);
			LOGF("  CS sampler[%u]: source=%u clamp=%u/%u/%u filter=%u/%u/%u mip=%u "
			     "lod=%u-%u bias=%d\n",
			     i, program.info.samplers[i].source, static_cast<uint32_t>(r.ClampX()),
			     static_cast<uint32_t>(r.ClampY()), static_cast<uint32_t>(r.ClampZ()),
			     static_cast<uint32_t>(r.XyMagFilter()), static_cast<uint32_t>(r.XyMinFilter()),
			     static_cast<uint32_t>(r.ZFilter()), static_cast<uint32_t>(r.MipFilter()),
			     static_cast<uint32_t>(r.MinLod()), static_cast<uint32_t>(r.MaxLod()),
			     static_cast<int32_t>(r.LodBias()));
		}
	}

	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	InsertDebugLabel(vk_buffer, "Dispatch {}x{}x{} submit={}", thread_group_x, thread_group_y,
	                 thread_group_z, submit_id);
	if (CommandScheduler::GpuProfileEnabled()) m_context.GetCommandScheduler().GpuProfileBegin();
	const auto crumb = GpuBreadcrumbs::Enabled(m_context.GetGraphics())
	                       ? GpuBreadcrumbs::Begin(m_context.GetGraphics(), vk_buffer,
	                                               DescribeDispatch("dispatch", submit_id, program, resources,
	                                                                thread_group_x, thread_group_y, thread_group_z))
	                       : 0u;
	constexpr uint32_t DISPATCH_INITIATOR_ORDERED_APPEND_ENABLE = 1u << 3u;
	const uint64_t     group_threads = uint64_t {std::max(input_info.threads_num[0], 1u)} *
	                                   std::max(input_info.threads_num[1], 1u) *
	                                   std::max(input_info.threads_num[2], 1u);
	if ((mode & DISPATCH_INITIATOR_ORDERED_APPEND_ENABLE) != 0 && program.info.uses_gds &&
	    group_threads <= input_info.wave_size && !input_info.lds_storage) {
		// A single guest wave per group can run in launch order without a device-wide
		// spinlock. Keep the guest workgroup IDs when splitting the dispatch.
		for (uint32_t z = 0; z < thread_group_z; ++z) {
			for (uint32_t y = 0; y < thread_group_y; ++y) {
				for (uint32_t x = 0; x < thread_group_x; ++x) {
					if ((x | y | z) != 0u) {
						ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
					}
					vk_buffer.dispatchBase(x, y, z, 1, 1, 1);
				}
			}
		}
	} else {
		vk_buffer.dispatch(thread_group_x, thread_group_y, thread_group_z);
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::Dispatches);
	if (crumb != 0) GpuBreadcrumbs::End(vk_buffer, crumb);
	if (CommandScheduler::GpuProfileEnabled()) {
		m_context.GetCommandScheduler().GpuProfileEnd((compute_program.id << 32u) | 0xffffffffu);
	}

	// Whatever is recorded next waits for the dispatch, unless it is a dispatch of the chain.
	buffer.ContinueComputeChain();
	ResetBindings();

	// A uniform fill of a colour metadata block is the guest's fast clear. Record the code so the
	// first draw on that target needs no GPU read-back to find it.
	ShaderBufferResource fill_descriptor;
	uint32_t             fill_value = 0;
	uint64_t             fill_size  = 0;
	if (resources.uniform_fill.kind == ShaderRecompiler::IR::UniformFillKind::Buffer &&
	    ResolveComputeBufferFill(input_info, guest_x, guest_y, guest_z, mode, fill_descriptor,
	                             fill_value, fill_size)) {
		m_context.GetTextureCache().RecordColorMetadataFill(fill_descriptor.Base48(), fill_size,
		                                                    fill_value);
	}
	m_context.GetCommandScheduler().NoteWork();
}

void RenderExecutor::DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer,
                                      uint64_t args_addr, uint32_t mode, bool async_compute) {
	PerfCounters::Add(PerfCounters::Counter::Dispatches);
	EXIT_IF(buffer.IsInvalid() || args_addr == 0 || (args_addr & 3u) != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchIndirect), submit_id,
	                    static_cast<uint32_t>(args_addr), static_cast<uint32_t>(args_addr >> 32u),
	                    0, mode, buffer.GetShaders().GetCs().cs_regs.data_addr);
	Common::LockGuard lock(m_context.GetMutex());
	vk::DispatchIndirectCommand args {};
	if (!m_context.GetBufferCache().HasGpuDirtyBytes(args_addr, sizeof(args)) &&
	    LibKernel::Memory::TryReadBacking(args_addr, &args, sizeof(args)) &&
	    (args.x == 0 || args.y == 0 || args.z == 0)) {
		return;
	}
	const auto& cs_regs = buffer.GetShaders().GetCs();
	if (cs_regs.cs_regs.data_addr == 0) {
		return;
	}
	ShaderComputeInputInfo input_info {};
	input_info.async_compute = async_compute;
	// The thread counts are known on the GPU only, and so is the extent of the shader's last
	// groups: its shader data goes in a buffer that the conversion writes.
	const bool use_thread_dimensions = (mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	input_info.dispatch_thread_dimensions = use_thread_dimensions;
	// In thread dimensions the arguments are thread counts, not the group counts the shader's
	// workgroup IDs can be specialized from, and the GPU may still write them.
	if (!use_thread_dimensions) {
		input_info.indirect_args_address = args_addr;
	}
	m_context.GetImageHeap().BeginDraw();
	const auto* compute_program = m_context.GetPipelineCache().GetComputeProgram(
	    cs_regs, input_info, use_thread_dimensions ? ShaderRecompiler::IR::PushData::NoStart : 0u);
	if (compute_program == nullptr) return;
	buffer.EndRendering();
	auto& pipeline = m_context.GetPipelineCache().GetComputePipeline(input_info);
	auto& bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	PreparedBindings* descriptor_stage = &bindings;
	FindBuffers(std::span {&descriptor_stage, 1u});
	const auto& program = input_info.stage.module->info;
	if (program.info.uses_dma) {
		m_context.PrepareBda(program.has_address_writes);
	}
	BindSharedMemory(m_context, input_info, bindings, args_addr);
	if (program.info.uses_image_heap) {
		m_context.GetImageHeap().Rebind(*this);
	}
	RebindImages(bindings);
	// Acquiring arguments can merge cache buffers; finalize shader bindings afterward.
	const auto [args_buffer, args_offset] = m_context.GetBufferCache().ObtainBuffer(
	    args_addr, sizeof(vk::DispatchIndirectCommand), false);
	EXIT_IF(args_buffer == nullptr || (args_offset & 3u) != 0);
	RebindBuffers(bindings);
	const auto vk_buffer = buffer.Handle();
	vk::Buffer indirect_buffer = args_buffer->Handle();
	uint64_t   indirect_offset = args_offset;
	if (!use_thread_dimensions) {
		// The host GPU reads the group counts when the dispatch runs; they can be other data then
		// (FC 27: billions of groups hung the GPU and the desktop). A dispatch of more groups
		// than any game asks for is dropped, on the GPU.
		constexpr float MaxIndirectGroups = 16777216.0f;
		auto&           stream = m_context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
		const auto      align  = m_context.GetGraphics().StorageMinAlignment();
		const std::array<uint32_t, 3> zero {};
		const ThreadDimensionDispatch::Target safe {
		    stream.Handle(), stream.Copy(zero.data(), sizeof(zero), align), sizeof(zero)};
		const ThreadDimensionDispatch::Target spare {
		    stream.Handle(), stream.Copy(zero.data(), sizeof(zero), align), sizeof(zero)};
		if (m_thread_dimensions == nullptr) {
			m_thread_dimensions = std::make_unique<ThreadDimensionDispatch>(m_context.GetGraphics());
		}
		m_thread_dimensions->RecordLimited(vk_buffer, indirect_buffer, indirect_offset, safe, spare,
		                                   MaxIndirectGroups);
		indirect_buffer = safe.buffer;
		indirect_offset = safe.offset;
	}
	if (use_thread_dimensions) {
		const auto& layout = program.bindings;
		EXIT_IF(layout.UsesPushData() ||
		        (layout.dispatch_thread_dword != ShaderRecompiler::IR::PushData::NoStart &&
		         bindings.shader_data_buffer.buffer == nullptr));
		auto&      stream = m_context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
		const auto align  = m_context.GetGraphics().StorageMinAlignment();
		const std::array<uint32_t, 3> zero {};
		const ThreadDimensionDispatch::Target groups {
		    stream.Handle(), stream.Copy(zero.data(), sizeof(zero), align), sizeof(zero)};
		// A shader that leaves the extent unread still gets a place to write it.
		const bool has_extent = layout.dispatch_thread_dword != ShaderRecompiler::IR::PushData::NoStart;
		const ThreadDimensionDispatch::Target extent =
		    has_extent ? ThreadDimensionDispatch::Target {bindings.shader_data_buffer.buffer,
		                                                  bindings.shader_data_buffer.offset,
		                                                  bindings.shader_data_buffer.range}
		               : groups;
		if (m_thread_dimensions == nullptr) {
			m_thread_dimensions = std::make_unique<ThreadDimensionDispatch>(m_context.GetGraphics());
		}
		const auto helper_crumb = GpuBreadcrumbs::Enabled(m_context.GetGraphics())
		                              ? GpuBreadcrumbs::Begin(m_context.GetGraphics(), vk_buffer,
		                                                      fmt::format("thread-dimension args={:x}", args_addr),
		                                                      args_addr, indirect_buffer, indirect_offset)
		                              : 0u;
		m_thread_dimensions->Record(vk_buffer, indirect_buffer, indirect_offset,
		                            {cs_regs.cs_regs.num_thread_x, cs_regs.cs_regs.num_thread_y,
		                             cs_regs.cs_regs.num_thread_z},
		                            groups, extent, has_extent ? layout.dispatch_thread_dword : 0u);
		if (helper_crumb != 0) GpuBreadcrumbs::End(vk_buffer, helper_crumb);
		indirect_buffer = groups.buffer;
		indirect_offset = groups.offset;
	}
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	const bool has_storage_writes = bindings.shared_memory.buffer != nullptr ||
	    HasShaderBufferWrites(input_info.stage) ||
	    std::any_of(program.info.images.begin(), program.info.images.end(), [](const auto& image) {
		    return image.written && image.resource_class ==
		                                ShaderRecompiler::IR::ImageResourceClass::Storage;
	    });
	if (has_storage_writes) {
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead;
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics |
	                              vk::PipelineStageFlagBits::eComputeShader |
	                              vk::PipelineStageFlagBits::eTransfer,
	                          vk::PipelineStageFlagBits::eDrawIndirect, {},
	                          1, &barrier, 0, nullptr, 0, nullptr);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	InsertDebugLabel(vk_buffer, "DispatchIndirect submit={}", submit_id);
	if (CommandScheduler::GpuProfileEnabled()) m_context.GetCommandScheduler().GpuProfileBegin();
	const auto crumb = GpuBreadcrumbs::Enabled(m_context.GetGraphics())
	                       ? GpuBreadcrumbs::Begin(m_context.GetGraphics(), vk_buffer,
	                                               DescribeDispatch("dispatch-indirect", submit_id, program,
	                                                                *input_info.stage.resources, 0, 0, 0) +
	                                                   fmt::format(" args={:x}", args_addr),
	                                               args_addr, indirect_buffer, indirect_offset)
	                       : 0u;
	vk_buffer.dispatchIndirect(indirect_buffer, indirect_offset);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::Dispatches);
	if (crumb != 0) GpuBreadcrumbs::End(vk_buffer, crumb);
	if (CommandScheduler::GpuProfileEnabled()) {
		m_context.GetCommandScheduler().GpuProfileEnd((compute_program->id << 32u) | 0xffffffffu);
	}
	ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	ResetBindings();
	m_context.GetCommandScheduler().NoteWork();
}

} // namespace Libs::Graphics
