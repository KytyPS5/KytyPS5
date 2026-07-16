#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEINFO_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEINFO_H_

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace Libs::Graphics {

struct ImageInfo {
	uint64_t address     = 0;
	uint64_t size        = 0;
	uint32_t format      = 0;
	uint32_t width       = 0;
	uint32_t height      = 0;
	uint32_t pitch       = 0;
	uint32_t base_level  = 0;
	uint32_t levels      = 1;
	uint32_t view_levels = 1;
	uint32_t tile        = 0;
	uint32_t swizzle     = 0;
	uint32_t depth       = 1;
	uint32_t type        = 0;
	uint32_t base_array  = 0;
};

struct RenderTargetInfo {
	uint64_t address           = 0;
	uint64_t size              = 0;
	VkFormat format            = VK_FORMAT_UNDEFINED;
	uint32_t width             = 0;
	uint32_t height            = 0;
	uint32_t pitch             = 0;
	uint32_t bytes_per_element = 0;
	uint32_t tile_mode         = 0;
	uint32_t levels            = 1;
};

// Common image-to-buffer copy description. Storage images and render targets keep distinct
// cache records today, but downloads should consume one normalized layout just as uploads do.
struct ColorImageTransferInfo {
	uint64_t address           = 0;
	uint64_t size              = 0;
	VkFormat format            = VK_FORMAT_UNDEFINED;
	uint32_t width             = 0;
	uint32_t height            = 0;
	uint32_t pitch             = 0;
	uint32_t bytes_per_element = 0;
	uint32_t tile_mode         = 0;
	uint32_t levels            = 1;
};

[[nodiscard]] inline ColorImageTransferInfo
MakeColorImageTransferInfo(const ImageInfo& info, VkFormat format,
                           uint32_t bytes_per_element) noexcept {
	return {info.address, info.size,         format,    info.width, info.height,
	        info.pitch,   bytes_per_element, info.tile, info.levels};
}

[[nodiscard]] inline ColorImageTransferInfo
MakeColorImageTransferInfo(const RenderTargetInfo& info) noexcept {
	return {
	    info.address,           info.size,      info.format, info.width, info.height, info.pitch,
	    info.bytes_per_element, info.tile_mode, info.levels};
}

struct DepthTargetInfo {
	uint64_t address            = 0;
	uint64_t size               = 0;
	uint64_t stencil_address    = 0;
	uint64_t stencil_size       = 0;
	uint64_t htile_address      = 0;
	uint64_t htile_size         = 0;
	VkFormat format             = VK_FORMAT_UNDEFINED;
	uint32_t guest_format       = 0;
	uint32_t width              = 0;
	uint32_t height             = 0;
	uint32_t pitch              = 0;
	uint32_t bytes_per_element  = 0;
	uint32_t tile_mode          = 0;
	bool     depth_load_clear   = false;
	bool     stencil_load_clear = false;
	bool     stencil_access     = false;
};

enum class VideoOutCompression : uint8_t { Uncompressed, Dcc256_64_64, Unsupported };

struct VideoOutInfo {
	uint64_t            address           = 0;
	uint64_t            size              = 0;
	uint64_t            metadata_address  = 0;
	VkFormat            format            = VK_FORMAT_UNDEFINED;
	uint32_t            guest_format      = 0;
	uint32_t            width             = 0;
	uint32_t            height            = 0;
	uint32_t            pitch             = 0;
	uint32_t            bytes_per_element = 0;
	uint32_t            tile_mode         = 0;
	uint32_t            dcc_control       = 0;
	VideoOutCompression compression       = VideoOutCompression::Unsupported;
};

[[nodiscard]] inline VideoOutCompression
ClassifyVideoOutCompression(bool compressed, uint64_t metadata_address, uint32_t dcc_control,
                            uint64_t dcc_clear_color) noexcept {
	constexpr uint32_t VIDEO_OUT_DCC_CONTROL_256_64_64 = 0x00000208u;
	if (!compressed) {
		return metadata_address == 0 && dcc_control == 0 && dcc_clear_color == 0
		           ? VideoOutCompression::Uncompressed
		           : VideoOutCompression::Unsupported;
	}
	return metadata_address != 0 && (metadata_address & 0xffu) == 0 &&
	               dcc_control == VIDEO_OUT_DCC_CONTROL_256_64_64 && dcc_clear_color == 0
	           ? VideoOutCompression::Dcc256_64_64
	           : VideoOutCompression::Unsupported;
}

[[nodiscard]] inline constexpr bool
CanUseVideoOutNativeWithoutUpload(VideoOutCompression compression, bool render_target,
                                  bool gpu_modified, bool guest_modified) noexcept {
	return compression == VideoOutCompression::Dcc256_64_64 && !guest_modified &&
	       (render_target || gpu_modified);
}

struct VideoOutPixelFormatInfo {
	VkFormat format       = VK_FORMAT_UNDEFINED;
	uint32_t guest_format = 0;
};

[[nodiscard]] inline bool DecodeVideoOutPixelFormat(uint64_t                 pixel_format,
                                                    VideoOutPixelFormatInfo* info) {
	if (info == nullptr) {
		return false;
	}
	VideoOutPixelFormatInfo next {};
	switch (pixel_format) {
		case 0x8000000022000000ull: // SCE_VIDEO_OUT_PIXEL_FORMAT2_R8_G8_B8_A8_SRGB
			next.format       = VK_FORMAT_R8G8B8A8_SRGB;
			next.guest_format = Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8Srgb);
			break;
		case 0x8000000000000000ull: // SCE_VIDEO_OUT_PIXEL_FORMAT2_B8_G8_R8_A8_SRGB
			next.format       = VK_FORMAT_B8G8R8A8_SRGB;
			next.guest_format = Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8Srgb);
			break;
		case 0x8100000022000000ull: // SCE_VIDEO_OUT_PIXEL_FORMAT2_R10_G10_B10_A2_SRGB
			next.format       = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
			next.guest_format = Prospero::GpuEnumValue(Prospero::BufferFormat::k10_10_10_2UNorm);
			break;
		case 0x8100000000000000ull: // SCE_VIDEO_OUT_PIXEL_FORMAT2_B10_G10_R10_A2_SRGB
			next.format       = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
			next.guest_format = Prospero::GpuEnumValue(Prospero::BufferFormat::k10_10_10_2UNorm);
			break;
		default: return false;
	}
	*info = next;
	return true;
}

