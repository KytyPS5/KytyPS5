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
	uint32_t   samples           = 1;
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
	uint32_t   samples           = 1;
};

[[nodiscard]] inline ColorImageTransferInfo
MakeColorImageTransferInfo(const ImageInfo& info, vk::Format format,
                           uint32_t bytes_per_element) noexcept {
	return {info.address, info.size,         format,    info.width,  info.height,
	        info.pitch,   bytes_per_element, info.tile, info.levels, 1};
}

[[nodiscard]] inline ColorImageTransferInfo
MakeColorImageTransferInfo(const RenderTargetInfo& info) noexcept {
	return {
	    info.address,           info.size,      info.format, info.width,  info.height, info.pitch,
	    info.bytes_per_element, info.tile_mode, info.levels, info.samples};
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
	uint32_t   samples                  = 1;
	bool       depth_load_clear         = false;
	bool       depth_access             = false;
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

[[nodiscard]] inline constexpr bool IsStencilAspectFormat(vk::Format format) noexcept {
	switch (format) {
		case vk::Format::eD16UnormS8Uint:
		case vk::Format::eD24UnormS8Uint:
		case vk::Format::eD32SfloatS8Uint: return true;
		default: return false;
	}
}

[[nodiscard]] inline constexpr bool IsSupportedSampledStencilFormat(vk::Format image_format,
                                                                    uint32_t guest_format,
                                                                    vk::Format view_format) noexcept {
	return IsStencilAspectFormat(image_format) &&
	       guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k8UInt) &&
	       view_format == vk::Format::eR8Uint;
}

[[nodiscard]] inline constexpr bool IsSupportedSampledStencilFormat(vk::Format image_format,
                                                                    vk::Format view_format) noexcept {
	return IsSupportedSampledStencilFormat(
	    image_format, Prospero::GpuEnumValue(Prospero::BufferFormat::k8UInt), view_format);
}

[[nodiscard]] inline constexpr vk::ImageAspectFlags
SampledDepthTargetAspect(vk::Format image_format, vk::Format view_format) noexcept {
	if (IsSupportedSampledDepthFormat(image_format, view_format)) {
		return vk::ImageAspectFlagBits::eDepth;
	}
	if (IsSupportedSampledStencilFormat(image_format, view_format)) {
		return vk::ImageAspectFlagBits::eStencil;
	}
	return {};
}

[[nodiscard]] inline constexpr bool IsSupportedDepthTargetFormat(const DepthTargetInfo& info) {
	const bool  has_stencil = info.stencil_address != 0 || info.stencil_size != 0;
	const auto* policy      = FindGuestDepthFormatPolicy(info.guest_format);
	return policy != nullptr && info.bytes_per_element == policy->bytes_per_element &&
	       (has_stencil ? IsStencilAttachmentFormat(*policy, info.format)
	                    : info.format == policy->depth_attachment_format);
}

