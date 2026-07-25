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
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/objects/label.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/descriptorCache.h"
#include "graphics/host_gpu/renderer/descriptors.h"
#include "graphics/host_gpu/renderer/framebufferCache.h"
#include "graphics/host_gpu/renderer/imageInfo.h"
#include "graphics/host_gpu/renderer/pipelineCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/shaderSubgroup.h"
#include "graphics/host_gpu/transfer.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
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

bool ResolveHtileClearTarget(const HW::DepthRenderTarget& z, uint64_t descriptor_size,
                             HtileClearTarget& resolved) {
	resolved = {};
	const bool has_stencil =
	    z.stencil_info.format != Prospero::GpuEnumValue(Prospero::StencilFormat::kInvalid);
	const auto* depth_policy = FindDepthFormatPolicy(z.z_info.format);
	const bool  supported_depth_state =
	    z.z_info.tile_surface_enable && depth_policy != nullptr && z.z_info.tile_mode_index == 0 &&
	    z.z_info.num_samples <= 3 && z.z_info.zrange_precision <= 1 && !z.z_info.expclear_enabled &&
	    !z.z_info.embedded_sample_locations && !z.z_info.partially_resident &&
	    z.z_info.num_mip_levels == 0 && z.z_info.plane_compression == 0 &&
	    z.depth_view.current_mip_level == 0 && z.depth_view.slice_start == 0 &&
	    z.depth_view.slice_max == 0 && z.depth_info.addr5_swizzle_mask == 0 &&
	    z.depth_info.array_mode == 0 && z.depth_info.pipe_config == 0 &&
	    z.depth_info.bank_width == 0 && z.depth_info.bank_height == 0 &&
	    z.depth_info.macro_tile_aspect == 0 && z.depth_info.num_banks == 0;
	const bool supported_stencil_state =
	    z.stencil_info.tile_mode_index == 0 && z.stencil_info.tile_split == 0 &&
	    !z.stencil_info.expclear_enabled &&
	    depth_htile_stencil_acceleration_compatible(has_stencil, true,
	                                                z.stencil_info.tile_stencil_disable) &&
	    !z.stencil_info.texture_compatible_stencil && !z.stencil_info.partially_resident &&
	    (!has_stencil ||
	     (z.stencil_info.format == Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt) &&
	      !z.depth_view.stencil_write_disable));
	const bool supported_htile_state =
	    z.htile_surface.linear == 0 && z.htile_surface.full_cache == 0 &&
	    z.htile_surface.htile_uses_preload_win == 0 && z.htile_surface.preload == 0 &&
	    z.htile_surface.prefetch_width == 0 && z.htile_surface.prefetch_height == 0 &&
	    z.htile_surface.dst_outside_zero_to_one == 0;
	const bool supported_addresses =
	    z.z_read_base_addr != 0 && z.z_write_base_addr == z.z_read_base_addr &&
	    (z.z_read_base_addr & 0xffffu) == 0 &&
	    (has_stencil ? z.stencil_read_base_addr != 0 &&
	                       z.stencil_write_base_addr == z.stencil_read_base_addr &&
	                       (z.stencil_read_base_addr & 0xffffu) == 0
	                 : z.stencil_read_base_addr == 0 && z.stencil_write_base_addr == 0) &&
	    z.htile_data_base_addr != 0 && (z.htile_data_base_addr & 0x7fffu) == 0 &&
	    descriptor_size != 0 && (descriptor_size & 0x7fffu) == 0 &&
	    z.htile_data_base_addr < TRACKER_ADDRESS_SIZE &&
	    descriptor_size <= TRACKER_ADDRESS_SIZE - z.htile_data_base_addr;
	if (!supported_depth_state || !supported_stencil_state || !supported_htile_state ||
	    !supported_addresses) {
		return false;
	}

	const bool size_xy_valid = z.size.valid;
	const bool wh_valid      = z.width_height_valid && z.width != 0 && z.height != 0;
	if (!size_xy_valid && !wh_valid) {
		// Prospero emits an exact full-surface metadata descriptor even
		// when the compute context omits depth extent registers. Admit only that wholly absent
		// layout state; partially programmed layouts remain unsupported.
		const bool descriptor_backed_state =
		    !z.size.valid && z.size.x_max == 0 && z.size.y_max == 0 && !z.width_height_valid &&
		    z.width == 0 && z.height == 0 && !z.pitch_height_valid && z.pitch_div8_minus1 == 0 &&
		    z.height_div8_minus1 == 0 && z.slice_div64_minus1 == 0;
		if (!descriptor_backed_state) {
			return false;
		}
		resolved = {.address = z.htile_data_base_addr, .size = descriptor_size};
		return true;
	}
	const uint32_t width  = size_xy_valid ? static_cast<uint32_t>(z.size.x_max) + 1u : z.width;
	const uint32_t height = size_xy_valid ? static_cast<uint32_t>(z.size.y_max) + 1u : z.height;
	if (width > 16384 || height > 16384 ||
	    (size_xy_valid && wh_valid && (width != z.width || height != z.height)) ||
	    (!z.pitch_height_valid &&
	     (z.pitch_div8_minus1 != 0 || z.height_div8_minus1 != 0 || z.slice_div64_minus1 != 0))) {
		return false;
	}
	TileSizeAlign htile_size {};
	if (!TileGetHtileSize(width, height, htile_size) || htile_size.size != descriptor_size) {
		return false;
	}
	resolved = {.address = z.htile_data_base_addr, .size = htile_size.size};
	return true;
}