[[nodiscard]] inline bool IsSupportedVideoOutFormat(uint32_t guest_format, VkFormat format) {
	const bool rgba8 = guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8Srgb) &&
	                   (format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB);
	const bool rgb10 = guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k10_10_10_2UNorm) &&
	                   (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
	                    format == VK_FORMAT_A2R10G10B10_UNORM_PACK32);
	return rgba8 || rgb10;
}

enum class DepthOverlap : uint8_t { None, RetireSampled, Unsupported };
enum class RenderTargetOverlap : uint8_t { None, RetireSampled, RetireTarget, Unsupported };
enum class SampledOverlap : uint8_t { None, ReadOnlyAlias, Unsupported };
enum class StorageSampledOverlap : uint8_t { None, ExactImage, Unsupported };
enum class HostWriteOverlap : uint8_t { None, InvalidateImage, Unsupported };
enum class BufferImageAlias : uint8_t { None, VideoOutWrite, RenderTargetWrite, Unsupported };
enum class MetaImageOverlap : uint8_t { RetainSampled, RetireTarget, Unsupported };

[[nodiscard]] inline MetaImageOverlap ClassifyMetaImageOverlap(bool sampled, bool render_target,
                                                               bool gpu_modified,
                                                               bool buffer_modified) {
	if (sampled && !gpu_modified) {
		return MetaImageOverlap::RetainSampled;
	}
	if (render_target && !gpu_modified && !buffer_modified) {
		return MetaImageOverlap::RetireTarget;
	}
	return MetaImageOverlap::Unsupported;
}

