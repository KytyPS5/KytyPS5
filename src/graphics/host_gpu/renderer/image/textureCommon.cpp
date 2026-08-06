#include "graphics/host_gpu/renderer/image/textureCommon.h"

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/host_gpu/renderer/image/tiler.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cstring>

namespace Libs::Graphics {
namespace {

struct RenderTargetFormatMapping {
	Prospero::ChannelLayout layout;
	Prospero::ChannelType   type;
	Prospero::ChannelOrder  order;
	RenderTargetFormatInfo  info;
};

constexpr RenderTargetFormatMapping kRenderTargetFormats[] = {
    {Prospero::ChannelLayout::k8,
     Prospero::ChannelType::kUInt,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR8Uint, 1}},
    {Prospero::ChannelLayout::k8_8,
     Prospero::ChannelType::kUNorm,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR8G8Unorm, 2}},
    {Prospero::ChannelLayout::k8_8_8_8,
     Prospero::ChannelType::kUNorm,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR8G8B8A8Unorm, 4}},
    {Prospero::ChannelLayout::k8_8_8_8,
     Prospero::ChannelType::kSNorm,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR8G8B8A8Snorm, 4}},
    {Prospero::ChannelLayout::k8_8_8_8,
     Prospero::ChannelType::kSrgb,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR8G8B8A8Srgb, 4}},
    {Prospero::ChannelLayout::k8_8_8_8,
     Prospero::ChannelType::kUNorm,
     Prospero::ChannelOrder::kAlt,
     {vk::Format::eB8G8R8A8Unorm, 4}},
    {Prospero::ChannelLayout::k8_8_8_8,
     Prospero::ChannelType::kSNorm,
     Prospero::ChannelOrder::kAlt,
     {vk::Format::eB8G8R8A8Snorm, 4}},
    {Prospero::ChannelLayout::k8_8_8_8,
     Prospero::ChannelType::kSrgb,
     Prospero::ChannelOrder::kAlt,
     {vk::Format::eB8G8R8A8Srgb, 4}},
    {Prospero::ChannelLayout::k5_5_5_1,
     Prospero::ChannelType::kUNorm,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR5G5B5A1UnormPack16, 2}},
    {Prospero::ChannelLayout::k4_4_4_4,
     Prospero::ChannelType::kUNorm,
     Prospero::ChannelOrder::kReversed,
     {vk::Format::eB4G4R4A4UnormPack16, 2}},
    {Prospero::ChannelLayout::k10_10_10_2,
     Prospero::ChannelType::kUNorm,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eA2B10G10R10UnormPack32, 4}},
    {Prospero::ChannelLayout::k10_10_10_2,
     Prospero::ChannelType::kUNorm,
     Prospero::ChannelOrder::kAlt,
     {vk::Format::eA2R10G10B10UnormPack32, 4}},
    {Prospero::ChannelLayout::k11_11_10,
     Prospero::ChannelType::kFloat,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eB10G11R11UfloatPack32, 4}},
    {Prospero::ChannelLayout::k5_6_5,
     Prospero::ChannelType::kUNorm,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eB5G6R5UnormPack16, 2}},
    {Prospero::ChannelLayout::k16,
     Prospero::ChannelType::kUNorm,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR16Unorm, 2}},
    {Prospero::ChannelLayout::k16,
     Prospero::ChannelType::kUInt,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR16Uint, 2}},
    {Prospero::ChannelLayout::k16,
     Prospero::ChannelType::kFloat,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR16Sfloat, 2}},
    {Prospero::ChannelLayout::k16_16,
     Prospero::ChannelType::kUNorm,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR16G16Unorm, 4}},
    {Prospero::ChannelLayout::k16_16,
     Prospero::ChannelType::kSNorm,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR16G16Snorm, 4}},
    {Prospero::ChannelLayout::k16_16,
     Prospero::ChannelType::kUInt,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR16G16Uint, 4}},
    {Prospero::ChannelLayout::k16_16,
     Prospero::ChannelType::kFloat,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR16G16Sfloat, 4}},
    {Prospero::ChannelLayout::k16_16_16_16,
     Prospero::ChannelType::kUNorm,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR16G16B16A16Unorm, 8}},
    {Prospero::ChannelLayout::k16_16_16_16,
     Prospero::ChannelType::kUInt,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR16G16B16A16Uint, 8}},
    {Prospero::ChannelLayout::k16_16_16_16,
     Prospero::ChannelType::kFloat,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR16G16B16A16Sfloat, 8}},
    {Prospero::ChannelLayout::k16_16_16_16,
     Prospero::ChannelType::kFloat,
     Prospero::ChannelOrder::kAlt,
     {vk::Format::eR16G16B16A16Sfloat, 8, Prospero::ColorMappingBgra}},
    {Prospero::ChannelLayout::k16_16_16_16,
     Prospero::ChannelType::kFloat,
     Prospero::ChannelOrder::kReversed,
     {vk::Format::eR16G16B16A16Sfloat, 8, Prospero::ColorMappingAbgr}},
    {Prospero::ChannelLayout::k32,
     Prospero::ChannelType::kFloat,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR32Sfloat, 4}},
    {Prospero::ChannelLayout::k32_32,
     Prospero::ChannelType::kUInt,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR32G32Uint, 8}},
    {Prospero::ChannelLayout::k32_32,
     Prospero::ChannelType::kFloat,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR32G32Sfloat, 8}},
    {Prospero::ChannelLayout::k32_32_32_32,
     Prospero::ChannelType::kFloat,
     Prospero::ChannelOrder::kStandard,
     {vk::Format::eR32G32B32A32Sfloat, 16}},
};

} // namespace

