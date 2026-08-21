#include "graphics/host_gpu/renderer/colorRenderTarget.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <atomic>

namespace Libs::Graphics {

static std::atomic<uint32_t> g_render_color_log_count = 0;

static void ResolveDccClearInfo(RenderColorInfo& info, vk::Format format, bool has_dcc,
                                uint32_t packed_clear) {
	// Register-backed DCC clears use the target's packed clear value. Decode one-word guest
	// formats here; unsupported encodings remain tracked without unsafe materialization.
	info.metadata_clear_supported =
	    has_dcc && DecodePackedColorClear(format, packed_clear, info.color_clear_value);
	switch (format) {
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2B10G10R10UnormPack32:
		case vk::Format::eA2R10G10B10UnormPack32:
			info.metadata_fixed_clear_supported = has_dcc;
			break;
		default: info.metadata_fixed_clear_supported = false; break;
	}
	if (!info.metadata_clear_supported) {
		info.color_clear_value = {};
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RenderExecutor::ResolveRenderColorTarget(uint64_t submit_id, RenderCommandBuffer& buffer,
                                              RenderColorInfo& r,
                                              uint32_t         render_target_slice_offset,
                                              uint32_t render_target_slot, bool ignore_target_mask,
                                              bool exact_format) {
	KYTY_PROFILER_FUNCTION();
	const auto& hw = buffer.GetRegisters();

	const auto  rt_slot = (render_target_slot == UINT32_MAX ? render_target_first_bound_slot(buffer)
	                                                        : render_target_slot);
	const auto& rt      = hw.GetRenderTarget(rt_slot);
	auto        mask    = render_target_mask_slot(hw.GetRenderTargetMask(), rt_slot);
	if (ignore_target_mask && rt.base.addr != 0 && mask == 0) {
		mask = 0x0f;
	}

	r.target_slot    = rt_slot;
	r.export_mapping = {};

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
		r.type                           = RenderColorType::NoColorOutput;
		r.desc                           = {};
		r.base_addr                      = 0;
		r.image_id                       = {};
		r.image_view                     = nullptr;
		r.format                         = vk::Format::eUndefined;
		r.extent                         = {};
		r.base_mip_level                 = 0;
		r.base_array_layer               = 0;
		r.buffer_size                    = 0;
		r.samples                        = 1;
		r.export_mapping                 = {};
		r.color_clear_enable             = false;
		r.metadata_clear_supported       = false;
		r.metadata_fixed_clear_supported = false;
		r.color_clear_value              = {};
		return;
	}
	const auto samples = render_sample_count(rt.attrib.num_fragments);
	if (samples == 0 || rt.attrib.num_samples != rt.attrib.num_fragments) {
		EXIT("unsupported render-target sample configuration: samples=%u fragments=%u\n",
		     rt.attrib.num_samples, rt.attrib.num_fragments);
	}
	const auto view = ResolveTargetViewInfo(
	    rt.view.base_array_slice_index, rt.view.last_array_slice_index, render_target_slice_offset);
	switch (view.type) {
		case TargetViewType::Image2D:
		case TargetViewType::Image2DArray: break;
		case TargetViewType::Unsupported:
			EXIT("invalid render-target view: base=%u last=%u draw_offset=%u\n",
			     rt.view.base_array_slice_index, rt.view.last_array_slice_index,
			     render_target_slice_offset);
	}
	r.base_array_layer    = view.base_layer;
	const uint32_t levels = rt.attrib2.num_mip_levels + 1u;
	if (levels == 0 || levels > 16 || rt.view.current_mip_level >= levels) {
		EXIT("unsupported render-target mip range: current=%u levels=%u\n",
		     rt.view.current_mip_level, levels);
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
			     static_cast<uint32_t>(rt.attrib3.tile_mode), rt.attrib3.dimension,
			     static_cast<uint32_t>(rt.info.format), static_cast<uint32_t>(rt.info.channel_type),
			     static_cast<uint32_t>(rt.info.channel_order));
		}
	}

	// Color-control state selects the color-buffer operation and logical blend operation.
	// The normal copy operation is a regular color write, not an attachment clear.
	// Nonlinear clear values are still stored as normalized components.
	// Fast color clears are metadata driven and must be handled explicitly when
	// that metadata path is implemented; render-pass load must preserve contents.
	r.color_clear_enable = false;
	r.color_clear_value  = {};