[[nodiscard]] inline float DecodeSrgbClearComponent(uint32_t value) {
	const auto encoded = static_cast<float>(value & 0xffu) / 255.0f;
	return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] inline bool DecodePackedColorClear(VkFormat format, uint32_t packed,
                                                 VkClearColorValue* clear) {
	if (clear == nullptr) {
		return false;
	}
	VkClearColorValue next {};
	const auto unorm8 = [](uint32_t value) { return static_cast<float>(value & 0xffu) / 255.0f; };
	switch (format) {
		case VK_FORMAT_R8G8B8A8_SRGB:
			next.float32[0] = DecodeSrgbClearComponent(packed);
			next.float32[1] = DecodeSrgbClearComponent(packed >> 8u);
			next.float32[2] = DecodeSrgbClearComponent(packed >> 16u);
			next.float32[3] = unorm8(packed >> 24u);
			break;
		case VK_FORMAT_B8G8R8A8_SRGB:
			next.float32[0] = DecodeSrgbClearComponent(packed >> 16u);
			next.float32[1] = DecodeSrgbClearComponent(packed >> 8u);
			next.float32[2] = DecodeSrgbClearComponent(packed);
			next.float32[3] = unorm8(packed >> 24u);
			break;
		case VK_FORMAT_R8G8B8A8_UNORM:
			next.float32[0] = unorm8(packed);
			next.float32[1] = unorm8(packed >> 8u);
			next.float32[2] = unorm8(packed >> 16u);
			next.float32[3] = unorm8(packed >> 24u);
			break;
		case VK_FORMAT_B8G8R8A8_UNORM:
			next.float32[0] = unorm8(packed >> 16u);
			next.float32[1] = unorm8(packed >> 8u);
			next.float32[2] = unorm8(packed);
			next.float32[3] = unorm8(packed >> 24u);
			break;
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
			next.float32[0] = static_cast<float>(packed & 0x3ffu) / 1023.0f;
			next.float32[1] = static_cast<float>((packed >> 10u) & 0x3ffu) / 1023.0f;
			next.float32[2] = static_cast<float>((packed >> 20u) & 0x3ffu) / 1023.0f;
			next.float32[3] = static_cast<float>((packed >> 30u) & 0x3u) / 3.0f;
			break;
		case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
			next.float32[0] = static_cast<float>((packed >> 20u) & 0x3ffu) / 1023.0f;
			next.float32[1] = static_cast<float>((packed >> 10u) & 0x3ffu) / 1023.0f;
			next.float32[2] = static_cast<float>(packed & 0x3ffu) / 1023.0f;
			next.float32[3] = static_cast<float>((packed >> 30u) & 0x3u) / 3.0f;
			break;
		default: return false;
	}
	*clear = next;
	return true;
}

[[nodiscard]] inline bool DecodePackedStencilClear(uint32_t packed, uint8_t* clear) {
	if (clear == nullptr) {
		return false;
	}
	const auto value = static_cast<uint8_t>(packed);
	if (packed != static_cast<uint32_t>(value) * 0x01010101u) {
		return false;
	}
	*clear = value;
	return true;
}

[[nodiscard]] inline bool DecodePackedDepthClear(VkFormat format, uint32_t packed, float* clear) {
	if (clear == nullptr ||
	    (format != VK_FORMAT_D32_SFLOAT && format != VK_FORMAT_D32_SFLOAT_S8_UINT)) {
		return false;
	}
	const auto value = std::bit_cast<float>(packed);
	if (!std::isfinite(value) || value < 0.0f || value > 1.0f) {
		return false;
	}
	*clear = value;
	return true;
}