// TODO: cleanup!
RenderTargetFormatInfo TextureGetRenderTargetFormat(uint32_t raw_layout, uint32_t raw_type,
                                                    uint32_t raw_order) {
	const auto layout = static_cast<Prospero::ChannelLayout>(raw_layout);
	const auto type   = static_cast<Prospero::ChannelType>(raw_type);
	const auto order  = static_cast<Prospero::ChannelOrder>(raw_order);

	if (layout == Prospero::ChannelLayout::k8 && type == Prospero::ChannelType::kUNorm &&
	    raw_order <= Prospero::GpuEnumValue(Prospero::ChannelOrder::kAltReversed)) {
		return {vk::Format::eR8Unorm, 1};
	}
	for (const auto& mapping: kRenderTargetFormats) {
		if (mapping.layout == layout && mapping.type == type && mapping.order == order) {
			return mapping.info;
		}
	}
	EXIT("unsupported render-target format combination: layout=%u type=%u order=%u\n", raw_layout,
	     raw_type, raw_order);
}

namespace {

static uint32_t GetTextureLevelDepth(uint32_t depth, uint32_t level, bool volume_texture) {
	return volume_texture ? std::max(depth >> level, 1u) : depth;
}

static size_t GetTextureRegionCount(uint32_t depth, uint64_t levels, bool volume_texture) {
	size_t count = 0;
	for (uint32_t level = 0; level < levels; level++) {
		count += GetTextureLevelDepth(depth, level, volume_texture);
	}
	return count;
}

vk::ComponentSwizzle TextureGetComponentSwizzle(uint8_t s) {
	switch (static_cast<Prospero::CompSwizzle>(s)) {
		case Prospero::CompSwizzle::kZero: return vk::ComponentSwizzle::eZero;
		case Prospero::CompSwizzle::kOne: return vk::ComponentSwizzle::eOne;
		case Prospero::CompSwizzle::kRed: return vk::ComponentSwizzle::eR;
		case Prospero::CompSwizzle::kGreen: return vk::ComponentSwizzle::eG;
		case Prospero::CompSwizzle::kBlue: return vk::ComponentSwizzle::eB;
		case Prospero::CompSwizzle::kAlpha: return vk::ComponentSwizzle::eA;
		default: EXIT("unknown swizzle: %d\n", static_cast<int>(s));
	}
	return vk::ComponentSwizzle::eIdentity;
}

static uint32_t TextureGetDstSel(uint32_t swizzle, uint32_t channel) {
	return (swizzle >> (channel * 3u)) & 0x7u;
}

} // namespace

vk::ComponentMapping TextureGetComponentMapping(uint32_t swizzle) {
	vk::ComponentMapping components {};
	components.r = TextureGetComponentSwizzle(static_cast<uint8_t>(TextureGetDstSel(swizzle, 0)));
	components.g = TextureGetComponentSwizzle(static_cast<uint8_t>(TextureGetDstSel(swizzle, 1)));
	components.b = TextureGetComponentSwizzle(static_cast<uint8_t>(TextureGetDstSel(swizzle, 2)));
	components.a = TextureGetComponentSwizzle(static_cast<uint8_t>(TextureGetDstSel(swizzle, 3)));
	return components;
}

vk::Format TextureGetFormat(uint32_t fmt) {
	const auto vk_format = VulkanFormat(fmt);
	if (vk_format != vk::Format::eUndefined) {
		return vk_format;
	}
	EXIT("unknown format: fmt = %u\n", fmt);
	return vk::Format::eUndefined;
}