	uint32_t   width  = 0;
	uint32_t   height = 0;
	uint32_t   pitch  = 0;
	uint64_t   size   = 0;
	bool       tile   = false;
	const bool volume = rt.attrib3.dimension == 2;
	if (rt.attrib3.dimension != 1 && !volume) {
		EXIT("unsupported render-target dimension: %u\n", rt.attrib3.dimension);
	}
	if (!volume && rt.attrib3.depth != 0) {
		EXIT("2D render target has nonzero depth: %u\n", rt.attrib3.depth);
	}
	if (volume && samples != 1) {
		EXIT("multisampled 3D render targets are unsupported\n");
	}
	const uint32_t depth        = volume ? rt.attrib3.depth + 1u : 1u;
	const bool     standard4    = rt.attrib3.tile_mode == Prospero::TileMode::kStandard4KB;
	const bool     standard64   = rt.attrib3.tile_mode == Prospero::TileMode::kStandard64KB;
	const bool     depth_tile   = rt.attrib3.tile_mode == Prospero::TileMode::kDepth;
	const bool     texture_tile = standard4 || standard64 || depth_tile;

	switch (rt.attrib3.tile_mode) {
		case Prospero::TileMode::kLinear:
		case Prospero::TileMode::kStandard4KB:
		case Prospero::TileMode::kStandard64KB:
		case Prospero::TileMode::kDepth:
		case Prospero::TileMode::kRenderTarget:
			tile = !RenderIsColorTileModeLinear(rt.attrib3.tile_mode);
			break;
		default: EXIT("unknown tile mode: %u\n", static_cast<uint32_t>(rt.attrib3.tile_mode));
	}
	if (!tile && levels > 1) {
		EXIT("linear mipmapped render targets are unsupported\n");
	}
	if (samples > 1 && (!tile || levels != 1)) {
		EXIT("multisampled render targets require a single-mip tiled surface\n");
	}
	if (texture_tile && samples != 1) {
		EXIT("texture-tiled color render targets do not support multisampling\n");
	}

	width  = rt.attrib2.width + 1;
	height = rt.attrib2.height + 1;
	const auto target_format =
	    TextureGetRenderTargetFormat(rt.info.format, rt.info.channel_type, rt.info.channel_order);
	const auto bytes_per_element = target_format.bytes_per_element;
	if (bytes_per_element == 0) {
		EXIT("render-target format has no valid element size\n");
	}
	const auto transfer_format = ImageOps::RenderTargetTransferFormat(bytes_per_element);
	TileTextureBlockLayout texture_tile_layout {};
	if (texture_tile &&
	    (!TileGetTextureBlockLayout(transfer_format, rt.attrib3.tile_mode, volume,
	                                texture_tile_layout) ||
	     rt.pitch.pitch_div8_minus1 != 0 ||
	     (rt.base.addr & (texture_tile_layout.block.block_size - 1u)) != 0 ||
	     rt.info.fmask_compression_enable || rt.info.fmask_data_compression_disable ||
	     rt.info.fmask_one_frag_mode || rt.info.cmask_fast_clear_enable ||
	     rt.info.dcc_compression_enable || rt.info.cmask_is_linear != 0 ||
	     rt.info.cmask_addr_type != 0 || rt.info.alt_tile_mode || rt.cmask.addr != 0 ||
	     rt.fmask.addr != 0 || rt.dcc_addr.addr != 0 || rt.dcc.data_write_on_dcc_clear_to_reg)) {
		EXIT("unsupported texture-tiled render target: addr=0x%016" PRIx64 " tile=%u"
		     " dimension=%u depth=%u levels=%u layer=%u/%u samples=%u fragments=%u bpe=%u"
		     " cmask=0x%016" PRIx64 " fmask=0x%016" PRIx64 " dcc=0x%016" PRIx64 "\n",
		     rt.base.addr, static_cast<uint32_t>(rt.attrib3.tile_mode), rt.attrib3.dimension,
		     rt.attrib3.depth, levels, view.base_layer, view.image_layers, rt.attrib.num_samples,
		     rt.attrib.num_fragments, bytes_per_element, rt.cmask.addr, rt.fmask.addr,
		     rt.dcc_addr.addr);
	}
	if ((standard64 || depth_tile) && (rt.attrib3.dimension != 1 || rt.attrib3.depth != 0 ||
	                                   view.base_layer != 0 || view.image_layers != 1)) {
		EXIT("unsupported 64KB texture-tiled render-target view: dimension=%u depth=%u"
		     " layer=%u/%u\n",
		     rt.attrib3.dimension, rt.attrib3.depth, view.base_layer, view.image_layers);
	}
	if (rt.pitch.pitch_div8_minus1 != 0) {
		pitch = (rt.pitch.pitch_div8_minus1 + 1u) << 3u;
	} else if (tile) {
		if (volume) {
			pitch = TileGetTexturePitch(transfer_format, width, rt.attrib3.tile_mode);
		} else if (texture_tile) {
			pitch = TileGetTexturePitch(transfer_format, width, rt.attrib3.tile_mode);
		} else {
			pitch = TileGetRenderTargetPitch(width, bytes_per_element, rt.attrib.num_fragments);
		}
		if (pitch == 0) {
			EXIT("unsupported render-target pitch: width=%u bytes=%u\n", width, bytes_per_element);
		}
	} else {
		pitch = width;
	}

