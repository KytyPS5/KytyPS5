#include "graphics/host_gpu/renderer/colorRenderTarget.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
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

namespace Libs::Graphics {

static std::atomic<uint32_t> g_render_color_log_count = 0;

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ResolveRenderColorTarget(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw,
                              RenderColorInfo* r, uint32_t render_target_slice_offset,
                              uint32_t render_target_slot, bool ignore_target_mask,
                              bool reuse_existing_render_texture) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(r == nullptr);

	const auto  rt_slot = (render_target_slot == UINT32_MAX ? render_target_first_bound_slot(hw)
	                                                        : render_target_slot);
	const auto& rt      = hw.GetRenderTarget(rt_slot);
	auto        mask    = render_target_mask_slot(hw.GetRenderTargetMask(), rt_slot);
	if (ignore_target_mask && rt.base.addr != 0 && mask == 0) {
		mask = 0x0f;
	}

	r->target_slot    = rt_slot;
	r->export_mapping = {};

	if (rt.base.addr == 0 || mask == 0) {
		if (graphics_debug_dump_enabled()) {
			static std::atomic_uint log_count = 0;
			const auto              log_id    = log_count.fetch_add(1, std::memory_order_relaxed);
			if (log_id < 128) {
				LOGF("RenderColorTarget: no color output slot=%" PRIu32 " base=0x%010" PRIx64
				     " slot_mask=0x%01" PRIx32 " target_mask=0x%08" PRIx32
				     " rt_slice_offset=%" PRIu32 "\n",
				     rt_slot, rt.base.addr, mask, hw.GetRenderTargetMask(),
				     render_target_slice_offset);
			}
		}

		// No color output
		r->type               = RenderColorType::NoColorOutput;
		r->base_addr          = 0;
		r->vulkan_buffer      = nullptr;
		r->vulkan_view        = nullptr;
		r->format             = vk::Format::eUndefined;
		r->extent             = {};
		r->base_mip_level     = 0;
		r->buffer_size        = 0;
		r->color_clear_enable = false;
		r->color_clear_value  = {};
		return;
	}
	const bool msaa_compat =
	    color_msaa_single_sample_compatible(rt.attrib.num_samples, rt.attrib.num_fragments);
	if (!msaa_compat && (rt.attrib.num_samples != 0 || rt.attrib.num_fragments != 0)) {
		EXIT("multisampled render targets are unsupported\n");
	}
	const auto view = ResolveTargetViewInfo(
	    rt.view.base_array_slice_index, rt.view.last_array_slice_index, render_target_slice_offset);
	switch (view.type) {
		case TargetViewType::Image2D: break;
		case TargetViewType::Image2DArray:
			EXIT("layered render-target views are unsupported: base=%u count=%u\n", view.base_layer,
			     view.layer_count);
		case TargetViewType::Unsupported:
			EXIT("invalid render-target view: base=%u last=%u draw_offset=%u\n",
			     rt.view.base_array_slice_index, rt.view.last_array_slice_index,
			     render_target_slice_offset);
	}
	r->base_array_layer   = view.base_layer;
	const uint32_t levels = rt.attrib2.num_mip_levels + 1u;
	if (levels == 0 || levels > 16 || rt.view.current_mip_level >= levels) {
		EXIT("unsupported render-target mip range: current=%u levels=%u\n",
		     rt.view.current_mip_level, levels);
	}
	if (msaa_compat) {
		static std::atomic<uint32_t> logged_fragments = 0;
		const uint32_t               bit              = 1u << rt.attrib.num_fragments;
		if ((logged_fragments.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
			LOGF("RenderColorTarget: compatibility: rendering PS5 %ux samples/fragments as "
			     "single-sample\n",
			     bit);
		}
	}

	if (graphics_debug_dump_enabled()) {
		static std::atomic_uint log_count = 0;
		const auto              log_id    = log_count.fetch_add(1, std::memory_order_relaxed);
		if (log_id < 128) {
			LOGF("RenderColorTarget: inspect slot=%" PRIu32 " base=0x%010" PRIx64
			     " mask=0x%01" PRIx32 " attrib2_width=%" PRIu32 " attrib2_height=%" PRIu32
			     " attrib3_tile=0x%08" PRIx32 " attrib3_dim=0x%08" PRIx32 " fmt=0x%08" PRIx32
			     " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32 "\n",
			     rt_slot, rt.base.addr, mask, rt.attrib2.width, rt.attrib2.height,
			     rt.attrib3.tile_mode, rt.attrib3.dimension, rt.info.format, rt.info.channel_type,
			     rt.info.channel_order);
		}
	}

	// CB_COLOR_CONTROL describes the color-buffer operation / ROP3 logic op.
	// ROP3 Copy is the normal color write path, not a render-target clear.
	// SRGB clear words are still encoded as normalized component values.
	// Fast color clears are metadata driven and must be handled explicitly when
	// that metadata path is implemented; render-pass load must preserve contents.
	r->color_clear_enable = false;
	r->color_clear_value  = {};

	uint32_t   width  = 0;
	uint32_t   height = 0;
	uint32_t   pitch  = 0;
	uint64_t   size   = 0;
	bool       tile   = false;
	const bool standard64 =
	    rt.attrib3.tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kStandard64KB);

