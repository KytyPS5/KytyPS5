#include "common/assert.h"
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
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/dispatchGuard.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/threadDispatch.h"
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics {
namespace {

class PreciseWriteScope {
public:
	explicit PreciseWriteScope(BufferCache& cache): m_cache(cache) { m_cache.BeginPreciseWrites(); }
	~PreciseWriteScope() { m_cache.EndPreciseWrites(); }
	KYTY_CLASS_NO_COPY(PreciseWriteScope);

private:
	BufferCache& m_cache;
};

} // namespace

static void PushThreadLimit(vk::CommandBuffer command, const PipelineCache::Pipeline& pipeline,
                            const ThreadDispatcher::Record& record) {
	const uint32_t words[ShaderRecompiler::IR::PushData::DispatchLimitDwordCount] {
	    static_cast<uint32_t>(record.limit_address),
	    static_cast<uint32_t>(record.limit_address >> 32u)};
	command.pushConstants(pipeline.pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
	                      sizeof(words), words);
}

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
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	if (resources.buffers.size() != program.info.buffers.size()) {
		EXIT("compute runtime buffer count does not match shader metadata\n");
	}
	// Consuming the dispatch discards every store it would have made, so the dispatch has to be a
	// clear of one metadata range and nothing else. Image, sampler and flat stores are never
	// metadata, and a program that mixes bits cannot be writing a constant.
	if (program.info.has_bitwise_xor || !program.info.images.empty() ||
	    !program.info.samplers.empty() || program.info.writes_dma) {
		return false;
	}
	uint32_t stores = 0;
	for (const auto& resource: program.info.buffers) {
		if (resource.written || resource.atomic) {
			if (resource.read || resource.atomic) {
				return false;
			}
			stores++;
		}
	}
	if (stores != 1) {
		return false;
	}
	auto&    cache           = buffer.GetContext().GetTextureCache();
	uint32_t metadata_writes = 0;
	uint32_t metadata_index  = 0;
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& resource   = program.info.buffers[i];
		const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		const bool  is_meta    = cache.IsMeta(descriptor.Base48());
		if (!resource.written && !resource.atomic) {
			// A clear may read its value from a side buffer, but reading metadata back means the
			// dispatch derives something from it instead of clearing it.
			if (is_meta) {
				return false;
			}
			continue;
		}
		if (resource.read || resource.atomic || !is_meta) {
			// A read/modify/write is not a clear, and the registry outlives the guest allocation,
			// so a base that is still registered is only a hint. Any other store is real work that
			// consuming the dispatch would silently drop.
			return false;
		}
		metadata_writes++;
		metadata_index = i;
	}
	if (metadata_writes != 1) {
		return false;
	}
	const auto metadata =
	    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[metadata_index]);
	const bool cleared = cache.ClearMetaSlices(metadata.Base48(), metadata.GetSize());
	return cleared && cache.IsWithinMeta(metadata.Base48(), metadata.GetSize());
}

bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                              uint32_t group_y, uint32_t group_z, uint32_t mode,
                              ShaderBufferResource& resolved_descriptor, uint32_t& resolved_clear,
                              uint64_t& resolved_size) {
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	if (fill.kind != ShaderRecompiler::IR::UniformFillKind::Buffer || !fill.words_agree) {
		return false;
	}
	const auto element_size = fill.words * sizeof(uint32_t);
	const auto descriptor =
	    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[fill.resource]);
	constexpr std::array formats {
	    Prospero::BufferFormat::k32UInt, Prospero::BufferFormat::k32_32UInt,
	    Prospero::BufferFormat::k32_32_32UInt, Prospero::BufferFormat::k32_32_32_32UInt};
	if (descriptor.Stride() != element_size || descriptor.Format() != formats[fill.words - 1] ||
	    descriptor.SwizzleEnabled() || descriptor.IndexStride() != 0 || descriptor.AddTid() ||
	    descriptor.Base48() == 0) {
		return false;
	}
	// Each invocation writes `stores` elements, so a workgroup covers local_size_x * stores of
	// them. The one-store shape is local_size_x, exactly as before.
	if (fill.stores == 0 || fill.stores > ShaderRecompiler::IR::UniformFill::MaxStores ||
	    input.threads_num[0] == 0 ||
	    static_cast<uint64_t>(input.threads_num[0]) * fill.stores != fill.group_stride[0] ||
	    input.threads_num[1] != 1 || input.threads_num[2] != 1 || group_x == 0 || group_y != 1 ||
	    group_z != 1 || mode != (input.dispatch_thread_dimensions ? 0x61u : 0x41u)) {
		return false;
	}
	const uint64_t invocations = input.dispatch_thread_dimensions
	                                 ? group_x
	                                 : static_cast<uint64_t>(group_x) * input.threads_num[0];
	const uint64_t elements    = invocations * fill.stores;
	const auto     size        = descriptor.GetSize();
	if (elements != descriptor.NumRecords() || size == 0 || size > UINT32_MAX ||
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

// KYTY_NO_COMPUTE_IMAGE_COPY=1 runs every buffer-copy dispatch as recorded.
static bool ComputeImageCopyEnabled() {
	static const bool enabled = [] {
		const char* text = std::getenv("KYTY_NO_COMPUTE_IMAGE_COPY");
		return text == nullptr || std::strcmp(text, "0") == 0;
	}();
	return enabled;
}

bool ResolveComputeBufferCopy(const ShaderComputeInputInfo& input, uint32_t group_x,
                              uint32_t group_y, uint32_t group_z, uint32_t mode, uint64_t& source,
                              uint64_t& destination, uint64_t& size) {
	const auto& resources = *input.stage.resources;
	const auto& copy      = resources.buffer_copy;
	if (!copy.valid || copy.pairs == 0 || copy.pairs > ShaderRecompiler::IR::BufferCopy::MaxPairs ||
	    copy.src >= resources.buffers.size() || copy.dst >= resources.buffers.size() ||
	    mode != 0x41u || input.dispatch_thread_dimensions) {
		return false;
	}
	const uint64_t threads = input.threads_num[0];
	if (threads == 0 || input.threads_num[1] != 1 || input.threads_num[2] != 1 || group_x == 0 ||
	    group_y != 1 || group_z != 1 || copy.group_stride != threads * copy.pairs) {
		return false;
	}
	std::array<uint32_t, ShaderRecompiler::IR::BufferCopy::MaxPairs> offsets = copy.offsets;
	std::sort(offsets.begin(), offsets.begin() + copy.pairs);
	for (uint32_t k = 0; k < copy.pairs; ++k) {
		if (offsets[k] != k * threads) {
			return false;
		}
	}
	const auto     from = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[copy.src]);
	const auto     to   = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[copy.dst]);
	const uint64_t element = uint64_t {copy.words} * sizeof(uint32_t);
	for (const auto& descriptor: {from, to}) {
		if (descriptor.Base48() == 0 || descriptor.Stride() != element ||
		    descriptor.SwizzleEnabled() || descriptor.IndexStride() != 0 || descriptor.AddTid()) {
			return false;
		}
	}
	const uint64_t records = to.NumRecords();
	const uint64_t covered = uint64_t {group_x} * copy.group_stride;
	if (records == 0 || from.NumRecords() != records || covered < records ||
	    covered - records >= copy.group_stride || records > UINT64_MAX / element) {
		return false;
	}
	size        = records * element;
	source      = from.Base48();
	destination = to.Base48();
	return size <= 0x40000000u && (source + size <= destination || destination + size <= source);
}