[[nodiscard]] inline bool CanNativeClearDepthFromBuffer(const DepthTargetInfo& target,
                                                        uint64_t address, uint64_t size) {
	const bool d32 =
	    target.format == VK_FORMAT_D32_SFLOAT || target.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
	return address == target.address && size == target.size && d32 &&
	       target.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float) &&
	       target.bytes_per_element == 4 && target.tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kDepth) &&
	       target.htile_address == 0 && target.htile_size == 0;
}

[[nodiscard]] inline bool CanLoadStencilAttachment(const DepthTargetInfo& target,
                                                   bool                   stencil_initialized) {
	const bool has_stencil = target.stencil_address != 0 || target.stencil_size != 0;
	return !has_stencil || !target.stencil_access || target.stencil_load_clear ||
	       stencil_initialized;
}

[[nodiscard]] inline bool IsDepthTargetRangeCompatible(const DepthTargetInfo& target,
                                                       uint64_t address, uint64_t size) {
	if (address == 0 || size == 0 || address > UINT64_MAX - size) {
		EXIT("invalid depth-target view range\n");
	}
	const bool depth   = address == target.address && size <= target.size;
	const bool stencil = target.stencil_address != 0 && address == target.stencil_address &&
	                     size == target.stencil_size;
	return depth || stencil;
}

[[nodiscard]] inline bool ImageRangeOverlaps(uint64_t left, uint64_t left_size, uint64_t right,
                                             uint64_t right_size) {
	if (left_size == 0 || right_size == 0 || left > UINT64_MAX - left_size ||
	    right > UINT64_MAX - right_size) {
		EXIT("invalid image overlap range\n");
	}
	return left < right + right_size && right < left + left_size;
}

[[nodiscard]] inline bool ImagePageRangesOverlap(uint64_t left, uint64_t left_size, uint64_t right,
                                                 uint64_t right_size) {
	if (left_size == 0 || right_size == 0 || left > UINT64_MAX - left_size ||
	    right > UINT64_MAX - right_size) {
		EXIT("invalid image page-overlap range\n");
	}
	const auto left_first  = left / TRACKER_PAGE_SIZE;
	const auto left_last   = (left + left_size - 1) / TRACKER_PAGE_SIZE;
	const auto right_first = right / TRACKER_PAGE_SIZE;
	const auto right_last  = (right + right_size - 1) / TRACKER_PAGE_SIZE;
	return left_first <= right_last && right_first <= left_last;
}

[[nodiscard]] inline SampledOverlap ClassifySampledOverlap(const ImageInfo& requested,
                                                           const ImageInfo& cached,
                                                           bool             cached_gpu_modified,
                                                           bool             same_context) {
	if (!ImagePageRangesOverlap(requested.address, requested.size, cached.address, cached.size)) {
		return SampledOverlap::None;
	}
	return !cached_gpu_modified && same_context ? SampledOverlap::ReadOnlyAlias
	                                            : SampledOverlap::Unsupported;
}

[[nodiscard]] inline bool IsRgba8SrgbReinterpretation(VkFormat cached, VkFormat requested) noexcept;

[[nodiscard]] inline StorageSampledOverlap
ClassifyStorageSampledOverlap(const ImageInfo& requested, const ImageInfo& cached,
                              VkFormat requested_view_format, VkFormat cached_image_format,
                              bool cached_gpu_modified, bool cached_cpu_dirty, bool same_context) {
	if (!ImagePageRangesOverlap(requested.address, requested.size, cached.address, cached.size)) {
		return StorageSampledOverlap::None;
	}
	const bool same_backing =
	    requested.address == cached.address && requested.size == cached.size &&
	    requested.width == cached.width && requested.height == cached.height &&
	    requested.pitch == cached.pitch && requested.base_level == cached.base_level &&
	    requested.levels == cached.levels && requested.view_levels == cached.view_levels &&
	    requested.tile == cached.tile && requested.depth == cached.depth &&
	    requested.type == cached.type && requested.base_array == cached.base_array;
	const bool compatible_format =
	    (requested.format == cached.format && requested_view_format == cached_image_format) ||
	    IsRgba8SrgbReinterpretation(cached_image_format, requested_view_format);
	return same_backing && compatible_format && cached_gpu_modified && !cached_cpu_dirty &&
	               same_context
	           ? StorageSampledOverlap::ExactImage
	           : StorageSampledOverlap::Unsupported;
}

