#include "common/timer.h"
#include <array>
#include <utility>
#include "common/assert.h"
#include <cstdlib>
#include <mutex>
#include <set>
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/graphicContext.h"
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
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {
static uint64_t BufferDescriptorSize(const ShaderBufferResource& descriptor) {
	const uint64_t records = descriptor.NumRecords();
	const uint64_t stride  = descriptor.Stride();
	if (stride != 0 && records > UINT64_MAX / stride) {
		EXIT("compute buffer descriptor footprint overflow\n");
	}
	return stride == 0 ? records : records * stride;
}

bool RenderExecutor::TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
                                                const CommandBuffer&          buffer) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	if (resources.buffers.size() != program.info.buffers.size()) {
		EXIT("compute runtime buffer count does not match shader metadata\n");
	}
	auto& cache = buffer.GetContext().GetTextureCache();
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& resource   = program.info.buffers[i];
		const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		if (!resource.written && cache.IsMeta(descriptor.Base48())) {
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

bool ResolveComputeImageClear(const ShaderComputeInputInfo& input, uint32_t group_x,
                              uint32_t group_y, uint32_t group_z, uint32_t mode,
                              ShaderBufferResource& resolved_descriptor, uint32_t& resolved_clear,
                              uint64_t& resolved_size) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	if (program.info.buffers.size() != 1 || resources.buffers.size() != 1 ||
	    !program.info.images.empty() || !program.info.samplers.empty() || program.info.uses_dma ||
	    !resources.images.empty() || !resources.samplers.empty()) {
		return false;
	}
	const auto& resource   = program.info.buffers.front();
	const auto& raw        = resources.buffers.front();
	const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(raw);
	if (!resource.formatted || !resource.written || resource.read || resource.atomic ||
	    resource.scalar || resource.max_byte_extent != 16 || descriptor.Stride() != 16 ||
	    descriptor.Format() != Prospero::BufferFormat::k32_32_32_32UInt ||
	    descriptor.SwizzleEnabled() || descriptor.IndexStride() != 0 || descriptor.AddTid() ||
	    resource.packed_stride != descriptor.PackedStride() || raw.dword_count != 4 ||
	    program.user_data_base != 0 || resources.user_data.size() != 8) {
		return false;
	}
	for (uint32_t i = 0; i < raw.dword_count; i++) {
		if (raw.dwords[i] != resources.user_data[i]) {
			return false;
		}
	}
	const uint32_t clear = resources.user_data[4];
	if (resources.user_data[5] != clear || resources.user_data[6] != clear ||
	    resources.user_data[7] != clear) {
		return false;
	}
	const bool full_dispatch =
	    input.dispatch_thread_dimensions && input.threads_num[0] == 64 &&
	    input.threads_num[1] == 1 && input.threads_num[2] == 1 && group_x != 0 && group_y == 1 &&
	    group_z == 1 && input.dispatch_threads_num[0] == group_x &&
	    input.dispatch_threads_num[1] == 1 && input.dispatch_threads_num[2] == 1 &&
	    input.group_id[0] && !input.group_id[1] && !input.group_id[2] &&
	    input.thread_ids_num == 1 && input.wave_size == 64 && !input.tg_size_en && mode == 0x61u &&
	    group_x % input.threads_num[0] == 0 && descriptor.NumRecords() == group_x;
	const auto size = BufferDescriptorSize(descriptor);
	if (!full_dispatch || size == 0) {
		return false;
	}
	resolved_descriptor = descriptor;
	resolved_clear      = clear;
	resolved_size       = size;
	return true;
}