	TileSizeOffset    mip_sizes[16] {};
	TilePaddedSize    mip_padded[16] {};
	TileSurfaceLayout volume_layout {};
	uint64_t          backing_size = 0;
	if (volume) {
		const TileSurfaceDescription description {transfer_format,
		                                          rt.attrib3.tile_mode,
		                                          TileSurfaceDimension::Dim3D,
		                                          width,
		                                          height,
		                                          depth,
		                                          levels,
		                                          1};
		if (!tile || !TileGetTiledTextureLayout(description, volume_layout)) {
			EXIT("unsupported 3D render-target layout: %ux%ux%u levels=%u tile=%u\n", width, height,
			     depth, levels, static_cast<uint32_t>(rt.attrib3.tile_mode));
		}
		size         = volume_layout.block_slice_size;
		backing_size = volume_layout.total_size;
	} else if (tile) {
		TileSizeAlign layout {};
		bool          valid_layout = false;
		if (texture_tile) {
			TileGetTextureSize(transfer_format, width, height, levels, rt.attrib3.tile_mode,
			                   &layout, mip_sizes, mip_padded);
			valid_layout = layout.size != 0 && layout.align == texture_tile_layout.block.block_size;
		} else {
			valid_layout =
			    levels == 1 ? TileGetRenderTargetSize(width, height, pitch, bytes_per_element,
			                                          layout, rt.attrib.num_fragments)
			                : TileGetRenderTargetMipLayout(width, height, pitch, bytes_per_element,
			                                               levels, layout, mip_sizes, mip_padded);
		}
		if (!valid_layout) {
			EXIT("unsupported render-target layout: %ux%u pitch=%u bytes=%u levels=%u\n", width,
			     height, pitch, bytes_per_element, levels);
		}
		size = layout.size;
		EXIT_IF(size > UINT32_MAX);
		if (levels == 1) {
			mip_sizes[0]  = {static_cast<uint32_t>(size), 0, 0, 0, 0, 0};
			mip_padded[0] = {pitch, height};
		}
	} else {
		size = static_cast<uint64_t>(pitch) * height * bytes_per_element * samples;
		if (size > UINT32_MAX) {
			EXIT("linear render-target slice exceeds the supported layout size\n");
		}
		mip_sizes[0]  = {static_cast<uint32_t>(size), 0, 0, 0, 0, 0};
		mip_padded[0] = {pitch, height};
	}
	if (rt.slice.slice_div64_minus1 != 0 &&
	    (static_cast<uint64_t>(rt.slice.slice_div64_minus1) + 1u) * 64u != size) {
		EXIT("render-target slice span mismatch: encoded=0x%016" PRIx64 " derived=0x%016" PRIx64
		     "\n",
		     (static_cast<uint64_t>(rt.slice.slice_div64_minus1) + 1u) * 64u, size);
	}
	if (size == 0 || (!volume && size > UINT64_MAX / view.image_layers)) {
		EXIT("render-target memory footprint is invalid\n");
	}
	if (!volume) {
		backing_size = size * view.image_layers;
	}
	if (backing_size == 0) {
		EXIT("render-target backing is empty\n");
	}
	if (backing_size > TRACKER_ADDRESS_SIZE - rt.base.addr) {
		EXIT("render-target backing range is invalid\n");
	}

