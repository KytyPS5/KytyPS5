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
#include "graphics/host_gpu/objects/textureCommon.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/descriptorCache.h"
#include "graphics/host_gpu/renderer/framebufferCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/transfer.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/displayBuffer.h"

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

static vk::StencilOp ConvertStencilOp(uint8_t value) {
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
		default: DepthFatal("unsupported stencil operation");
	}
}

static bool UsesStencilOpValue(uint8_t fail, uint8_t pass, uint8_t depth_fail) {
	return fail == Prospero::GpuEnumValue(Prospero::StencilOp::kReplaceOp) ||
	       pass == Prospero::GpuEnumValue(Prospero::StencilOp::kReplaceOp) ||
	       depth_fail == Prospero::GpuEnumValue(Prospero::StencilOp::kReplaceOp);
}

[[nodiscard]] static vk::Format ResolveHostDepthAttachmentFormat(GraphicContext*          ctx,
                                                                 const DepthFormatPolicy& policy,
                                                                 bool has_stencil) {
	if (!has_stencil) {
		return policy.depth_attachment_format;
	}
	switch (policy.depth_format) {
		case Prospero::DepthFormat::kZ32F: return policy.stencil_attachment_formats.front();
		case Prospero::DepthFormat::kZ16: {
			if (ctx == nullptr) {
				return vk::Format::eUndefined;
			}
			for (const auto format: policy.stencil_attachment_formats) {
				vk::ImageFormatProperties properties {};
				if (ctx->GetImageFormatProperties(format, vk::ImageType::e2D,
				                                  vk::ImageTiling::eOptimal,
				                                  DepthTargetImageUsage(), vk::ImageCreateFlags {},
				                                  &properties) == vk::Result::eSuccess) {
					return format;
				}
			}
			return vk::Format::eUndefined;
		}
		default: return vk::Format::eUndefined;
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ResolveRenderDepthTarget(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw,
                              RenderDepthInfo* r) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(r == nullptr);
	(void)submit_id;
	(void)buffer;
	const auto& z  = hw.GetDepthRenderTarget();
	const auto& rc = hw.GetRenderControl();
	const auto& dc = hw.GetDepthControl();
	const auto& sc = hw.GetStencilControl();
	const auto& sm = hw.GetStencilMask();
	const bool  depth_active =
	    dc.z_enable || dc.z_write_enable || dc.depth_bounds_enable || rc.depth_clear_enable;
	const bool stencil_active = dc.stencil_enable || rc.stencil_clear_enable;
	if (!depth_active && !stencil_active) {
		return;
	}
	const bool attachment_unbound =
	    z.z_info.format == Prospero::GpuEnumValue(Prospero::DepthFormat::kInvalid) &&
	    z.stencil_info.format == Prospero::GpuEnumValue(Prospero::StencilFormat::kInvalid) &&
	    z.z_info.tile_mode_index == 0 && z.z_info.num_samples == 0 &&
	    z.z_info.zrange_precision <= 1 && !z.z_info.expclear_enabled &&
	    !z.z_info.embedded_sample_locations && !z.z_info.partially_resident &&
	    z.z_info.num_mip_levels == 0 && z.z_info.plane_compression == 0 &&
	    z.stencil_info.tile_mode_index == 0 && z.stencil_info.tile_split == 0 &&
	    !z.stencil_info.expclear_enabled && !z.stencil_info.texture_compatible_stencil &&
	    !z.stencil_info.partially_resident && z.depth_view.slice_start == 0 &&
	    z.depth_view.slice_max == 0 && z.depth_view.current_mip_level == 0 &&
	    !z.depth_view.depth_write_disable && !z.depth_view.stencil_write_disable &&
	    z.depth_info.addr5_swizzle_mask == 0 && z.depth_info.array_mode == 0 &&
	    z.depth_info.pipe_config == 0 && z.depth_info.bank_width == 0 &&
	    z.depth_info.bank_height == 0 && z.depth_info.macro_tile_aspect == 0 &&
	    z.depth_info.num_banks == 0 && z.htile_surface.linear == 0 &&
	    z.htile_surface.full_cache == 0 && z.htile_surface.htile_uses_preload_win == 0 &&
	    z.htile_surface.preload == 0 && z.htile_surface.prefetch_width == 0 &&
	    z.htile_surface.prefetch_height == 0 && z.htile_surface.dst_outside_zero_to_one == 0 &&
	    z.z_read_base_addr == 0 && z.z_write_base_addr == 0 && z.stencil_read_base_addr == 0 &&
	    z.stencil_write_base_addr == 0 && z.htile_data_base_addr == 0 &&
	    // DB_DEPTH_SIZE_XY is independent state and may remain programmed after the attachment
	    // formats and addresses are unbound. A zero encoding is the valid 1x1 value, so its
	    // presence alone must not manufacture a depth attachment.
	    !z.z_info.tile_surface_enable && !z.width_height_valid && !z.pitch_height_valid &&
	    z.size.x_max == 0 && z.size.y_max == 0 && z.pitch_div8_minus1 == 0 &&
	    z.height_div8_minus1 == 0 && z.slice_div64_minus1 == 0 && z.width == 0 && z.height == 0;
	if (attachment_unbound) {
		static std::atomic_bool logged = false;
		if (!logged.exchange(true, std::memory_order_relaxed)) {
			LOGF("DepthTarget: ignoring enabled depth/stencil state without a bound attachment\n");
		}
		return;
	}
	const bool has_stencil =
	    z.stencil_info.format != Prospero::GpuEnumValue(Prospero::StencilFormat::kInvalid);
	const bool has_htile            = z.z_info.tile_surface_enable;
	const bool msaa_compat          = depth_msaa_single_sample_compatible(z.z_info.num_samples);
	const bool htile_stencil_compat = depth_htile_stencil_acceleration_compatible(
	    has_stencil, has_htile, z.stencil_info.tile_stencil_disable);
	const auto view = ResolveTargetViewInfo(z.depth_view.slice_start, z.depth_view.slice_max);
	switch (view.type) {
		case TargetViewType::Image2D: break;
		case TargetViewType::Image2DArray:
			DepthFatal("layered depth views are unsupported: base=%u count=%u", view.base_layer,
			           view.layer_count);
		case TargetViewType::Unsupported:
			DepthFatal("invalid depth view: base=%u last=%u", z.depth_view.slice_start,
			           z.depth_view.slice_max);
	}
	// Prospero defines the compression-disable bits as tile writeback policy. Vulkan attachments
	// expose the same logical depth/stencil values regardless of the driver's backing compression.
	if ((stencil_active && !has_stencil) || rc.resummarize_enable || rc.copy_centroid ||
	    rc.copy_sample != 0 || z.z_info.expclear_enabled || z.stencil_info.expclear_enabled ||
	    z.z_info.embedded_sample_locations || z.z_info.partially_resident ||
	    z.stencil_info.partially_resident || z.z_info.plane_compression != 0 ||
	    (z.z_info.num_samples != 0 && !msaa_compat) || z.z_info.num_mip_levels != 0 ||
	    z.z_info.tile_mode_index != 0 || z.z_info.zrange_precision > 1 ||
	    z.depth_view.current_mip_level != 0 || z.depth_info.addr5_swizzle_mask != 0 ||
	    z.depth_info.array_mode != 0 || z.depth_info.pipe_config != 0 ||
	    z.depth_info.bank_width != 0 || z.depth_info.bank_height != 0 ||
	    z.depth_info.macro_tile_aspect != 0 || z.depth_info.num_banks != 0 ||
	    z.htile_surface.linear != 0 || z.htile_surface.full_cache != 0 ||
	    z.htile_surface.htile_uses_preload_win != 0 || z.htile_surface.preload != 0 ||
	    z.htile_surface.prefetch_width != 0 || z.htile_surface.prefetch_height != 0 ||
	    z.htile_surface.dst_outside_zero_to_one != 0 || z.z_read_base_addr == 0 ||
	    z.z_write_base_addr != z.z_read_base_addr || (z.z_read_base_addr & 0xffffu) != 0 ||
	    dc.zfunc > static_cast<uint8_t>(vk::CompareOp::eAlways)) {
		DepthFatal("unsupported depth register state");
	}
	if (msaa_compat) {
		static std::atomic<uint32_t> logged_fragments = 0;
		const uint32_t               bit              = 1u << z.z_info.num_samples;
		if ((logged_fragments.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
			LOGF("DepthTarget: compatibility: rendering PS5 %ux depth fragments as single-sample\n",
			     bit);
		}
	}
	if (has_stencil) {
		// Prospero defines Hi-Stencil as HTile-backed acceleration of the logical stencil plane.
		// Keep the plane native in Vulkan while tracking HTile separately.
		if (z.stencil_info.format != Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt) ||
		    z.stencil_info.tile_mode_index != 0 || z.stencil_info.tile_split != 0 ||
		    !htile_stencil_compat || z.stencil_info.texture_compatible_stencil ||
		    z.stencil_read_base_addr == 0 ||
		    z.stencil_write_base_addr != z.stencil_read_base_addr ||
		    (z.stencil_read_base_addr & 0xffffu) != 0 || z.depth_view.stencil_write_disable) {
			DepthFatal("unsupported stencil attachment state");
		}
		if (!z.stencil_info.tile_stencil_disable) {
			static std::atomic_bool logged = false;
			if (!logged.load(std::memory_order_relaxed) &&
			    !logged.exchange(true, std::memory_order_relaxed)) {
				LOGF("DepthTarget: compatibility: using native stencil with PS5 HTILE "
				     "acceleration\n");
			}
		}
	} else if (z.stencil_read_base_addr != 0 || z.stencil_write_base_addr != 0 ||
	           z.stencil_info.tile_mode_index != 0 || z.stencil_info.tile_split != 0 ||
	           !htile_stencil_compat || z.stencil_info.texture_compatible_stencil) {
		DepthFatal("stencil state without an active stencil attachment");
	}
	if (has_htile) {
		if (z.htile_data_base_addr == 0 || (z.htile_data_base_addr & 0x7fffu) != 0) {
			DepthFatal("invalid HTile metadata address");
		}
		if (z.depth_view.slice_max >= 32) {
			DepthFatal("HTile clear tracking supports at most 32 slices");
		}
	} else if (z.htile_data_base_addr != 0) {
		DepthFatal("HTile address without an enabled tile surface");
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
	r->format =
	    ResolveHostDepthAttachmentFormat(g_render_ctx->GetGraphicCtx(), *policy, has_stencil);
	if (r->format == vk::Format::eUndefined) {
		DepthFatal("no host depth/stencil format supports required usage for %s",
		           VulkanToString(ideal_format).c_str());
	}
	const uint32_t guest_format = Prospero::GpuEnumValue(policy->guest_format);
	const uint32_t bytes        = policy->bytes_per_element;
	const auto     pitch = TileGetTexturePitch(guest_format, width, 1,
	                                           Prospero::GpuEnumValue(Prospero::TileMode::kDepth));
	if (z.pitch_height_valid && ((static_cast<uint64_t>(z.pitch_div8_minus1) + 1u) * 8u != pitch ||
	                             (static_cast<uint64_t>(z.height_div8_minus1) + 1u) * 8u !=
	                                 ((static_cast<uint64_t>(height) + 7u) & ~7ull))) {
		DepthFatal("encoded depth pitch or height mismatch");
	}
	const uint32_t block_width = bytes == 2 ? 256u : 128u;
	const uint64_t padded_width =
	    (static_cast<uint64_t>(pitch) + block_width - 1u) & ~(block_width - 1u);
	const uint64_t padded_height =
	    (static_cast<uint64_t>(height) + 127u) & ~static_cast<uint64_t>(127u);
	if (padded_width > UINT64_MAX / padded_height ||
	    padded_width * padded_height > UINT64_MAX / bytes) {
		DepthFatal("depth footprint overflow");
	}
	const uint64_t expected_size = padded_width * padded_height * bytes;
	TileSizeAlign  depth_size {};
	TileSizeAlign  stencil_size {};
	TileSizeAlign  htile_size {};
	if (has_stencil || has_htile) {
		if (!TileGetDepthSize(width, height, 0, z.z_info.format, z.stencil_info.format, has_htile,
		                      &stencil_size, &htile_size, &depth_size) ||
		    depth_size.align != 65536 || depth_size.size != expected_size ||
		    (has_stencil != (stencil_size.align == 65536 && stencil_size.size != 0)) ||
		    (has_htile != (htile_size.align == 32768 && htile_size.size != 0))) {
			DepthFatal("unsupported depth/stencil/HTile footprint");
		}
	} else {
		TileGetTextureTotalSize(guest_format, width, height, 1, pitch, 1,
		                        Prospero::GpuEnumValue(Prospero::TileMode::kDepth), false,
		                        &depth_size);
	}
	if (expected_size == 0 || expected_size > UINT32_MAX || depth_size.align != 65536 ||
	    depth_size.size != expected_size ||
	    (z.pitch_height_valid &&
	     (static_cast<uint64_t>(z.slice_div64_minus1) + 1u) * 64u != expected_size)) {
		DepthFatal("depth footprint mismatch: extent=%ux%u pitch=%u expected=0x%016" PRIx64
		           " calculated=0x%016" PRIx64 "/0x%016" PRIx64
		           " encoded_valid=%u encoded=0x%016" PRIx64,
		           width, height, pitch, expected_size, depth_size.size, depth_size.align,
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
	r->htile                = has_htile;
	r->width                = width;
	r->height               = height;
	r->depth_buffer_size    = depth_backing_size;
	r->depth_buffer_vaddr   = z.z_read_base_addr;
	r->stencil_buffer_size  = has_stencil ? stencil_backing_size : 0;
	r->stencil_buffer_vaddr = has_stencil ? z.stencil_read_base_addr : 0;
	r->htile_buffer_size    = has_htile ? htile_backing_size : 0;
	r->htile_buffer_vaddr   = has_htile ? z.htile_data_base_addr : 0;
	auto* cache             = g_render_ctx->GetTextureCache();
	if (has_htile) {
		cache->RegisterMeta(r->htile_buffer_vaddr, r->htile_buffer_size, view.image_layers);
	}
	if (has_htile && rc.depth_clear_enable && !cache->ClearMeta(z.htile_data_base_addr)) {
		DepthFatal("failed to acquire HTile metadata for a depth clear");
	}
	const bool meta_clear =
	    has_htile && cache->IsMetaCleared(z.htile_data_base_addr, z.depth_view.slice_start);
	r->depth_clear_enable      = rc.depth_clear_enable;
	r->depth_meta_clear_enable = meta_clear;
	r->depth_load_clear_enable = r->depth_clear_enable || r->depth_meta_clear_enable;
	r->depth_clear_value       = hw.GetDepthClearValue();
	r->depth_test_enable       = dc.z_enable;
	r->depth_write_enable      = dc.z_write_enable && !z.depth_view.depth_write_disable;
	r->depth_compare_op        = static_cast<vk::CompareOp>(dc.zfunc);

	r->depth_bounds_test_enable = dc.depth_bounds_enable;
	r->depth_min_bounds         = hw.GetDepthBoundsMin();
	r->depth_max_bounds         = hw.GetDepthBoundsMax();

	r->stencil_clear_enable = rc.stencil_clear_enable;
	r->stencil_clear_value  = hw.GetStencilClearValue();
	r->stencil_test_enable  = dc.stencil_enable;
	if (dc.stencil_enable) {
		if (dc.stencilfunc > static_cast<uint8_t>(vk::CompareOp::eAlways) ||
		    (dc.backface_enable &&
		     dc.stencilfunc_bf > static_cast<uint8_t>(vk::CompareOp::eAlways)) ||
		    (UsesStencilOpValue(sc.stencil_fail, sc.stencil_zpass, sc.stencil_zfail) &&
		     sm.stencil_opval != sm.stencil_testval) ||
		    (dc.backface_enable &&
		     UsesStencilOpValue(sc.stencil_fail_bf, sc.stencil_zpass_bf, sc.stencil_zfail_bf) &&
		     sm.stencil_opval_bf != sm.stencil_testval_bf)) {
			DepthFatal("unsupported stencil compare or replacement state");
		}
		r->stencil_static_front = {
		    ConvertStencilOp(sc.stencil_fail), ConvertStencilOp(sc.stencil_zpass),
		    ConvertStencilOp(sc.stencil_zfail), static_cast<vk::CompareOp>(dc.stencilfunc)};
		r->stencil_dynamic_front = {sm.stencil_mask,
		                            rc.stencil_clear_enable ? 0u : sm.stencil_writemask,
		                            sm.stencil_testval};
		if (dc.backface_enable) {
			r->stencil_static_back  = {ConvertStencilOp(sc.stencil_fail_bf),
			                           ConvertStencilOp(sc.stencil_zpass_bf),
			                           ConvertStencilOp(sc.stencil_zfail_bf),
			                           static_cast<vk::CompareOp>(dc.stencilfunc_bf)};
			r->stencil_dynamic_back = {sm.stencil_mask_bf,
			                           rc.stencil_clear_enable ? 0u : sm.stencil_writemask_bf,
			                           sm.stencil_testval_bf};
		} else {
			r->stencil_static_back  = r->stencil_static_front;
			r->stencil_dynamic_back = r->stencil_dynamic_front;
		}
	}
	r->vaddr_num = has_stencil ? 2 : 1;
	r->vaddr[0]  = r->depth_buffer_vaddr;
	r->size[0]   = r->depth_buffer_size;
	if (has_stencil) {
		r->vaddr[1] = r->stencil_buffer_vaddr;
		r->size[1]  = r->stencil_buffer_size;
	}
	DepthTargetInfo info {};
	info.address            = r->depth_buffer_vaddr;
	info.size               = r->depth_buffer_size;
	info.stencil_address    = r->stencil_buffer_vaddr;
	info.stencil_size       = r->stencil_buffer_size;
	info.htile_address      = r->htile_buffer_vaddr;
	info.htile_size         = r->htile_buffer_size;
	info.format             = r->format;
	info.guest_format       = guest_format;
	info.width              = width;
	info.height             = height;
	info.pitch              = pitch;
	info.bytes_per_element  = bytes;
	info.tile_mode          = Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
	info.layers             = view.image_layers;
	info.depth_load_clear   = r->depth_load_clear_enable;
	info.stencil_load_clear = rc.stencil_clear_enable;
	info.stencil_access =
	    r->stencil_clear_enable ||
	    (r->stencil_test_enable &&
	     (stencil_face_accesses_attachment(r->stencil_static_front, r->stencil_dynamic_front) ||
	      stencil_face_accesses_attachment(r->stencil_static_back, r->stencil_dynamic_back)));
	info.stencil_htile_compressed =
	    has_stencil && has_htile && !z.stencil_info.tile_stencil_disable;
	r->vulkan_buffer = cache->FindDepthTarget(buffer, g_render_ctx->GetGraphicCtx(), info);
	r->vulkan_view   = cache->GetDepthTargetAttachmentView(
	    g_render_ctx->GetGraphicCtx(), r->vulkan_buffer, view.base_layer, view.layer_count);
	if (meta_clear && !cache->TouchMeta(z.htile_data_base_addr, z.depth_view.slice_start, false)) {
		DepthFatal("failed to consume HTile clear state");
	}
}

void MarkRenderTargetGpuWritten(const RenderDepthInfo& target) {
	const bool with_depth =
	    target.format != vk::Format::eUndefined && target.vulkan_buffer != nullptr;

	if (with_depth && !depth_attachment_read_only(&target)) {
		g_render_ctx->GetTextureCache()->MarkGpuWritten(target.vulkan_buffer);
	}
}

} // namespace Libs::Graphics