static bool TryConsumeComputeImageClear(const ShaderComputeInputInfo& input, CommandBuffer& command,
                                        uint32_t group_x, uint32_t group_y, uint32_t group_z,
                                        uint32_t mode) {
	ShaderBufferResource descriptor;
	uint32_t             packed_clear = 0;
	uint64_t             size         = 0;
	if (!ResolveComputeImageClear(input, group_x, group_y, group_z, mode, descriptor, packed_clear,
	                              size)) {
		return false;
	}
	auto& cache = command.GetContext().GetTextureCache();
	if (!cache.ClearImageFromBuffer(command, descriptor.Base48(), size, packed_clear)) {
		// Recognized metadata-fill shaders access DCC as an ordinary storage buffer and may run
		// before the render target is bound. TryConsumeDccFill either consumes registered state
		// or retains a PendingDcc fill while allowing the dispatch to run.
		const bool registered_metadata =
		    cache.TryConsumeDccFill(descriptor.Base48(), size, packed_clear);
		static std::atomic<uint32_t> logged_metadata_clears {0};
		if (logged_metadata_clears.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: %s metadata clear shader=0x%016" PRIx64
			     " addr=0x%016" PRIx64 " size=0x%016" PRIx64 " value=0x%08" PRIx32 "\n",
			     registered_metadata ? "tracked" : "deferred", input.stage.program->shader_hash,
			     descriptor.Base48(), size, packed_clear);
		}
		if (registered_metadata) {
			return true;
		}
		return false;
	}
	static std::atomic<uint32_t> logged_clears {0};
	if (logged_clears.fetch_add(1, std::memory_order_relaxed) < 32) {
		LOGF("GraphicsRenderDispatchDirect: compute image clear shader=0x%016" PRIx64
		     " addr=0x%016" PRIx64 " size=0x%016" PRIx64 " value=0x%08" PRIx32 "\n",
		     input.stage.program->shader_hash, descriptor.Base48(), size, packed_clear);
	}
	return true;
}

