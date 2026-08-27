#include "graphics/host_gpu/renderer/depthRenderTarget.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>

namespace Libs::Graphics {

[[noreturn]] static void DepthFatal(const char* format, ...) {
	std::fputs("Depth target fatal: ", stderr);
	va_list args;
	va_start(args, format);
	std::vfprintf(stderr, format, args);
	va_end(args);
	std::fputc('\n', stderr);
	std::fflush(stderr);
	EXIT("unsupported render state; details were printed above\n");
}

static vk::StencilOp ConvertStencilOp(uint8_t value, uint8_t write_mask, uint8_t op_value) {
	if (write_mask == 0) {
		return vk::StencilOp::eKeep;
	}
	switch (static_cast<Prospero::StencilOp>(value)) {
		case Prospero::StencilOp::kKeep: return vk::StencilOp::eKeep;
		case Prospero::StencilOp::kZero: return vk::StencilOp::eZero;
		case Prospero::StencilOp::kReplaceTest:
		case Prospero::StencilOp::kReplaceOp: return vk::StencilOp::eReplace;
		case Prospero::StencilOp::kAddClamp: return vk::StencilOp::eIncrementAndClamp;
		case Prospero::StencilOp::kSubClamp: return vk::StencilOp::eDecrementAndClamp;
		case Prospero::StencilOp::kInvert: return vk::StencilOp::eInvert;
		case Prospero::StencilOp::kAddWrap: return vk::StencilOp::eIncrementAndWrap;
		case Prospero::StencilOp::kSubWrap: return vk::StencilOp::eDecrementAndWrap;
		case Prospero::StencilOp::kXor:
			if ((write_mask & op_value) == 0) {
				return vk::StencilOp::eKeep;
			}
			if ((write_mask & ~op_value) != 0) {
				DepthFatal("unsupported stencil XOR operands: write mask=0x%02" PRIx8
				           ", operation value=0x%02" PRIx8,
				           write_mask, op_value);
			}
			return vk::StencilOp::eInvert;
		default: DepthFatal("unsupported stencil operation: 0x%02" PRIx8, value);
	}
}

static bool UsesStencilOpValue(uint8_t fail, uint8_t pass, uint8_t depth_fail) {
	constexpr auto replace_op = static_cast<uint8_t>(Prospero::StencilOp::kReplaceOp);
	return fail == replace_op || pass == replace_op || depth_fail == replace_op;
}

[[nodiscard]] static vk::Format ResolveHostDepthAttachmentFormat(const CommandBuffer&     buffer,
                                                                 const DepthFormatPolicy& policy,
                                                                 bool     has_stencil,
                                                                 uint32_t samples) {
	auto&      graphics         = buffer.GetGraphics();
	const auto required_samples = vulkan_sample_count(samples);
	const auto supports         = [&](vk::Format format) {
		vk::ImageFormatProperties properties {};
		return format != vk::Format::eUndefined &&
		       graphics.GetImageFormatProperties(
		           format, vk::ImageType::e2D, vk::ImageTiling::eOptimal, DepthTargetImageUsage(),
		           vk::ImageCreateFlags {}, &properties) == vk::Result::eSuccess &&
		       static_cast<bool>(properties.sampleCounts & required_samples);
	};
	if (!has_stencil) {
		return supports(policy.depth_attachment_format) ? policy.depth_attachment_format
		                                                : vk::Format::eUndefined;
	}
	for (const auto format: policy.stencil_attachment_formats) {
		if (supports(format)) {
			return format;
		}
	}
	return vk::Format::eUndefined;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RenderExecutor::ResolveRenderDepthTarget(uint64_t submit_id, CommandBuffer& buffer,
                                              RenderDepthInfo& r) {
	KYTY_PROFILER_FUNCTION();
	(void)submit_id;
	const auto& hw          = buffer.GetRegisters();
	const auto& z           = hw.GetDepthRenderTarget();
	const auto& rc          = hw.GetRenderControl();
	const auto& dc          = hw.GetDepthControl();
	const auto& sc          = hw.GetStencilControl();
	const auto& sm          = hw.GetStencilMask();
	const bool  has_stencil = z.stencil_info.format != Prospero::StencilFormat::kInvalid;
	const bool  depth_active =
	    dc.z_enable || dc.z_write_enable || dc.depth_bounds_enable || rc.depth_clear_enable;
	const bool stencil_active = has_stencil && (dc.stencil_enable || rc.stencil_clear_enable);
	if (!depth_active && !stencil_active) {
		return;
	}
	if (!z.z_info.HasValidTextureCompatibility() ||
	    !z.stencil_info.HasValidTextureCompatibility()) {
		DepthFatal("invalid PS5 depth texture-compatibility encoding");
	}
	const bool attachment_unbound =
	    z.z_info.format == Prospero::DepthFormat::kInvalid &&
	    z.stencil_info.format == Prospero::StencilFormat::kInvalid && z.z_info.num_samples == 0 &&
	    z.z_info.texture_compatibility == Prospero::TextureCompatiblePlaneCompression::kDisable &&
	    !z.z_info.expclear_enabled && !z.z_info.partially_resident && z.z_info.max_mip_level == 0 &&
	    z.stencil_info.texture_compatibility == Prospero::TextureCompatibleStencil::kDisable &&
	    !z.stencil_info.expclear_enabled && !z.stencil_info.partially_resident &&
	    z.depth_view.slice_start == 0 && z.depth_view.slice_max == 0 &&
	    z.depth_view.current_mip_level == 0 && !z.depth_view.depth_write_disable &&
	    !z.depth_view.stencil_write_disable && z.depth_info.addr5_swizzle_mask == 0 &&
	    z.depth_info.array_mode == 0 && z.depth_info.pipe_config == 0 &&
	    z.depth_info.bank_width == 0 && z.depth_info.bank_height == 0 &&
	    z.depth_info.macro_tile_aspect == 0 && z.depth_info.num_banks == 0 &&
	    z.htile_surface.linear == 0 && z.htile_surface.full_cache == 0 &&
	    z.htile_surface.htile_uses_preload_win == 0 && z.htile_surface.preload == 0 &&
	    z.htile_surface.prefetch_width == 0 && z.htile_surface.prefetch_height == 0 &&
	    z.htile_surface.dst_outside_zero_to_one == 0 && z.z_read_base_addr == 0 &&
	    z.z_write_base_addr == 0 && z.stencil_read_base_addr == 0 &&
	    z.stencil_write_base_addr == 0 && z.htile_data_base_addr == 0 &&
	    // DB_DEPTH_SIZE_XY is independent state and may remain programmed after the attachment
	    // formats and addresses are unbound. A zero encoding is the valid 1x1 value, so its
	    // presence alone must not manufacture a depth attachment.
	    !z.z_info.htile_acceleration && !z.width_height_valid && !z.pitch_height_valid &&
	    z.size.x_max == 0 && z.size.y_max == 0 && z.pitch_div8_minus1 == 0 &&
	    z.height_div8_minus1 == 0 && z.slice_div64_minus1 == 0 && z.width == 0 && z.height == 0;
	if (attachment_unbound) {
		static std::atomic_bool logged = false;
		if (!logged.exchange(true, std::memory_order_relaxed)) {
			LOGF("DepthTarget: ignoring enabled depth/stencil state without a bound attachment\n");
		}
		return;
	}
	const bool has_htile = z.z_info.htile_acceleration;
	const auto samples   = render_sample_count(z.z_info.num_samples);
	if (samples == 0) {
		DepthFatal("unsupported depth fragment count: %u", z.z_info.num_samples);
	}
	const bool htile_stencil_compat = depth_htile_stencil_acceleration_compatible(
	    has_stencil, has_htile, z.stencil_info.htile_stencil_disabled);
	const auto view = ResolveTargetViewInfo(z.depth_view.slice_start, z.depth_view.slice_max);
	switch (view.type) {
		case TargetViewType::Image2D:
		case TargetViewType::Image2DArray: break;
		case TargetViewType::Unsupported:
			DepthFatal("invalid depth view: base=%u last=%u", z.depth_view.slice_start,
			           z.depth_view.slice_max);
	}
	if (rc.copy_centroid || rc.copy_sample != 0 || z.z_info.expclear_enabled ||
	    z.stencil_info.expclear_enabled || z.z_info.partially_resident ||
	    z.stencil_info.partially_resident || z.z_info.max_mip_level != 0 ||
	    z.depth_view.current_mip_level != 0 || z.depth_info.addr5_swizzle_mask != 0 ||
	    z.depth_info.array_mode != 0 || z.depth_info.pipe_config != 0 ||
	    z.depth_info.bank_width != 0 || z.depth_info.bank_height != 0 ||
	    z.depth_info.macro_tile_aspect != 0 || z.depth_info.num_banks != 0 ||
	    z.htile_surface.linear != 0 || z.htile_surface.full_cache != 0 ||
	    z.htile_surface.htile_uses_preload_win != 0 || z.htile_surface.preload != 0 ||
	    z.htile_surface.prefetch_width != 0 || z.htile_surface.prefetch_height != 0 ||
	    z.htile_surface.dst_outside_zero_to_one != 0 || z.z_read_base_addr == 0 ||
	    (!z.depth_view.depth_write_disable && z.z_write_base_addr != z.z_read_base_addr) ||
	    (z.z_read_base_addr & 0xffffu) != 0 ||
	    dc.zfunc > static_cast<uint8_t>(vk::CompareOp::eAlways)) {
		DepthFatal("unsupported depth register state");
	}
	if (has_stencil) {
		if (z.stencil_info.format != Prospero::StencilFormat::k8UInt || !htile_stencil_compat ||
		    z.stencil_read_base_addr == 0 ||
		    (!z.depth_view.stencil_write_disable &&
		     z.stencil_write_base_addr != z.stencil_read_base_addr) ||
		    (z.stencil_read_base_addr & 0xffffu) != 0) {
			DepthFatal("unsupported stencil attachment state");
		}
	} else if (z.stencil_read_base_addr != 0 || z.stencil_write_base_addr != 0 ||
	           !htile_stencil_compat ||
	           z.stencil_info.texture_compatibility !=
	               Prospero::TextureCompatibleStencil::kDisable) {
		DepthFatal("stencil state without an active stencil attachment");
	}
	if (has_htile) {
		if (z.htile_data_base_addr == 0 || (z.htile_data_base_addr & 0x7fffu) != 0) {
			DepthFatal("invalid HTile metadata address");
		}
		if (z.depth_view.slice_max >= 32) {
			DepthFatal("HTile clear tracking supports at most 32 slices");
		}
	}
	const bool size_xy_valid = z.size.valid;
	const bool wh_valid      = z.width_height_valid && z.width != 0 && z.height != 0;
	if (!size_xy_valid && !wh_valid) {
		DepthFatal("missing depth extent");
	}
	const uint32_t width  = size_xy_valid ? static_cast<uint32_t>(z.size.x_max) + 1u : z.width;
	const uint32_t height = size_xy_valid ? static_cast<uint32_t>(z.size.y_max) + 1u : z.height;
	if (width > 16384 || height > 16384 ||
	    (size_xy_valid && wh_valid && (width != z.width || height != z.height)) ||
	    (!z.pitch_height_valid &&
	     (z.pitch_div8_minus1 != 0 || z.height_div8_minus1 != 0 || z.slice_div64_minus1 != 0))) {
		DepthFatal("inconsistent depth extent or encoded layout");
	}
	const auto* policy = FindDepthFormatPolicy(z.z_info.format);
	if (policy == nullptr) {
		DepthFatal("unsupported depth/stencil format pair");
	}
	const auto ideal_format = DepthAttachmentFormat(*policy, has_stencil);
	r.format = ResolveHostDepthAttachmentFormat(buffer, *policy, has_stencil, samples);
	if (r.format == vk::Format::eUndefined) {
		DepthFatal("no host depth/stencil format supports required usage for %s",
		           VulkanToString(ideal_format).c_str());
	}
	const auto     guest_format = policy->guest_format;
	const uint32_t bytes        = policy->bytes_per_element;
	const auto     pitch        = TileGetDepthPitch(width, bytes, z.z_info.num_samples);
	if (z.pitch_height_valid && ((static_cast<uint64_t>(z.pitch_div8_minus1) + 1u) * 8u != pitch ||
	                             (static_cast<uint64_t>(z.height_div8_minus1) + 1u) * 8u !=
	                                 ((static_cast<uint64_t>(height) + 7u) & ~7ull))) {
		DepthFatal("encoded depth pitch or height mismatch");
	}
	TileSizeAlign depth_size {};
	TileSizeAlign stencil_size {};
	TileSizeAlign htile_size {};
	if (!TileGetDepthSize(width, height, 0, z.z_info.format, z.stencil_info.format, has_htile,
	                      stencil_size, htile_size, depth_size, z.z_info.num_samples) ||
	    depth_size.align != 65536 || depth_size.size == 0 ||
	    (has_stencil != (stencil_size.align == 65536 && stencil_size.size != 0)) ||
	    (has_htile != (htile_size.align == 32768 && htile_size.size != 0))) {
		DepthFatal("unsupported depth/stencil/HTile footprint");
	}
	if (z.pitch_height_valid &&
	    (static_cast<uint64_t>(z.slice_div64_minus1) + 1u) * 64u != depth_size.size) {
		DepthFatal("depth footprint mismatch: extent=%ux%u pitch=%u expected=0x%016" PRIx64
		           " align=0x%016" PRIx64 " encoded_valid=%u encoded=0x%016" PRIx64,
		           width, height, pitch, depth_size.size, depth_size.align,
		           z.pitch_height_valid ? 1u : 0u,
		           (static_cast<uint64_t>(z.slice_div64_minus1) + 1u) * 64u);
	}
	if (depth_size.size > UINT64_MAX / view.image_layers ||
	    stencil_size.size > UINT64_MAX / view.image_layers ||
	    htile_size.size > UINT64_MAX / view.image_layers) {
		DepthFatal("layered depth footprint overflow");
	}
	const auto depth_backing_size   = depth_size.size * view.image_layers;
	const auto stencil_backing_size = stencil_size.size * view.image_layers;
	const auto htile_backing_size   = htile_size.size * view.image_layers;
	if (depth_backing_size > TRACKER_ADDRESS_SIZE - z.z_read_base_addr ||
	    (has_stencil && stencil_backing_size > TRACKER_ADDRESS_SIZE - z.stencil_read_base_addr) ||
	    (has_htile && htile_backing_size > TRACKER_ADDRESS_SIZE - z.htile_data_base_addr)) {
		DepthFatal("layered depth backing range is invalid");
	}
	r.htile                   = has_htile;
	r.width                   = width;
	r.height                  = height;
	r.samples                 = samples;
	r.depth_buffer_size       = depth_backing_size;
	r.depth_buffer_vaddr      = z.z_read_base_addr;
	r.stencil_buffer_size     = has_stencil ? stencil_backing_size : 0;
	r.stencil_buffer_vaddr    = has_stencil ? z.stencil_read_base_addr : 0;
	r.htile_buffer_size       = has_htile ? htile_backing_size : 0;
	r.htile_buffer_vaddr      = has_htile ? z.htile_data_base_addr : 0;
	r.depth_clear_enable      = rc.depth_clear_enable;
	r.depth_meta_clear_enable = false;
	r.depth_load_clear_enable = r.depth_clear_enable;
	r.depth_clear_value       = hw.GetDepthClearValue();
	r.depth_test_enable       = dc.z_enable;
	r.depth_write_enable      = dc.z_write_enable && !z.depth_view.depth_write_disable;
	r.depth_compare_op        = static_cast<vk::CompareOp>(dc.zfunc);

	r.depth_bounds_test_enable = dc.depth_bounds_enable;
	r.depth_min_bounds         = hw.GetDepthBoundsMin();
	r.depth_max_bounds         = hw.GetDepthBoundsMax();

	r.stencil_clear_enable =
	    has_stencil && rc.stencil_clear_enable && !z.depth_view.stencil_write_disable;
	r.stencil_clear_value = hw.GetStencilClearValue();
	r.stencil_test_enable = has_stencil && dc.stencil_enable;
	if (r.stencil_test_enable) {
		const bool stencil_ops_disabled =
		    rc.stencil_clear_enable || z.depth_view.stencil_write_disable;
		const uint8_t front_write_mask = stencil_ops_disabled ? 0 : sm.stencil_writemask;
		const uint8_t back_write_mask  = stencil_ops_disabled ? 0 : sm.stencil_writemask_bf;
		if (dc.stencilfunc > static_cast<uint8_t>(vk::CompareOp::eAlways) ||
		    (dc.backface_enable &&
		     dc.stencilfunc_bf > static_cast<uint8_t>(vk::CompareOp::eAlways)) ||
		    (front_write_mask != 0 &&
		     UsesStencilOpValue(sc.stencil_fail, sc.stencil_zpass, sc.stencil_zfail) &&
		     sm.stencil_opval != sm.stencil_testval) ||
		    (dc.backface_enable && back_write_mask != 0 &&
		     UsesStencilOpValue(sc.stencil_fail_bf, sc.stencil_zpass_bf, sc.stencil_zfail_bf) &&
		     sm.stencil_opval_bf != sm.stencil_testval_bf)) {
			DepthFatal("unsupported stencil compare or replacement state");
		}
		r.stencil_static_front = {
		    ConvertStencilOp(sc.stencil_fail, front_write_mask, sm.stencil_opval),
		    ConvertStencilOp(sc.stencil_zpass, front_write_mask, sm.stencil_opval),
		    ConvertStencilOp(sc.stencil_zfail, front_write_mask, sm.stencil_opval),
		    static_cast<vk::CompareOp>(dc.stencilfunc)};
		r.stencil_dynamic_front = {sm.stencil_mask, front_write_mask, sm.stencil_testval};
		if (dc.backface_enable) {
			r.stencil_static_back = {
			    ConvertStencilOp(sc.stencil_fail_bf, back_write_mask, sm.stencil_opval_bf),
			    ConvertStencilOp(sc.stencil_zpass_bf, back_write_mask, sm.stencil_opval_bf),
			    ConvertStencilOp(sc.stencil_zfail_bf, back_write_mask, sm.stencil_opval_bf),
			    static_cast<vk::CompareOp>(dc.stencilfunc_bf)};
			r.stencil_dynamic_back = {sm.stencil_mask_bf, back_write_mask, sm.stencil_testval_bf};
		} else {
			r.stencil_static_back  = r.stencil_static_front;
			r.stencil_dynamic_back = r.stencil_dynamic_front;
		}
	}
	r.vaddr_num = has_stencil ? 2 : 1;
	r.vaddr[0]  = r.depth_buffer_vaddr;
	r.size[0]   = r.depth_buffer_size;
	if (has_stencil) {
		r.vaddr[1] = r.stencil_buffer_vaddr;
		r.size[1]  = r.stencil_buffer_size;
	}
	TextureCache::ImageDesc desc {};
	desc.type                 = TextureCache::BindingType::DepthTarget;
	desc.info.data            = {r.depth_buffer_vaddr, r.depth_buffer_size};
	desc.info.stencil         = {r.stencil_buffer_vaddr, r.stencil_buffer_size};
	desc.info.pixel_format    = r.format;
	desc.info.guest_format    = guest_format;
	desc.info.type            = Prospero::ImageType::kColor2D;
	desc.info.extent          = {width, height, 1};
	desc.info.resources       = {1, view.image_layers};
	desc.info.pitch           = pitch;
	desc.info.bytes_per_block = bytes;
	desc.info.samples         = samples;
	desc.info.tile_mode       = Prospero::TileMode::kDepth;
	desc.info.mip_layout[0]   = {0, r.depth_buffer_size, pitch, height};
	desc.info.metadata.range  = {r.htile_buffer_vaddr, r.htile_buffer_size};
	desc.info.metadata.kind   = has_htile ? ImageMetadataKind::Htile : ImageMetadataKind::None;
	desc.info.metadata.stencil_compressed =
	    has_stencil && has_htile && !z.stencil_info.htile_stencil_disabled;
	desc.view_info.format = r.format;
	desc.view_info.type =
	    view.layer_count == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray;
	desc.view_info.aspect      = ImageViewOps::DepthAspectMask(r.format);
	desc.view_info.base_level  = 0;
	desc.view_info.level_count = 1;
	desc.view_info.base_layer  = view.base_layer;
	desc.view_info.layer_count = view.layer_count;
	desc.view_info.usage       = vk::ImageUsageFlagBits::eDepthStencilAttachment;
	r.desc                     = std::move(desc);
	auto& cache                = m_context.GetTextureCache();
	r.image_id                 = cache.FindImage(r.desc);
	r.image_view               = nullptr;
	BindRenderTarget(r.image_id);
}

vk::ImageAspectFlags RenderDepthInfo::AttachmentWriteAspects() const {
	if (format == vk::Format::eUndefined) {
		return {};
	}

	const auto           available = ImageViewOps::DepthAspectMask(format);
	vk::ImageAspectFlags writes {};
	if ((available & vk::ImageAspectFlagBits::eDepth) &&
	    (depth_load_clear_enable || (depth_test_enable && depth_write_enable))) {
		writes |= vk::ImageAspectFlagBits::eDepth;
	}
	if (!(available & vk::ImageAspectFlagBits::eStencil)) {
		return writes;
	}

	const auto face_writes = [&](const PipelineStencilStaticState&  state,
	                             const PipelineStencilDynamicState& dynamic) {
		if (dynamic.writeMask == 0) {
			return false;
		}
		bool can_pass = state.compareOp != vk::CompareOp::eNever;
		bool can_fail = state.compareOp != vk::CompareOp::eAlways;
		if (dynamic.compareMask == 0) {
			switch (state.compareOp) {
				case vk::CompareOp::eEqual:
				case vk::CompareOp::eLessOrEqual:
				case vk::CompareOp::eGreaterOrEqual:
				case vk::CompareOp::eAlways:
					can_pass = true;
					can_fail = false;
					break;
				case vk::CompareOp::eNever:
				case vk::CompareOp::eLess:
				case vk::CompareOp::eGreater:
				case vk::CompareOp::eNotEqual:
					can_pass = false;
					can_fail = true;
					break;
				default: break;
			}
		}
		const bool depth_pass = !depth_test_enable || depth_compare_op != vk::CompareOp::eNever;
		const bool depth_fail = depth_test_enable && depth_compare_op != vk::CompareOp::eAlways;
		return (can_fail && state.failOp != vk::StencilOp::eKeep) ||
		       (can_pass && depth_pass && state.passOp != vk::StencilOp::eKeep) ||
		       (can_pass && depth_fail && state.depthFailOp != vk::StencilOp::eKeep);
	};
	if (stencil_clear_enable ||
	    (stencil_test_enable && (face_writes(stencil_static_front, stencil_dynamic_front) ||
	                             face_writes(stencil_static_back, stencil_dynamic_back)))) {
		writes |= vk::ImageAspectFlagBits::eStencil;
	}
	return writes;
}

} // namespace Libs::Graphics