static void ValidateFullHtileClearDispatch(const ShaderComputeInputInfo& input,
                                           const ShaderBufferResource& metadata, uint32_t group_x,
                                           uint32_t group_y, uint32_t group_z, uint32_t mode) {
	const bool thread_dimensions = input.dispatch_thread_dimensions;
	const bool supported_shape =
	    input.threads_num[0] == 64 && input.threads_num[1] == 1 && input.threads_num[2] == 1 &&
	    group_x != 0 && group_y == 1 && group_z == 1 && input.group_id[0] && !input.group_id[1] &&
	    !input.group_id[2] && input.thread_ids_num == 1 && input.wave_size == 32 &&
	    !input.tg_size_en && mode == (thread_dimensions ? 0x61u : 0x41u);
	const bool dimensions_match =
	    thread_dimensions
	        ? input.dispatch_threads_num[0] == group_x && input.dispatch_threads_num[1] == 1 &&
	              input.dispatch_threads_num[2] == 1 && group_x % input.threads_num[0] == 0
	        : input.dispatch_threads_num[0] == 0 && input.dispatch_threads_num[1] == 0 &&
	              input.dispatch_threads_num[2] == 0;
	const uint64_t launched_threads =
	    thread_dimensions ? group_x : static_cast<uint64_t>(group_x) * input.threads_num[0];
	if (!supported_shape || !dimensions_match || metadata.Stride() == 0 ||
	    launched_threads != metadata.NumRecords()) {
		EXIT("HTile compute clear does not cover the complete metadata surface\n");
	}
}