void RenderExecutor::DispatchDirect(uint64_t submit_id, CommandBuffer& buffer,
                                    uint32_t thread_group_x, uint32_t thread_group_y,
                                    uint32_t thread_group_z, uint32_t mode) {
	EXIT_IF(buffer.IsInvalid());
	m_context.GetCommandScheduler().PopPendingOperations();
	GraphicsWorkCounters::dispatches.fetch_add(1, std::memory_order_relaxed);
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchDirect), submit_id,
	                    thread_group_x, thread_group_y, thread_group_z, mode,
	                    sh_ctx.GetCs().cs_regs.data_addr);

	const auto cs_addr  = sh_ctx.GetCs().cs_regs.data_addr;
	uint32_t   cs_index = UINT32_MAX;
	constexpr uint32_t skip_groups_over = 0u;
	if (skip_groups_over != 0 && (thread_group_x > skip_groups_over ||
	                              thread_group_y > skip_groups_over ||
	                              thread_group_z > skip_groups_over)) {
		ResetBindings();
		return;
	}
	{
		static std::mutex              census_mutex;
		static std::vector<uint64_t>   census;
		std::lock_guard<std::mutex>    lock(census_mutex);
		const auto found = std::find(census.begin(), census.end(), cs_addr);
		cs_index         = found != census.end() ? static_cast<uint32_t>(found - census.begin())
		                                         : static_cast<uint32_t>(census.size());
		if (found == census.end() && census.size() < 64) {
			census.push_back(cs_addr);
		}
	}
	constexpr std::pair<uint32_t, uint32_t> skip_index_range {1u, 0u};
	if (cs_index >= skip_index_range.first && cs_index <= skip_index_range.second) {
		ResetBindings();
		return;
	}
	constexpr uint64_t skip_cs_addr = 0ull;
	if (skip_cs_addr != 0 && sh_ctx.GetCs().cs_regs.data_addr == skip_cs_addr) {
		ResetBindings();
		return;
	}

	Common::LockGuard lock(m_context.GetMutex());
	if (sh_ctx.GetCs().cs_regs.data_addr == 0) {
		LOGF("GraphicsRenderDispatchDirect: temporary: ignoring dispatch with null CS shader, "
		     "groups=%ux%ux%u mode=%u\n",
		     thread_group_x, thread_group_y, thread_group_z, mode);
		return;
	}

	if (!ShaderAddressValid(sh_ctx.GetCs().cs_regs.data_addr)) {
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
	const auto& sh_regs = ctx.GetShaderRegisters();

	{
		static std::mutex         seen_mutex;
		static std::set<uint64_t> seen;
		const auto                addr  = cs_regs.cs_regs.data_addr;
		bool                      first = false;
		{
			std::lock_guard lock(seen_mutex);
			first = seen.insert(addr).second;
		}
	}

	ShaderComputeInputInfo input_info {};
	const bool use_thread_dimensions = (mode & DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	input_info.dispatch_thread_dimensions = use_thread_dimensions;
	const auto compute_program =
	    m_context.GetPipelineCache().GetComputeProgram(cs_regs, sh_regs, input_info);
	if (use_thread_dimensions) {
		input_info.dispatch_threads_num[0]    = thread_group_x;
		input_info.dispatch_threads_num[1]    = thread_group_y;
		input_info.dispatch_threads_num[2]    = thread_group_z;
	}

	const uint32_t frame_num = static_cast<uint32_t>(m_context.GetGpu().GetFrameNum());
	const bool     large_workgroup =
	    (input_info.threads_num[0] * input_info.threads_num[1] * input_info.threads_num[2] >= 512);
	if (!compute_program || !input_info.stage) {
		ResetBindings();
		return;
	}
	const auto& program   = *input_info.stage.program;
	const auto& resources = *input_info.stage.resources;
	if (TryConsumeComputeMetaClear(input_info, buffer)) {
		ResetBindings();
		return;
	}
	if (TryConsumeComputeImageClear(input_info, buffer, thread_group_x, thread_group_y,
	                                thread_group_z, mode)) {
		ResetBindings();
		return;
	}
	const auto sampled_images = std::count_if(
	    program.info.images.begin(), program.info.images.end(), [](const auto& image) {
		    return image.kind == ShaderRecompiler::IR::ResourceKind::Image ||
		           image.kind == ShaderRecompiler::IR::ResourceKind::ImageUint;
	    });
	const bool                   has_sampler = !program.info.samplers.empty();
	static std::atomic<uint32_t> dispatch_log_count {0};
	constexpr uint64_t traced_hash = 0ull;
	constexpr uint64_t writers_of = 0ull;
	if (writers_of != 0) {
		for (uint32_t b = 0; b < program.info.buffers.size(); b++) {
			const auto r    = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[b]);
			const auto base = r.Base48();
			const auto span = static_cast<uint64_t>(r.Stride()) * r.NumRecords();
			if (writers_of < base || writers_of >= base + std::max<uint64_t>(span, 1)) {
				continue;
			}
		}
	}
	const bool force_trace = traced_hash != 0 && sh_ctx.GetCs().cs_regs.data_addr == traced_hash;
	if (force_trace || ((large_workgroup || has_sampler) &&
	                    dispatch_log_count.fetch_add(1, std::memory_order_relaxed) < 512)) {
		if (force_trace) {
			std::string words;
			for (uint32_t i = 0; i < resources.user_data.size() && i < 16; i++) {
				words += fmt::format("{}[{}]=0x{:08x}", i == 0 ? "" : " ", i,
				                     resources.user_data[i]);
			}
		}
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& buffer = program.info.buffers[i];
			const auto  r      = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
			if (force_trace) {
				const auto span = static_cast<uint64_t>(r.Stride()) * r.NumRecords();
				if (span != 0 && span <= 2048) {
					std::array<uint8_t, 2048> raw {};
					if (Libs::LibKernel::Memory::TryReadBacking(r.Base48(), raw.data(), span)) {
						std::string bytes;
						for (uint64_t b = 0; b < span; b++) {
							bytes += fmt::format("{}{:02x}", b == 0 ? "" : " ", raw[b]);
						}
					}
				}
			}
			if (force_trace && !buffer.written && r.Stride() == 8) {
				const auto mapped = Libs::LibKernel::Memory::MappedExtentFrom(r.Base48());
				constexpr uint64_t dump_at = 0ull;
				if (false && mapped > 0x200) {
					const auto* word     = reinterpret_cast<const uint32_t*>(r.Base48());
					const auto  node_end = std::min(mapped, uint64_t {0x180} + 0x1b9c0);
					std::vector<uint32_t> queue {6u};
					uint64_t              visited = 0;
					uint64_t              found   = 0;
					uint32_t              found_type = 0;
					uint32_t              boxes   = 0;
					uint32_t              found_raw = 0;
					uint64_t              parent  = 0;
					while (!queue.empty() && visited < 4096 && found == 0) {
						const auto unit = queue.back();
						queue.pop_back();
						visited++;
						const uint64_t offset = static_cast<uint64_t>(unit) * 64;
						if (offset + 128 > node_end) {
							continue;
						}
						boxes++;
						const auto* node = word + offset / 4;
						for (uint32_t c = 0; c < 4; c++) {
							const auto ptr = node[c];
							const auto t   = ptr & 7u;
							if (t == 7u || ptr == 0) {
								continue;
							}
							if (t == 5u) {
								queue.push_back(ptr >> 3u);
							} else {
								found      = static_cast<uint64_t>(ptr >> 3u) * 64;
								found_type = t;
								found_raw  = ptr;
								parent     = offset;
								break;
							}
						}
					}
					if (found != 0 && found + 64 <= mapped) {
						const auto* box = reinterpret_cast<const uint32_t*>(r.Base48() + parent);
						for (uint64_t row = 0; row < 8; row++) {
						}
						const auto* tri = reinterpret_cast<const uint32_t*>(r.Base48() + found);
						for (uint64_t row = 0; row < 4; row++) {
						}
					}
				}
				constexpr uint64_t DumpBytes = 256;
				if (mapped >= dump_at + DumpBytes) {
					const auto* word = reinterpret_cast<const uint32_t*>(r.Base48() + dump_at);
					for (uint64_t row = 0; row < DumpBytes / 16; row++) {
					}
				}
			}
		}
		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			const auto& image = program.info.images[i];
			const auto  r     = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
			LOGF("  CS texture[%u]: source=%u usage=%s sampled=%s addr=0x%010" PRIx64
			     " type=%u fmt=%u extent=%ux%u depth=%u levels=%u tile=%u\n",
			     i, image.source, image.written ? "read-write" : "read-only",
			     (image.kind == ShaderRecompiler::IR::ResourceKind::Image ||
			      image.kind == ShaderRecompiler::IR::ResourceKind::ImageUint)
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
			const auto r = DecodeNativeDescriptor<ShaderSamplerResource>(resources.samplers[i]);
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

	if (use_thread_dimensions) {
		auto groups_from_threads = [](uint32_t threads, uint32_t group_size) {
			return (threads == 0
			            ? 0u
			            : (threads + std::max(group_size, 1u) - 1u) / std::max(group_size, 1u));
		};

		const uint32_t old_x = thread_group_x;
		const uint32_t old_y = thread_group_y;
		const uint32_t old_z = thread_group_z;
		thread_group_x       = groups_from_threads(thread_group_x, cs_regs.cs_regs.num_thread_x);
		thread_group_y       = groups_from_threads(thread_group_y, cs_regs.cs_regs.num_thread_y);
		thread_group_z       = groups_from_threads(thread_group_z, cs_regs.cs_regs.num_thread_z);

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

	buffer.EndRendering();
	auto& pipeline =
	    m_context.GetPipelineCache().CreateComputePipeline(input_info, compute_program);
	auto bindings = PrepareBindings(input_info.stage);
	FindBuffers(bindings);
	if (program.info.uses_dma) {
		m_context.GetGpuResources().PrepareBda();
	}
	RebindBuffers(bindings);
	RebindImages(bindings);

	auto              vk_buffer        = buffer.Handle();
	PreparedBindings* descriptor_stage = &bindings;
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	bool has_storage_writes = HasShaderBufferWrites(input_info.stage);
	bool writes_storage_image = false;
	EXIT_IF(bindings.resources.images.size() != program.info.images.size());
	for (size_t i = 0; i < program.info.images.size(); i++) {
		const auto& image = program.info.images[i];
		if (image.written && (image.kind == ShaderRecompiler::IR::ResourceKind::StorageImage ||
		                      image.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint)) {
			writes_storage_image = true;
			const auto addr = bindings.resources.images[i].desc.info.data.address;
			GraphicsWorkCounters::NoteTarget(
			    addr, GraphicsWorkCounters::PackImageId(
			              bindings.resources.images[i].image_id.index,
			              bindings.resources.images[i].image_id.generation));
			constexpr uint64_t watch = 0ull;
			if (watch != 0 && addr == watch) {
				const auto& img = m_context.GetTextureCache().GetImage(
				    bindings.resources.images[i].image_id);
			}
		}
	}
	has_storage_writes = writes_storage_image || has_storage_writes;
	if (has_storage_writes) {
		// A host fence used to serialize every dispatch. Preserve its read-before-write ordering
		// while allowing the queue to execute asynchronously.
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	constexpr uint64_t serialize_hash = 0ull;
	if (serialize_hash != 0 && sh_ctx.GetCs().cs_regs.data_addr == serialize_hash &&
	    thread_group_x * thread_group_y * thread_group_z > 1) {
		for (uint32_t z = 0; z < thread_group_z; z++) {
			for (uint32_t y = 0; y < thread_group_y; y++) {
				for (uint32_t x = 0; x < thread_group_x; x++) {
					(void)x;
					(void)y;
					(void)z;
					vk_buffer.dispatch(1, 1, 1);
					ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
				}
			}
		}
	} else {
		vk_buffer.dispatch(thread_group_x, thread_group_y, thread_group_z);
	}

	// The removed host fence also ordered read-only dispatches before later writers.
	ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);

	ResetBindings();
}

} // namespace Libs::Graphics
