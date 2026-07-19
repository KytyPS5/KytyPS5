#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEINFO_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEINFO_H_

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

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
	uint64_t   address           = 0;
	uint64_t   size              = 0;
	vk::Format format            = vk::Format::eUndefined;
	uint32_t   width             = 0;
	uint32_t   height            = 0;
	uint32_t   pitch             = 0;
	uint32_t   bytes_per_element = 0;
	uint32_t   tile_mode         = 0;
	uint32_t   levels            = 1;
	uint32_t   layers            = 1;
};

// Common image-to-buffer copy description. Storage images and render targets keep distinct
// cache records today, but downloads should consume one normalized layout just as uploads do.
struct ColorImageTransferInfo {
	uint64_t   address           = 0;
	uint64_t   size              = 0;
	vk::Format format            = vk::Format::eUndefined;
	uint32_t   width             = 0;
	uint32_t   height            = 0;
	uint32_t   pitch             = 0;
	uint32_t   bytes_per_element = 0;
	uint32_t   tile_mode         = 0;
	uint32_t   levels            = 1;
};

[[nodiscard]] inline ColorImageTransferInfo
MakeColorImageTransferInfo(const ImageInfo& info, vk::Format format,
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
	uint64_t   address                  = 0;
	uint64_t   size                     = 0;
	uint64_t   stencil_address          = 0;
	uint64_t   stencil_size             = 0;
	uint64_t   htile_address            = 0;
	uint64_t   htile_size               = 0;
	vk::Format format                   = vk::Format::eUndefined;
	uint32_t   guest_format             = 0;
	uint32_t   width                    = 0;
	uint32_t   height                   = 0;
	uint32_t   pitch                    = 0;
	uint32_t   bytes_per_element        = 0;
	uint32_t   tile_mode                = 0;
	uint32_t   layers                   = 1;
	bool       depth_load_clear         = false;
	bool       stencil_load_clear       = false;
	bool       stencil_access           = false;
	bool       stencil_htile_compressed = false;
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
FindDepthFormatPolicy(uint32_t depth_format) noexcept {
	for (const auto& policy: DEPTH_FORMAT_POLICIES) {
		if (Prospero::GpuEnumValue(policy.depth_format) == depth_format) {
			return &policy;
		}
	}
	return nullptr;
}

[[nodiscard]] inline constexpr const DepthFormatPolicy*
FindGuestDepthFormatPolicy(uint32_t guest_format) noexcept {
	for (const auto& policy: DEPTH_FORMAT_POLICIES) {
		if (Prospero::GpuEnumValue(policy.guest_format) == guest_format) {
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

[[nodiscard]] inline constexpr vk::Format DepthAttachmentFormat(uint32_t depth_format,
                                                                uint32_t stencil_format) noexcept {
	bool has_stencil = false;
	switch (static_cast<Prospero::StencilFormat>(stencil_format)) {
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

[[nodiscard]] inline constexpr uint32_t DepthAspectTransferBytes(vk::Format format) noexcept {
	switch (DepthAspectTransferFormat(format)) {
		case vk::Format::eD16Unorm: return 2;
		case vk::Format::eX8D24UnormPack32:
		case vk::Format::eD32Sfloat: return 4;
		default: return 0;
	}
}

[[nodiscard]] inline constexpr uint32_t EncodeD16AsD24(uint16_t value) noexcept {
	// Preserve the guest UNORM value when widening 16 bits to the 24-bit transfer plane.
	return static_cast<uint32_t>((static_cast<uint64_t>(value) * 0x00ffffffu + 0x7fffu) / 0xffffu);
}

[[nodiscard]] inline uint32_t EncodeD16AsD32(uint16_t value) noexcept {
	return std::bit_cast<uint32_t>(static_cast<float>(value) / 65535.0f);
}

[[nodiscard]] inline constexpr bool IsSupportedSampledDepthFormat(vk::Format image_format,
                                                                  uint32_t   guest_format,
                                                                  vk::Format view_format) noexcept {
	const auto* policy = FindGuestDepthFormatPolicy(guest_format);
	return policy != nullptr && view_format == policy->sampled_view_format &&
	       (image_format == policy->depth_attachment_format ||
	        IsStencilAttachmentFormat(*policy, image_format));
}

[[nodiscard]] inline constexpr bool IsSupportedSampledDepthFormat(vk::Format image_format,
                                                                  vk::Format view_format) noexcept {
	for (const auto& policy: DEPTH_FORMAT_POLICIES) {
		if (IsSupportedSampledDepthFormat(image_format, Prospero::GpuEnumValue(policy.guest_format),
		                                  view_format)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] inline constexpr bool IsSupportedDepthTargetFormat(const DepthTargetInfo& info) {
	const bool  has_stencil = info.stencil_address != 0 || info.stencil_size != 0;
	const auto* policy      = FindGuestDepthFormatPolicy(info.guest_format);
	return policy != nullptr && info.bytes_per_element == policy->bytes_per_element &&
	       (has_stencil ? IsStencilAttachmentFormat(*policy, info.format)
	                    : info.format == policy->depth_attachment_format);
}

enum class VideoOutCompression : uint8_t { Uncompressed, Dcc256_256_0, Dcc256_64_64, Unsupported };

struct VideoOutInfo {
	uint64_t            address           = 0;
	uint64_t            size              = 0;
	uint64_t            metadata_address  = 0;
	vk::Format          format            = vk::Format::eUndefined;
	uint32_t            guest_format      = 0;
	uint32_t            width             = 0;
	uint32_t            height            = 0;
	uint32_t            pitch             = 0;
	uint32_t            bytes_per_element = 0;
	uint32_t            tile_mode         = 0;
	uint32_t            dcc_control       = 0;
	VideoOutCompression compression       = VideoOutCompression::Unsupported;
	bool                bgra16            = false;
};

[[nodiscard]] inline VideoOutCompression
ClassifyVideoOutCompression(bool compressed, uint64_t metadata_address, uint32_t dcc_control,
                            uint64_t dcc_clear_color) noexcept {
	constexpr uint32_t VIDEO_OUT_DCC_CONTROL_256_256_0 = 0x00000048u;
	constexpr uint32_t VIDEO_OUT_DCC_CONTROL_256_64_64 = 0x00000208u;
	if (!compressed) {
		return metadata_address == 0 && dcc_control == 0 && dcc_clear_color == 0
		           ? VideoOutCompression::Uncompressed
		           : VideoOutCompression::Unsupported;
	}
	if (metadata_address == 0 || (metadata_address & 0xffu) != 0 || dcc_clear_color != 0) {
		return VideoOutCompression::Unsupported;
	}
	switch (dcc_control) {
		case VIDEO_OUT_DCC_CONTROL_256_256_0: return VideoOutCompression::Dcc256_256_0;
		case VIDEO_OUT_DCC_CONTROL_256_64_64: return VideoOutCompression::Dcc256_64_64;
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
	vk::Format format            = vk::Format::eUndefined;
	uint32_t   guest_format      = 0;
	uint32_t   bytes_per_element = 0;
	bool       bgra16            = false;
};

struct VideoOutFormatPolicy {
	uint64_t                pixel_format;
	VideoOutPixelFormatInfo info;
};

inline constexpr std::array<VideoOutFormatPolicy, 6> VIDEO_OUT_FORMAT_POLICIES {{
    {0x8000000022000000ull,
     {vk::Format::eR8G8B8A8Srgb, Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8Srgb), 4,
      false}},
    {0x8000000000000000ull,
     {vk::Format::eB8G8R8A8Srgb, Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8Srgb), 4,
      false}},
    {0x8100000022000000ull,
     {vk::Format::eA2B10G10R10UnormPack32,
      Prospero::GpuEnumValue(Prospero::BufferFormat::k10_10_10_2UNorm), 4, false}},
    {0x8100000000000000ull,
     {vk::Format::eA2R10G10B10UnormPack32,
      Prospero::GpuEnumValue(Prospero::BufferFormat::k10_10_10_2UNorm), 4, false}},
    {0xc001000622000000ull,
     {vk::Format::eR16G16B16A16Sfloat,
      Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float), 8, false}},
    {0xc001000600000000ull,
     {vk::Format::eR16G16B16A16Sfloat,
      Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float), 8, true}},
}};

[[nodiscard]] inline bool DecodeVideoOutPixelFormat(uint64_t                 pixel_format,
                                                    VideoOutPixelFormatInfo* info) {
	if (info == nullptr) {
		return false;
	}
	for (const auto& policy: VIDEO_OUT_FORMAT_POLICIES) {
		if (policy.pixel_format == pixel_format) {
			*info = policy.info;
			return true;
		}
	}
	return false;
}

[[nodiscard]] inline bool IsSupportedVideoOutFormat(const VideoOutInfo& info) {
	for (const auto& policy: VIDEO_OUT_FORMAT_POLICIES) {
		if (info.format == policy.info.format && info.guest_format == policy.info.guest_format &&
		    info.bytes_per_element == policy.info.bytes_per_element &&
		    info.bgra16 == policy.info.bgra16) {
			return true;
		}
	}
	return false;
}

enum class DepthOverlap : uint8_t {
	None,
	RetireSampled,
	RetireStorage,
	ExpandTarget,
	DiscardTarget,
	Unsupported
};
enum class DepthTransitionSource : uint8_t { None, Guest, Native };
enum class RenderTargetOverlap : uint8_t {
	None,
	RetireSampled,
	PreserveStorage,
	ExpandTarget,
	RetireTarget,
	Unsupported
};
enum class SampledOverlap : uint8_t { None, ReadOnlyAlias, Unsupported };
enum class StorageSampledOverlap : uint8_t { None, ExactImage, Unsupported };
enum class StorageSampledViewShape : uint8_t { Image2D, Image2DArray, Image3D, Unsupported };
enum class StorageImageOverlap : uint8_t { None, RetireSampled, PageNeighbor, Unsupported };
enum class HostWriteOverlap : uint8_t { None, InvalidateImage, Unsupported };
enum class BufferImageBinding : uint8_t {
	Texture,
	VideoOut,
	RenderTarget,
	StorageTexture,
	DepthTarget,
	Unsupported
};
enum class BufferImageWrite : uint8_t {
	None,
	InvalidateTexture,
	InvalidateVideoOut,
	InvalidateStorageTexture,
	InvalidateDepthTarget,
	InvalidateRenderTarget,
	SynchronizeRenderTarget,
	SynchronizeStorageTexture,
	SynchronizeDepthTarget,
	SynchronizeVideoOut,
	Unsupported
};

enum class StorageBufferRebind : uint8_t { Reuse, RefreshFromBacking, Unsupported };
enum class MetaImageOverlap : uint8_t { RetainSampled, RetireTarget, Unsupported };

[[nodiscard]] inline constexpr uint32_t SelectImageBackingBaseLevel(bool     storage,
                                                                    uint32_t view_base_level) {
	// Storage descriptors select a per-mip view of one full allocation. Backing creation must not
	// depend on which view happens to be bound first.
	return storage ? 0u : view_base_level;
}

[[nodiscard]] inline constexpr bool
IsDepthUintTextureReinterpretation(vk::Format image_format, uint32_t guest_format,
                                   vk::Format view_format) noexcept {
	switch (image_format) {
		case vk::Format::eD32Sfloat:
			return guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt) &&
			       view_format == vk::Format::eR32Uint;
		default: return false;
	}
}

[[nodiscard]] inline constexpr bool NeedsStaticSampledArrayView(bool shader_array,
                                                                bool dynamic_view_selected) {
	return shader_array && !dynamic_view_selected;
}

[[nodiscard]] inline constexpr StorageSampledViewShape
SelectStorageSampledViewShape(uint32_t type, uint32_t depth, uint32_t backing_layers) noexcept {
	switch (static_cast<Prospero::ImageType>(type)) {
		case Prospero::ImageType::kColor2D:
			return depth == 1 && backing_layers == 1 ? StorageSampledViewShape::Image2D
			                                         : StorageSampledViewShape::Unsupported;
		case Prospero::ImageType::kColor2DArray:
			return depth != 0 && depth == backing_layers ? StorageSampledViewShape::Image2DArray
			                                             : StorageSampledViewShape::Unsupported;
		case Prospero::ImageType::kColor3D:
			return depth != 0 && backing_layers == 1 ? StorageSampledViewShape::Image3D
			                                         : StorageSampledViewShape::Unsupported;
		default: return StorageSampledViewShape::Unsupported;
	}
}

[[nodiscard]] inline constexpr bool IsSupportedRenderTargetElementSize(uint32_t size) noexcept {
	switch (size) {
		case 1:
		case 2:
		case 4:
		case 8: return true;
		default: return false;
	}
}

[[nodiscard]] inline constexpr bool
IsSupportedDisplayRenderTargetTileMode(uint32_t tile_mode) noexcept {
	return tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
}

[[nodiscard]] inline constexpr bool
IsSupportedStandard64RenderTarget(const RenderTargetInfo& info) noexcept {
	if (info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kStandard64KB) ||
	    info.address == 0 || (info.address & 0xffffu) != 0 || info.width == 0 || info.height == 0 ||
	    info.bytes_per_element != 4 || info.levels != 1 || info.layers != 1) {
		return false;
	}
	const auto expected_pitch = (static_cast<uint64_t>(info.width) + 127u) & ~uint64_t {127u};
	const auto padded_height  = (static_cast<uint64_t>(info.height) + 127u) & ~uint64_t {127u};
	return expected_pitch <= UINT32_MAX && info.pitch == expected_pitch &&
	       expected_pitch <= UINT64_MAX / padded_height / info.bytes_per_element &&
	       info.size == expected_pitch * padded_height * info.bytes_per_element;
}

[[nodiscard]] inline constexpr bool IsTiledRenderTarget(const RenderTargetInfo& info) noexcept {
	return info.tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) ||
	       IsSupportedStandard64RenderTarget(info);
}

[[nodiscard]] inline constexpr DepthTransitionSource
SelectDepthTransitionSource(bool depth_load_clear, bool sampled_native_available,
                            bool sampled_cpu_dirty, bool sampled_buffer_modified,
                            bool buffer_overlap, bool buffer_cpu_dirty) noexcept {
	if (depth_load_clear) {
		return DepthTransitionSource::None;
	}
	return sampled_native_available && !sampled_cpu_dirty && !sampled_buffer_modified &&
	               !(buffer_overlap && buffer_cpu_dirty)
	           ? DepthTransitionSource::Native
	           : DepthTransitionSource::Guest;
}

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

[[nodiscard]] inline bool DecodePackedColorClear(vk::Format format, uint32_t packed,
                                                 vk::ClearColorValue* clear) {
	if (clear == nullptr) {
		return false;
	}
	vk::ClearColorValue next {};
	const auto unorm8 = [](uint32_t value) { return static_cast<float>(value & 0xffu) / 255.0f; };
	switch (format) {
		case vk::Format::eR8G8B8A8Srgb:
			next.float32[0] = DecodeSrgbClearComponent(packed);
			next.float32[1] = DecodeSrgbClearComponent(packed >> 8u);
			next.float32[2] = DecodeSrgbClearComponent(packed >> 16u);
			next.float32[3] = unorm8(packed >> 24u);
			break;
		case vk::Format::eB8G8R8A8Srgb:
			next.float32[0] = DecodeSrgbClearComponent(packed >> 16u);
			next.float32[1] = DecodeSrgbClearComponent(packed >> 8u);
			next.float32[2] = DecodeSrgbClearComponent(packed);
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

[[nodiscard]] inline bool DecodePackedDepthClear(vk::Format format, uint32_t packed, float* clear) {
	if (clear == nullptr ||
	    (format != vk::Format::eD32Sfloat && format != vk::Format::eD32SfloatS8Uint)) {
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
	    target.format == vk::Format::eD32Sfloat || target.format == vk::Format::eD32SfloatS8Uint;
	return address == target.address && size == target.size && target.layers == 1 && d32 &&
	       target.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float) &&
	       target.bytes_per_element == 4 &&
	       target.tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kDepth) &&
	       target.htile_address == 0 && target.htile_size == 0;
}

[[nodiscard]] inline bool CanLoadStencilAttachment(const DepthTargetInfo& target,
                                                   bool                   stencil_initialized) {
	const bool has_stencil = target.stencil_address != 0 || target.stencil_size != 0;
	return !has_stencil || !target.stencil_access || target.stencil_load_clear ||
	       stencil_initialized;
}

[[nodiscard]] inline bool CanLoadRawStencilPlane(const DepthTargetInfo& target) {
	const bool has_stencil = target.stencil_address != 0 || target.stencil_size != 0;
	return has_stencil && !target.stencil_htile_compressed;
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

[[nodiscard]] inline bool IsRgba8SrgbReinterpretation(vk::Format cached,
                                                      vk::Format requested) noexcept;

[[nodiscard]] inline bool IsR32UintFloatReinterpretation(vk::Format cached,
                                                         vk::Format requested) noexcept {
	switch (cached) {
		case vk::Format::eR32Uint: return requested == vk::Format::eR32Sfloat;
		case vk::Format::eR32Sfloat: return requested == vk::Format::eR32Uint;
		default: return false;
	}
}

[[nodiscard]] inline bool IsRgba16UintFloatReinterpretation(vk::Format cached,
                                                            vk::Format requested) noexcept {
	switch (cached) {
		case vk::Format::eR16G16B16A16Sfloat: return requested == vk::Format::eR16G16B16A16Uint;
		case vk::Format::eR16G16B16A16Uint: return requested == vk::Format::eR16G16B16A16Sfloat;
		default: return false;
	}
}

[[nodiscard]] inline bool IsRgba8UnormUintReinterpretation(vk::Format cached,
                                                           vk::Format requested) noexcept {
	switch (cached) {
		case vk::Format::eR8G8B8A8Unorm: return requested == vk::Format::eR8G8B8A8Uint;
		case vk::Format::eR8G8B8A8Uint: return requested == vk::Format::eR8G8B8A8Unorm;
		default: return false;
	}
}

[[nodiscard]] inline bool IsSampledDepthExpansion(const ImageInfo&       sampled,
                                                  const DepthTargetInfo& target) noexcept {
	const bool array_expansion =
	    sampled.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray) &&
	    sampled.depth > 1 && target.size <= UINT64_MAX / sampled.depth &&
	    sampled.size == target.size * sampled.depth && sampled.width == target.width &&
	    sampled.height == target.height && sampled.pitch == target.pitch;
	return sampled.address == target.address && sampled.size > target.size &&
	       sampled.format == target.guest_format && target.format == vk::Format::eD32Sfloat &&
	       target.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float) &&
	       target.bytes_per_element == 4 && target.stencil_address == 0 &&
	       target.stencil_size == 0 && target.htile_address == 0 && target.htile_size == 0 &&
	       target.layers == 1 && array_expansion && sampled.base_level == 0 &&
	       sampled.levels == 1 && sampled.view_levels == 1 && sampled.tile == target.tile_mode &&
	       sampled.base_array == 0;
}

[[nodiscard]] inline StorageSampledOverlap
ClassifyStorageSampledOverlap(const ImageInfo& requested, const ImageInfo& cached,
                              vk::Format requested_view_format, vk::Format cached_image_format,
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
	    IsRgba8SrgbReinterpretation(cached_image_format, requested_view_format) ||
	    IsR32UintFloatReinterpretation(cached_image_format, requested_view_format);
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

[[nodiscard]] inline BufferImageWrite
ClassifyBufferImageWrite(uint64_t buffer_address, uint64_t buffer_size, uint64_t image_address,
                         uint64_t image_size, BufferImageBinding binding, bool image_gpu_modified,
                         bool buffer_formatted, bool image_buffer_modified = false) {
	if (!ImagePageRangesOverlap(buffer_address, buffer_size, image_address, image_size)) {
		return BufferImageWrite::None;
	}
	const bool exact = buffer_address == image_address && buffer_size == image_size;
	const auto offset =
	    buffer_address >= image_address ? buffer_address - image_address : UINT64_MAX;
	const bool contained = offset <= image_size && buffer_size <= image_size - offset;
	const auto image_offset =
	    image_address >= buffer_address ? image_address - buffer_address : UINT64_MAX;
	const bool image_contained =
	    image_offset <= buffer_size && image_size <= buffer_size - image_offset;
	const bool buffer_page_aligned =
	    ((buffer_address | buffer_size) & (TRACKER_PAGE_SIZE - 1)) == 0;
	const bool image_page_aligned = ((image_address | image_size) & (TRACKER_PAGE_SIZE - 1)) == 0;
	switch (binding) {
		case BufferImageBinding::Texture:
			return contained && image_page_aligned && !image_gpu_modified
			           ? BufferImageWrite::InvalidateTexture
			           : BufferImageWrite::Unsupported;
		case BufferImageBinding::VideoOut:
			if (!exact || !buffer_page_aligned || !buffer_formatted) {
				return BufferImageWrite::Unsupported;
			}
			return image_gpu_modified ? BufferImageWrite::SynchronizeVideoOut
			                          : BufferImageWrite::InvalidateVideoOut;
		case BufferImageBinding::RenderTarget:
			if (!exact || !buffer_page_aligned || !buffer_formatted) {
				return BufferImageWrite::Unsupported;
			}
			if (image_gpu_modified && !image_buffer_modified) {
				return BufferImageWrite::SynchronizeRenderTarget;
			}
			return !image_gpu_modified && image_buffer_modified
			           ? BufferImageWrite::InvalidateRenderTarget
			           : BufferImageWrite::Unsupported;
		case BufferImageBinding::StorageTexture:
			if (!buffer_page_aligned || !image_page_aligned || !buffer_formatted) {
				return BufferImageWrite::Unsupported;
			}
			if (contained && image_gpu_modified && !image_buffer_modified) {
				return BufferImageWrite::SynchronizeStorageTexture;
			}
			return image_contained && !image_gpu_modified && image_buffer_modified
			           ? BufferImageWrite::InvalidateStorageTexture
			           : BufferImageWrite::Unsupported;
		case BufferImageBinding::DepthTarget:
			if (!exact || !buffer_page_aligned || !buffer_formatted) {
				return BufferImageWrite::Unsupported;
			}
			if (image_gpu_modified && !image_buffer_modified) {
				return BufferImageWrite::SynchronizeDepthTarget;
			}
			return !image_gpu_modified && image_buffer_modified
			           ? BufferImageWrite::InvalidateDepthTarget
			           : BufferImageWrite::Unsupported;
		case BufferImageBinding::Unsupported: return BufferImageWrite::Unsupported;
	}
	return BufferImageWrite::Unsupported;
}

[[nodiscard]] inline StorageBufferRebind
ClassifyStorageBufferRebind(bool buffer_overlap, bool cached_gpu_modified,
                            bool cached_buffer_modified, bool tracker_gpu_modified,
                            bool tracker_cpu_modified, bool coherent_guest_source) noexcept {
	if (cached_gpu_modified != tracker_gpu_modified ||
	    (tracker_gpu_modified && tracker_cpu_modified)) {
		return StorageBufferRebind::Unsupported;
	}
	if (!cached_buffer_modified) {
		return StorageBufferRebind::Reuse;
	}
	return buffer_overlap && !tracker_gpu_modified && coherent_guest_source
	           ? StorageBufferRebind::RefreshFromBacking
	           : StorageBufferRebind::Unsupported;
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
	const bool  has_stencil  = depth.stencil_address != 0 || depth.stencil_size != 0;
	const auto* depth_policy = FindGuestDepthFormatPolicy(depth.guest_format);
	const bool  exact_depth_format =
	    sampled.format == depth.guest_format && depth_policy != nullptr &&
	    depth.bytes_per_element == depth_policy->bytes_per_element &&
	    DepthAspectTransferBytes(depth.format) == depth.bytes_per_element &&
	    (has_stencil ? IsStencilAttachmentFormat(*depth_policy, depth.format)
	                 : depth.format == depth_policy->depth_attachment_format);
	// Keep this equivalent to ResolveDepthOverlap: recreate the sampled color image as
	// depth and preserve its depth aspect. A disjoint stencil component is initialized separately.
	const bool exact_depth_load =
	    !stencil_overlap && !depth.depth_load_clear && sampled.address == depth.address &&
	    sampled.size == depth.size && sampled.width == depth.width &&
	    sampled.height == depth.height && sampled.pitch == depth.pitch && sampled.base_level == 0 &&
	    sampled.levels == 1 && sampled.view_levels == 1 && sampled.tile == depth.tile_mode &&
	    sampled.depth == 1 &&
	    sampled.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) &&
	    depth.layers == 1 && sampled.base_array == 0 && exact_depth_format;
	if (!sampled_gpu_modified && exact_depth_load) {
		return DepthOverlap::RetireSampled;
	}
	if (sampled.address == depth.address && !sampled_gpu_modified && depth.depth_load_clear &&
	    (!has_stencil ||
	     (depth.stencil_address != 0 && depth.stencil_size != 0 && depth.stencil_load_clear))) {
		return DepthOverlap::RetireSampled;
	}
	return DepthOverlap::Unsupported;
}

// Mirrors storage -> depth-target binding transition. An inaccessible stencil aspect
// does not require the otherwise necessary image content copy.
[[nodiscard]] inline DepthOverlap ClassifyStorageDepthOverlap(const ImageInfo&       storage,
                                                              const DepthTargetInfo& depth) {
	const bool overlaps_depth =
	    ImageRangeOverlaps(storage.address, storage.size, depth.address, depth.size);
	const bool overlaps_stencil =
	    depth.stencil_address != 0 && ImageRangeOverlaps(storage.address, storage.size,
	                                                     depth.stencil_address, depth.stencil_size);
	if (!overlaps_depth && !overlaps_stencil) {
		return DepthOverlap::None;
	}
	return !overlaps_depth && overlaps_stencil && !depth.stencil_access
	           ? DepthOverlap::RetireStorage
	           : DepthOverlap::Unsupported;
}

[[nodiscard]] inline RenderTargetOverlap
ClassifyRenderTargetOverlap(const ImageInfo& sampled, bool sampled_gpu_modified, bool same_context,
                            const RenderTargetInfo& target) {
	if (!ImagePageRangesOverlap(sampled.address, sampled.size, target.address, target.size)) {
		return RenderTargetOverlap::None;
	}
	if (!ImageRangeOverlaps(sampled.address, sampled.size, target.address, target.size)) {
		return RenderTargetOverlap::None;
	}
	return !sampled_gpu_modified && same_context ? RenderTargetOverlap::RetireSampled
	                                             : RenderTargetOverlap::Unsupported;
}

[[nodiscard]] inline RenderTargetOverlap
ClassifyStorageRenderTargetOverlap(const ImageInfo& storage, vk::Format storage_format,
                                   bool storage_gpu_modified, bool storage_buffer_modified,
                                   bool storage_cpu_dirty, bool same_context,
                                   const RenderTargetInfo& target) {
	if (!ImagePageRangesOverlap(storage.address, storage.size, target.address, target.size)) {
		return RenderTargetOverlap::None;
	}
	const bool exact_native_image =
	    storage.address == target.address && storage.size == target.size &&
	    storage_format == target.format && storage.width == target.width &&
	    storage.height == target.height && storage.pitch == target.pitch &&
	    storage.base_level == 0 && storage.levels == 1 && storage.view_levels == 1 &&
	    storage.tile == target.tile_mode && storage.depth == 1 &&
	    storage.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) &&
	    storage.base_array == 0 && target.levels == 1 && target.layers == 1;
	return exact_native_image && storage_gpu_modified && !storage_buffer_modified &&
	               !storage_cpu_dirty && same_context
	           ? RenderTargetOverlap::PreserveStorage
	           : RenderTargetOverlap::Unsupported;
}

[[nodiscard]] inline bool IsRgba8SrgbViewFormat(vk::Format format) noexcept {
	return format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eR8G8B8A8Srgb ||
	       format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
}

[[nodiscard]] inline bool IsRgba8SrgbReinterpretation(vk::Format cached,
                                                      vk::Format requested) noexcept {
	switch (cached) {
		case vk::Format::eR8G8B8A8Unorm: return requested == vk::Format::eR8G8B8A8Srgb;
		case vk::Format::eR8G8B8A8Srgb: return requested == vk::Format::eR8G8B8A8Unorm;
		case vk::Format::eB8G8R8A8Unorm: return requested == vk::Format::eB8G8R8A8Srgb;
		case vk::Format::eB8G8R8A8Srgb: return requested == vk::Format::eB8G8R8A8Unorm;
		default: return false;
	}
}

[[nodiscard]] inline bool IsCompatibleRenderTargetView(const RenderTargetInfo& cached,
                                                       const RenderTargetInfo& requested) noexcept {
	return cached.address == requested.address && cached.size == requested.size &&
	       cached.width == requested.width && cached.height == requested.height &&
	       cached.pitch == requested.pitch &&
	       cached.bytes_per_element == requested.bytes_per_element &&
	       cached.tile_mode == requested.tile_mode && cached.levels == requested.levels &&
	       cached.layers == requested.layers &&
	       IsRgba8SrgbReinterpretation(cached.format, requested.format);
}

[[nodiscard]] inline RenderTargetOverlap
ClassifySampledRenderTargetOverlap(const ImageInfo& sampled, const RenderTargetInfo& target,
                                   bool target_buffer_modified, bool same_context) {
	if (!ImagePageRangesOverlap(sampled.address, sampled.size, target.address, target.size)) {
		return RenderTargetOverlap::None;
	}
	if (!ImageRangeOverlaps(sampled.address, sampled.size, target.address, target.size)) {
		return RenderTargetOverlap::None;
	}
	return same_context && !target_buffer_modified ? RenderTargetOverlap::RetireTarget
	                                               : RenderTargetOverlap::Unsupported;
}

[[nodiscard]] inline StorageImageOverlap
ClassifyStorageImageOverlap(uint64_t requested_address, uint64_t requested_size,
                            uint64_t cached_address, uint64_t cached_size, bool sampled,
                            bool same_context, bool gpu_modified, bool buffer_modified,
                            bool tracker_gpu_modified) {
	if (!ImagePageRangesOverlap(requested_address, requested_size, cached_address, cached_size)) {
		return StorageImageOverlap::None;
	}
	if (!ImageRangeOverlaps(requested_address, requested_size, cached_address, cached_size)) {
		return StorageImageOverlap::PageNeighbor;
	}
	return sampled && same_context && !gpu_modified && !buffer_modified && !tracker_gpu_modified
	           ? StorageImageOverlap::RetireSampled
	           : StorageImageOverlap::Unsupported;
}

[[nodiscard]] inline constexpr bool LayeredBackingContains(uint64_t container_size,
                                                           uint32_t container_layers,
                                                           uint64_t view_size,
                                                           uint32_t view_layers) {
	return container_layers >= view_layers && container_layers != 0 && view_layers != 0 &&
	       container_size % container_layers == 0 && view_size % view_layers == 0 &&
	       container_size / container_layers == view_size / view_layers;
}

[[nodiscard]] inline constexpr bool
LayeredPlaneContains(uint64_t container_address, uint64_t container_size, uint32_t container_layers,
                     uint64_t view_address, uint64_t view_size, uint32_t view_layers) {
	return container_address == 0 ? view_address == 0 && view_size == 0
	                              : view_address == container_address &&
	                                    LayeredBackingContains(container_size, container_layers,
	                                                           view_size, view_layers);
}

[[nodiscard]] inline bool
IsCompatibleRenderTargetBacking(const RenderTargetInfo& cached,
                                const RenderTargetInfo& requested) noexcept {
	return cached.address == requested.address &&
	       LayeredBackingContains(cached.size, cached.layers, requested.size, requested.layers) &&
	       cached.width == requested.width && cached.height == requested.height &&
	       cached.pitch == requested.pitch &&
	       cached.bytes_per_element == requested.bytes_per_element &&
	       cached.tile_mode == requested.tile_mode && cached.levels == requested.levels &&
	       (cached.format == requested.format ||
	        IsRgba8SrgbReinterpretation(cached.format, requested.format));
}

[[nodiscard]] inline bool
IsCompatibleDepthTargetBacking(const DepthTargetInfo& cached,
                               const DepthTargetInfo& requested) noexcept {
	return cached.address == requested.address &&
	       LayeredBackingContains(cached.size, cached.layers, requested.size, requested.layers) &&
	       LayeredPlaneContains(cached.stencil_address, cached.stencil_size, cached.layers,
	                            requested.stencil_address, requested.stencil_size,
	                            requested.layers) &&
	       LayeredPlaneContains(cached.htile_address, cached.htile_size, cached.layers,
	                            requested.htile_address, requested.htile_size, requested.layers) &&
	       cached.format == requested.format && cached.guest_format == requested.guest_format &&
	       cached.width == requested.width && cached.height == requested.height &&
	       cached.pitch == requested.pitch &&
	       cached.bytes_per_element == requested.bytes_per_element &&
	       cached.tile_mode == requested.tile_mode &&
	       cached.stencil_htile_compressed == requested.stencil_htile_compressed;
}

[[nodiscard]] inline RenderTargetOverlap
ClassifyRenderTargetOverlap(const RenderTargetInfo& cached, bool cached_gpu_modified,
                            bool cached_buffer_modified, bool same_context,
                            const RenderTargetInfo& requested) {
	if (!ImagePageRangesOverlap(cached.address, cached.size, requested.address, requested.size)) {
		return RenderTargetOverlap::None;
	}
	const bool expand = requested.layers > cached.layers &&
	                    IsCompatibleRenderTargetBacking(requested, cached) &&
	                    cached.format == requested.format;
	if (expand && cached_gpu_modified && !cached_buffer_modified && same_context) {
		return RenderTargetOverlap::ExpandTarget;
	}
	// For an equal-address allocation-pool entry, a changed block raster or block size is a new
	// image allocation, not a view of the old image. In Kyty we can only retire an
	// already-published target with page-isolated storage.
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

[[nodiscard]] inline DepthOverlap ClassifyDepthTargetOverlap(const DepthTargetInfo& cached,
                                                             bool cached_gpu_modified,
                                                             bool cached_buffer_modified,
                                                             bool same_context,
                                                             const DepthTargetInfo& requested) {
	if (!ImagePageRangesOverlap(cached.address, cached.size, requested.address, requested.size)) {
		return DepthOverlap::None;
	}
	const bool expand =
	    requested.layers > cached.layers && IsCompatibleDepthTargetBacking(requested, cached);
	if (expand && cached_gpu_modified && !cached_buffer_modified && same_context) {
		return DepthOverlap::ExpandTarget;
	}
	const bool exact_discard =
	    requested.depth_load_clear && cached_gpu_modified && !cached_buffer_modified &&
	    same_context && cached.address == requested.address && cached.size == requested.size &&
	    cached.stencil_address == 0 && cached.stencil_size == 0 && cached.htile_address == 0 &&
	    cached.htile_size == 0 && requested.stencil_address == 0 && requested.stencil_size == 0 &&
	    requested.htile_address == 0 && requested.htile_size == 0;
	return exact_discard ? DepthOverlap::DiscardTarget : DepthOverlap::Unsupported;
}

[[nodiscard]] inline bool CanRetireBufferOwnedDepthForRenderTarget(
    const DepthTargetInfo& depth, bool gpu_modified, bool buffer_modified, bool same_context,
    bool containing_buffer_source, const RenderTargetInfo& target) noexcept {
	return !gpu_modified && buffer_modified && same_context && containing_buffer_source &&
	       depth.address == target.address && depth.size == target.size &&
	       depth.stencil_address == 0 && depth.stencil_size == 0 && depth.htile_address == 0 &&
	       depth.htile_size == 0;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEINFO_H_
