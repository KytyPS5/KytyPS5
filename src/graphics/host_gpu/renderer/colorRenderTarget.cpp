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
#include "graphics/host_gpu/renderer/descriptorCache.h"
#include "graphics/host_gpu/renderer/framebufferCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderState.h"
#include "graphics/host_gpu/utils.h"
#include "graphics/presentation/displayBuffer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <vulkan/vk_enum_string_helper.h>

namespace Libs::Graphics {

static std::atomic<uint32_t> g_render_color_log_count = 0;

static float ColorClearUnorm(uint32_t value, uint32_t bits) {
	return static_cast<float>(value) / static_cast<float>((1u << bits) - 1u);
}

static float ColorClearSnorm(uint32_t value, uint32_t bits) {
	const uint32_t mask     = (1u << bits) - 1u;
	const uint32_t sign_bit = 1u << (bits - 1u);
	value &= mask;

	auto signed_value = static_cast<int32_t>(value);
	if ((value & sign_bit) != 0) {
		signed_value = static_cast<int32_t>(value | ~mask);
	}

	const auto denominator = static_cast<float>((1u << (bits - 1u)) - 1u);
	return std::max(-1.0f, static_cast<float>(signed_value) / denominator);
}

static float ColorClearF16(uint32_t value) {
	const uint32_t sign     = (value >> 15u) & 0x1u;
	const uint32_t exponent = (value >> 10u) & 0x1fu;
	const uint32_t mantissa = value & 0x3ffu;
	const float    sign_mul = (sign != 0 ? -1.0f : 1.0f);

	if (exponent == 0) {
		if (mantissa == 0) {
			return sign_mul * 0.0f;
		}
		return sign_mul * std::ldexp(static_cast<float>(mantissa), -24);
	}

	if (exponent == 0x1fu) {
		return mantissa == 0 ? sign_mul * std::numeric_limits<float>::infinity()
		                     : std::numeric_limits<float>::quiet_NaN();
	}

	return sign_mul *
	       std::ldexp(static_cast<float>(0x400u | mantissa), static_cast<int>(exponent) - 25);
}

[[maybe_unused]] static VkClearColorValue ColorClearValue(const HW::RenderTarget& rt) {
	const uint32_t c0 = rt.clear_word0.word0;
	const uint32_t c1 = rt.clear_word1.word1;

	VkClearColorValue color {};

	switch (static_cast<Prospero::ChannelType>(rt.info.channel_type)) {
		case Prospero::ChannelType::kUNorm:
		case Prospero::ChannelType::kSrgb:
			// SRGB clear words are still encoded as normalized component values.
			switch (static_cast<Prospero::ChannelLayout>(rt.info.format)) {
				case Prospero::ChannelLayout::k8:
					color.float32[0] = ColorClearUnorm(c0 & 0xffu, 8);
					color.float32[1] = 0.0f;
					color.float32[2] = 0.0f;
					color.float32[3] = 1.0f;
					break;
				case Prospero::ChannelLayout::k8_8:
					color.float32[0] = ColorClearUnorm(c0 & 0xffu, 8);
					color.float32[1] = ColorClearUnorm((c0 >> 8u) & 0xffu, 8);
					color.float32[2] = 0.0f;
					color.float32[3] = 1.0f;
					break;
				case Prospero::ChannelLayout::k8_8_8_8:
					color.float32[0] = ColorClearUnorm(c0 & 0xffu, 8);
					color.float32[1] = ColorClearUnorm((c0 >> 8u) & 0xffu, 8);
					color.float32[2] = ColorClearUnorm((c0 >> 16u) & 0xffu, 8);
					color.float32[3] = ColorClearUnorm((c0 >> 24u) & 0xffu, 8);
					if (rt.info.channel_order ==
					    Prospero::GpuEnumValue(Prospero::ChannelOrder::kAlt)) {
						std::swap(color.float32[0], color.float32[2]);
					}
					break;
				case Prospero::ChannelLayout::k11_11_10:
					color.float32[0] = 0.0f;
					color.float32[1] = 0.0f;
					color.float32[2] = 0.0f;
					color.float32[3] = 1.0f;
					break;
				case Prospero::ChannelLayout::k10_10_10_2:
					color.float32[0] = ColorClearUnorm(c0 & 0x3ffu, 10);
					color.float32[1] = ColorClearUnorm((c0 >> 10u) & 0x3ffu, 10);
					color.float32[2] = ColorClearUnorm((c0 >> 20u) & 0x3ffu, 10);
					color.float32[3] = ColorClearUnorm((c0 >> 30u) & 0x3u, 2);
					if (rt.info.channel_order ==
					    Prospero::GpuEnumValue(Prospero::ChannelOrder::kAlt)) {
						std::swap(color.float32[0], color.float32[2]);
					}
					break;
				case Prospero::ChannelLayout::k16_16:
					color.float32[0] = 0.0f;
					color.float32[1] = 0.0f;
					color.float32[2] = 0.0f;
					color.float32[3] = 1.0f;
					break;
				case Prospero::ChannelLayout::k32_32_32_32:
					color.float32[0] = 0.0f;
					color.float32[1] = 0.0f;
					color.float32[2] = 0.0f;
					color.float32[3] = 1.0f;
					break;
				default:
					EXIT("%s\n unsupported color clear format\n",
					     Common::Concat(rt_print("RenderTarget", rt), "").c_str());
			}
			break;
		case Prospero::ChannelType::kSNorm:
			switch (static_cast<Prospero::ChannelLayout>(rt.info.format)) {
				case Prospero::ChannelLayout::k16_16:
					color.float32[0] = ColorClearSnorm(c0 & 0xffffu, 16);
					color.float32[1] = ColorClearSnorm((c0 >> 16u) & 0xffffu, 16);
					color.float32[2] = 0.0f;
					color.float32[3] = 1.0f;
					break;
				case Prospero::ChannelLayout::k8_8_8_8:
					color.float32[0] = ColorClearSnorm(c0 & 0xffu, 8);
					color.float32[1] = ColorClearSnorm((c0 >> 8u) & 0xffu, 8);
					color.float32[2] = ColorClearSnorm((c0 >> 16u) & 0xffu, 8);
					color.float32[3] = ColorClearSnorm((c0 >> 24u) & 0xffu, 8);
					if (rt.info.channel_order ==
					    Prospero::GpuEnumValue(Prospero::ChannelOrder::kAlt)) {
						std::swap(color.float32[0], color.float32[2]);
					}
					break;
				default:
					EXIT("%s\n unsupported snorm color clear format\n",
					     Common::Concat(rt_print("RenderTarget", rt), "").c_str());
			}
			break;
		case Prospero::ChannelType::kFloat:
			switch (static_cast<Prospero::ChannelLayout>(rt.info.format)) {
				case Prospero::ChannelLayout::k16:
					color.float32[0] = ColorClearF16(c0 & 0xffffu);
					color.float32[1] = 0.0f;
					color.float32[2] = 0.0f;
					color.float32[3] = 1.0f;
					break;
				case Prospero::ChannelLayout::k16_16:
					color.float32[0] = ColorClearF16(c0 & 0xffffu);
					color.float32[1] = ColorClearF16((c0 >> 16u) & 0xffffu);
					color.float32[2] = 0.0f;
					color.float32[3] = 1.0f;
					break;
				case Prospero::ChannelLayout::k16_16_16_16:
					color.float32[0] = ColorClearF16(c0 & 0xffffu);
					color.float32[1] = ColorClearF16((c0 >> 16u) & 0xffffu);
					color.float32[2] = ColorClearF16(c1 & 0xffffu);
					color.float32[3] = ColorClearF16((c1 >> 16u) & 0xffffu);
					break;
				case Prospero::ChannelLayout::k32:
				case Prospero::ChannelLayout::k11_11_10:
				case Prospero::ChannelLayout::k32_32_32_32:
					color.float32[0] = 0.0f;
					color.float32[1] = 0.0f;
					color.float32[2] = 0.0f;
					color.float32[3] = 1.0f;
					break;
				default:
					EXIT("%s\n unsupported float color clear format\n",
					     Common::Concat(rt_print("RenderTarget", rt), "").c_str());
			}
			break;
		case Prospero::ChannelType::kUInt:
		case Prospero::ChannelType::kSInt:
			switch (static_cast<Prospero::ChannelLayout>(rt.info.format)) {
				case Prospero::ChannelLayout::k8:
					color.uint32[0] = c0;
					color.uint32[1] = 0;
					color.uint32[2] = 0;
					color.uint32[3] = 1;
					break;
				case Prospero::ChannelLayout::k16:
					color.uint32[0] = c0 & 0xffffu;
					color.uint32[1] = 0;
					color.uint32[2] = 0;
					color.uint32[3] = 1;
					break;
				case Prospero::ChannelLayout::k16_16:
					color.uint32[0] = c0 & 0xffffu;
					color.uint32[1] = (c0 >> 16u) & 0xffffu;
					color.uint32[2] = 0;
					color.uint32[3] = 1;
					break;
				case Prospero::ChannelLayout::k8_8_8_8:
					color.uint32[0] = c0 & 0xffu;
					color.uint32[1] = (c0 >> 8u) & 0xffu;
					color.uint32[2] = (c0 >> 16u) & 0xffu;
					color.uint32[3] = c0 >> 24u;
					if (rt.info.channel_order ==
					    Prospero::GpuEnumValue(Prospero::ChannelOrder::kAlt)) {
						std::swap(color.uint32[0], color.uint32[2]);
					}
					break;
				case Prospero::ChannelLayout::k32_32:
					color.uint32[0] = c0;
					color.uint32[1] = c1;
					color.uint32[2] = 0;
					color.uint32[3] = 1;
					break;
				default:
					EXIT("%s\n unsupported integer color clear format\n",
					     Common::Concat(rt_print("RenderTarget", rt), "").c_str());
			}
			break;
		default:
			EXIT("%s\n unsupported color clear number type\n",
			     Common::Concat(rt_print("RenderTarget", rt), "").c_str());
	}

	EXIT_NOT_IMPLEMENTED(
	    c1 != 0 &&
	    !(rt.info.channel_type == Prospero::GpuEnumValue(Prospero::ChannelType::kFloat) &&
	      rt.info.format == Prospero::GpuEnumValue(Prospero::ChannelLayout::k16_16_16_16)) &&
	    !((rt.info.channel_type == Prospero::GpuEnumValue(Prospero::ChannelType::kUInt) ||
	       rt.info.channel_type == Prospero::GpuEnumValue(Prospero::ChannelType::kSInt)) &&
	      rt.info.format == Prospero::GpuEnumValue(Prospero::ChannelLayout::k32_32)));

	return color;
}

static uint32_t RenderGetColorBpp(uint32_t dfmt) {
	switch (static_cast<Prospero::ChannelLayout>(dfmt)) {
		case Prospero::ChannelLayout::k8: return 8;
		case Prospero::ChannelLayout::k16:
		case Prospero::ChannelLayout::k8_8: return 16;
		case Prospero::ChannelLayout::k32:
		case Prospero::ChannelLayout::k16_16:
		case Prospero::ChannelLayout::k11_11_10:
		case Prospero::ChannelLayout::k10_10_10_2:
		case Prospero::ChannelLayout::k8_8_8_8: return 32;
		case Prospero::ChannelLayout::k32_32:
		case Prospero::ChannelLayout::k16_16_16_16: return 64;
		case Prospero::ChannelLayout::k32_32_32_32: return 128;
		case Prospero::ChannelLayout::k5_5_5_1:
		case Prospero::ChannelLayout::k4_4_4_4: return 16;
		default: EXIT("unsupported render-target channel layout: %u\n", dfmt);
	}
}

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