	const vk::Extent2D view_extent = {std::max(width >> rt.view.current_mip_level, 1u),
	                                  std::max(height >> rt.view.current_mip_level, 1u)};
	const uint32_t     view_depth  = std::max(depth >> rt.view.current_mip_level, 1u);
	if (volume &&
	    (view.base_layer >= view_depth || view.layer_count > view_depth - view.base_layer)) {
		EXIT("3D render-target view exceeds mip depth: base=%u count=%u depth=%u mip=%u\n",
		     view.base_layer, view.layer_count, view_depth, rt.view.current_mip_level);
	}

	auto decision_log_id = g_render_color_log_count.fetch_add(1);
	if (decision_log_id < 128) {
		LOGF("RenderColorTarget: slot=%" PRIu32 " addr=0x%010" PRIx64 " size=0x%016" PRIx64
		     " extent=%ux%ux%u view_mip=%u view_extent=%ux%u levels=%u pitch=%u"
		     " fmt=0x%08" PRIx32 " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32 " samples=%u tile=%s\n",
		     rt_slot, rt.base.addr, backing_size, width, height, depth, rt.view.current_mip_level,
		     view_extent.width, view_extent.height, levels, pitch,
		     static_cast<uint32_t>(rt.info.format), static_cast<uint32_t>(rt.info.channel_type),
		     static_cast<uint32_t>(rt.info.channel_order), samples, tile ? "tiled" : "linear");
	}

	TextureCache::ImageDesc desc {};
	desc.type              = TextureCache::BindingType::RenderTarget;
	desc.info.data         = {rt.base.addr, backing_size};
	desc.info.pixel_format = target_format.format;
	desc.info.guest_format = transfer_format;
	desc.info.type         = volume ? Prospero::ImageType::kColor3D : Prospero::ImageType::kColor2D;
	desc.info.extent       = {width, height, depth};
	desc.info.resources    = {levels, volume ? 1u : view.image_layers};
	desc.info.pitch        = pitch;
	desc.info.bytes_per_block = bytes_per_element;
	desc.info.samples         = samples;
	desc.info.tile_mode       = rt.attrib3.tile_mode;
	const bool has_dcc        = rt.info.dcc_compression_enable && rt.dcc_addr.addr != 0;
	if (has_dcc) {
		// DCC uses a separate metadata allocation. Carry its address through ImageInfo so
		// TextureCache can associate metadata fills observed before target registration.
		desc.info.metadata.kind          = ImageMetadataKind::Dcc;
		desc.info.metadata.range.address = rt.dcc_addr.addr;
	}
	for (uint32_t level = 0; level < levels; level++) {
		if (volume) {
			const auto& mip             = volume_layout.mips[level];
			desc.info.mip_layout[level] = {
			    mip.offset,
			    mip.size,
			    mip.padded_width,
			    mip.padded_height,
			};
			continue;
		}
		const auto level_offset =
		    mip_sizes[level].src_size != 0 ? mip_sizes[level].src_offset : mip_sizes[level].offset;
		const auto level_size =
		    static_cast<uint64_t>(mip_sizes[level].src_size != 0 ? mip_sizes[level].src_size
		                                                         : mip_sizes[level].size) *
		    view.image_layers;
		desc.info.mip_layout[level] = {
		    level_offset,
		    level_size,
		    mip_padded[level].width,
		    mip_padded[level].height,
		};
	}
	desc.view_info.format = target_format.format;
	desc.view_info.type =
	    view.layer_count == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray;
	desc.view_info.aspect      = vk::ImageAspectFlagBits::eColor;
	desc.view_info.base_level  = rt.view.current_mip_level;
	desc.view_info.level_count = 1;
	desc.view_info.base_layer  = view.base_layer;
	desc.view_info.layer_count = view.layer_count;
	desc.view_info.usage       = vk::ImageUsageFlagBits::eColorAttachment;
	auto& texture_cache        = m_context.GetTextureCache();
	r.desc                     = std::move(desc);
	r.image_id                 = texture_cache.FindImage(r.desc, exact_format);
	r.type                     = RenderColorType::RenderTexture;
	r.base_addr                = rt.base.addr;
	r.image_view               = nullptr;
	r.format                   = r.desc.view_info.format;
	r.extent                   = view_extent;
	r.base_mip_level           = rt.view.current_mip_level;
	r.buffer_size              = backing_size;
	r.samples                  = samples;
	r.export_mapping           = target_format.export_mapping;
	r.color_clear_enable       = false;
	ResolveDccClearInfo(r, target_format.format, has_dcc, rt.clear_word0.word0);
	BindRenderTarget(r.image_id);
}

} // namespace Libs::Graphics