	switch (rt.attrib3.tile_mode) {
		case Prospero::GpuEnumValue(Prospero::TileMode::kLinear):
		case Prospero::GpuEnumValue(Prospero::TileMode::kStandard64KB):
		case Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget):
			tile = !RenderIsColorTileModeLinear(rt.attrib3.tile_mode);
			break;
		default: EXIT("unknown tile mode: %u\n", rt.attrib3.tile_mode);
	}
	if (!tile && levels > 1) {
		EXIT("linear mipmapped render targets are unsupported\n");
	}

	width  = rt.attrib2.width + 1;
	height = rt.attrib2.height + 1;
	const auto target_format =
	    TextureGetRenderTargetFormat(rt.info.format, rt.info.channel_type, rt.info.channel_order);
	const auto bytes_per_element = target_format.bytes_per_element;
	if (bytes_per_element == 0) {
		EXIT("render-target format has no valid element size\n");
	}
	if (standard64 &&
	    (rt.attrib3.dimension != 1 || rt.attrib3.depth != 0 || levels != 1 ||
	     rt.view.current_mip_level != 0 || view.base_layer != 0 || view.image_layers != 1 ||
	     rt.attrib.num_samples != 0 || rt.attrib.num_fragments != 0 || bytes_per_element != 4 ||
	     rt.pitch.pitch_div8_minus1 != 0 || (rt.base.addr & 0xffffu) != 0 ||
	     rt.info.fmask_compression_enable || rt.info.fmask_data_compression_disable ||
	     rt.info.fmask_one_frag_mode || rt.info.cmask_fast_clear_enable ||
	     rt.info.dcc_compression_enable || rt.info.cmask_is_linear != 0 ||
	     rt.info.cmask_addr_type != 0 || rt.info.alt_tile_mode || rt.cmask.addr != 0 ||
	     rt.fmask.addr != 0 || rt.dcc_addr.addr != 0 || rt.dcc.data_write_on_dcc_clear_to_reg)) {
		EXIT("unsupported Standard64KB render target: addr=0x%016" PRIx64
		     " dimension=%u depth=%u levels=%u layer=%u/%u samples=%u fragments=%u bpe=%u"
		     " cmask=0x%016" PRIx64 " fmask=0x%016" PRIx64 " dcc=0x%016" PRIx64 "\n",
		     rt.base.addr, rt.attrib3.dimension, rt.attrib3.depth, levels, view.base_layer,
		     view.image_layers, rt.attrib.num_samples, rt.attrib.num_fragments, bytes_per_element,
		     rt.cmask.addr, rt.fmask.addr, rt.dcc_addr.addr);
	}
	if (rt.pitch.pitch_div8_minus1 != 0) {
		pitch = (rt.pitch.pitch_div8_minus1 + 1u) << 3u;
	} else if (tile) {
		pitch = standard64
		            ? TileGetTexturePitch(Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float),
		                                  width, levels, rt.attrib3.tile_mode)
		            : TileGetRenderTargetPitch(width, bytes_per_element);
		if (pitch == 0) {
			EXIT("unsupported render-target pitch: width=%u bytes=%u\n", width, bytes_per_element);
		}
	} else {
		pitch = width;
	}

	if (tile) {
		TileSizeAlign layout {};
		bool          valid_layout = false;
		if (standard64) {
			TileGetTextureSize(Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float), width,
			                   height, pitch, levels, rt.attrib3.tile_mode, &layout, nullptr,
			                   nullptr);
			valid_layout = layout.size != 0 && layout.align == 65536;
		} else {
			valid_layout =
			    levels == 1
			        ? TileGetRenderTargetSize(width, height, pitch, bytes_per_element, &layout)
			        : TileGetRenderTargetMipLayout(width, height, pitch, bytes_per_element, levels,
			                                       &layout, nullptr, nullptr);
		}
		if (!valid_layout) {
			EXIT("unsupported render-target layout: %ux%u pitch=%u bytes=%u levels=%u\n", width,
			     height, pitch, bytes_per_element, levels);
		}
		size = layout.size;
		if (rt.slice.slice_div64_minus1 != 0 &&
		    (static_cast<uint64_t>(rt.slice.slice_div64_minus1) + 1u) * 64u != size) {
			EXIT("render-target slice span mismatch: encoded=0x%016" PRIx64 " derived=0x%016" PRIx64
			     "\n",
			     (static_cast<uint64_t>(rt.slice.slice_div64_minus1) + 1u) * 64u, size);
		}
	} else {
		size = static_cast<uint64_t>(pitch) * height * bytes_per_element;
	}
	if (size == 0 || size > UINT64_MAX / view.image_layers) {
		EXIT("render-target memory footprint is invalid\n");
	}
	const auto backing_size = size * view.image_layers;
	if (backing_size > TRACKER_ADDRESS_SIZE - rt.base.addr) {
		EXIT("render-target backing range is invalid\n");
	}

	auto video_image = Presentation::DisplayBufferFind(rt.base.addr, true);
	if (video_image.image != nullptr &&
	    !IsSupportedDisplayRenderTargetTileMode(rt.attrib3.tile_mode)) {
		EXIT("unsupported display render-target tile mode: tile=%u expected=%u addr=0x%010" PRIx64
		     " backing_size=0x%016" PRIx64 " video_size=0x%016" PRIx64 "\n",
		     rt.attrib3.tile_mode, Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget),
		     rt.base.addr, backing_size, video_image.size);
	}
	bool render_to_texture = view.base_layer != 0 || video_image.image == nullptr;
	if (!render_to_texture && (levels != 1 || rt.view.current_mip_level != 0)) {
		EXIT("mipmapped display render targets are unsupported\n");
	}
	const vk::Extent2D view_extent = {std::max(width >> rt.view.current_mip_level, 1u),
	                                  std::max(height >> rt.view.current_mip_level, 1u)};

	auto decision_log_id = g_render_color_log_count.fetch_add(1);
	if (decision_log_id < 128 || !render_to_texture) {
		LOGF("RenderColorTarget: slot=%" PRIu32 " addr=0x%010" PRIx64 " size=0x%016" PRIx64
		     " extent=%ux%u view_mip=%u view_extent=%ux%u levels=%u pitch=%u"
		     " fmt=0x%08" PRIx32 " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32
		     " tile=%s target=%s video_size=0x%016" PRIx64 " video_pitch=%" PRIu64 "\n",
		     rt_slot, rt.base.addr, backing_size, width, height, rt.view.current_mip_level,
		     view_extent.width, view_extent.height, levels, pitch, rt.info.format,
		     rt.info.channel_type, rt.info.channel_order, tile ? "tiled" : "linear",
		     render_to_texture ? "RenderTexture" : "DisplayBuffer", video_image.size,
		     video_image.pitch);
	}

	if (render_to_texture) {
		(void)reuse_existing_render_texture;
		RenderTargetInfo target {};
		target.address           = rt.base.addr;
		target.size              = backing_size;
		target.format            = target_format.format;
		target.width             = width;
		target.height            = height;
		target.pitch             = pitch;
		target.bytes_per_element = target_format.bytes_per_element;
		target.tile_mode         = rt.attrib3.tile_mode;
		target.levels            = levels;
		target.layers            = view.image_layers;
		auto* texture_cache      = g_render_ctx->GetTextureCache();
		auto* buffer_vulkan =
		    texture_cache->FindRenderTarget(buffer, g_render_ctx->GetGraphicCtx(), target);
		r->type          = RenderColorType::RenderTexture;
		r->base_addr     = rt.base.addr;
		r->vulkan_buffer = buffer_vulkan;
		r->vulkan_view   = texture_cache->GetRenderTargetAttachmentView(
		    g_render_ctx->GetGraphicCtx(), buffer_vulkan, target.format, rt.view.current_mip_level,
		    view.base_layer, view.layer_count);
		r->format         = target.format;
		r->extent         = view_extent;
		r->base_mip_level = rt.view.current_mip_level;
		r->buffer_size    = backing_size;
		r->export_mapping = target_format.export_mapping;
	} else {
		const auto layout = static_cast<Prospero::ChannelLayout>(rt.info.format);
		const auto type   = static_cast<Prospero::ChannelType>(rt.info.channel_type);
		const auto order  = static_cast<Prospero::ChannelOrder>(rt.info.channel_order);

		bool supported_display_format =
		    (layout == Prospero::ChannelLayout::k8_8_8_8 &&
		     (type == Prospero::ChannelType::kSrgb || type == Prospero::ChannelType::kUNorm) &&
		     (order == Prospero::ChannelOrder::kStandard ||
		      order == Prospero::ChannelOrder::kAlt)) ||
		    (layout == Prospero::ChannelLayout::k10_10_10_2 &&
		     type == Prospero::ChannelType::kUNorm &&
		     (order == Prospero::ChannelOrder::kStandard ||
		      order == Prospero::ChannelOrder::kAlt)) ||
		    (layout == Prospero::ChannelLayout::k16_16_16_16 &&
		     type == Prospero::ChannelType::kFloat &&
		     (order == Prospero::ChannelOrder::kStandard || order == Prospero::ChannelOrder::kAlt));

		EXIT_NOT_IMPLEMENTED(!supported_display_format);

		// Display buffer
		if (video_image.size != size) {
			LOGF("RenderColorTarget: display buffer size differs from render target span, "
			     "video_size=0x%016" PRIx64 " render_size=0x%016" PRIx64 "\n",
			     video_image.size, size);
		}
		EXIT_NOT_IMPLEMENTED(video_image.size < size);
		EXIT_NOT_IMPLEMENTED(video_image.pitch != pitch);
		r->type           = RenderColorType::DisplayBuffer;
		r->base_addr      = rt.base.addr;
		r->vulkan_buffer  = video_image.image;
		r->vulkan_view    = video_image.image->image_view[VulkanImage::VIEW_DEFAULT];
		r->format         = video_image.image->format;
		r->extent         = video_image.image->extent;
		r->base_mip_level = 0;
		r->buffer_size    = video_image.size;
		r->export_mapping = target_format.export_mapping;
	}
}

void MarkRenderTargetGpuWritten(const RenderColorInfo& target) {
	const bool with_color = target.vulkan_buffer != nullptr;

	if (with_color) {
		if (target.type == RenderColorType::RenderTexture ||
		    target.type == RenderColorType::DisplayBuffer) {
			g_render_ctx->GetTextureCache()->MarkGpuWritten(target.vulkan_buffer);
		} else {
			EXIT("unknown writable render-color resource type\n");
		}
	}
}

} // namespace Libs::Graphics