namespace {

uint64_t CalcTextureSliceStride(const TextureUploadMipLayout* mips, uint64_t levels,
                                uint64_t total_size, uint32_t depth) {
	uint64_t stride = 0;
	for (uint32_t i = 0; i < levels; i++) {
		stride = std::max(stride, mips[i].offset + mips[i].size);
	}

	if (depth > 1 && total_size != 0 && total_size % depth == 0) {
		const auto guest_stride = total_size / depth;
		if (guest_stride >= stride) {
			stride = guest_stride;
		}
	}

	return stride;
}

uint64_t CalcLinearUploadLevelSize(const TileTextureElementLayout& element, uint32_t pitch,
                                   uint32_t height) {
	const uint32_t elements_w =
	    std::max((pitch + element.texel_width - 1u) / element.texel_width, 1u);
	const uint32_t elements_h =
	    std::max((height + element.texel_height - 1u) / element.texel_height, 1u);
	return static_cast<uint64_t>(elements_w) * elements_h * element.bytes;
}

uint64_t SetLinearUploadLevels(TextureUploadMipLayout*         mips,
                               const TileTextureElementLayout& element, uint64_t height,
                               uint64_t levels, uint32_t base_pitch) {
	uint64_t offset = 0;
	auto     pitch  = base_pitch;
	auto     h      = static_cast<uint32_t>(height);

	for (uint32_t i = 0; i < levels; i++) {
		const auto size = CalcLinearUploadLevelSize(element, pitch, h);
		mips[i].size    = size;
		mips[i].offset  = offset;

		offset += size;
		if (pitch > 1) {
			pitch /= 2;
		}
		if (h > 1) {
			h /= 2;
		}
	}

	return offset;
}

} // namespace

TextureUploadLayout TextureCalcUploadLayout(uint32_t fmt, uint32_t width, uint32_t height,
                                            uint32_t levels, uint32_t depth, uint32_t tile,
                                            uint64_t upload_size, bool allow_depth_tile,
                                            bool volume_texture, const char* owner) {
	TextureUploadLayout layout {};
	layout.surface.description = {
	    fmt,
	    tile,
	    volume_texture ? TileSurfaceDimension::Dim3D : TileSurfaceDimension::Dim2D,
	    width,
	    height,
	    volume_texture ? depth : 1u,
	    levels,
	    volume_texture ? 1u : depth,
	};
	const auto&              description = layout.surface.description;
	const auto               tile_mode   = static_cast<Prospero::TileMode>(description.tile_mode);
	TileTextureElementLayout element {};

	if (fmt == 0) {
		EXIT("%s: legacy texture upload format unsupported: fmt=0 tile=%u size=%" PRIu64
		     " extent=%ux%u levels=%u\n",
		     owner, description.tile_mode, upload_size, width, height, levels);
	}

	switch (tile_mode) {
		case Prospero::TileMode::kLinear:
			if (!TileGetTextureElementLayout(fmt, element)) {
				EXIT("%s: unsupported linear texture format: fmt=%u\n", owner, fmt);
			}
			break;
		case Prospero::TileMode::kDepth:
			if (!allow_depth_tile || !TileGetTiledTextureLayout(description, layout.surface)) {
				EXIT("%s: unsupported typed tiled upload: fmt=%u tile=%u "
				     "size=%" PRIu64 " extent=%ux%u levels=%u\n",
				     owner, static_cast<uint32_t>(fmt), description.tile_mode, upload_size, width,
				     height, levels);
			}
			break;
		default:
			if (!TileGetTiledTextureLayout(description, layout.surface)) {
				EXIT("%s: unsupported typed tiled upload: fmt=%u tile=%u "
				     "size=%" PRIu64 " extent=%ux%u levels=%u\n",
				     owner, static_cast<uint32_t>(fmt), description.tile_mode, upload_size, width,
				     height, levels);
			}
			break;
	}

	if (tile_mode != Prospero::TileMode::kLinear) {
		element = {layout.surface.texture.block.bytes_per_element,
		           layout.surface.texture.texel_width, layout.surface.texture.texel_height};
	}
	layout.pitch = TileGetTexturePitch(fmt, width, description.tile_mode);
	if (tile_mode == Prospero::TileMode::kLinear) {
		TileSizeOffset level_sizes[16] {};
		TilePaddedSize padded_sizes[16] {};
		TileGetTextureSize(fmt, width, height, levels, description.tile_mode, nullptr, level_sizes,
		                   padded_sizes);
		for (uint32_t level = 0; level < levels; ++level) {
			layout.mips[level] = {level_sizes[level].offset, level_sizes[level].size,
			                      padded_sizes[level].width, padded_sizes[level].height};
		}
	} else if (volume_texture) {
		layout.slice_stride = SetLinearUploadLevels(layout.mips, element, height, levels, width);
	} else {
		layout.source_slice_stride = layout.surface.block_slice_size;
		if (depth > 1 && upload_size != 0 && upload_size % depth == 0) {
			layout.source_slice_stride = std::max(layout.source_slice_stride, upload_size / depth);
		}
		SetLinearUploadLevels(layout.mips, element, height, levels, layout.pitch);
	}

	if (!volume_texture || tile_mode == Prospero::TileMode::kLinear) {
		layout.slice_stride = CalcTextureSliceStride(layout.mips, levels, upload_size, depth);
	}
	return layout;
}

