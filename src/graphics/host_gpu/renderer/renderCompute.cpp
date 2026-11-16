#include "common/assert.h"
#include "common/common.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "gpu_tiler_shaders/indirect_thread_dispatch_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/occlusionQueries.h"
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

// Stores through V#s selected from a descriptor table write guest memory by device address.
// Cache every target of the tables so the BDA page table covers them. Returns whether any table
// names a target.
// Whether a dispatch may write memory that no bound range names: address stores and stores through
// V#s selected from descriptor tables or sized by a header (self-sized buffers). Other buffers
// reached through memory offsets are written within the ranges their V#s bind, recorded as GPU
// writes like any written binding.
static bool MayWriteThroughDeviceAddresses(const ShaderRecompiler::IR::CompiledShaderInfo& program) {
	return program.has_address_writes ||
	       std::ranges::any_of(program.info.self_sized_buffers,
	                           [](const auto& buffer) { return buffer.written; }) ||
	       (program.bindings.descriptor_counts[static_cast<size_t>(ShaderRecompiler::IR::DescriptorBindingKind::Buffers)] != 0 &&
	        std::ranges::any_of(program.info.buffers, [](const auto& buffer) {
		        return buffer.indirect_write_table;
	        }));
}

// Self-sized buffers (see ShaderInfo::SelfSizedBuffer): their memory is cached from the base before
// PrepareBda() uploads the CPU writes of cached buffers, and the pages their stores write come back
// through the write bitmap. Returns whether the program stores to one.
static bool PrepareSelfSizedBuffers(RenderContext& context, const ShaderStageRuntime& stage) {
	if (stage.module->info.info.self_sized_buffers.empty() || !context.CacheSelfSizedBuffers(stage)) {
		return false;
	}
	context.GetIndirectWriteTables().NotePendingWrites();
	return true;
}

// The host acceleration structures of a program whose native walks trace through a guest TLAS (see
// RayTracing::Prepare()), recorded before its dispatch, outside a render pass. Indirect
// dispatches trace too (Astro Bot's RT passes are dispatched with thread dimensions read from
// memory). Apart from PrepareRayQuery(), which serves the walks of the ray query pass: that one
// writes its context address into the shader data, which RebindBuffers() uploads, so it comes
// before; this one reads the TLAS header address from the flattened SRT that RebindBuffers()
// uploads (with the dwords GPU work wrote), so it comes after.
static void PrepareNativeRayTracing(RenderContext& context, const CommandRecorder& command,
                                    const HW::ComputeShaderInfo& cs_regs, const PreparedBindings& bindings) {
	auto& native_rt = *context.GetRayTracing();
	if (!native_rt.Enabled()) {
		return;
	}
	const auto& module = *bindings.runtime->module;
	const auto& info   = module.info.info;
	if (RayTracing::TlasSource source; RayTracing::FindTlasSource(info, source)) {
		const auto& user    = cs_regs.cs_user_sgpr.value;
		// Through a V# (Marvel's Wolverine), read from guest memory: the game writes its tables.
		const auto  pointer = RayTracing::UserPointer(
		    source, uint64_t {user[source.user_sgpr]} | (uint64_t {user[source.user_sgpr + 1]} << 32u),
		    [&](uint64_t address, uint64_t size) { return context.IsMapped(address, size); });
		native_rt.Prepare(command, pointer, source, bindings.flattened_srt, context.GetGpu().GetFrameNum(),
		                  module.info.shader_hash);
	} else if (info.bvh) {
		// It walks a guest BVH on its own (see the "[RT]" summaries).
		using Kind = RayTracing::GuestDispatch;
		RayTracing::NoteGuestDispatch(module.guest_traversal ? Kind::NullTlas : Kind::Unrecognized,
		                                    module.info.shader_hash);
	}
}