bool ResolveComputeRecordFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                              uint32_t group_y, uint32_t group_z, uint32_t mode,
                              ShaderBufferResource&    resolved_descriptor,
                              std::array<uint32_t, 4>& pattern, uint32_t& pattern_words,
                              uint64_t& resolved_size) {
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	if (fill.kind != ShaderRecompiler::IR::UniformFillKind::Buffer || fill.stores != 1 ||
	    mode != 0x41u || input.dispatch_thread_dimensions) {
		return false;
	}
	const auto descriptor =
	    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[fill.resource]);
	uint32_t words = 0;
	switch (descriptor.Format()) {
		case Prospero::BufferFormat::k32UInt: words = 1; break;
		case Prospero::BufferFormat::k32_32UInt: words = 2; break;
		case Prospero::BufferFormat::k32_32_32UInt: words = 3; break;
		case Prospero::BufferFormat::k32_32_32_32UInt: words = 4; break;
		default: return false;
	}
	const uint64_t element = words * sizeof(uint32_t);
	const uint64_t stride  = descriptor.Stride();
	// Console clear helpers store one element per record and mean the whole record cleared.
	if (words > fill.words || stride <= element || stride % element != 0 ||
	    descriptor.SwizzleEnabled() || descriptor.IndexStride() != 0 || descriptor.AddTid() ||
	    descriptor.Base48() == 0) {
		return false;
	}
	const uint64_t threads = input.threads_num[0];
	const uint64_t records = descriptor.NumRecords();
	if (threads == 0 || threads != fill.group_stride[0] || input.threads_num[1] != 1 ||
	    input.threads_num[2] != 1 || group_y != 1 || group_z != 1 || records == 0 ||
	    group_x != (records + threads - 1) / threads) {
		return false;
	}
	const uint64_t size = records * stride;
	if (size > 0x10000000u ||
	    !FillSourcesDisjoint(resources.buffers, {descriptor.Base48(), size}, fill.resource)) {
		return false;
	}
	resolved_descriptor = descriptor;
	pattern             = fill.word_values;
	pattern_words       = words;
	resolved_size       = size;
	return true;
}

bool RenderExecutor::TryConsumeComputeRecordFill(const ShaderComputeInputInfo& input,
                                                 CommandBuffer& command, uint32_t group_x,
                                                 uint32_t group_y, uint32_t group_z,
                                                 uint32_t mode) {
	ShaderBufferResource    descriptor;
	std::array<uint32_t, 4> pattern {};
	uint32_t                words = 0;
	uint64_t                size  = 0;
	if (!ResolveComputeRecordFill(input, group_x, group_y, group_z, mode, descriptor, pattern,
	                              words, size)) {
		return false;
	}
	command.GetContext().GetBufferCache().FillBufferPattern(descriptor.Base48(), size,
	                                                        pattern.data(), words);
	return true;
}