std::vector<vk::BufferImageCopy> TextureBuildImageCopies(const TextureUploadLayout& layout) {
	const auto& description    = layout.surface.description;
	const bool  volume_texture = description.dimension == TileSurfaceDimension::Dim3D;
	const auto  depth          = volume_texture ? description.depth : description.layers;
	uint32_t    mip_width      = description.width;
	uint32_t    mip_height     = description.height;
	uint32_t mip_pitch = volume_texture && static_cast<Prospero::TileMode>(description.tile_mode) !=
	                                           Prospero::TileMode::kLinear
	                         ? description.width
	                         : layout.pitch;

	std::vector<vk::BufferImageCopy> regions;
	regions.reserve(GetTextureRegionCount(depth, description.levels, volume_texture));
	for (uint32_t i = 0; i < description.levels; i++) {
		EXIT_NOT_IMPLEMENTED(layout.mips[i].size == 0);

		const auto mip_depth = GetTextureLevelDepth(depth, i, volume_texture);

		for (uint32_t z = 0; z < mip_depth; z++) {
			const auto          slice_offset = z * layout.slice_stride;
			vk::BufferImageCopy region {};
			region.bufferOffset     = layout.mips[i].offset + slice_offset;
			region.imageSubresource = {vk::ImageAspectFlagBits::eColor, i, volume_texture ? 0u : z,
			                           1};
			region.imageOffset.z    = volume_texture ? static_cast<int>(z) : 0;
			region.imageExtent      = {mip_width, mip_height, 1};
			const bool linear       = static_cast<Prospero::TileMode>(description.tile_mode) ==
			                          Prospero::TileMode::kLinear;
			if (linear) {
				region.bufferRowLength   = layout.mips[i].row_length;
				region.bufferImageHeight = layout.mips[i].image_height;
			} else {
				const auto texel_width = layout.surface.texture.texel_width;
				const auto align       = [](uint32_t value, uint32_t block) {
					return ((value + block - 1u) / block) * block;
				};
				const auto pitch       = align(mip_pitch, texel_width);
				region.bufferRowLength = pitch > align(mip_width, texel_width) ? pitch : 0;
			}
			regions.push_back(region);
		}

		if (mip_width > 1) {
			mip_width /= 2;
		}
		if (mip_height > 1) {
			mip_height /= 2;
		}
		if (mip_pitch > 1) {
			mip_pitch /= 2;
		}
	}

	return regions;
}

static bool SetGpuTileSize(uint64_t offset, uint64_t length, uint64_t capacity, uint64_t& size) {
	if (offset > capacity || length > capacity - offset) {
		return false;
	}
	size = length;
	return true;
}