static bool PrepareIndirectWriteTargets(RenderContext& context, const PreparedBindings& bindings) {
	const auto& program = bindings.runtime->module->info;
	bool        prepared = false;
	for (const auto& resource: program.info.buffers) {
		// Buffer sources follow the bound resources, which leave out buffers the shader never
		// accesses, not the shader's buffer list.
		if (!resource.indirect_write_table || resource.descriptor_index == UINT32_MAX ||
		    resource.descriptor_index >= bindings.buffer_sources.size()) {
			continue;
		}
		const auto& table = bindings.buffer_sources[resource.descriptor_index];
		if (table.address == 0 || table.size <= resource.indirect_table_offset) {
			continue;
		}
		prepared |= context.GetIndirectWriteTables().Prepare(
		    table.address + resource.indirect_table_offset,
		    table.size - resource.indirect_table_offset);
	}
	return prepared;
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

	if (program.info.has_bitwise_xor) {
		return false;
	}
	// The clear replaces every store of the dispatch: each buffer it writes must be metadata it
	// covers. Guest memory can hold an image's metadata and other data in turn (SAROS in PS5 Pro
	// mode keeps the counters of a work queue where a depth target has its HTile), and a smaller
	// or other write is that data.
	bool clears = false;
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		if (!program.info.buffers[i].written) {
			continue;
		}
		const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		if (!cache.CoversMeta(descriptor.Base48(), descriptor.GetSize())) {
			return false;
		}
		clears = true;
	}
	for (uint32_t i = 0; clears && i < program.info.buffers.size(); i++) {
		if (program.info.buffers[i].written) {
			const auto descriptor =
			    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
			(void)cache.ClearMeta(descriptor.Base48());
		}
	}
	return clears;
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
	if (fill.kind == ShaderRecompiler::IR::UniformFillKind::Replicate) {
		// Its result is known here: write it in guest memory, so readers on the CPU (the SRT walks
		// of the draws that read the table) find it there instead of waiting for the GPU.
		if (!(mode == 0x41u && !input.dispatch_thread_dimensions &&
		      input.threads_num[0] == 64 && input.threads_num[1] == 1 && input.threads_num[2] == 1 &&
		      group_y == 1 && group_z == 1))
			return false;
		const auto destination =
		    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[fill.resource]);
		const auto source =
		    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[fill.source]);
		const auto plain = [](const ShaderBufferResource& buffer) {
			const auto format = buffer.Format();
			return buffer.Stride() == 4 && !buffer.SwizzleEnabled() && buffer.IndexStride() == 0 &&
			       !buffer.AddTid() && buffer.Base48() != 0 &&
			       (format == Prospero::BufferFormat::k32UInt ||
			        format == Prospero::BufferFormat::k32SInt ||
			        format == Prospero::BufferFormat::k32Float);
		};
		const uint64_t count   = fill.bounded_elements;
		const uint64_t pattern = fill.pattern_elements;
		if (!plain(destination) || !plain(source) || destination.Format() != source.Format() ||
		    destination.DstSelX() != source.DstSelX() || uint64_t {group_x} * 64u < count ||
		    count > destination.NumRecords() || pattern > source.NumRecords() ||
		    !m_context.IsMapped(destination.Base48(), count * 4u) ||
		    !m_context.IsMapped(source.Base48(), pattern * 4u) ||
		    cache.IsRegionRegistered(destination.Base48(), count * 4u))
			return false;
		auto& buffers = m_context.GetBufferCache();
		if (buffers.ReplicateBuffer(destination.Base48(), count, source.Base48(), pattern)) {
			return true;
		}
		// The source holds GPU writes (the CPU cannot copy it): when its period covers the count the program copies
		// count dwords, which a GPU copy does without the dispatch, and a snapshot of the destination taken behind it
		// serves the guest's read of the result (Demon's Souls' cs_memset32: ~60 of its ~1 KB results a frame, each a
		// fault and a readback). Overlapping ranges keep the program (its order of reads and writes).
		const uint64_t bytes = count * 4u;
		if (pattern < count || (destination.Base48() < source.Base48() + bytes &&
		                        source.Base48() < destination.Base48() + bytes))
			return false;
		buffers.CopyBuffer(destination.Base48(), source.Base48(), bytes, false, false);
		buffers.ScheduleCopyFeedback(destination.Base48(), bytes);
		return true;
	}
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
		const auto  binding     = ResolveTexture(resource, resources.images[0]);
		const auto& destination = binding.desc.info.data;
		if (!FillSourcesDisjoint(resources.buffers, destination)) return false;
		std::scoped_lock lock {cache.m_lock};
		const auto&      image = cache.GetImage(binding.image_id);
		const auto&      view  = binding.desc.view_info;
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
		cache.ClearImage(command, binding.image_id, image.backing.format, range, clear);
		return true;
	}
	ShaderBufferResource descriptor;
	uint32_t             packed_clear = 0;
	uint64_t             size         = 0;
	if (!ResolveComputeBufferFill(input, group_x, group_y, group_z, mode, descriptor, packed_clear,
	                              size)) {
		return false;
	}
	const bool image_clear =
	    cache.ClearImageFromBuffer(command, descriptor.Base48(), size, packed_clear);
	if (!image_clear) {
		// CPU fills invalidate overlapping cached images. Preserve the native dispatch for
		// partial or ambiguous image backings, including pooled metadata aliases.
		if ((descriptor.Base48() & 3u) != 0 ||
		    !m_context.IsMapped(descriptor.Base48(), size) ||
		    cache.IsRegionRegistered(descriptor.Base48(), size)) {
			return false;
		}
		// Metadata need not have an image backing yet. Keep proven fills in guest memory
		// when CPU-owned, so color-clear discovery can read them without a GPU download.
		m_context.GetBufferCache().FillBuffer(descriptor.Base48(), size, packed_clear, false);
	}
	static std::atomic<uint32_t> logged_clears {0};
	if (logged_clears.fetch_add(1, std::memory_order_relaxed) < 32) {
		LOGF("GraphicsRenderDispatchDirect: compute %s shader=0x%016" PRIx64
		     " addr=0x%016" PRIx64 " size=0x%016" PRIx64 " value=0x%08" PRIx32 "\n",
		     image_clear ? "image clear" : "buffer fill", input.stage.module->info.shader_hash,
		     descriptor.Base48(), size, packed_clear);
	}
	return true;
}

