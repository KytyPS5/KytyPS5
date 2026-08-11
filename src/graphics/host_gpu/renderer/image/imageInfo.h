#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEINFO_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEINFO_H_

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace Libs::Graphics {

struct GuestRange {
	uint64_t address = 0;
	uint64_t size    = 0;

	[[nodiscard]] constexpr bool Empty() const noexcept { return address == 0 || size == 0; }
	[[nodiscard]] constexpr bool Valid() const noexcept {
		return !Empty() && address < TRACKER_ADDRESS_SIZE && size <= TRACKER_ADDRESS_SIZE - address;
	}
	[[nodiscard]] constexpr uint64_t End() const noexcept { return address + size; }
	auto                             operator<=>(const GuestRange&) const = default;
};

enum class VideoOutCompression : uint8_t { Uncompressed, Dcc256_256_0, Dcc256_64_64, Unsupported };

enum class ImageMetadataKind : uint8_t { None, Htile, Dcc };

struct ImageMetadataInfo {
	GuestRange          range;
	ImageMetadataKind   kind               = ImageMetadataKind::None;
	uint32_t            control            = 0;
	VideoOutCompression compression        = VideoOutCompression::Uncompressed;
	bool                stencil_compressed = false;
};

struct ImageSubresources {
	uint32_t levels                                      = 1;
	uint32_t layers                                      = 1;
	auto     operator<=>(const ImageSubresources&) const = default;
};

struct ImageSubresourceRange {
	uint32_t base_level                                      = 0;
	uint32_t level_count                                     = 1;
	uint32_t base_layer                                      = 0;
	uint32_t layer_count                                     = 1;
	auto     operator<=>(const ImageSubresourceRange&) const = default;
};

struct ImageMipInfo {
	uint64_t offset                                 = 0;
	uint64_t size                                   = 0;
	uint32_t pitch                                  = 0;
	uint32_t height                                 = 0;
	auto     operator<=>(const ImageMipInfo&) const = default;
};

struct ImageInfo {
	GuestRange                   data;
	GuestRange                   stencil;
	ImageMetadataInfo            metadata;
	uint32_t                     htile_clear_mask = UINT32_MAX;
	vk::Format                   pixel_format     = vk::Format::eUndefined;
	Prospero::BufferFormat       guest_format     = Prospero::BufferFormat::kInvalid;
	Prospero::ImageType          type             = Prospero::ImageType::kColor2D;
	vk::Extent3D                 extent           = {1, 1, 1};
	ImageSubresources            resources;
	uint32_t                     pitch           = 0;
	uint32_t                     bytes_per_block = 0;
	uint32_t                     samples         = 1;
	Prospero::TileMode           tile_mode       = Prospero::TileMode::kLinear;
	bool                         bgra16          = false;
	std::array<ImageMipInfo, 16> mip_layout {};

	[[nodiscard]] constexpr bool HasStencil() const noexcept { return !stencil.Empty(); }
	[[nodiscard]] constexpr bool HasMetadata() const noexcept {
		return metadata.kind != ImageMetadataKind::None;
	}
	[[nodiscard]] bool IsDepth() const noexcept;
	[[nodiscard]] bool IsBlock() const noexcept {
		return Prospero::BlockCompressedBytesPerBlock(guest_format) != 0;
	}
	[[nodiscard]] bool IsTiled() const noexcept { return tile_mode != Prospero::TileMode::kLinear; }
	[[nodiscard]] constexpr bool IsVolume() const noexcept {
		return type == Prospero::ImageType::kColor3D;
	}
	[[nodiscard]] constexpr bool IsLayered() const noexcept {
		return !IsVolume() && resources.layers > 1;
	}
	[[nodiscard]] constexpr uint32_t TransferLayers() const noexcept {
		return IsVolume() ? extent.depth : resources.layers;
	}
	[[nodiscard]] vk::Extent2D BlockExtent() const noexcept {
		const auto shift = Prospero::BlockCompressedBytesPerBlock(guest_format) != 0 ? 2u : 0u;
		return {pitch >> shift, extent.height >> shift};
	}
	[[nodiscard]] bool IsCompatible(const ImageInfo& other) const noexcept {
		return pixel_format == other.pixel_format && samples == other.samples &&
		       bytes_per_block == other.bytes_per_block;
	}
	[[nodiscard]] int32_t MipOf(const ImageInfo& container) const noexcept {
		if (!IsCompatible(container) || tile_mode != container.tile_mode || resources.levels != 1 ||
		    container.resources.layers == 0 ||
		    container.resources.levels > container.mip_layout.size()) {
			return -1;
		}
		if (HasStencil() != container.HasStencil() ||
		    (HasStencil() && (stencil.address < container.stencil.address ||
		                      stencil.End() > container.stencil.End()))) {
			return -1;
		}

		int32_t mip = -1;
		for (uint32_t level = 0; level < container.resources.levels; level++) {
			const auto& layout = container.mip_layout[level];
			if (layout.size == 0 || layout.size % container.resources.layers != 0 ||
			    container.data.address > UINT64_MAX - layout.offset) {
				continue;
			}
			const auto mip_base   = container.data.address + layout.offset;
			const auto slice_size = layout.size / container.resources.layers;
			if (slice_size == 0 || mip_base > UINT64_MAX - layout.size) {
				continue;
			}
			const auto mip_end = mip_base + layout.size;
			if (data.address >= mip_base && data.address < mip_end &&
			    (data.address - mip_base) % slice_size == 0) {
				mip = static_cast<int32_t>(level);
				break;
			}
		}
		if (mip < 0) {
			return -1;
		}

		const auto level = static_cast<uint32_t>(mip);
		if (extent.width != std::max(container.extent.width >> level, 1u) ||
		    extent.height != std::max(container.extent.height >> level, 1u)) {
			return -1;
		}
		const auto mip_depth = std::max(container.extent.depth >> level, 1u);
		if (container.type == Prospero::ImageType::kColor3D &&
		    type == Prospero::ImageType::kColor2D) {
			if (resources.layers != mip_depth) {
				return -1;
			}
		} else if (type != container.type) {
			return -1;
		}
		return mip;
	}
	[[nodiscard]] int32_t SliceOf(const ImageInfo& container, int32_t mip) const noexcept {
		if (!IsCompatible(container) || type != container.type || mip < 0 ||
		    static_cast<uint32_t>(mip) >= container.resources.levels ||
		    container.resources.levels > container.mip_layout.size() ||
		    container.resources.layers == 0 || data.size == 0) {
			return -1;
		}
		const auto level = static_cast<uint32_t>(mip);
		if (extent.width != std::max(container.extent.width >> level, 1u) ||
		    extent.height != std::max(container.extent.height >> level, 1u)) {
			return -1;
		}
		const auto& layout = container.mip_layout[level];
		if (layout.size == 0 || layout.size % container.resources.layers != 0 ||
		    container.data.address > UINT64_MAX - layout.offset) {
			return -1;
		}
		const auto slice_size = layout.size / container.resources.layers;
		if (slice_size == 0 || data.size % slice_size != 0) {
			return -1;
		}
		const auto mip_base = container.data.address + layout.offset;
		if (data.address < mip_base) {
			return -1;
		}
		const auto address_delta = data.address - mip_base;
		if (address_delta % data.size != 0 || address_delta / data.size > INT32_MAX) {
			return -1;
		}
		return static_cast<int32_t>(address_delta / data.size);
	}
};

struct ImageViewInfo {
	vk::Format           format      = vk::Format::eUndefined;
	vk::ImageViewType    type        = vk::ImageViewType::e2D;
	vk::ImageAspectFlags aspect      = vk::ImageAspectFlagBits::eColor;
	uint32_t             base_level  = 0;
	uint32_t             level_count = 1;
	uint32_t             base_layer  = 0;
	uint32_t             layer_count = 1;
	vk::ComponentMapping mapping     = {};
	vk::ImageUsageFlags  usage       = vk::ImageUsageFlagBits::eSampled;

	[[nodiscard]] bool operator==(const ImageViewInfo& rhs) const noexcept {
		return format == rhs.format && type == rhs.type && aspect == rhs.aspect &&
		       base_level == rhs.base_level && level_count == rhs.level_count &&
		       base_layer == rhs.base_layer && layer_count == rhs.layer_count &&
		       mapping.r == rhs.mapping.r && mapping.g == rhs.mapping.g &&
		       mapping.b == rhs.mapping.b && mapping.a == rhs.mapping.a && usage == rhs.usage;
	}
};

struct DepthFormatPolicy {
	Prospero::DepthFormat     depth_format;
	Prospero::BufferFormat    guest_format;
	uint32_t                  bytes_per_element;
	vk::Format                sampled_view_format;
	vk::Format                depth_attachment_format;
	std::array<vk::Format, 3> stencil_attachment_formats;
};

inline constexpr std::array<DepthFormatPolicy, 2> DEPTH_FORMAT_POLICIES {{
    {Prospero::DepthFormat::kZ16,
     Prospero::BufferFormat::k16UNorm,
     2,
     vk::Format::eR16Unorm,
     vk::Format::eD16Unorm,
     {vk::Format::eD16UnormS8Uint, vk::Format::eD24UnormS8Uint, vk::Format::eD32SfloatS8Uint}},
    {Prospero::DepthFormat::kZ32F,
     Prospero::BufferFormat::k32Float,
     4,
     vk::Format::eR32Sfloat,
     vk::Format::eD32Sfloat,
     {vk::Format::eD32SfloatS8Uint, vk::Format::eUndefined, vk::Format::eUndefined}},
}};

[[nodiscard]] inline constexpr const DepthFormatPolicy*
FindDepthFormatPolicy(Prospero::DepthFormat depth_format) noexcept {
	for (const auto& policy: DEPTH_FORMAT_POLICIES) {
		if (policy.depth_format == depth_format) {
			return &policy;
		}
	}
	return nullptr;
}

[[nodiscard]] inline constexpr const DepthFormatPolicy*
FindGuestDepthFormatPolicy(Prospero::BufferFormat guest_format) noexcept {
	for (const auto& policy: DEPTH_FORMAT_POLICIES) {
		if (policy.guest_format == guest_format) {
			return &policy;
		}
	}
	return nullptr;
}

[[nodiscard]] inline constexpr bool IsStencilAttachmentFormat(const DepthFormatPolicy& policy,
                                                              vk::Format format) noexcept {
	for (const auto candidate: policy.stencil_attachment_formats) {
		if (candidate != vk::Format::eUndefined && candidate == format) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] inline constexpr vk::Format DepthAttachmentFormat(const DepthFormatPolicy& policy,
                                                                bool has_stencil) noexcept {
	return has_stencil ? policy.stencil_attachment_formats.front() : policy.depth_attachment_format;
}

[[nodiscard]] inline constexpr vk::Format
DepthAttachmentFormat(Prospero::DepthFormat   depth_format,
                      Prospero::StencilFormat stencil_format) noexcept {
	bool has_stencil = false;
	switch (stencil_format) {
		case Prospero::StencilFormat::kInvalid: break;
		case Prospero::StencilFormat::k8UInt: has_stencil = true; break;
		default: return vk::Format::eUndefined;
	}
	const auto* policy = FindDepthFormatPolicy(depth_format);
	return policy == nullptr ? vk::Format::eUndefined : DepthAttachmentFormat(*policy, has_stencil);
}

[[nodiscard]] inline constexpr vk::ImageUsageFlags DepthTargetImageUsage() noexcept {
	return vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled |
	       vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
}

[[nodiscard]] inline constexpr vk::Format DepthAspectTransferFormat(vk::Format format) noexcept {
	switch (format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD16UnormS8Uint: return vk::Format::eD16Unorm;
		case vk::Format::eD24UnormS8Uint: return vk::Format::eX8D24UnormPack32;
		case vk::Format::eD32Sfloat:
		case vk::Format::eD32SfloatS8Uint: return vk::Format::eD32Sfloat;
		default: return vk::Format::eUndefined;
	}
}

inline bool ImageInfo::IsDepth() const noexcept {
	return DepthAspectTransferFormat(pixel_format) != vk::Format::eUndefined;
}

[[nodiscard]] inline constexpr uint32_t DepthAspectTransferBytes(vk::Format format) noexcept {
	switch (DepthAspectTransferFormat(format)) {
		case vk::Format::eD16Unorm: return 2;
		case vk::Format::eX8D24UnormPack32:
		case vk::Format::eD32Sfloat: return 4;
		default: return 0;
	}
}

[[nodiscard]] inline constexpr uint32_t EncodeD16AsD24(uint16_t value) noexcept {
	return static_cast<uint32_t>((static_cast<uint64_t>(value) * 0x00ffffffu + 0x7fffu) / 0xffffu);
}

[[nodiscard]] inline uint32_t EncodeD16AsD32(uint16_t value) noexcept {
	return std::bit_cast<uint32_t>(static_cast<float>(value) / 65535.0f);
}

[[nodiscard]] inline constexpr bool
IsSupportedSampledDepthFormat(vk::Format image_format, Prospero::BufferFormat guest_format,
                              vk::Format view_format) noexcept {
	const auto* policy = FindGuestDepthFormatPolicy(guest_format);
	return policy != nullptr && view_format == policy->sampled_view_format &&
	       (image_format == policy->depth_attachment_format ||
	        IsStencilAttachmentFormat(*policy, image_format));
}

[[nodiscard]] inline constexpr bool IsSupportedSampledDepthFormat(vk::Format image_format,
                                                                  vk::Format view_format) noexcept {
	for (const auto& policy: DEPTH_FORMAT_POLICIES) {
		if (IsSupportedSampledDepthFormat(image_format, policy.guest_format, view_format)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] inline constexpr bool IsSupportedDepthTargetFormat(const ImageInfo& info) {
	const auto* policy = FindGuestDepthFormatPolicy(info.guest_format);
	return policy != nullptr && info.bytes_per_block == policy->bytes_per_element &&
	       (info.HasStencil() ? IsStencilAttachmentFormat(*policy, info.pixel_format)
	                          : info.pixel_format == policy->depth_attachment_format);
}

[[nodiscard]] inline constexpr bool IsSupportedDepthPlaneReadback(const ImageInfo& info) {
	if (!IsSupportedDepthTargetFormat(info)) {
		return false;
	}
	const auto transfer_bytes = DepthAspectTransferBytes(info.pixel_format);
	return transfer_bytes == info.bytes_per_block ||
	       (info.bytes_per_block == sizeof(uint16_t) && transfer_bytes == sizeof(uint32_t));
}

[[nodiscard]] inline VideoOutCompression
ClassifyVideoOutCompression(bool compressed, uint64_t metadata_address, uint32_t dcc_control,
                            uint64_t dcc_clear_color) noexcept {
	constexpr uint32_t DCC_256_256_0 = 0x00000048u;
	constexpr uint32_t DCC_256_64_64 = 0x00000208u;
	if (!compressed) {
		return metadata_address == 0 && dcc_control == 0 && dcc_clear_color == 0
		           ? VideoOutCompression::Uncompressed
		           : VideoOutCompression::Unsupported;
	}
	if (metadata_address == 0 || (metadata_address & 0xffu) != 0 || dcc_clear_color != 0) {
		return VideoOutCompression::Unsupported;
	}
	switch (dcc_control) {
		case DCC_256_256_0: return VideoOutCompression::Dcc256_256_0;
		case DCC_256_64_64: return VideoOutCompression::Dcc256_64_64;
		default: return VideoOutCompression::Unsupported;
	}
}

[[nodiscard]] inline constexpr bool
CanUseVideoOutNativeWithoutUpload(VideoOutCompression compression, bool render_target,
                                  bool gpu_modified, bool guest_modified) noexcept {
	return compression != VideoOutCompression::Uncompressed &&
	       compression != VideoOutCompression::Unsupported && !guest_modified &&
	       (render_target || gpu_modified);
}

struct VideoOutPixelFormatInfo {
	vk::Format             format            = vk::Format::eUndefined;
	Prospero::BufferFormat guest_format      = Prospero::BufferFormat::kInvalid;
	uint32_t               bytes_per_element = 0;
	bool                   bgra16            = false;
};

struct VideoOutFormatPolicy {
	uint64_t                pixel_format;
	VideoOutPixelFormatInfo info;
};

inline constexpr std::array<VideoOutFormatPolicy, 6> VIDEO_OUT_FORMAT_POLICIES {{
    {0x8000000022000000ull,
     {vk::Format::eR8G8B8A8Srgb, Prospero::BufferFormat::k8_8_8_8Srgb, 4, false}},
    {0x8000000000000000ull,
     {vk::Format::eB8G8R8A8Srgb, Prospero::BufferFormat::k8_8_8_8Srgb, 4, false}},
    {0x8100000022000000ull,
     {vk::Format::eA2B10G10R10UnormPack32, Prospero::BufferFormat::k10_10_10_2UNorm, 4, false}},
    {0x8100000000000000ull,
     {vk::Format::eA2R10G10B10UnormPack32, Prospero::BufferFormat::k10_10_10_2UNorm, 4, false}},
    {0xc001000622000000ull,
     {vk::Format::eR16G16B16A16Sfloat, Prospero::BufferFormat::k16_16_16_16Float, 8, false}},
    {0xc001000600000000ull,
     {vk::Format::eR16G16B16A16Sfloat, Prospero::BufferFormat::k16_16_16_16Float, 8, true}},
}};

[[nodiscard]] inline bool DecodeVideoOutPixelFormat(uint64_t                 pixel_format,
                                                    VideoOutPixelFormatInfo& info) {
	for (const auto& policy: VIDEO_OUT_FORMAT_POLICIES) {
		if (policy.pixel_format == pixel_format) {
			info = policy.info;
			return true;
		}
	}
	return false;
}

[[nodiscard]] inline bool IsSupportedVideoOutFormat(const ImageInfo& info) {
	for (const auto& policy: VIDEO_OUT_FORMAT_POLICIES) {
		if (info.pixel_format == policy.info.format &&
		    info.guest_format == policy.info.guest_format &&
		    info.bytes_per_block == policy.info.bytes_per_element &&
		    info.bgra16 == policy.info.bgra16) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] inline constexpr bool
IsSupportedDisplayRenderTargetTileMode(Prospero::TileMode tile_mode) noexcept {
	return tile_mode == Prospero::TileMode::kRenderTarget;
}

[[nodiscard]] inline constexpr bool IsSupportedStandard64RenderTarget(const ImageInfo& info) {
	if (info.tile_mode != Prospero::TileMode::kStandard64KB || info.data.address == 0 ||
	    (info.data.address & 0xffffu) != 0 || info.extent.width == 0 || info.extent.height == 0 ||
	    info.bytes_per_block != 4 || info.resources.levels != 1 || info.resources.layers != 1 ||
	    info.samples != 1) {
		return false;
	}
	const auto expected_pitch =
	    (static_cast<uint64_t>(info.extent.width) + 127u) & ~uint64_t {127u};
	const auto padded_height =
	    (static_cast<uint64_t>(info.extent.height) + 127u) & ~uint64_t {127u};
	return expected_pitch <= UINT32_MAX && info.pitch == expected_pitch &&
	       expected_pitch <= UINT64_MAX / padded_height / info.bytes_per_block &&
	       info.data.size == expected_pitch * padded_height * info.bytes_per_block;
}

[[nodiscard]] inline constexpr bool IsTiledRenderTarget(const ImageInfo& info) noexcept {
	return info.tile_mode == Prospero::TileMode::kRenderTarget ||
	       IsSupportedStandard64RenderTarget(info);
}

[[nodiscard]] inline bool DecodePackedColorClear(vk::Format format, uint32_t packed,
                                                 vk::ClearColorValue& clear) {
	vk::ClearColorValue next {};
	const auto unorm8 = [](uint32_t value) { return static_cast<float>(value & 0xffu) / 255.0f; };
	const auto srgb8  = [](uint32_t value) {
		const auto encoded = static_cast<float>(value & 0xffu) / 255.0f;
		return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
	};
	switch (format) {
		case vk::Format::eR32Uint: next.uint32[0] = packed; break;
		case vk::Format::eR32Sint: next.int32[0] = static_cast<int32_t>(packed); break;
		case vk::Format::eR8G8B8A8Srgb:
			next.float32[0] = srgb8(packed);
			next.float32[1] = srgb8(packed >> 8u);
			next.float32[2] = srgb8(packed >> 16u);
			next.float32[3] = unorm8(packed >> 24u);
			break;
		case vk::Format::eB8G8R8A8Srgb:
			next.float32[0] = srgb8(packed >> 16u);
			next.float32[1] = srgb8(packed >> 8u);
			next.float32[2] = srgb8(packed);
			next.float32[3] = unorm8(packed >> 24u);
			break;
		case vk::Format::eR8G8B8A8Unorm:
			next.float32[0] = unorm8(packed);
			next.float32[1] = unorm8(packed >> 8u);
			next.float32[2] = unorm8(packed >> 16u);
			next.float32[3] = unorm8(packed >> 24u);
			break;
		case vk::Format::eB8G8R8A8Unorm:
			next.float32[0] = unorm8(packed >> 16u);
			next.float32[1] = unorm8(packed >> 8u);
			next.float32[2] = unorm8(packed);
			next.float32[3] = unorm8(packed >> 24u);
			break;
		case vk::Format::eA2B10G10R10UnormPack32:
			next.float32[0] = static_cast<float>(packed & 0x3ffu) / 1023.0f;
			next.float32[1] = static_cast<float>((packed >> 10u) & 0x3ffu) / 1023.0f;
			next.float32[2] = static_cast<float>((packed >> 20u) & 0x3ffu) / 1023.0f;
			next.float32[3] = static_cast<float>((packed >> 30u) & 0x3u) / 3.0f;
			break;
		case vk::Format::eA2R10G10B10UnormPack32:
			next.float32[0] = static_cast<float>((packed >> 20u) & 0x3ffu) / 1023.0f;
			next.float32[1] = static_cast<float>((packed >> 10u) & 0x3ffu) / 1023.0f;
			next.float32[2] = static_cast<float>(packed & 0x3ffu) / 1023.0f;
			next.float32[3] = static_cast<float>((packed >> 30u) & 0x3u) / 3.0f;
			break;
		default: return false;
	}
	clear = next;
	return true;
}

[[nodiscard]] inline bool DecodePackedStencilClear(uint32_t packed, uint8_t& clear) {
	const auto value = static_cast<uint8_t>(packed);
	if (packed != static_cast<uint32_t>(value) * 0x01010101u) {
		return false;
	}
	clear = value;
	return true;
}

[[nodiscard]] inline bool DecodePackedDepthClear(vk::Format format, uint32_t packed, float& clear) {
	if (format != vk::Format::eD32Sfloat && format != vk::Format::eD32SfloatS8Uint) {
		return false;
	}
	const auto value = std::bit_cast<float>(packed);
	if (!std::isfinite(value) || value < 0.0f || value > 1.0f) {
		return false;
	}
	clear = value;
	return true;
}

[[nodiscard]] inline bool ImageRangeOverlaps(uint64_t left, uint64_t left_size, uint64_t right,
                                             uint64_t right_size) {
	if (left_size == 0 || right_size == 0 || left > UINT64_MAX - left_size ||
	    right > UINT64_MAX - right_size) {
		EXIT("invalid image overlap range\n");
	}
	return left < right + right_size && right < left + left_size;
}

[[nodiscard]] inline bool ImageRangeOverlaps(GuestRange left, GuestRange right) {
	return ImageRangeOverlaps(left.address, left.size, right.address, right.size);
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

[[nodiscard]] inline bool ImagePageRangesOverlap(GuestRange left, GuestRange right) {
	return ImagePageRangesOverlap(left.address, left.size, right.address, right.size);
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEINFO_H_