bool TextureBuildGpuTileInfos(uint64_t tiled_size, const std::vector<vk::BufferImageCopy>& regions,
                              const TextureUploadLayout& layout, uint32_t levels,
                              std::vector<GpuTileInfo>& out_infos) {
	const auto& description    = layout.surface.description;
	const bool  volume_texture = description.dimension == TileSurfaceDimension::Dim3D;
	const auto  depth          = volume_texture ? description.depth : description.layers;
	if (tiled_size == 0 || levels == 0 || levels > 16 || depth == 0 ||
	    regions.size() != GetTextureRegionCount(depth, levels, volume_texture) ||
	    Prospero::IsFmaskTextureFormat(description.format)) {
		return false;
	}

	const auto& surface = layout.surface;
	const auto& texture = surface.texture;
	const auto& block   = texture.block;
	if (surface.description.levels < levels || block.bytes_per_element == 0 ||
	    block.family == TileBlockFamily::Count) {
		return false;
	}

	std::vector<GpuTileInfo> infos;
	infos.reserve(regions.size());
	if (volume_texture) {
		size_t region_base = 0;
		for (uint32_t level = 0; level < levels; ++level) {
			const uint32_t mip_depth     = GetTextureLevelDepth(depth, level, true);
			const bool     tail          = level >= surface.first_tail_level;
			const uint64_t linear_stride = layout.slice_stride;
			for (uint32_t z = 0; z < mip_depth; z += block.block_depth) {
				const uint32_t copy_depth = std::min(block.block_depth, mip_depth - z);
				const auto&    region     = regions[region_base + z];
				const auto     pitch =
				    region.bufferRowLength != 0 ? region.bufferRowLength : region.imageExtent.width;
				const auto  logical_height = region.bufferImageHeight != 0
				                                 ? region.bufferImageHeight
				                                 : region.imageExtent.height;
				GpuTileInfo info {};
				info.family            = block.family;
				info.bytes_per_element = block.bytes_per_element;
				info.linear_offset     = region.bufferOffset;
				info.tiled_offset =
				    static_cast<uint64_t>(z / block.block_depth) * surface.block_slice_size +
				    surface.mips[level].offset;
				const uint64_t linear_span =
				    static_cast<uint64_t>(copy_depth - 1u) * linear_stride +
				    layout.mips[level].size;
				if (!SetGpuTileSize(info.linear_offset, linear_span, UINT64_MAX,
				                    info.linear_size) ||
				    !SetGpuTileSize(info.tiled_offset, surface.mips[level].size, tiled_size,
				                    info.tiled_size)) {
					return false;
				}
				info.linear_slice_stride = linear_stride;
				info.width  = std::max((region.imageExtent.width + texture.texel_width - 1u) /
				                           texture.texel_width,
				                       1u);
				info.height = std::max(
				    (logical_height + texture.texel_height - 1u) / texture.texel_height, 1u);
				info.depth = copy_depth;
				info.surface_z =
				    block.block_depth == 1 ? static_cast<uint32_t>(region.imageOffset.z) : 0;
				info.pitch = std::max((pitch + texture.texel_width - 1u) / texture.texel_width, 1u);
				info.tail_x       = tail ? surface.mips[level].tail_x : 0;
				info.tail_y       = tail ? surface.mips[level].tail_y : 0;
				info.tail         = tail;
				info.tiled_width  = surface.mips[level].padded_width;
				info.tiled_height = surface.mips[level].padded_height;
				infos.push_back(info);
			}
			region_base += mip_depth;
		}
	} else {
		size_t region_index = 0;
		for (uint32_t level = 0; level < levels; level++) {
			const auto& level_size  = layout.mips[level];
			const auto& mip         = surface.mips[level];
			const bool  tail        = level >= surface.first_tail_level;
			const auto  level_depth = GetTextureLevelDepth(depth, level, false);
			for (uint32_t z = 0; z < level_depth; z++) {
				const auto& region = regions[region_index++];
				const auto  pitch =
				    region.bufferRowLength != 0 ? region.bufferRowLength : region.imageExtent.width;
				const auto  logical_height = region.bufferImageHeight != 0
				                                 ? region.bufferImageHeight
				                                 : region.imageExtent.height;
				GpuTileInfo info {};
				info.family              = block.family;
				info.bytes_per_element   = block.bytes_per_element;
				info.linear_offset       = region.bufferOffset;
				const auto source_stride = layout.source_slice_stride != 0
				                               ? layout.source_slice_stride
				                               : surface.block_slice_size;
				if (z > (UINT64_MAX - mip.offset) / source_stride) {
					return false;
				}
				info.tiled_offset = mip.offset + static_cast<uint64_t>(z) * source_stride;
				if (!SetGpuTileSize(info.linear_offset, level_size.size, UINT64_MAX,
				                    info.linear_size) ||
				    !SetGpuTileSize(info.tiled_offset, mip.size, tiled_size, info.tiled_size)) {
					return false;
				}
				info.width  = std::max((region.imageExtent.width + texture.texel_width - 1u) /
				                           texture.texel_width,
				                       1u);
				info.height = std::max(
				    (logical_height + texture.texel_height - 1u) / texture.texel_height, 1u);
				info.surface_z = block.family == TileBlockFamily::RenderTarget64KB ||
				                         block.family == TileBlockFamily::Depth64KB
				                     ? region.imageSubresource.baseArrayLayer
				                     : 0;
				info.pitch = std::max((pitch + texture.texel_width - 1u) / texture.texel_width, 1u);
				info.tail  = tail;
				info.tail_x       = tail ? mip.tail_x : 0;
				info.tail_y       = tail ? mip.tail_y : 0;
				info.tiled_width  = mip.padded_width;
				info.tiled_height = mip.padded_height;
				infos.push_back(info);
			}
		}
	}

	if (infos.empty()) {
		return false;
	}
	out_infos = std::move(infos);
	return true;
}

} // namespace Libs::Graphics