bool RenderExecutor::TryConsumeComputeImageClear(const ShaderComputeInputInfo& input,
                                                 CommandBuffer& command, uint32_t group_x,
                                                 uint32_t group_y, uint32_t group_z,
                                                 uint32_t mode) {
	const auto& program   = *input.stage.program;
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
			    groups[axis] * threads > UINT32_MAX)
				return false;
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
		    std::max(1u, image.info.extent.height >> view.base_level) != extents[1])
			return false;
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eStencil, view.base_level,
		                                       1, view.base_layer, view.layer_count};
		vk::ClearValue                  clear {};
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
	if (!cache.ClearImageFromBuffer(command, descriptor.Base48(), size, packed_clear)) {
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

bool RenderExecutor::TryConsumeComputeImageCopy(const ShaderComputeInputInfo& input,
                                                CommandBuffer& command, uint32_t group_x,
                                                uint32_t group_y, uint32_t group_z, uint32_t mode) {
	if (!ComputeImageCopyEnabled()) {
		return false;
	}
	uint64_t source      = 0;
	uint64_t destination = 0;
	uint64_t size        = 0;
	if (!ResolveComputeBufferCopy(input, group_x, group_y, group_z, mode, source, destination,
	                              size)) {
		return false;
	}
	return command.GetContext().GetBufferCache().CopyImageForBufferCopy(source, destination, size);
}

static void ReportOverLimitDispatch(RenderContext& context, uint64_t shader_hash,
                                    std::array<uint32_t, 3> counts, uint32_t mode,
                                    std::array<uint32_t, 3> local_size, const DispatchPlan& plan,
                                    std::array<uint32_t, 3> max_groups, uint64_t indirect_args_addr,
                                    uint64_t submit_id) {
	static std::atomic<uint64_t>        rejected {0};
	static std::mutex                   reported_mutex;
	static std::unordered_set<uint64_t> reported;
	const auto total = rejected.fetch_add(1, std::memory_order_relaxed) + 1;
	{
		std::lock_guard lock(reported_mutex);
		if (!reported.insert(shader_hash).second) {
			return;
		}
	}
	const bool indirect   = indirect_args_addr != 0;
	int        registered = -1;
	if (indirect && GuestRange {indirect_args_addr, 12}.Valid()) {
		registered = context.GetBufferCache().IsRegionRegistered(indirect_args_addr, 12) ? 1 : 0;
	}
	LOGF("GraphicsRenderDispatchDirect: skipping dispatch over the workgroup limit "
	     "shader=0x%016" PRIx64 " source=%s args_addr=0x%016" PRIx64
	     " args_in_gpu_buffer=%d raw=%ux%ux%u "
	     "(0x%08" PRIx32 ",0x%08" PRIx32 ",0x%08" PRIx32 ") mode=0x%08" PRIx32
	     " thread_dimensions=%d local=%ux%ux%u groups=%ux%ux%u axis=%u max=%ux%ux%u submit=%" PRIu64
	     " rejected_total=%" PRIu64 "\n",
	     shader_hash, indirect ? "indirect" : "packet", indirect_args_addr, registered, counts[0],
	     counts[1], counts[2], counts[0], counts[1], counts[2], mode,
	     (mode & (1u << 5u)) != 0 ? 1 : 0, local_size[0], local_size[1], local_size[2],
	     plan.groups[0], plan.groups[1], plan.groups[2], plan.failed_axis, max_groups[0],
	     max_groups[1], max_groups[2], submit_id, total);
	std::printf("warning: skipped compute dispatch over the workgroup limit shader=0x%016" PRIx64
	            " groups=%ux%ux%u source=%s\n",
	            shader_hash, plan.groups[0], plan.groups[1], plan.groups[2],
	            indirect ? "indirect" : "packet");
	std::fflush(stdout);
}

static void MarkStorageImagesFlushed(TextureCache& cache, const PreparedBindings& bindings) {
	for (const auto& binding: bindings.images) {
		if (binding.desc.type != TextureCache::BindingType::Storage) {
			continue;
		}
		auto*      image = &cache.GetImage(binding.image_id);
		const bool whole =
		    image->binding.force_general && !image->info.IsDepth() && !image->info.data.Empty();
		const auto&                          view = binding.desc.view_info;
		std::optional<ImageSubresourceRange> range;
		if (!whole) {
			range = ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
			                               view.layer_count};
		}
		image->MarkShaderWritesFlushed(range);
	}
}

template <typename Handle>
static uint64_t HandleKey(Handle handle) {
	return (uint64_t)(handle); // NOLINT: a non-dispatchable handle is a pointer or an integer
}

static bool BindlessReadOnlyAccessEnabled() {
	static const bool enabled = [] {
		const char* text = std::getenv("KYTY_NO_BINDLESS_READ_ONLY_ACCESS");
		return text == nullptr || std::strcmp(text, "0") == 0;
	}();
	return enabled;
}