[[nodiscard]] inline constexpr bool IsSupportedDepthReadbackFormat(const DepthTargetInfo& info) {
	const bool has_stencil = info.stencil_address != 0 || info.stencil_size != 0;
	return IsSupportedDepthTargetFormat(info) &&
	       DepthAspectTransferBytes(info.format) == info.bytes_per_element &&
	       (!has_stencil || !info.stencil_htile_compressed);
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
	                                                VideoOutPixelFormatInfo& info) {
	for (const auto& policy: VIDEO_OUT_FORMAT_POLICIES) {
		if (policy.pixel_format == pixel_format) {
			info = policy.info;
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
	RecreateTarget,
	Unsupported
};
enum class DepthTransitionSource : uint8_t { None, Guest, Native };
enum class RenderTargetOverlap : uint8_t {
	None,
	RetireSampled,
	RetireStorage,
	PreserveStorage,
	ExpandTarget,
	RetireTarget,
	Unsupported
};
enum class SampledOverlap : uint8_t { None, ReadOnlyAlias, Unsupported };
enum class StorageSampledOverlap : uint8_t { None, ExactImage, RetireStorage, Unsupported };
enum class StorageSampledViewShape : uint8_t { Image2D, Image2DArray, Image3D, Unsupported };
enum class StorageImageOverlap : uint8_t {
	None,
	RetireSampled,
	RetireStorage,
	PageNeighbor,
	Unsupported
};
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

[[nodiscard]] inline constexpr bool
HasGuestCurrentImageOwnership(bool image_gpu_modified, bool buffer_modified, bool cpu_dirty,
                              bool tracker_gpu_modified) noexcept {
	return !image_gpu_modified && !buffer_modified && !cpu_dirty && !tracker_gpu_modified;
}

enum class StorageBufferRebind : uint8_t { Reuse, RefreshFromBacking, Unsupported };
enum class MetaImageOverlap : uint8_t { RetainSampled, RetireImage, Unsupported };

[[nodiscard]] inline constexpr bool CanRetireGuestCurrentDepthForMetadataReuse(
    bool depth_gpu_modified, bool depth_buffer_modified, bool depth_tracker_gpu_modified,
    bool metadata_gpu_modified, bool metadata_tracker_gpu_modified,
    uint32_t metadata_clear_mask) noexcept {
	return !metadata_gpu_modified && !metadata_tracker_gpu_modified && metadata_clear_mask == 0 &&
	       HasGuestCurrentImageOwnership(depth_gpu_modified, depth_buffer_modified, false,
	                                     depth_tracker_gpu_modified);
}

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

// Same idea as IsDepthUintTextureReinterpretation, but for a guest access that reads/writes the
// depth buffer's values directly as float (its natural representation) rather than reinterpreting
// the raw bits as uint32 -- e.g. a compute pass binding a depth-tiled 2D storage image with
// BufferFormat::k32Float over a D32_SFLOAT depth target.
[[nodiscard]] inline constexpr bool
IsDepthFloatTextureDirectView(vk::Format image_format, uint32_t guest_format,
                              vk::Format view_format) noexcept {
	switch (image_format) {
		case vk::Format::eD32Sfloat:
			return guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float) &&
			       view_format == vk::Format::eR32Sfloat;
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
		// No separate 1D dimension is tracked through the shader IR (a 1D image decodes the same
		// as 2D, see IsSupportedStorageTextureDescriptor in descriptors.cpp and FindStorageTexture
		// in textureCache.cpp), so a kColor1D storage texture gets the same 2D sampled view shape.
		case Prospero::ImageType::kColor1D:
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
	    info.bytes_per_element != 4 || info.levels != 1 || info.layers != 1 || info.samples != 1) {
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

[[nodiscard]] inline MetaImageOverlap ClassifyMetaImageOverlap(bool sampled, bool writable_image,
                                                               bool gpu_modified,
                                                               bool buffer_modified,
                                                               bool cpu_dirty) {
	if (sampled && !gpu_modified) {
		return MetaImageOverlap::RetainSampled;
	}
	if (writable_image && !gpu_modified && !buffer_modified && !cpu_dirty) {
		return MetaImageOverlap::RetireImage;
	}
	return MetaImageOverlap::Unsupported;
}

[[nodiscard]] inline float DecodeSrgbClearComponent(uint32_t value) {
	const auto encoded = static_cast<float>(value & 0xffu) / 255.0f;
	return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] inline bool DecodePackedColorClear(vk::Format format, uint32_t packed,
	                                             vk::ClearColorValue& clear) {
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

[[nodiscard]] inline bool CanNativeClearDepthFromBuffer(const DepthTargetInfo& target,
                                                        uint64_t address, uint64_t size) {
	const bool d32 =
	    target.format == vk::Format::eD32Sfloat || target.format == vk::Format::eD32SfloatS8Uint;
	return address == target.address && size == target.size && target.layers == 1 && d32 &&
	       target.samples == 1 &&
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
                                                           bool             cached_gpu_modified) {
	if (!ImagePageRangesOverlap(requested.address, requested.size, cached.address, cached.size)) {
		return SampledOverlap::None;
	}
	return !cached_gpu_modified ? SampledOverlap::ReadOnlyAlias : SampledOverlap::Unsupported;
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

[[nodiscard]] inline StorageSampledOverlap ClassifyStorageSampledOverlap(
    const ImageInfo& requested, const ImageInfo& cached, vk::Format requested_view_format,
    vk::Format cached_image_format, bool cached_gpu_modified, bool cached_cpu_dirty,
    bool exact_mip_subresource = false, bool cached_buffer_modified = false,
    bool tracker_gpu_modified = true) {
	if (!ImagePageRangesOverlap(requested.address, requested.size, cached.address, cached.size)) {
		return StorageSampledOverlap::None;
	}
	const bool same_backing =
	    requested.address == cached.address && requested.size == cached.size &&
	    requested.width == cached.width && requested.height == cached.height &&
	    requested.pitch == cached.pitch && requested.levels <= cached.levels &&
	    requested.base_level < cached.levels &&
	    requested.view_levels <= cached.levels - requested.base_level &&
	    requested.tile == cached.tile && requested.depth == cached.depth &&
	    requested.type == cached.type && requested.base_array == cached.base_array;
	const bool compatible_format =
	    (requested.format == cached.format && requested_view_format == cached_image_format) ||
	    IsRgba8SrgbReinterpretation(cached_image_format, requested_view_format) ||
	    IsR32UintFloatReinterpretation(cached_image_format, requested_view_format);
	if (same_backing && compatible_format && cached_gpu_modified && !cached_cpu_dirty) {
		return StorageSampledOverlap::ExactImage;
	}
	if (exact_mip_subresource && cached_gpu_modified && tracker_gpu_modified &&
	    !cached_buffer_modified && !cached_cpu_dirty) {
		return StorageSampledOverlap::RetireStorage;
	}
	return StorageSampledOverlap::Unsupported;
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
	const bool byte_overlap =
	    ImageRangeOverlaps(buffer_address, buffer_size, image_address, image_size);
	const bool buffer_page_aligned =
	    ((buffer_address | buffer_size) & (TRACKER_PAGE_SIZE - 1)) == 0;
	const bool image_page_aligned = ((image_address | image_size) & (TRACKER_PAGE_SIZE - 1)) == 0;
	switch (binding) {
		case BufferImageBinding::Texture:
			// Same reasoning as the RenderTarget case below: a GPU cache invalidate/flush range
			// operates at cache-line granularity over whatever span the guest names and does not
			// have to be fully contained within (or exactly match) a single sampled texture's
			// bounds. InvalidateTexture only flags the cached image's GPU-side copy stale (never
			// destructive), which is correct for a write touching any part of it.
			//
			// The image's own allocation need not be page aligned either. Invalidation is not a
			// page operation -- it marks this cached image's GPU copy stale so it is re-read from
			// guest memory -- so a texture that merely starts mid-page (Dead Cells has one at
			// ...300 inside a 32 MB allocation) is no less safe to invalidate than an aligned one.
			// Requiring alignment turned that into a hard abort. Over-invalidating costs a
			// re-upload; under-invalidating would show stale pixels, so err this way.
			return !image_gpu_modified ? BufferImageWrite::InvalidateTexture
			                           : BufferImageWrite::Unsupported;
		case BufferImageBinding::VideoOut:
			if (!exact || !buffer_page_aligned || !buffer_formatted) {
				return BufferImageWrite::Unsupported;
			}
			return image_gpu_modified ? BufferImageWrite::SynchronizeVideoOut
			                          : BufferImageWrite::InvalidateVideoOut;
		case BufferImageBinding::RenderTarget:
			// GPU cache invalidate/flush ranges operate at cache-line granularity over whatever
			// address span the guest asked for, which does not have to line up exactly with a
			// single render target's bounds (e.g. a flush that also covers an adjacent
			// allocation). Requiring an exact address+size match rejected any such partially
			// overlapping flush outright. Any page-aligned, formatted write that overlaps the
			// target's bytes at all is safe to treat the same as an exact one: the sync path only
			// downloads GPU content (never destructive), and the invalidate path just marks the
			// GPU-side copy stale, which is correct for a write touching any part of it.
			if (!buffer_page_aligned || !buffer_formatted) {
				return BufferImageWrite::Unsupported;
			}
			if (image_gpu_modified && !image_buffer_modified) {
				return BufferImageWrite::SynchronizeRenderTarget;
			}
			return !image_gpu_modified && image_buffer_modified
			           ? BufferImageWrite::InvalidateRenderTarget
			           : BufferImageWrite::Unsupported;
		case BufferImageBinding::StorageTexture:
			if (!buffer_page_aligned || !buffer_formatted) {
				return BufferImageWrite::Unsupported;
			}
			// Synchronization reconstructs the complete image backing and publishes only the
			// intersection with the impending buffer write. Therefore the formatted flush range
			// may cross either image edge, and the image allocation itself need not end on a
			// tracker-page boundary.
			if (byte_overlap && image_gpu_modified && !image_buffer_modified) {
				return BufferImageWrite::SynchronizeStorageTexture;
			}
			return byte_overlap && !image_gpu_modified && image_buffer_modified
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
[[nodiscard]] inline DepthOverlap
ClassifyStorageDepthOverlap(const ImageInfo& storage, bool storage_gpu_modified,
                            bool storage_buffer_modified, bool storage_cpu_dirty,
                            bool tracker_gpu_modified, const DepthTargetInfo& depth) {
	const bool overlaps_depth =
	    ImageRangeOverlaps(storage.address, storage.size, depth.address, depth.size);
	const bool overlaps_stencil =
	    depth.stencil_address != 0 && ImageRangeOverlaps(storage.address, storage.size,
	                                                     depth.stencil_address, depth.stencil_size);
	if (!overlaps_depth && !overlaps_stencil) {
		return DepthOverlap::None;
	}
	const bool guest_current = HasGuestCurrentImageOwnership(
	    storage_gpu_modified, storage_buffer_modified, storage_cpu_dirty, tracker_gpu_modified);
	const bool aspects_discarded =
	    (!overlaps_depth || !depth.depth_access) && (!overlaps_stencil || !depth.stencil_access);
	return guest_current || (!storage_buffer_modified && !storage_cpu_dirty && aspects_discarded)
	           ? DepthOverlap::RetireStorage
	           : DepthOverlap::Unsupported;
}

[[nodiscard]] inline bool
CanRetireGuestCurrentDepthForSampled(const ImageInfo& sampled, const DepthTargetInfo& depth,
                                     bool depth_gpu_modified, bool depth_buffer_modified,
                                     bool depth_tracker_gpu_modified,
                                     bool stencil_tracker_gpu_modified) noexcept {
	const bool overlaps_depth =
	    ImageRangeOverlaps(sampled.address, sampled.size, depth.address, depth.size);
	const bool overlaps_stencil =
	    depth.stencil_address != 0 && ImageRangeOverlaps(sampled.address, sampled.size,
	                                                     depth.stencil_address, depth.stencil_size);
	return (overlaps_depth || overlaps_stencil) &&
	       HasGuestCurrentImageOwnership(depth_gpu_modified, depth_buffer_modified, false,
	                                     depth_tracker_gpu_modified ||
	                                         stencil_tracker_gpu_modified);
}

[[nodiscard]] inline bool
CanRetireGuestCurrentSampledForDepth(const ImageInfo& sampled, const DepthTargetInfo& depth,
                                     bool sampled_gpu_modified, bool sampled_buffer_modified,
                                     bool sampled_cpu_dirty, bool tracker_gpu_modified,
                                     bool guest_source_current) noexcept {
	const bool overlaps_depth =
	    ImageRangeOverlaps(sampled.address, sampled.size, depth.address, depth.size);
	const bool overlaps_stencil =
	    depth.stencil_address != 0 && ImageRangeOverlaps(sampled.address, sampled.size,
	                                                     depth.stencil_address, depth.stencil_size);
	return (overlaps_depth || overlaps_stencil) && guest_source_current &&
	       HasGuestCurrentImageOwnership(sampled_gpu_modified, sampled_buffer_modified,
	                                     sampled_cpu_dirty, tracker_gpu_modified);
}

[[nodiscard]] inline RenderTargetOverlap
ClassifyRenderTargetOverlap(const ImageInfo& sampled, bool sampled_gpu_modified,
                            const RenderTargetInfo& target) {
	if (!ImagePageRangesOverlap(sampled.address, sampled.size, target.address, target.size)) {
		return RenderTargetOverlap::None;
	}
	if (!ImageRangeOverlaps(sampled.address, sampled.size, target.address, target.size)) {
		return RenderTargetOverlap::None;
	}
	return !sampled_gpu_modified ? RenderTargetOverlap::RetireSampled
	                             : RenderTargetOverlap::Unsupported;
}

[[nodiscard]] inline RenderTargetOverlap
ClassifyStorageRenderTargetOverlap(const ImageInfo& storage, vk::Format storage_format,
                                   bool storage_gpu_modified, bool storage_buffer_modified,
                                   bool storage_cpu_dirty, bool tracker_gpu_modified,
                                   const RenderTargetInfo& target) {
	if (!ImagePageRangesOverlap(storage.address, storage.size, target.address, target.size)) {
		return RenderTargetOverlap::None;
	}
	if (!ImageRangeOverlaps(storage.address, storage.size, target.address, target.size)) {
		return RenderTargetOverlap::None;
	}
	const bool exact_native_image =
	    storage.address == target.address && storage.size == target.size &&
	    storage_format == target.format && storage.width == target.width &&
	    storage.height == target.height && storage.pitch == target.pitch &&
	    storage.base_level == 0 && storage.levels == 1 && storage.view_levels == 1 &&
	    storage.tile == target.tile_mode && storage.depth == 1 &&
	    storage.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) &&
	    storage.base_array == 0 && target.levels == 1 && target.layers == 1 && target.samples == 1;
	if (exact_native_image && storage_gpu_modified && tracker_gpu_modified &&
	    !storage_buffer_modified && !storage_cpu_dirty) {
		return RenderTargetOverlap::PreserveStorage;
	}
	// Unlike PreserveStorage, retirement here always discards the storage image outright and
	// creates a fresh render target over the (possibly only partially overlapping) requested
	// range. A pending GPU write (storage_gpu_modified/tracker_gpu_modified) is not a reason to
	// refuse that -- it is a reason the caller must synchronize the storage image to guest memory
	// first so that content is not lost, which it does before adding this entry to the retire set.
	// storage_buffer_modified is left permissive too since a sync leaves it set. Only genuinely
	// unresolved CPU-side writes (storage_cpu_dirty) are refused here.
	return !storage_cpu_dirty ? RenderTargetOverlap::RetireStorage
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
	       cached.layers == requested.layers && cached.samples == requested.samples &&
	       IsRgba8SrgbReinterpretation(cached.format, requested.format);
}

[[nodiscard]] inline RenderTargetOverlap
ClassifySampledRenderTargetOverlap(const ImageInfo& sampled, const RenderTargetInfo& target,
                                   bool target_buffer_modified) {
	if (!ImagePageRangesOverlap(sampled.address, sampled.size, target.address, target.size)) {
		return RenderTargetOverlap::None;
	}
	if (!ImageRangeOverlaps(sampled.address, sampled.size, target.address, target.size)) {
		return RenderTargetOverlap::None;
	}
	return !target_buffer_modified ? RenderTargetOverlap::RetireTarget
	                               : RenderTargetOverlap::Unsupported;
}

[[nodiscard]] inline StorageImageOverlap
ClassifyStorageImageOverlap(uint64_t requested_address, uint64_t requested_size,
                            uint64_t cached_address, uint64_t cached_size, bool sampled,
                            bool storage, bool render_target, bool gpu_modified,
                            bool buffer_modified, bool tracker_gpu_modified) {
	if (!ImagePageRangesOverlap(requested_address, requested_size, cached_address, cached_size)) {
		return StorageImageOverlap::None;
	}
	if (!ImageRangeOverlaps(requested_address, requested_size, cached_address, cached_size)) {
		return StorageImageOverlap::PageNeighbor;
	}
	// A render target overlapping a fresh storage-image request (e.g. a post-process compute pass
	// writing back into a just-rendered color buffer) is always discarded outright and replaced,
	// same as the render-target retirement paths above -- so a pending GPU write is not a reason
	// to refuse it, only a reason the caller must download it to guest memory first (which
	// ResolveStorageImageOverlaps's retire loop already does for any gpu_modified entry).
	if (render_target) {
		return StorageImageOverlap::RetireSampled;
	}
	// A differently shaped storage view may partially overlap the previous one. When ownership is
	// coherent, retire the old view after downloading its complete contents; the replacement then
	// uploads from guest backing and preserves the intersection.
	if (storage) {
		return gpu_modified == tracker_gpu_modified && !buffer_modified
		           ? StorageImageOverlap::RetireStorage
		           : StorageImageOverlap::Unsupported;
	}
	return sampled && !gpu_modified && !buffer_modified && !tracker_gpu_modified
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
	       cached.samples == requested.samples &&
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
	       cached.tile_mode == requested.tile_mode && cached.samples == requested.samples &&
	       cached.stencil_htile_compressed == requested.stencil_htile_compressed;
}

[[nodiscard]] inline RenderTargetOverlap
ClassifyRenderTargetOverlap(const RenderTargetInfo& cached, bool cached_gpu_modified,
                            bool cached_buffer_modified, bool tracker_gpu_modified,
                            bool guest_source_current, const RenderTargetInfo& requested) {
	if (!ImagePageRangesOverlap(cached.address, cached.size, requested.address, requested.size)) {
		return RenderTargetOverlap::None;
	}
	if (!ImageRangeOverlaps(cached.address, cached.size, requested.address, requested.size)) {
		return RenderTargetOverlap::None;
	}
	const bool expand = requested.layers > cached.layers &&
	                    IsCompatibleRenderTargetBacking(requested, cached) &&
	                    cached.format == requested.format;
	if (expand && cached_gpu_modified && tracker_gpu_modified && !cached_buffer_modified) {
		return RenderTargetOverlap::ExpandTarget;
	}
	// A clean, page-isolated overlap is allocation-pool reuse, including a contained subrange.
	// Compatible views are handled before this classifier and are never retired here.
	const bool page_isolated =
	    cached.address % TRACKER_PAGE_SIZE == 0 && cached.size % TRACKER_PAGE_SIZE == 0 &&
	    requested.address % TRACKER_PAGE_SIZE == 0 && requested.size % TRACKER_PAGE_SIZE == 0;
	const bool incompatible_format = cached.format != requested.format &&
	                                 !IsRgba8SrgbReinterpretation(cached.format, requested.format);
	const bool pool_storage_shape_changed =
	    incompatible_format || cached.width != requested.width ||
	    cached.height != requested.height || cached.pitch != requested.pitch ||
	    cached.bytes_per_element != requested.bytes_per_element ||
	    cached.tile_mode != requested.tile_mode || cached.levels != requested.levels ||
	    cached.layers != requested.layers || cached.samples != requested.samples;
	const bool new_allocation = cached.address != requested.address ||
	                            cached.size != requested.size || pool_storage_shape_changed;
	// This path discards the cached image outright and creates a fresh one over the (possibly
	// only partially overlapping) requested range, so cached_buffer_modified -- "guest memory has
	// bytes newer than this GPU image" -- does not matter: nobody is going to read the retired
	// image again. A pending GPU-side write (cached_gpu_modified/tracker_gpu_modified) is also not
	// refused here: the caller downloads a gpu_modified render target to guest memory before
	// retiring it (same as the storage-image and storage-render-target retirement paths), so no
	// content is lost either way.
	return page_isolated && new_allocation && guest_source_current
	           ? RenderTargetOverlap::RetireTarget
	           : RenderTargetOverlap::Unsupported;
}

[[nodiscard]] inline DepthOverlap ClassifyDepthTargetOverlap(const DepthTargetInfo& cached,
                                                             bool cached_gpu_modified,
                                                             bool cached_buffer_modified,
                                                             const DepthTargetInfo& requested) {
	const auto overlaps = [](uint64_t left, uint64_t left_size, uint64_t right, uint64_t right_size,
	                         bool pages) {
		if (left == 0 || left_size == 0 || right == 0 || right_size == 0) {
			return false;
		}
		return pages ? ImagePageRangesOverlap(left, left_size, right, right_size)
		             : ImageRangeOverlaps(left, left_size, right, right_size);
	};
	const auto planes_overlap = [&](bool pages) {
		return overlaps(cached.address, cached.size, requested.address, requested.size, pages) ||
		       overlaps(cached.address, cached.size, requested.stencil_address,
		                requested.stencil_size, pages) ||
		       overlaps(cached.stencil_address, cached.stencil_size, requested.address,
		                requested.size, pages) ||
		       overlaps(cached.stencil_address, cached.stencil_size, requested.stencil_address,
		                requested.stencil_size, pages);
	};
	if (!planes_overlap(true) || !planes_overlap(false)) {
		return DepthOverlap::None;
	}
	const bool expand =
	    requested.layers > cached.layers && IsCompatibleDepthTargetBacking(requested, cached);
	if (expand && cached_gpu_modified && !cached_buffer_modified) {
		return DepthOverlap::ExpandTarget;
	}
	const bool exact_discard =
	    requested.depth_load_clear && cached_gpu_modified && !cached_buffer_modified &&
	    cached.address == requested.address && cached.size == requested.size &&
	    cached.samples == requested.samples && cached.stencil_address == 0 &&
	    cached.stencil_size == 0 && cached.htile_address == 0 && cached.htile_size == 0 &&
	    requested.stencil_address == 0 && requested.stencil_size == 0 &&
	    requested.htile_address == 0 && requested.htile_size == 0;
	if (exact_discard) {
		return DepthOverlap::DiscardTarget;
	}
	const bool exact_plane_rebind =
	    (cached.address == requested.address && cached.size == requested.size) ||
	    (cached.address == requested.stencil_address && cached.size == requested.stencil_size) ||
	    (cached.stencil_address == requested.address && cached.stencil_size == requested.size) ||
	    (cached.stencil_address == requested.stencil_address &&
	     cached.stencil_size == requested.stencil_size);
	const bool same_shape = cached.format == requested.format &&
	                        cached.guest_format == requested.guest_format &&
	                        cached.width == requested.width && cached.height == requested.height &&
	                        cached.pitch == requested.pitch &&
	                        cached.bytes_per_element == requested.bytes_per_element &&
	                        cached.tile_mode == requested.tile_mode && cached.layers == 1 &&
	                        requested.layers == 1 && cached.samples == 1 && requested.samples == 1;
	return exact_plane_rebind && same_shape && !cached_buffer_modified
	           ? DepthOverlap::RecreateTarget
	           : DepthOverlap::Unsupported;
}

[[nodiscard]] inline bool CanRecreateDepthForRenderTarget(const DepthTargetInfo& depth,
                                                          bool gpu_modified, bool buffer_modified,
                                                          bool tracker_gpu_modified,
                                                          bool guest_source_current,
                                                          const RenderTargetInfo& target) noexcept {
	const bool exact_depth = depth.address == target.address && depth.size == target.size;
	const bool exact_stencil =
	    depth.stencil_address == target.address && depth.stencil_size == target.size;
	const auto contains = [&](uint64_t address, uint64_t size) {
		if (address == 0 || size == 0 || address < target.address) {
			return false;
		}
		const auto offset = address - target.address;
		return offset <= target.size && size <= target.size - offset;
	};
	const bool page_isolated_rebind =
	    target.address % TRACKER_PAGE_SIZE == 0 && target.size % TRACKER_PAGE_SIZE == 0 &&
	    ((contains(depth.address, depth.size) && depth.address % TRACKER_PAGE_SIZE == 0 &&
	      depth.size % TRACKER_PAGE_SIZE == 0) ||
	     (contains(depth.stencil_address, depth.stencil_size) &&
	      depth.stencil_address % TRACKER_PAGE_SIZE == 0 &&
	      depth.stencil_size % TRACKER_PAGE_SIZE == 0));
	const bool ownership_consistent = gpu_modified == tracker_gpu_modified && !buffer_modified;
	const bool source_available =
	    ((exact_depth || exact_stencil) && gpu_modified) || guest_source_current;
	return ownership_consistent && source_available &&
	       (exact_depth || exact_stencil || page_isolated_rebind) && depth.layers == 1 &&
	       target.levels == 1 && target.layers == 1 && depth.samples == 1 && target.samples == 1;
}

[[nodiscard]] inline bool CanRecreateRenderTargetForDepth(const RenderTargetInfo& target,
                                                          bool gpu_modified, bool buffer_modified,
                                                          bool tracker_gpu_modified,
                                                          bool guest_source_current,
                                                          const DepthTargetInfo& depth) noexcept {
	const bool exact_depth = target.address == depth.address && target.size == depth.size;
	const bool exact_stencil =
	    target.address == depth.stencil_address && target.size == depth.stencil_size;
	const auto overlaps_plane = [&](uint64_t address, uint64_t size) {
		return address != 0 && size != 0 &&
		       ImageRangeOverlaps(target.address, target.size, address, size);
	};
	const bool page_isolated_rebind =
	    target.address % TRACKER_PAGE_SIZE == 0 && target.size % TRACKER_PAGE_SIZE == 0 &&
	    ((overlaps_plane(depth.address, depth.size) && depth.address % TRACKER_PAGE_SIZE == 0 &&
	      depth.size % TRACKER_PAGE_SIZE == 0) ||
	     (overlaps_plane(depth.stencil_address, depth.stencil_size) &&
	      depth.stencil_address % TRACKER_PAGE_SIZE == 0 &&
	      depth.stencil_size % TRACKER_PAGE_SIZE == 0));
	const bool source_available =
	    (gpu_modified && (exact_depth || exact_stencil)) || guest_source_current;
	return gpu_modified == tracker_gpu_modified && !buffer_modified && source_available &&
	       (exact_depth || exact_stencil || page_isolated_rebind) && target.levels == 1 &&
	       target.layers == 1 && depth.layers == 1 && target.samples == 1 && depth.samples == 1;
}

[[nodiscard]] inline bool RequiresMultisampleDepthRefresh(const DepthTargetInfo& info,
                                                          bool                   buffer_modified,
                                                          bool                   depth_cpu_modified,
                                                          bool stencil_cpu_modified) noexcept {
	if (info.samples == 1) {
		return false;
	}
	const bool depth_refresh =
	    info.depth_access && !info.depth_load_clear && (buffer_modified || depth_cpu_modified);
	const bool stencil_refresh = info.stencil_access && !info.stencil_load_clear &&
	                             (buffer_modified || stencil_cpu_modified);
	return depth_refresh || stencil_refresh;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEINFO_H_