static bool TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
                                       const RenderCommandBuffer& buffer, uint32_t group_x,
                                       uint32_t group_y, uint32_t group_z, uint32_t mode) {
	const auto& ctx       = buffer.GetRegisters();
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	if (resources.buffers.size() != program.info.buffers.size()) {
		EXIT("compute runtime buffer count does not match shader metadata\n");
	}
	const auto&                 z                   = ctx.GetDepthRenderTarget();
	const uint64_t              meta_addr           = z.htile_data_base_addr;
	auto&                       cache               = GetRenderContext().GetTextureCache();
	uint32_t                    current_references  = 0;
	uint32_t                    registered_writes   = 0;
	uint64_t                    described_meta_size = 0;
	HtileClearTarget            registered_target {};
	TextureCache::MetaRangeInfo registered_meta {};
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& resource   = program.info.buffers[i];
		const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		const auto  descriptor_size = BufferDescriptorSize(descriptor);
		if (meta_addr != 0 && descriptor.Base48() == meta_addr) {
			current_references++;
			described_meta_size = descriptor_size;
		}
		// An exact registered metadata range remains
		// identifiable even when it is no longer the currently bound depth target.
		TextureCache::MetaRangeInfo resolved_meta {};
		if (resource.written &&
		    cache.ResolveMetaRange(descriptor.Base48(), descriptor_size, resolved_meta)) {
			registered_writes++;
			registered_target = {.address = descriptor.Base48(), .size = descriptor_size};
			registered_meta   = resolved_meta;
		}
	}
	if (current_references == 0 && registered_writes == 0) {
		return false;
	}
	if (current_references > 1 || (current_references == 0 && registered_writes > 1)) {
		EXIT("HTile clear has ambiguous metadata descriptors: current=%u registered=%u\n",
		     current_references, registered_writes);
	}
	HtileClearTarget target {};
	if (current_references != 0) {
		if (!ResolveHtileClearTarget(z, described_meta_size, target)) {
			EXIT("unsupported HTile compute-clear target state: current=%u registered=%u "
			     "meta=0x%016" PRIx64 "+0x%016" PRIx64 " depth=0x%016" PRIx64 "/0x%016" PRIx64
			     " stencil=0x%016" PRIx64 "/0x%016" PRIx64
			     " extent=%d:%ux%u wh=%d:%ux%u pitch=%d:%u/%u/%u zfmt=%u sfmt=%u samples=%u\n",
			     current_references, registered_writes, meta_addr, described_meta_size,
			     z.z_read_base_addr, z.z_write_base_addr, z.stencil_read_base_addr,
			     z.stencil_write_base_addr, z.size.valid, static_cast<uint32_t>(z.size.x_max) + 1u,
			     static_cast<uint32_t>(z.size.y_max) + 1u, z.width_height_valid, z.width, z.height,
			     z.pitch_height_valid, z.pitch_div8_minus1, z.height_div8_minus1,
			     z.slice_div64_minus1, z.z_info.format, z.stencil_info.format,
			     z.z_info.num_samples);
		}
		cache.RegisterMeta(target.address, target.size);
		if (!cache.ResolveMetaRange(target.address, target.size, registered_meta)) {
			EXIT("failed to resolve registered HTile compute-clear range\n");
		}
	} else {
		target = registered_target;
	}
	GetRenderContext().GetBufferCache().ValidateGpuAccess(target.address, target.size, false, true);

	uint32_t             metadata_writes = 0;
	ShaderBufferResource metadata_descriptor {};
	if (!program.info.images.empty() || !program.info.samplers.empty() ||
	    !program.info.addresses.empty()) {
		EXIT("HTile clear with non-buffer resources is unsupported: images=%zu samplers=%zu "
		     "addresses=%zu\n",
		     program.info.images.size(), program.info.samplers.size(),
		     program.info.addresses.size());
	}
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& resource   = program.info.buffers[i];
		const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		const auto  descriptor_size = BufferDescriptorSize(descriptor);
		if (descriptor.Base48() == target.address) {
			if (!resource.written || resource.read || resource.atomic ||
			    descriptor_size != target.size || descriptor.SwizzleEnabled() ||
			    descriptor.IndexStride() != 0 || descriptor.AddTid() ||
			    resource.packed_stride != descriptor.PackedStride()) {
				EXIT("unsupported HTile compute metadata access\n");
			}
			metadata_descriptor = descriptor;
			metadata_writes++;
			continue;
		}
		if (resource.written || !resource.read || resource.atomic || descriptor.Base48() == 0 ||
		    descriptor_size == 0 ||
		    cache.QueryRegion(descriptor.Base48(), descriptor_size).metadata_pages) {
			EXIT("unsupported HTile clear side-buffer access\n");
		}
		GetRenderContext().GetBufferCache().ValidateGpuAccess(descriptor.Base48(), descriptor_size,
		                                                      true, false);
	}
	if (metadata_writes != 1) {
		EXIT("HTile clear requires exactly one write-only metadata buffer, writes=%u\n",
		     metadata_writes);
	}
	ValidateFullHtileClearDispatch(input, metadata_descriptor, group_x, group_y, group_z, mode);
	const bool recorded = registered_meta.full ? cache.ClearMeta(registered_meta.metadata_address)
	                                           : cache.TouchMeta(registered_meta.metadata_address,
	                                                             registered_meta.slice, true);
	if (!recorded) {
		EXIT("failed to record HTile compute clear\n");
	}
	return true;
}