// Global loads read pages without a buffer as zero. Cache the 32 KiB after each base the shader
// takes from user data or scalar loads before the dispatch reads it.
static void CacheDmaBases(RenderContext& context, const ShaderStageRuntime& stage) {
	const auto back = [&context](uint32_t low, uint32_t high) {
		const auto base = low | uint64_t {high} << 32u;
		for (uint64_t index = 0; index <= 2; ++index) {
			const auto page = base + index * BufferCache::CACHING_PAGESIZE;
			if (context.IsMapped(page, 1)) (void)context.GetBufferCache().FindBuffer(page, 1);
		}
	};
	const auto& info = stage.module->info.info;
	const auto& flat = stage.resources->flattened_srt;
	const auto& user = stage.resources->user_data;
	for (const auto [low, high]: info.address_bases) back(flat[low], flat[high]);
	for (const auto [low, high]: info.user_address_bases) back(user[low], user[high]);
}

// Recognized traversals trace the mirror of the TLAS whose header address the shader loads from
// scalar data.
static void PrepareRayQuery(RenderContext& context, const ShaderStageRuntime& stage,
                            PreparedBindings& bindings) {
	const auto& program = stage.module->info;
	const auto  dword   = program.bindings.ray_query_dword;
	if (dword == ShaderRecompiler::IR::PushData::NoStart) return;
	const auto& flat       = stage.resources->flattened_srt;
	const auto [low, high] = program.info.ray_query_tlas;
	const auto address =
	    context.GetRayTracing()->Prepare(flat[low] | uint64_t {flat[high]} << 32u, context.GetGpu().GetFrameNum());
	bindings.shader_data[dword]     = static_cast<uint32_t>(address);
	bindings.shader_data[dword + 1] = static_cast<uint32_t>(address >> 32u);
}

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