	r->target_slot = rt_slot;

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
		r->format             = VK_FORMAT_UNDEFINED;
		r->extent             = {};
		r->base_mip_level     = 0;
		r->buffer_size        = 0;
		r->color_clear_enable = false;
		r->color_clear_value  = {};
		return;
	}
	const bool msaa_compat =
	    color_msaa_single_sample_compatible(rt.attrib.num_samples, rt.attrib.num_fragments);
	if ((!msaa_compat && (rt.attrib.num_samples != 0 || rt.attrib.num_fragments != 0)) ||
	    rt.view.base_array_slice_index != 0 || render_target_slice_offset != 0) {
		EXIT("multisampled or sliced render targets are unsupported\n");
	}
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
	// Fast color clears are metadata driven and must be handled explicitly when
	// that metadata path is implemented; render-pass load must preserve contents.
	r->color_clear_enable = false;
	r->color_clear_value  = {};

	uint32_t width  = 0;
	uint32_t height = 0;
	uint32_t pitch  = 0;
	uint64_t size   = 0;
	bool     tile   = false;

	switch (rt.attrib3.tile_mode) {
		case Prospero::GpuEnumValue(Prospero::TileMode::kLinear):
		case Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget):
			tile = !RenderIsColorTileModeLinear(rt.attrib3.tile_mode);
			break;
		default: EXIT("unknown tile mode: %u\n", rt.attrib3.tile_mode);
	}
	if (!tile && levels > 1) {
		EXIT("linear mipmapped render targets are unsupported\n");
	}

	width                        = rt.attrib2.width + 1;
	height                       = rt.attrib2.height + 1;
	const auto bytes_per_element = RenderGetColorBpp(rt.info.format) / 8u;
	if (bytes_per_element == 0) {
		EXIT("render-target format has no valid element size\n");
	}
	if (rt.pitch.pitch_div8_minus1 != 0) {
		pitch = (rt.pitch.pitch_div8_minus1 + 1u) << 3u;
	} else if (tile) {
		pitch = TileGetRenderTargetPitch(width, bytes_per_element);
		if (pitch == 0) {
			EXIT("unsupported render-target pitch: width=%u bytes=%u\n", width, bytes_per_element);
		}
	} else {
		pitch = width;
	}

	if (tile) {
		TileSizeAlign layout {};
		const bool    valid_layout =
		    levels == 1 ? TileGetRenderTargetSize(width, height, pitch, bytes_per_element, &layout)
		                : TileGetRenderTargetMipLayout(width, height, pitch, bytes_per_element,
		                                               levels, &layout, nullptr, nullptr);
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
	if (size == 0 || size > TRACKER_ADDRESS_SIZE - rt.base.addr) {
		EXIT("render-target memory footprint is invalid\n");
	}

	const auto effective_slice = rt.view.base_array_slice_index + render_target_slice_offset;
	const auto effective_base  = rt.base.addr + static_cast<uint64_t>(size) * effective_slice;

	auto video_image       = Presentation::DisplayBufferFind(effective_base, true);
	bool render_to_texture = (video_image.image == nullptr);
	if (!render_to_texture && (levels != 1 || rt.view.current_mip_level != 0)) {
		EXIT("mipmapped display render targets are unsupported\n");
	}
	const VkExtent2D view_extent = {std::max(width >> rt.view.current_mip_level, 1u),
	                                std::max(height >> rt.view.current_mip_level, 1u)};

	auto decision_log_id = g_render_color_log_count.fetch_add(1);
	if (decision_log_id < 128 || !render_to_texture) {
		LOGF("RenderColorTarget: slot=%" PRIu32 " addr=0x%010" PRIx64 " size=0x%016" PRIx64
		     " extent=%ux%u view_mip=%u view_extent=%ux%u levels=%u pitch=%u"
		     " fmt=0x%08" PRIx32 " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32
		     " tile=%s target=%s video_size=0x%016" PRIx64 " video_pitch=%" PRIu64 "\n",
		     rt_slot, effective_base, size, width, height, rt.view.current_mip_level,
		     view_extent.width, view_extent.height, levels, pitch, rt.info.format,
		     rt.info.channel_type, rt.info.channel_order, tile ? "tiled" : "linear",
		     render_to_texture ? "RenderTexture" : "DisplayBuffer", video_image.size,
		     video_image.pitch);
	}

	if (render_to_texture) {
		const auto rt_format = TextureGetRenderTargetFormat(rt.info.format, rt.info.channel_type,
		                                                    rt.info.channel_order);

		(void)reuse_existing_render_texture;
		RenderTargetInfo target {};
		target.address           = effective_base;
		target.size              = size;
		target.format            = rt_format.format;
		target.width             = width;
		target.height            = height;
		target.pitch             = pitch;
		target.bytes_per_element = rt_format.bytes_per_element;
		target.tile_mode         = rt.attrib3.tile_mode;
		target.levels            = levels;
		auto* texture_cache      = g_render_ctx->GetTextureCache();
		auto* buffer_vulkan =
		    texture_cache->FindRenderTarget(buffer, g_render_ctx->GetGraphicCtx(), target);
		r->type          = RenderColorType::RenderTexture;
		r->base_addr     = effective_base;
		r->vulkan_buffer = buffer_vulkan;
		r->vulkan_view   = texture_cache->GetRenderTargetAttachmentView(
		    g_render_ctx->GetGraphicCtx(), buffer_vulkan, target.format, rt.view.current_mip_level);
		r->format         = target.format;
		r->extent         = view_extent;
		r->base_mip_level = rt.view.current_mip_level;
		r->buffer_size    = size;
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
		r->base_addr      = effective_base;
		r->vulkan_buffer  = video_image.image;
		r->vulkan_view    = video_image.image->image_view[VulkanImage::VIEW_DEFAULT];
		r->format         = video_image.image->format;
		r->extent         = video_image.image->extent;
		r->base_mip_level = 0;
		r->buffer_size    = video_image.size;
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