// What the dispatch about to be recorded reads and writes, for JoinDeferredBarrier.
static void CollectDispatchAccess(TextureCache& textures, const ShaderStageRuntime& stage,
                                  const PreparedBindings& bindings, DispatchAccess& access) {
	using Space = DispatchAccess::Space;
	access.Clear();
	const auto& program  = *stage.program;
	const auto& layout   = program.bindings;
	const bool  bindless = layout.uses_bindless || !bindings.bindless_patches.empty();
	if (program.info.uses_dma || bindless) {
		access.reads_all  = true;
		access.writes_all = program.info.writes_dma || program.has_address_writes ||
		                    (bindless && !BindlessReadOnlyAccessEnabled());
	}
	if (bindings.gds.buffer != nullptr) {
		access.reads_all  = true;
		access.writes_all = true;
		// Named as well, so the guest-synced comparison still orders GDS users.
		access.Add(Space::HostBuffer, HandleKey(static_cast<VkBuffer>(bindings.gds.buffer)),
		           bindings.gds.offset, bindings.gds.range, true);
	}
	const auto buffer_count = layout.descriptor_counts[static_cast<size_t>(
	    ShaderRecompiler::IR::DescriptorBindingKind::Buffers)];
	if (buffer_count != 0) {
		if (bindings.buffer_sources.size() != buffer_count) {
			access.reads_all  = true;
			access.writes_all = true;
		} else {
			for (const auto& resource: program.info.buffers) {
				if (resource.descriptor_index == UINT32_MAX) {
					continue;
				}
				const auto& source = bindings.buffer_sources[resource.descriptor_index];
				access.Add(Space::Guest, 0, source.address, source.size, resource.written);
			}
		}
	}
	EXIT_IF(bindings.images.size() < program.info.images.size());
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto& resource = program.info.images[i];
		const bool  write =
		    resource.written &&
		    resource.resource_class == ShaderRecompiler::IR::ImageResourceClass::Storage;
		if (resource.bindless) {
			// A heap record names no image of this binding; reads are already all-reads.
			access.writes_all = access.writes_all || resource.written || resource.atomic;
			continue;
		}
		const auto& image = textures.GetImage(bindings.images[i].image_id);
		access.Add(Space::HostImage, HandleKey(static_cast<VkImage>(image.backing.image)), 0, 1,
		           write);
		// An alias of the same guest memory is another host image; its guest range names it.
		for (const auto range: {image.info.data, image.info.stencil, image.info.metadata.range}) {
			access.Add(Space::Guest, 0, range.address, range.size, write);
		}
	}
	if (bindings.shared_memory.buffer != nullptr) {
		const auto& memory = bindings.shared_memory;
		access.Add(Space::HostBuffer, HandleKey(static_cast<VkBuffer>(memory.buffer)),
		           memory.offset, memory.range, true);
	}
}