static bool HasStorageWrites(const ShaderComputeInputInfo& input_info,
                             const PreparedBindings&       bindings, bool indirect_writes) {
	const auto& images = input_info.stage.module->info.info.images;
	return bindings.shared_memory.buffer != nullptr || HasShaderBufferWrites(input_info.stage) ||
	       indirect_writes ||
	       std::ranges::any_of(images, [](const auto& image) {
		       return image.written &&
			          image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Storage;
	       });
}

void RenderExecutor::DispatchDirect(CommandBuffer& buffer, uint32_t thread_group_x,
                                    uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode,
                                    bool async_compute) {
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& sh_ctx = buffer.GetShaders();

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

	constexpr uint32_t DISPATCH_INITIATOR_BASE_BITS     = 0x41u;
	constexpr uint32_t DISPATCH_INITIATOR_MODIFIER_BITS = 0xa038u;
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
	const bool use_thread_dimensions =
	    (mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	input_info.dispatch_thread_dimensions = use_thread_dimensions;
	input_info.workgroup_counts[0]        = thread_group_x;
	input_info.workgroup_counts[1]        = thread_group_y;
	input_info.workgroup_counts[2]        = thread_group_z;
	if (use_thread_dimensions) {
		const uint32_t group_sizes[] = {cs_regs.cs_regs.num_thread_x, cs_regs.cs_regs.num_thread_y,
		                                cs_regs.cs_regs.num_thread_z};
		for (uint32_t axis = 0; axis < 3u; ++axis) {
			const auto size = std::max(group_sizes[axis], 1u);
			const auto threads = input_info.workgroup_counts[axis];
			input_info.workgroup_counts[axis] = threads / size + (threads % size != 0u);
		}
	}
	// A dispatch of a shader pair that learned its table program (tableXpr.inc; KYTY_TABLE_DISPATCH).
	if (!use_thread_dimensions &&
	    TableDispatch(buffer, thread_group_x, thread_group_y, thread_group_z, mode, async_compute)) {
		TableNoteDispatched(cs_regs, true);
		m_resolved_draw = nullptr;
		return;
	}
	TableNoteDispatched(cs_regs, false);
	m_context.GetImageHeap().BeginDraw();
	const ShaderModule* compute_module = nullptr;
	if (!TakeResolvedDispatch(input_info, compute_module)) {
		compute_module = m_context.GetPipelineCache().GetComputeProgram(cs_regs, input_info);
	}
	if (compute_module == nullptr) return;
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

	// The shader pair of a plain dispatch learns its table program (the dispatches after it go through the table).
	if (!use_thread_dimensions && mode == 0x41u) TableDispatchLearn(cs_regs, *compute_module, async_compute);

	if (use_thread_dimensions) {
		thread_group_x = input_info.workgroup_counts[0];
		thread_group_y = input_info.workgroup_counts[1];
		thread_group_z = input_info.workgroup_counts[2];
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
	CacheDmaBases(m_context, input_info.stage);
	FindBuffers(std::span {&descriptor_stage, 1u});
	// New target buffers must exist before PrepareBda uploads the CPU writes of cached buffers.
	const bool indirect_writes = PrepareIndirectWriteTargets(m_context, bindings) |
	                             PrepareSelfSizedBuffers(m_context, input_info.stage);
	if (program.info.uses_dma) {
		m_context.CacheDmaBases(input_info.stage);
		const ShaderStageRuntime* const dma_stage = &input_info.stage;
		const bool reach_synchronized = m_context.SynchronizeDmaFootprint(std::span {&dma_stage, 1});
		m_context.PrepareBda(MayWriteThroughDeviceAddresses(program), !reach_synchronized);
		if (program.bindings.descriptor_counts[static_cast<size_t>(ShaderRecompiler::IR::DescriptorBindingKind::Buffers)] != 0) {
			// Its results are often read back soon: submit it at once (see CommitWriteTicks()).
			m_context.GetBufferCache().RequestFlushForReadbacks();
		}
	}
	PrepareRayQuery(m_context, input_info.stage, bindings);
	if (program.info.uses_image_heap) {
		m_context.GetImageHeap().Rebind(*this);
	}
	RebindImages(bindings);
	BindSharedMemory(m_context, input_info, bindings);
	RebindBuffers(bindings);
	m_context.GetOcclusionQueries().WriteForReaders();

	const auto vk_buffer = buffer.Handle();
	PrepareNativeRayTracing(m_context, vk_buffer, cs_regs, bindings);
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	bool has_storage_writes = HasStorageWrites(input_info, bindings, indirect_writes);
	// On the guest's GPU, dispatches of one queue overlap unless the guest orders them (a wait,
	// a partial flush, an ACQUIRE_MEM; the operations other than dispatches record their own
	// barriers). Without such an order since the previous direct dispatch of its queue, nothing
	// was recorded after that one and its barrier is still deferred: the barrier before the
	// earlier dispatch already ordered everything before it, so neither is needed and the two
	// overlap. GTA V's ray tracing structure builds run hundreds of such dispatches per frame.
	const bool chained = std::exchange(m_dispatch_chained, false) && m_deferred_barrier;
	if (has_storage_writes && !chained) {
		// A host fence used to serialize every dispatch. Preserve its read-before-write ordering
		// while allowing the queue to execute asynchronously.
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
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
			const auto  max_mip = image.r128 ? r.LastLevel() : r.MaxMip();
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
			         : static_cast<uint32_t>(max_mip) + 1u,
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

	// The removed host fence also ordered read-only dispatches before later writers. Recorded
	// before the next operation, unless that is a dispatch the guest did not order after this one.
	m_deferred_barrier       = true;
	m_deferred_barrier_queue = m_guest_queue;
	ResetBindings();
}

struct IndirectThreadPass {
	explicit IndirectThreadPass(GraphicContext& graphics): graphics(graphics) {
		const vk::DescriptorSetLayoutBinding bindings[] {
		    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
		    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
		    {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
		};
		vk::DescriptorSetLayoutCreateInfo layout_info {};
		layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		layout_info.bindingCount = static_cast<uint32_t>(std::size(bindings));
		layout_info.pBindings    = bindings;
		RequireVulkanSuccess(graphics.device.createDescriptorSetLayout(&layout_info, nullptr,
		                                                               &descriptor_layout),
		                     "create indirect-thread descriptor layout");
		vk::PushConstantRange push {vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
		vk::PipelineLayoutCreateInfo pipeline_layout_info {};
		pipeline_layout_info.setLayoutCount         = 1;
		pipeline_layout_info.pSetLayouts            = &descriptor_layout;
		pipeline_layout_info.pushConstantRangeCount = 1;
		pipeline_layout_info.pPushConstantRanges    = &push;
		RequireVulkanSuccess(graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
		                                                          &pipeline_layout),
		                     "create indirect-thread pipeline layout");
		const auto module = CompileSPV(INDIRECT_THREAD_DISPATCH_SPV, graphics.device);
		vk::PipelineShaderStageCreateInfo stage {};
		stage.stage  = vk::ShaderStageFlagBits::eCompute;
		stage.module = module;
		stage.pName  = "main";
		vk::ComputePipelineCreateInfo pipeline_info {};
		pipeline_info.stage  = stage;
		pipeline_info.layout = pipeline_layout;
		const auto result =
		    graphics.device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &pipeline);
		graphics.device.destroyShaderModule(module, nullptr);
		RequireVulkanSuccess(result, "create indirect-thread pipeline");
	}
	~IndirectThreadPass() {
		graphics.device.destroyPipeline(pipeline, nullptr);
		graphics.device.destroyPipelineLayout(pipeline_layout, nullptr);
		graphics.device.destroyDescriptorSetLayout(descriptor_layout, nullptr);
	}
	KYTY_CLASS_NO_COPY(IndirectThreadPass);

	struct Push {
		uint32_t local_size[3];
		uint32_t args_index;
		uint32_t groups_index;
		uint32_t limit_index;
	};

	GraphicContext&         graphics;
	vk::DescriptorSetLayout descriptor_layout = nullptr;
	vk::PipelineLayout      pipeline_layout   = nullptr;
	vk::Pipeline            pipeline          = nullptr;
};

void RenderExecutor::DispatchIndirectThreads(CommandBuffer& buffer, uint64_t args_addr,
                                             uint32_t mode, bool async_compute) {
	EXIT_IF(args_addr == 0 || (args_addr & 3u) != 0 ||
	        (mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) == 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	Common::LockGuard lock(m_context.GetMutex());
	const auto& cs_regs = buffer.GetShaders().GetCs();
	if (cs_regs.cs_regs.data_addr == 0) {
		return;
	}
	ShaderComputeInputInfo input_info {};
	m_context.GetImageHeap().BeginDraw();
	const ShaderModule*    compute_module = nullptr;
	if (!TakeResolvedDispatch(input_info, compute_module)) {
		input_info.async_compute              = async_compute;
		input_info.dispatch_thread_dimensions = true;
		input_info.dispatch_indirect_threads  = true;
		compute_module = m_context.GetPipelineCache().GetComputeProgram(cs_regs, input_info);
	}
	if (compute_module == nullptr) {
		return;
	}
	const auto& program = input_info.stage.module->info;
	EXIT_IF(program.bindings.UsesPushData());
	// Metadata clears are tracked as in the direct path; it does not depend on the size.
	if (input_info.stage.resources->specialization_reads.empty()) {
		(void)TryConsumeComputeMetaClear(input_info, buffer);
	}
	buffer.EndRendering();
	auto& pipeline = m_context.GetPipelineCache().GetComputePipeline(input_info);
	auto& bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	PreparedBindings* descriptor_stage = &bindings;
	FindBuffers(std::span {&descriptor_stage, 1u});
	// New target buffers must exist before PrepareBda uploads the CPU writes of cached buffers.
	const bool self_sized_writes = PrepareSelfSizedBuffers(m_context, input_info.stage);
	if (program.info.uses_dma) {
		m_context.CacheDmaBases(input_info.stage);
		const ShaderStageRuntime* const dma_stage = &input_info.stage;
		const bool reach_synchronized = m_context.SynchronizeDmaFootprint(std::span {&dma_stage, 1});
		m_context.PrepareBda(MayWriteThroughDeviceAddresses(program), !reach_synchronized);
		if (program.bindings.descriptor_counts[static_cast<size_t>(ShaderRecompiler::IR::DescriptorBindingKind::Buffers)] != 0) {
			// Its results are often read back soon: submit it at once (see CommitWriteTicks()).
			m_context.GetBufferCache().RequestFlushForReadbacks();
		}
	}
	PrepareRayQuery(m_context, input_info.stage, bindings);
	// The heap resolves the elements this dispatch acquired before its descriptors are written.
	if (program.info.uses_image_heap) {
		m_context.GetImageHeap().Rebind(*this);
	}
	RebindImages(bindings);
	(void)PrepareIndirectWriteTargets(m_context, bindings);
	constexpr uint64_t ArgsSize = 3 * sizeof(uint32_t);
	if (program.bindings.descriptor_counts[static_cast<size_t>(
	        ShaderRecompiler::IR::DescriptorBindingKind::SharedMemory)] != 0) {
		// LDS kept in a device buffer is sized by the workgroup count: read the thread counts.
		m_context.GetBufferCache().ReadMemory(args_addr, ArgsSize);
		const auto* threads = reinterpret_cast<const uint32_t*>(args_addr);
		const uint32_t group_sizes[] = {cs_regs.cs_regs.num_thread_x, cs_regs.cs_regs.num_thread_y,
		                                cs_regs.cs_regs.num_thread_z};
		for (uint32_t axis = 0; axis < 3u; ++axis) {
			const auto size = std::max(group_sizes[axis], 1u);
			input_info.workgroup_counts[axis] = threads[axis] / size + (threads[axis] % size != 0u);
		}
		BindSharedMemory(m_context, input_info, bindings);
	}
	// Acquiring arguments can merge cache buffers; finalize shader bindings afterward.
	const auto [args_buffer, args_offset] =
	    m_context.GetBufferCache().ObtainBuffer(args_addr, ArgsSize, false);
	EXIT_IF(args_buffer == nullptr || (args_offset & 3u) != 0);
	RebindBuffers(bindings);
	m_context.GetOcclusionQueries().WriteForReaders();
	PrepareNativeRayTracing(m_context, buffer.Handle(), cs_regs, bindings);
	const auto& shader_data = bindings.shader_data_buffer;
	const bool  thread_limit =
	    program.bindings.dispatch_thread_dword != ShaderRecompiler::IR::PushData::NoStart;
	EXIT_IF(thread_limit && shader_data.buffer == nullptr);

	auto&      stream  = m_context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
	const std::array<uint32_t, 8> scratch_init {};
	const auto scratch_offset =
	    m_context.GetBufferCache().StreamUpload(scratch_init.data(), sizeof(scratch_init), 256);

	if (!m_indirect_thread_pass) {
		m_indirect_thread_pass = {new IndirectThreadPass(m_context.GetGraphics()),
		                          [](IndirectThreadPass* pass) { delete pass; }};
	}
	auto&      pass      = *m_indirect_thread_pass;
	const auto vk_buffer = buffer.Handle();
	const auto alignment = m_context.GetGraphics().StorageMinAlignment();
	const auto args_base = Common::AlignDown(args_offset, alignment);
	const vk::DescriptorBufferInfo infos[] {
	    {args_buffer->Handle(), args_base, args_offset - args_base + ArgsSize},
	    {stream.Handle(), scratch_offset, sizeof(scratch_init)},
	    thread_limit
	        ? vk::DescriptorBufferInfo {shader_data.buffer, shader_data.offset, shader_data.range}
	        : vk::DescriptorBufferInfo {stream.Handle(), scratch_offset, sizeof(scratch_init)},
	};
	std::array<vk::WriteDescriptorSet, 3> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	IndirectThreadPass::Push push {};
	push.local_size[0] = std::max(cs_regs.cs_regs.num_thread_x, 1u);
	push.local_size[1] = std::max(cs_regs.cs_regs.num_thread_y, 1u);
	push.local_size[2] = std::max(cs_regs.cs_regs.num_thread_z, 1u);
	push.args_index    = static_cast<uint32_t>((args_offset - args_base) / sizeof(uint32_t));
	push.groups_index  = 0;
	push.limit_index   = thread_limit ? program.bindings.dispatch_thread_dword : 4u;

	vk::MemoryBarrier before {};
	before.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite |
	                       vk::AccessFlagBits::eHostWrite;
	before.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                          vk::PipelineStageFlagBits::eComputeShader, {}, 1, &before, 0, nullptr,
	                          0, nullptr);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pass.pipeline);
	vk_buffer.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pass.pipeline_layout, 0,
	                               writes);
	vk_buffer.pushConstants(pass.pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
	                        sizeof(push), &push);
	vk_buffer.dispatch(1, 1, 1);
	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	after.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead |
	                      vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eUniformRead;
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                          vk::PipelineStageFlagBits::eDrawIndirect |
	                              vk::PipelineStageFlagBits::eComputeShader,
	                          {}, 1, &after, 0, nullptr, 0, nullptr);

	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	const bool has_storage_writes = bindings.shared_memory.buffer != nullptr ||
	    HasShaderBufferWrites(input_info.stage) || self_sized_writes ||
	    std::any_of(program.info.images.begin(), program.info.images.end(), [](const auto& image) {
		    return image.written && image.resource_class ==
		                                ShaderRecompiler::IR::ImageResourceClass::Storage;
	    });
	if (has_storage_writes) {
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	vk_buffer.dispatchIndirect(stream.Handle(), scratch_offset);
	ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	ResetBindings();
}

void RenderExecutor::DispatchIndirect(CommandBuffer& buffer, uint64_t args_addr, uint32_t mode,
                                      bool async_compute) {
	EXIT_IF(args_addr == 0 || (args_addr & 3u) != 0 ||
	        (mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
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
	m_context.GetImageHeap().BeginDraw();
	const ShaderModule* compute_module = nullptr;
	if (!TakeResolvedDispatch(input_info, compute_module)) {
		input_info.async_compute         = async_compute;
		input_info.indirect_args_address = args_addr;
		compute_module = m_context.GetPipelineCache().GetComputeProgram(cs_regs, input_info);
	}
	if (compute_module == nullptr) return;
	buffer.EndRendering();
	auto& pipeline = m_context.GetPipelineCache().GetComputePipeline(input_info);
	auto& bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	PreparedBindings* descriptor_stage = &bindings;
	CacheDmaBases(m_context, input_info.stage);
	FindBuffers(std::span {&descriptor_stage, 1u});
	const auto& program = input_info.stage.module->info;
	// New target buffers must exist before PrepareBda uploads the CPU writes of cached buffers.
	const bool indirect_writes = PrepareIndirectWriteTargets(m_context, bindings) |
	                             PrepareSelfSizedBuffers(m_context, input_info.stage);
	if (program.info.uses_dma) {
		m_context.CacheDmaBases(input_info.stage);
		const ShaderStageRuntime* const dma_stage = &input_info.stage;
		const bool reach_synchronized = m_context.SynchronizeDmaFootprint(std::span {&dma_stage, 1});
		m_context.PrepareBda(MayWriteThroughDeviceAddresses(program), !reach_synchronized);
		if (program.bindings.descriptor_counts[static_cast<size_t>(ShaderRecompiler::IR::DescriptorBindingKind::Buffers)] != 0) {
			// Its results are often read back soon: submit it at once (see CommitWriteTicks()).
			m_context.GetBufferCache().RequestFlushForReadbacks();
		}
	}
	PrepareRayQuery(m_context, input_info.stage, bindings);
	BindSharedMemory(m_context, input_info, bindings, args_addr);
	if (program.info.uses_image_heap) {
		m_context.GetImageHeap().Rebind(*this);
	}
	RebindImages(bindings);
	// Acquiring arguments can merge cache buffers; finalize shader bindings afterward.
	const auto [args_buffer, args_offset] = m_context.GetBufferCache().ObtainBuffer(
	    args_addr, sizeof(vk::DispatchIndirectCommand), false);
	EXIT_IF((args_offset & 3u) != 0);
	RebindBuffers(bindings);
	m_context.GetOcclusionQueries().WriteForReaders();
	PrepareNativeRayTracing(m_context, buffer.Handle(), cs_regs, bindings);
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	const auto vk_buffer = buffer.Handle();
	// As DispatchDirect(): without a guest order since the previous dispatch of its queue, the barrier before that
	// dispatch already ordered everything before it (the arguments included) and the two overlap on the guest's GPU.
	const bool chained = std::exchange(m_dispatch_chained, false) && m_deferred_barrier;
	if (!chained) {
		if (HasStorageWrites(input_info, bindings, indirect_writes)) {
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
	}
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	vk_buffer.dispatchIndirect(args_buffer->Handle(), args_offset);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::Dispatches);
	m_deferred_barrier       = true;
	m_deferred_barrier_queue = m_guest_queue;
	ResetBindings();
}

} // namespace Libs::Graphics