[[nodiscard]] inline HostWriteOverlap
ClassifyHostWriteOverlap(uint64_t write_address, uint64_t write_size, uint64_t image_address,
                         uint64_t image_size, bool host_refreshable, bool gpu_modified,
                         bool metadata_overlap) {
	if (!ImagePageRangesOverlap(write_address, write_size, image_address, image_size)) {
		return HostWriteOverlap::None;
	}
	return host_refreshable && !gpu_modified && !metadata_overlap
	           ? HostWriteOverlap::InvalidateImage
	           : HostWriteOverlap::Unsupported;
}

[[nodiscard]] inline bool IsSampledBufferWriteTransition(uint64_t buffer_address,
                                                         uint64_t buffer_size,
                                                         uint64_t image_address,
                                                         uint64_t image_size,
                                                         bool     image_gpu_modified) {
	if (!ImagePageRangesOverlap(buffer_address, buffer_size, image_address, image_size)) {
		return false;
	}
	const bool exact        = buffer_address == image_address && buffer_size == image_size;
	const bool page_aligned = ((buffer_address | buffer_size) & (TRACKER_PAGE_SIZE - 1)) == 0;
	return exact && page_aligned && !image_gpu_modified;
}

[[nodiscard]] inline BufferImageAlias
ClassifyBufferImageAlias(uint64_t buffer_address, uint64_t buffer_size, uint64_t image_address,
                         uint64_t image_size, bool video_out, bool render_target,
                         bool image_gpu_modified, bool buffer_written, bool buffer_formatted) {
	if (!ImagePageRangesOverlap(buffer_address, buffer_size, image_address, image_size)) {
		return BufferImageAlias::None;
	}
	const bool exact = buffer_address == image_address && buffer_size == image_size;
	if (!exact || !buffer_written || !buffer_formatted) {
		return BufferImageAlias::Unsupported;
	}
	if (video_out && !render_target && !image_gpu_modified) {
		return BufferImageAlias::VideoOutWrite;
	}
	const bool page_aligned = ((buffer_address | buffer_size) & (TRACKER_PAGE_SIZE - 1)) == 0;
	if (render_target && !video_out && image_gpu_modified && page_aligned) {
		return BufferImageAlias::RenderTargetWrite;
	}
	return BufferImageAlias::Unsupported;
}

[[nodiscard]] inline DepthOverlap ClassifyDepthOverlap(const ImageInfo&       sampled,
                                                       bool                   sampled_gpu_modified,
                                                       const DepthTargetInfo& depth) {
	const bool depth_overlap =
	    ImageRangeOverlaps(sampled.address, sampled.size, depth.address, depth.size);
	const bool stencil_overlap =
	    depth.stencil_address != 0 && ImageRangeOverlaps(sampled.address, sampled.size,
	                                                     depth.stencil_address, depth.stencil_size);
	if (!depth_overlap && !stencil_overlap) {
		return DepthOverlap::None;
	}
	const bool has_stencil = depth.stencil_address != 0 || depth.stencil_size != 0;
	if (sampled.address == depth.address && !sampled_gpu_modified && depth.depth_load_clear &&
	    (!has_stencil ||
	     (depth.stencil_address != 0 && depth.stencil_size != 0 && depth.stencil_load_clear))) {
		return DepthOverlap::RetireSampled;
	}
	return DepthOverlap::Unsupported;
}