static void BindSharedMemory(RenderContext& context, ShaderComputeInputInfo& input,
                             PreparedBindings& bindings, uint64_t indirect_args = 0) {
	if (input.stage.program->bindings.descriptor_counts[static_cast<size_t>(
	        ShaderRecompiler::IR::DescriptorBindingKind::SharedMemory)] == 0) {
		return;
	}
	auto& cache = context.GetBufferCache();
	if (indirect_args != 0) {
		cache.ReadMemory(indirect_args, sizeof(vk::DispatchIndirectCommand));
		std::memcpy(input.workgroup_counts, reinterpret_cast<const void*>(indirect_args),
		            sizeof(input.workgroup_counts));
		if (input.dispatch_thread_dimensions) {
			for (uint32_t axis = 0; axis < 3u; ++axis) {
				const auto size = std::max(input.threads_num[axis], 1u);
				const auto threads = input.workgroup_counts[axis];
				input.workgroup_counts[axis] = threads / size + (threads % size != 0u);
			}
		}
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
                                    uint32_t thread_group_z, uint32_t mode, bool async_compute,
                                    uint64_t indirect_args_addr) {
	EXIT_IF(buffer.IsInvalid());
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ctx    = buffer.GetRegisters();
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

	if (sh_ctx.GetCs().cs_regs.data_addr == 0) {
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

	ShaderComputeInputInfo input_info {};
	input_info.async_compute              = async_compute;
	const bool use_thread_dimensions      = (mode & DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
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
	const auto compute_program =
	    m_context.GetPipelineCache().GetComputeProgram(cs_regs, sh_regs, input_info);
	// No program, or no bound stage, means this pass could not derive the shader's
	// descriptors. Either way the dispatch is dropped and the session kept.
	if (!compute_program || !input_info.stage) {
		ResetBindings();
		return;
	}
	if (use_thread_dimensions) {
		input_info.dispatch_threads_num[0] = thread_group_x;
		input_info.dispatch_threads_num[1] = thread_group_y;
		input_info.dispatch_threads_num[2] = thread_group_z;
	}

	const auto& program       = *input_info.stage.program;
	const auto& resources     = *input_info.stage.resources;
	const auto& device_limits = m_context.GetGraphics().physical_device_properties.limits;
	const std::array<uint32_t, 3> max_groups {device_limits.maxComputeWorkGroupCount[0],
	                                          device_limits.maxComputeWorkGroupCount[1],
	                                          device_limits.maxComputeWorkGroupCount[2]};
	const std::array<uint32_t, 3> counts {thread_group_x, thread_group_y, thread_group_z};
	const std::array<uint32_t, 3> local_size {
	    cs_regs.cs_regs.num_thread_x, cs_regs.cs_regs.num_thread_y, cs_regs.cs_regs.num_thread_z};
	const auto plan = PlanComputeDispatch(counts, local_size, use_thread_dimensions, max_groups);
	if (plan.action == DispatchPlanAction::SkipOverLimit) {
		ReportOverLimitDispatch(m_context, program.shader_hash, counts, mode, local_size, plan,
		                        max_groups, indirect_args_addr, submit_id);
		ResetBindings();
		return;
	}
	if (resources.specialization_reads.empty() &&
	    (TryConsumeComputeMetaClear(input_info, buffer) ||
	     TryConsumeComputeImageClear(input_info, buffer, thread_group_x, thread_group_y,
	                                 thread_group_z, mode) ||
	     TryConsumeComputeRecordFill(input_info, buffer, thread_group_x, thread_group_y,
	                                 thread_group_z, mode) ||
	     TryConsumeComputeImageCopy(input_info, buffer, thread_group_x, thread_group_y,
	                                thread_group_z, mode))) {
		ResetBindings();
		return;
	}

	const bool large_workgroup =
	    (input_info.threads_num[0] * input_info.threads_num[1] * input_info.threads_num[2] >= 512);
	const bool                   has_sampler = !program.info.samplers.empty();
	static std::atomic<uint32_t> dispatch_log_count {0};
	if ((large_workgroup || has_sampler) &&
	    dispatch_log_count.fetch_add(1, std::memory_order_relaxed) < 512u) {
		const auto sampled_images = std::count_if(
		    program.info.images.begin(), program.info.images.end(), [](const auto& image) {
			    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
		    });
		const uint32_t frame_num = static_cast<uint32_t>(m_context.GetGpu().GetFrameNum());
		LOGF("GraphicsRenderDispatchDirect: frame=%u shader=0x%016" PRIx64
		     " hash=0x%016" PRIx64 " tick=%" PRIu64
		     " groups=%ux%ux%u mode=0x%08" PRIx32 " local=%ux%ux%u "
		     "buffers=%zu textures=%zu sampled=%zu storage=%zu samplers=%zu push=%u\n",
		     frame_num, sh_ctx.GetCs().cs_regs.data_addr, program.shader_hash,
		     m_context.GetCommandScheduler().CurrentTick(),
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

	if (use_thread_dimensions) {
		const uint32_t old_x = thread_group_x;
		const uint32_t old_y = thread_group_y;
		const uint32_t old_z = thread_group_z;
		thread_group_x       = plan.groups[0];
		thread_group_y       = plan.groups[1];
		thread_group_z       = plan.groups[2];

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

	if (plan.action == DispatchPlanAction::SkipEmpty) {
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
	// The probe state block is a few kilobytes, so it is only reset for the BVH shader set.
	auto& pipeline = m_context.GetPipelineCache().GetComputePipeline(input_info, compute_program);
	StreamHold stream_hold(m_context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream));
	auto&      bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	PreparedBindings* descriptor_stage = &bindings;
	FindBuffers(std::span {&descriptor_stage, 1u});
	if (program.info.uses_dma) {
		m_context.PrepareBda(program.info.writes_dma);
	}
	PreparedBindings* bindless_stage = &bindings;
	PrepareBindlessTables(std::span {&bindless_stage, 1u});
	RebindImages(bindings);
	BindSharedMemory(m_context, input_info, bindings);
	const PreciseWriteScope precise_writes(m_context.GetBufferCache());
	RebindBuffers(bindings);
	bool has_storage_writes =
	    bindings.shared_memory.buffer != nullptr || HasShaderBufferWrites(input_info.stage);
	has_storage_writes =
	    std::any_of(program.info.images.begin(), program.info.images.end(),
	                [](const auto& image) {
		                return image.written &&
		                       image.resource_class ==
		                           ShaderRecompiler::IR::ImageResourceClass::Storage;
	                }) ||
	    has_storage_writes;
	const bool                  overlap = DispatchOverlapEnabled() && !GuestSyncOnly();
	CommandBuffer::DeferredJoin join {};
	if (overlap) {
		CollectDispatchAccess(m_context.GetTextureCache(), input_info.stage, bindings,
		                      m_dispatch_access);
		// Writing the thread-dimension record is a transfer the group must be ordered before.
		if (!use_thread_dimensions) {
			join = buffer.JoinDeferredBarrier(m_dispatch_access, has_storage_writes);
		}
		if (!join.joined) {
			buffer.FlushDeferredBarrier();
		}
	}
	const bool ordered_before =
	    join.joined ? join.ordered_before : BarrierElisionEnabled() && buffer.FullBarrierIsLast();

	auto                     vk_buffer = buffer.Handle();
	ThreadDispatcher::Record thread_record {};
	if (use_thread_dimensions) {
		thread_record = ThreadDispatch().Write(vk_buffer, {input_info.dispatch_threads_num[0],
		                                                   input_info.dispatch_threads_num[1],
		                                                   input_info.dispatch_threads_num[2]});
	}
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	const bool hazard_barrier = has_storage_writes && !GuestSyncOnly() && !ordered_before;
	if (hazard_barrier) {
		// A host fence used to serialize every dispatch. Preserve its read-before-write ordering
		// while allowing the queue to execute asynchronously.
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	if (use_thread_dimensions) {
		PushThreadLimit(vk_buffer, pipeline, thread_record);
	}
	const bool ordered_append = (mode & Pm4::COMPUTE_DISPATCH_INITIATOR_ORDERED_APPEND_ENBL) != 0 &&
	                            program.info.uses_gds && !use_thread_dimensions &&
	                            thread_group_x * thread_group_y * thread_group_z > 1u;
	if (ordered_append) {
		// Ordered append hands out GDS counts in workgroup launch order, X fastest.
		for (uint32_t z = 0; z < thread_group_z; z++) {
			for (uint32_t y = 0; y < thread_group_y; y++) {
				for (uint32_t x = 0; x < thread_group_x; x++) {
					if (x + y + z != 0u) {
						ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
					}
					vk_buffer.dispatchBase(x, y, z, 1, 1, 1);
				}
			}
		}
	} else {
		vk_buffer.dispatch(thread_group_x, thread_group_y, thread_group_z);
	}
	m_context.GetIndirectKeyFeedback().Flush(vk_buffer);

	// The removed host fence also ordered read-only dispatches before later writers.
	if (!GuestSyncOnly()) {
		if (overlap) {
			buffer.DeferBarrier(m_dispatch_access, hazard_barrier || ordered_before);
		} else {
			ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
			if (hazard_barrier || ordered_before) {
				buffer.MarkFullBarrier();
			}
		}
		MarkStorageImagesFlushed(m_context.GetTextureCache(), bindings);
	}
	ResetBindings();
}

void RenderExecutor::DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer, uint64_t args_addr,
                                      uint32_t mode, bool async_compute) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid() || args_addr == 0 || (args_addr & 3u) != 0);
	const bool thread_dimensions =
	    (mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	{
		KYTY_PROFILER_BLOCK("DispatchIndirect PopPendingOperations");
		m_context.GetCommandScheduler().PopPendingOperations();
	}
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchIndirect), submit_id,
	                    static_cast<uint32_t>(args_addr), static_cast<uint32_t>(args_addr >> 32u),
	                    0, mode, buffer.GetShaders().GetCs().cs_regs.data_addr);
	const Common::LockGuard lock = [&] {
		KYTY_PROFILER_BLOCK("DispatchIndirect context lock");
		return Common::LockGuard(m_context.GetMutex());
	}();
	const auto& cs_regs = buffer.GetShaders().GetCs();
	if (cs_regs.cs_regs.data_addr == 0) {
		return;
	}
	ShaderComputeInputInfo input_info {};
	input_info.async_compute              = async_compute;
	input_info.dispatch_thread_dimensions = thread_dimensions;
	const auto compute_program            = m_context.GetPipelineCache().GetComputeProgram(
	    cs_regs, buffer.GetRegisters().GetShaderRegisters(), input_info);
	if (!compute_program || !input_info.stage) {
		ResetBindings();
		return;
	}
	buffer.EndRendering();
	auto& pipeline = m_context.GetPipelineCache().GetComputePipeline(input_info, compute_program);
	auto& bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	PreparedBindings* descriptor_stage = &bindings;
	FindBuffers(std::span {&descriptor_stage, 1u});
	const auto& program = *input_info.stage.program;
	if (program.info.uses_dma) {
		KYTY_PROFILER_BLOCK("DispatchIndirect PrepareBda");
		m_context.PrepareBda(program.info.writes_dma);
	}
	PreparedBindings* bindless_stage = &bindings;
	PrepareBindlessTables(std::span {&bindless_stage, 1u});
	BindSharedMemory(m_context, input_info, bindings, args_addr);
	RebindImages(bindings);
	// Acquiring arguments can merge cache buffers; finalize shader bindings afterward.
	const auto [args_buffer, args_offset] = [&] {
		KYTY_PROFILER_BLOCK("DispatchIndirect ObtainBuffer(args)");
		return m_context.GetBufferCache().ObtainBuffer(args_addr,
		                                               sizeof(vk::DispatchIndirectCommand), false);
	}();
	EXIT_IF(args_buffer == nullptr || (args_offset & 3u) != 0);
	const PreciseWriteScope precise_writes(m_context.GetBufferCache());
	RebindBuffers(bindings);
	// An indirect dispatch never joins a group: its argument barrier waits for compute anyway.
	const bool overlap = DispatchOverlapEnabled() && !GuestSyncOnly() && !thread_dimensions;
	if (overlap) {
		CollectDispatchAccess(m_context.GetTextureCache(), input_info.stage, bindings,
		                      m_dispatch_access);
		m_dispatch_access.Add(DispatchAccess::Space::Guest, 0, args_addr,
		                      sizeof(vk::DispatchIndirectCommand), false);
	}
	buffer.FlushDeferredBarrier();
	const bool               ordered_before = BarrierElisionEnabled() && buffer.FullBarrierIsLast();
	ThreadDispatcher::Record dispatch_record {args_buffer->Handle(), args_offset, 0};
	if (thread_dimensions) {
		const auto& limits = m_context.GetGraphics().physical_device_properties.limits;
		const std::array<uint32_t, 3> local_size {std::max(cs_regs.cs_regs.num_thread_x, 1u),
		                                          std::max(cs_regs.cs_regs.num_thread_y, 1u),
		                                          std::max(cs_regs.cs_regs.num_thread_z, 1u)};
		const std::array<uint32_t, 3> max_groups {limits.maxComputeWorkGroupCount[0],
		                                          limits.maxComputeWorkGroupCount[1],
		                                          limits.maxComputeWorkGroupCount[2]};
		dispatch_record = ThreadDispatch().Convert(buffer.Handle(), args_buffer->Handle(),
		                                           args_offset, local_size, max_groups);
	}
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	const auto vk_buffer = buffer.Handle();
	const bool has_storage_writes = bindings.shared_memory.buffer != nullptr ||
	    HasShaderBufferWrites(input_info.stage) ||
	    std::any_of(program.info.images.begin(), program.info.images.end(), [](const auto& image) {
		    return image.written &&
		           image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Storage;
	    });
	const bool hazard_barrier = has_storage_writes && !GuestSyncOnly() && !ordered_before;
	if (hazard_barrier) {
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead;
	vk_buffer.pipelineBarrier(
	    vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader |
	        vk::PipelineStageFlagBits::eTransfer,
	    vk::PipelineStageFlagBits::eDrawIndirect, {}, 1, &barrier, 0, nullptr, 0, nullptr);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	if (thread_dimensions) {
		PushThreadLimit(vk_buffer, pipeline, dispatch_record);
	}
	vk_buffer.dispatchIndirect(dispatch_record.buffer, dispatch_record.groups_offset);
	if (!GuestSyncOnly()) {
		if (overlap) {
			buffer.DeferBarrier(m_dispatch_access, hazard_barrier || ordered_before);
		} else {
			ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
			if (hazard_barrier || ordered_before) {
				buffer.MarkFullBarrier();
			}
		}
		MarkStorageImagesFlushed(m_context.GetTextureCache(), bindings);
	}
	ResetBindings();
}

} // namespace Libs::Graphics
