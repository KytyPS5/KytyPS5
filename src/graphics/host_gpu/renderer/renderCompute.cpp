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
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {
constexpr uint64_t kUfcHangCsHash = 0xea0aceac518ec52dull;

bool ParseHexU64(const char* text, uint64_t* out) {
	if (text == nullptr || out == nullptr) {
		return false;
	}
	while (*text == ' ' || *text == '\t') {
		++text;
	}
	if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		text += 2;
	}
	if (*text == '\0') {
		return false;
	}
	char*                    end   = nullptr;
	const unsigned long long value = std::strtoull(text, &end, 16);
	if (end == text) {
		return false;
	}
	*out = static_cast<uint64_t>(value);
	return true;
}

bool EnvListContainsHash(const char* env_name, uint64_t hash) {
	const char* env = std::getenv(env_name);
	if (env == nullptr || env[0] == '\0') {
		return false;
	}
	if ((env[0] == '*' && env[1] == '\0') || (env[0] == '1' && env[1] == '\0')) {
		return hash == kUfcHangCsHash;
	}
	const char* cursor = env;
	while (*cursor != '\0') {
		while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
			++cursor;
		}
		if (*cursor == '\0') {
			break;
		}
		const char* start = cursor;
		while (*cursor != '\0' && *cursor != ',') {
			++cursor;
		}
		std::string token(start, static_cast<size_t>(cursor - start));
		while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
			token.pop_back();
		}
		uint64_t parsed = 0;
		if (ParseHexU64(token.c_str(), &parsed) && parsed == hash) {
			return true;
		}
	}
	return false;
}

bool ShouldSkipComputeHash(uint64_t hash) {
	return EnvListContainsHash("KYTY_SKIP_CS_HASH", hash);
}

static bool TryParseEnvU32(const char* name, uint32_t& out) {
	const char* env = std::getenv(name);
	if (env == nullptr || env[0] == '\0') {
		return false;
	}
	char*              end   = nullptr;
	const unsigned long value = std::strtoul(env, &end, 0);
	if (end == env || value > std::numeric_limits<uint32_t>::max()) {
		return false;
	}
	out = static_cast<uint32_t>(value);
	return true;
}

static bool ReadGdsDwords(RenderContext& context, std::array<uint32_t, 8>& words) {
	auto* gds = context.GetBufferCache().GetGdsBuffer();
	if (gds == nullptr || gds->Handle() == nullptr) {
		return false;
	}
	constexpr uint32_t kProbeDwords = 8;
	const uint64_t     probe_bytes  = kProbeDwords * sizeof(uint32_t);
	auto&              download     = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Download);
	auto&              scheduler    = context.GetCommandScheduler();
	const auto [mapped, offset]     = download.Map(probe_bytes, 4);
	if (mapped == nullptr) {
		return false;
	}
	download.CopyFrom(scheduler.Current(), *gds, 0, offset, probe_bytes,
	                  vk::AccessFlagBits::eMemoryWrite, vk::AccessFlags {},
	                  vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
	                  vk::AccessFlagBits::eHostRead);
	download.Commit();
	scheduler.Finish("gds-probe");
	download.Invalidate(offset, probe_bytes);
	std::memcpy(words.data(), mapped, static_cast<size_t>(probe_bytes));
	return true;
}

static void FillGdsDword(Buffer* gds, uint32_t dword_index, uint32_t value) {
	if (gds == nullptr || gds->Handle() == nullptr) {
		return;
	}
	gds->Fill(static_cast<uint64_t>(dword_index) * sizeof(uint32_t), sizeof(uint32_t), value);
}

static uint64_t BufferDescriptorSize(const ShaderBufferResource& descriptor) {
	const uint64_t records = descriptor.NumRecords();
	const uint64_t stride  = descriptor.Stride();
	return stride == 0 ? records : records * stride;
}

static bool FillSourcesDisjoint(std::span<const ShaderRecompiler::IR::DescriptorValue> sources,
                                 GuestRange destination, uint32_t output_buffer = UINT32_MAX) {
	for (uint32_t i = 0; i < sources.size(); ++i) {
		if (i == output_buffer) continue;
		const auto source = DecodeNativeDescriptor<ShaderBufferResource>(sources[i]);
		const auto bytes  = BufferDescriptorSize(source);
		if (source.Base48() < destination.End() && destination.address < source.Base48() + bytes)
			return false;
	}
	return true;
}