[[nodiscard]] inline RenderTargetOverlap
ClassifyRenderTargetOverlap(const ImageInfo& sampled, bool sampled_gpu_modified, bool same_context,
                            const RenderTargetInfo& target) {
	if (!ImagePageRangesOverlap(sampled.address, sampled.size, target.address, target.size)) {
		return RenderTargetOverlap::None;
	}
	const bool exact = sampled.address == target.address && sampled.size == target.size;
	const bool page_isolated_overlap =
	    sampled.address % TRACKER_PAGE_SIZE == 0 && sampled.size % TRACKER_PAGE_SIZE == 0 &&
	    target.address % TRACKER_PAGE_SIZE == 0 && target.size % TRACKER_PAGE_SIZE == 0 &&
	    ImageRangeOverlaps(sampled.address, sampled.size, target.address, target.size);
	return !sampled_gpu_modified && same_context && (exact || page_isolated_overlap)
	           ? RenderTargetOverlap::RetireSampled
	           : RenderTargetOverlap::Unsupported;
}

[[nodiscard]] inline bool IsRgba8SrgbViewFormat(VkFormat format) noexcept {
	return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB ||
	       format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

[[nodiscard]] inline bool IsRgba8SrgbReinterpretation(VkFormat cached,
                                                      VkFormat requested) noexcept {
	return (cached == VK_FORMAT_R8G8B8A8_UNORM && requested == VK_FORMAT_R8G8B8A8_SRGB) ||
	       (cached == VK_FORMAT_R8G8B8A8_SRGB && requested == VK_FORMAT_R8G8B8A8_UNORM) ||
	       (cached == VK_FORMAT_B8G8R8A8_UNORM && requested == VK_FORMAT_B8G8R8A8_SRGB) ||
	       (cached == VK_FORMAT_B8G8R8A8_SRGB && requested == VK_FORMAT_B8G8R8A8_UNORM);
}

[[nodiscard]] inline bool IsCompatibleRenderTargetView(const RenderTargetInfo& cached,
                                                       const RenderTargetInfo& requested) noexcept {
	return cached.address == requested.address && cached.size == requested.size &&
	       cached.width == requested.width && cached.height == requested.height &&
	       cached.pitch == requested.pitch &&
	       cached.bytes_per_element == requested.bytes_per_element &&
	       cached.tile_mode == requested.tile_mode && cached.levels == requested.levels &&
	       IsRgba8SrgbReinterpretation(cached.format, requested.format);
}

[[nodiscard]] inline RenderTargetOverlap
ClassifyRenderTargetOverlap(const RenderTargetInfo& cached, bool cached_gpu_modified,
                            bool cached_buffer_modified, bool same_context,
                            const RenderTargetInfo& requested) {
	if (!ImagePageRangesOverlap(cached.address, cached.size, requested.address, requested.size)) {
		return RenderTargetOverlap::None;
	}
	// For an equal-address allocation-pool entry, a changed block raster or block size is a new
	// image allocation, not a view of the old image. Kyty cannot keep overlapping tracker
	// owners, so only retire an already-published target with page-isolated storage.
	const bool page_isolated =
	    cached.address % TRACKER_PAGE_SIZE == 0 && cached.size % TRACKER_PAGE_SIZE == 0 &&
	    requested.address % TRACKER_PAGE_SIZE == 0 && requested.size % TRACKER_PAGE_SIZE == 0;
	const bool pool_storage_shape_changed = cached.pitch != requested.pitch ||
	                                        cached.height != requested.height ||
	                                        cached.bytes_per_element != requested.bytes_per_element;
	return cached.address == requested.address && page_isolated && pool_storage_shape_changed &&
	               !cached_gpu_modified && !cached_buffer_modified && same_context
	           ? RenderTargetOverlap::RetireTarget
	           : RenderTargetOverlap::Unsupported;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEINFO_H_