bool ResolveComputeImageClear(const ShaderComputeInputInfo& input, uint32_t group_x,
                              uint32_t group_y, uint32_t group_z, uint32_t mode,
                              ShaderBufferResource& resolved_descriptor, uint32_t& resolved_clear,
                              uint64_t& resolved_size) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	if (program.info.buffers.size() != 1 || resources.buffers.size() != 1 ||
	    !program.info.images.empty() || !program.info.samplers.empty() ||
	    !program.info.addresses.empty() || !resources.images.empty() ||
	    !resources.samplers.empty() || !resources.addresses.empty()) {
		return false;
	}
	const auto& resource   = program.info.buffers.front();
	const auto& raw        = resources.buffers.front();
	const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(raw);
	if (!resource.formatted || !resource.written || resource.read || resource.atomic ||
	    resource.scalar || resource.max_byte_extent != 16 || descriptor.Stride() != 16 ||
	    descriptor.Format() != Prospero::GpuEnumValue(Prospero::BufferFormat::k32_32_32_32UInt) ||
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
	    input.thread_ids_num == 1 && input.wave_size == 32 && !input.tg_size_en && mode == 0x61u &&
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
	auto& cache = GetRenderContext().GetTextureCache();
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

void RenderDispatchDirect(uint64_t submit_id, RenderCommandBuffer& buffer, uint32_t thread_group_x,
                          uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode) {
	EXIT_IF(buffer.IsInvalid());
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchDirect), submit_id,
	                    thread_group_x, thread_group_y, thread_group_z, mode,
	                    sh_ctx.GetCs().cs_regs.data_addr);

	Common::LockGuard lock(GetRenderContext().GetMutex());

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

	ShaderComputeInputInfo    input_info {};
	std::span<const uint32_t> cs_shader;
	if (!ShaderCompileInfoCS(cs_regs, sh_regs, input_info, cs_shader)) {
		// The compiler already logged the specific reason (bounded). Drop this dispatch rather
		// than crashing the process so the rest of the frame can still be produced.
		static std::atomic<uint32_t> logged_failures {0};
		if (logged_failures.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: ShaderCompileInfoCS failed for CS shader "
			     "0x%016" PRIx64 ", dispatch skipped\n",
			     cs_regs.cs_regs.data_addr);
		}
		return;
	}

	const bool use_thread_dimensions = (mode & DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	if (use_thread_dimensions) {
		input_info.dispatch_thread_dimensions = true;
		input_info.dispatch_threads_num[0]    = thread_group_x;
		input_info.dispatch_threads_num[1]    = thread_group_y;
		input_info.dispatch_threads_num[2]    = thread_group_z;
	}

	const uint32_t frame_num = GraphicsRunGetFrameNum();
	const bool     large_workgroup =
	    (input_info.threads_num[0] * input_info.threads_num[1] * input_info.threads_num[2] >= 512);
	const auto& program   = *input_info.stage.program;
	const auto& resources = *input_info.stage.resources;
	if (TryConsumeComputeMetaClear(input_info, buffer, thread_group_x, thread_group_y,
	                               thread_group_z, mode)) {
		return;
	}
	if (TryConsumeComputeImageClear(input_info, buffer, thread_group_x, thread_group_y,
	                                thread_group_z, mode)) {
		return;
	}
	const auto sampled_images = std::count_if(
	    program.info.images.begin(), program.info.images.end(), [](const auto& image) {
		    return image.kind == ShaderRecompiler::IR::ResourceKind::Image ||
		           image.kind == ShaderRecompiler::IR::ResourceKind::ImageUint;
	    });
	const bool                   has_sampler = !program.info.samplers.empty();
	static std::atomic<uint32_t> dispatch_log_count {0};
	if ((large_workgroup || has_sampler) &&
	    dispatch_log_count.fetch_add(1, std::memory_order_relaxed) < 512) {
		LOGF("GraphicsRenderDispatchDirect: frame=%u shader=0x%016" PRIx64
		     " groups=%ux%ux%u mode=0x%08" PRIx32 " local=%ux%ux%u "
		     "buffers=%zu textures=%zu sampled=%zu storage=%zu samplers=%zu push=%u\n",
		     frame_num, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x, thread_group_y,
		     thread_group_z, mode, input_info.threads_num[0], input_info.threads_num[1],
		     input_info.threads_num[2], program.info.buffers.size(), program.info.images.size(),
		     sampled_images, program.info.images.size() - sampled_images,
		     program.info.samplers.size(), program.bindings.push_constant_size);
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& buffer = program.info.buffers[i];
			const auto  r      = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
			LOGF("  CS buffer[%u]: source=%u usage=%s addr=0x%012" PRIx64
			     " stride=%u records=%u format=%u\n",
			     i, buffer.source, buffer.written ? "read-write" : "read-only", r.Base48(),
			     r.Stride(), r.NumRecords(), r.Format());
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
			     r.Base40(), static_cast<uint32_t>(r.Type()), r.Format(),
			     static_cast<uint32_t>(r.Width5()) + 1u, static_cast<uint32_t>(r.Height5()) + 1u,
			     static_cast<uint32_t>(r.Depth()) + 1u,
			     std::max<uint32_t>(static_cast<uint32_t>(r.LastLevel()),
			                        static_cast<uint32_t>(r.MaxMip())) +
			         1u,
			     r.TileMode());
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

	for (;;) {
		const auto recording_generation = buffer.GetRecordingGeneration();
		auto       vk_buffer            = buffer.Handle();
		auto&      pipeline = GetRenderContext().GetPipelineCache().CreateComputePipeline(
		    input_info, sh_ctx.GetCs(), cs_shader);

		vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);

		BindDescriptors(submit_id, buffer, vk::PipelineBindPoint::eCompute,
		                pipeline.pipeline_layout, input_info.stage,
		                vk::ShaderStageFlagBits::eCompute, DescriptorCache::Stage::Compute);
		if (buffer.GetRecordingGeneration() != recording_generation) {
			continue;
		}

		vk_buffer.dispatch(thread_group_x, thread_group_y, thread_group_z);

		bool has_storage_writes = HasShaderBufferWrites(input_info.stage);
		has_storage_writes =
		    std::any_of(
		        program.info.images.begin(), program.info.images.end(),
		        [](const auto& image) {
			        return image.written &&
			               (image.kind == ShaderRecompiler::IR::ResourceKind::StorageImage ||
			                image.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint);
		        }) ||
		    has_storage_writes;
		if (has_storage_writes) {
			ShaderWriteBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
		}
		break;
	}
}

} // namespace Libs::Graphics