bool RenderExecutor::TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
                                                const CommandBuffer&          buffer) {
	const auto& program   = *input.stage.program;
	const auto& resources = input.stage.resources;
	if (resources.buffers.size() != program.info.buffers.size()) {
		EXIT("compute runtime buffer count does not match shader metadata\n");
	}
	auto& cache = buffer.GetContext().GetTextureCache();
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& resource   = program.info.buffers[i];
		const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		// A metadata resource that is also read is not proven to be a full overwrite. Execute it
		// conservatively instead of replacing the dispatch with a coarse full-surface clear.
		if (cache.IsMeta(descriptor.Base48()) && (!resource.written || resource.read)) {
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
	const auto& resources = input.stage.resources;
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
	if (descriptor.Stride() != element_size || descriptor.Format() != formats[fill.words - 1] ||
	    descriptor.SwizzleEnabled() || descriptor.IndexStride() != 0 || descriptor.AddTid() ||
	    descriptor.Base48() == 0) {
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
	const auto     size        = BufferDescriptorSize(descriptor);
	if (invocations != descriptor.NumRecords() || size == 0 || size > UINT32_MAX ||
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

bool RenderExecutor::TryConsumeComputeImageClear(const ShaderComputeInputInfo& input,
                                                CommandBuffer& command, uint32_t group_x,
                                                uint32_t group_y, uint32_t group_z, uint32_t mode) {
	const auto& program   = *input.stage.program;
	const auto& resources = input.stage.resources;
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
		cache.ClearImage(command, binding.image_id, range, clear);
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
		// Track deferred DCC state while the original dispatch writes the metadata allocation.
		cache.TrackDccFill(descriptor.Base48(), size, packed_clear);
		static std::atomic<uint32_t> logged_metadata_clears {0};
		if (logged_metadata_clears.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: metadata fill shader=0x%016" PRIx64
			     " addr=0x%016" PRIx64 " size=0x%016" PRIx64 " value=0x%08" PRIx32 "\n",
			     input.stage.program->shader_hash, descriptor.Base48(), size, packed_clear);
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
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchDirect), submit_id,
	                    thread_group_x, thread_group_y, thread_group_z, mode,
	                    sh_ctx.GetCs().cs_regs.data_addr);

	Common::LockGuard lock(m_context.GetMutex());
	FrameWorkScope    frame_work(FrameWorkKind::Dispatch);
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
	const auto& program   = *input_info.stage.program;
	const auto& resources = input_info.stage.resources;
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
		    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
	    });
	const bool                   has_sampler = !program.info.samplers.empty();
	const bool                   fullscreen_cs =
	    thread_group_x >= 200 && thread_group_y >= 100 && thread_group_z <= 1;
	const uint64_t shader_hash = program.shader_hash;
	const bool     skip_cs     = ShouldSkipComputeHash(shader_hash);
	const bool     watch_cs =
	    shader_hash == kUfcHangCsHash || skip_cs ||
	    EnvListContainsHash("KYTY_DUMP_SHADER_HASH", shader_hash);
	static std::atomic<uint32_t> dispatch_log_count {0};
	static std::atomic<uint32_t> fullscreen_log_count {0};
	std::array<uint32_t, 8> gds_words {};
	bool                    have_gds_words = false;
	if (shader_hash == kUfcHangCsHash && !skip_cs) {
		have_gds_words = ReadGdsDwords(m_context, gds_words);
		static std::atomic<uint32_t> gds_probe_count {0};
		if (gds_probe_count.fetch_add(1, std::memory_order_relaxed) < 8) {
			if (!have_gds_words) {
				LOGF("GraphicsRenderDispatchDirect: GDS probe hash=0x%016" PRIx64
				     " buffer missing or download map failed\n",
				     shader_hash);
			} else {
				LOGF("GraphicsRenderDispatchDirect: GDS probe hash=0x%016" PRIx64
				     " limit[0]=0x%08" PRIx32 " counter[1]=0x%08" PRIx32
				     " dw=[0x%08" PRIx32 ",0x%08" PRIx32 ",0x%08" PRIx32 ",0x%08" PRIx32
				     ",0x%08" PRIx32 ",0x%08" PRIx32 ",0x%08" PRIx32 ",0x%08" PRIx32 "]\n",
				     shader_hash, gds_words[0], gds_words[1], gds_words[0], gds_words[1],
				     gds_words[2], gds_words[3], gds_words[4], gds_words[5], gds_words[6],
				     gds_words[7]);
			}
		}
	}
	uint32_t       work_limit     = have_gds_words ? gds_words[0] : 0;
	const uint32_t original_limit = work_limit;
	uint32_t       work_counter   = have_gds_words ? gds_words[1] : 0;
	uint32_t       limit_cap      = 0;
	if (!skip_cs && shader_hash == kUfcHangCsHash && TryParseEnvU32("KYTY_GDS_LIMIT_CAP", limit_cap) &&
	    limit_cap > 0) {
		if (work_limit == 0 || limit_cap < work_limit) {
			work_limit = limit_cap;
		}
		LOGF("GraphicsRenderDispatchDirect: GDS limit cap hash=0x%016" PRIx64
		     " limit[0]=%u original=%u\n",
		     shader_hash, work_limit, original_limit);
	}
	uint32_t gds_chunk = shader_hash == kUfcHangCsHash ? 4u : 0u;
	if (shader_hash == kUfcHangCsHash) {
		TryParseEnvU32("KYTY_GDS_CHUNK", gds_chunk);
	}
	if (watch_cs || ((large_workgroup || has_sampler) &&
	                 dispatch_log_count.fetch_add(1, std::memory_order_relaxed) < 512) ||
	    (fullscreen_cs && fullscreen_log_count.fetch_add(1, std::memory_order_relaxed) < 32)) {
		LOGF("GraphicsRenderDispatchDirect: frame=%u shader=0x%016" PRIx64
		     " hash=0x%016" PRIx64 " groups=%ux%ux%u mode=0x%08" PRIx32 " local=%ux%ux%u "
		     "buffers=%zu textures=%zu sampled=%zu storage=%zu samplers=%zu push=%u%s\n",
		     frame_num, sh_ctx.GetCs().cs_regs.data_addr, shader_hash, thread_group_x,
		     thread_group_y, thread_group_z, mode, input_info.threads_num[0],
		     input_info.threads_num[1], input_info.threads_num[2], program.info.buffers.size(),
		     program.info.images.size(), sampled_images,
		     program.info.images.size() - sampled_images, program.info.samplers.size(),
		     program.bindings.UsesPushData()
		         ? static_cast<uint32_t>(sizeof(ShaderRecompiler::IR::PushData))
		         : 0u,
		     skip_cs ? " skip=1" : "");
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

	if (skip_cs) {
		// Rather than dropping the dispatch outright (which leaves the occlusion-cull
		// shader's Hi-Z / software-depth outputs holding stale data, so every GPU-driven
		// draw is culled and the scene renders black), stub it to "nothing occludes":
		// clear its read-write images to zero. Zero is what the real shader stores into a
		// tile with no occluder (IMAGE_STORE v=0 at tile setup), and with Frostbite's
		// reverse-Z that is the far plane, so downstream visibility tests pass everything.
		auto&    cache        = buffer.GetContext().GetTextureCache();
		uint32_t cleared      = 0;
		for (uint32_t i = 0; i < program.info.images.size() && i < resources.images.size(); i++) {
			const auto& resource = program.info.images[i];
			if (!resource.written) {
				continue;
			}
			const auto binding = ResolveTexture(resource, resources.images[i]);
			if (!binding.image_id) {
				continue;
			}
			std::scoped_lock lock {cache.m_lock};
			const auto&      image = cache.GetImage(binding.image_id);
			if (image.backing.image == nullptr || image.depth_id ||
			    image.info.resources.levels == 0) {
				continue;
			}
			const bool is_depth = image.info.IsDepth();
			const vk::ImageSubresourceRange range {
			    is_depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor, 0,
			    image.info.resources.levels, 0, image.backing.layers};
			vk::ClearValue clear {};
			if (is_depth) {
				clear.depthStencil = vk::ClearDepthStencilValue {0.0f, 0};
			} else {
				clear.color = vk::ClearColorValue {std::array<float, 4> {0.0f, 0.0f, 0.0f, 0.0f}};
			}
			cache.ClearImage(buffer, binding.image_id, range, clear);
			cleared++;
		}
		LOGF("GraphicsRenderDispatchDirect: stubbing watched compute shader hash=0x%016" PRIx64
		     " addr=0x%016" PRIx64 " groups=%ux%ux%u local=%ux%ux%u cleared_images=%u\n",
		     shader_hash, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x, thread_group_y,
		     thread_group_z, input_info.threads_num[0], input_info.threads_num[1],
		     input_info.threads_num[2], cleared);
		ResetBindings();
		return;
	}

	buffer.EndRendering();
	auto& pipeline =
	    m_context.GetPipelineCache().CreateComputePipeline(input_info, compute_program);
	bool has_storage_writes = HasShaderBufferWrites(input_info.stage);
	has_storage_writes =
	    std::any_of(program.info.images.begin(), program.info.images.end(),
	                [](const auto& image) {
		                return image.written &&
		                       image.resource_class ==
		                           ShaderRecompiler::IR::ImageResourceClass::Storage;
	                }) ||
	    has_storage_writes;
	const bool uses_gds =
	    ShaderRecompiler::IR::FindBinding(
	        program.bindings, ShaderRecompiler::IR::DescriptorBindingKind::Gds) != nullptr;
	// Read-only dispatches need ordering, but no memory visibility operation.
	const bool writes_memory =
	    has_storage_writes || program.info.uses_dma || uses_gds ||
	    std::any_of(program.info.buffers.begin(), program.info.buffers.end(),
	                [](const auto& resource) { return resource.written || resource.atomic; }) ||
	    std::any_of(program.info.images.begin(), program.info.images.end(),
	                [](const auto& resource) { return resource.written || resource.atomic; });

	auto record_dispatch = [&]() {
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
		if (has_storage_writes) {
			// A host fence used to serialize every dispatch. Preserve its read-before-write
			// ordering while allowing the queue to execute asynchronously.
			ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
		}
		vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
		vk_buffer.dispatch(thread_group_x, thread_group_y, thread_group_z);
		ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader, writes_memory);
		ResetBindings();
	};

	auto*      gds         = m_context.GetBufferCache().GetGdsBuffer();
	const bool chunk_queue = shader_hash == kUfcHangCsHash && gds_chunk > 0 &&
	                         work_limit > gds_chunk && gds != nullptr && gds->Handle() != nullptr;
	if (chunk_queue) {
		uint32_t start = work_counter < work_limit ? work_counter : 0;
		const uint32_t chunk_count =
		    (work_limit - start + gds_chunk - 1u) / gds_chunk;
		LOGF("GraphicsRenderDispatchDirect: GDS chunk begin hash=0x%016" PRIx64
		     " items=%u start=%u chunk=%u submits=%u original_limit=%u\n",
		     shader_hash, work_limit, start, gds_chunk, chunk_count, original_limit);
		uint32_t chunk_index = 0;
		while (start < work_limit) {
			const uint32_t end = std::min(start + gds_chunk, work_limit);
			FillGdsDword(gds, 0, end);
			FillGdsDword(gds, 1, start);
			const auto chunk_t0 = std::chrono::steady_clock::now();
			record_dispatch();
			m_context.GetCommandScheduler().Finish("gds-chunk");
			const double chunk_ms = std::chrono::duration<double, std::milli>(
			                            std::chrono::steady_clock::now() - chunk_t0)
			                            .count();
			++chunk_index;
			if (chunk_index <= 8 || chunk_index == chunk_count || (chunk_index % 32u) == 0) {
				LOGF("GraphicsRenderDispatchDirect: GDS chunk %u/%u processed [%u,%u) "
				     "ms=%.1f\n",
				     chunk_index, chunk_count, start, end, chunk_ms);
			}
			start = end;
		}
		FillGdsDword(gds, 0, original_limit != 0 ? original_limit : work_limit);
		FillGdsDword(gds, 1, work_limit);
		LOGF("GraphicsRenderDispatchDirect: GDS chunk done hash=0x%016" PRIx64
		     " chunks=%u items=%u\n",
		     shader_hash, chunk_index, work_limit);
	} else {
		if (limit_cap > 0) {
			FillGdsDword(gds, 0, work_limit);
		}
		record_dispatch();
	}
}

} // namespace Libs::Graphics
