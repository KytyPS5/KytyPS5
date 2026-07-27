#include "graphics/host_gpu/objects/textureCommon.h"

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/host_gpu/renderer/tiler.h"
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

static uint64_t GetLevelSrcOffset(const TileSizeOffset& level_size) {
	return (level_size.src_size != 0 ? level_size.src_offset : level_size.offset);
}

static uint64_t GetLevelSrcSize(const TileSizeOffset& level_size) {
	return (level_size.src_size != 0 ? level_size.src_size : level_size.size);
}

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

uint64_t TextureUploadSliceSourceOffset(const TextureUploadLayout& layout, uint32_t level,
                                        uint32_t slice) {
	if (level >= 16 || layout.level_sizes[level].size == 0) {
		EXIT("invalid texture upload slice source, level=%u slice=%u\n", level, slice);
	}
	const auto level_offset = GetLevelSrcOffset(layout.level_sizes[level]);
	const auto slice_stride =
	    layout.source_slice_stride != 0 ? layout.source_slice_stride : layout.slice_stride;
	if (slice_stride != 0 && slice > (UINT64_MAX - level_offset) / slice_stride) {
		EXIT("texture upload slice source offset overflow, level=%u slice=%u\n", level, slice);
	}
	return level_offset + static_cast<uint64_t>(slice) * slice_stride;
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

uint64_t CalcTextureSliceStride(const TileSizeOffset* level_sizes, uint64_t levels,
                                uint64_t total_size, uint32_t depth) {
	uint64_t stride = 0;
	for (uint32_t i = 0; i < levels; i++) {
		stride =
		    std::max(stride, static_cast<uint64_t>(level_sizes[i].offset) + level_sizes[i].size);
	}

	if (depth > 1 && total_size != 0 && total_size % depth == 0) {
		const auto guest_stride = total_size / depth;
		if (guest_stride >= stride) {
			stride = guest_stride;
		}
	}

	return stride;
}

uint64_t CalcLinearUploadLevelSize(uint32_t fmt, uint32_t pitch, uint32_t height) {
	if (const uint32_t bytes_per_element = Prospero::NumBytesPerElement(fmt);
	    bytes_per_element != 0) {
		return static_cast<uint64_t>(pitch) * height * bytes_per_element;
	}

	if (const uint32_t bytes_per_block = Prospero::BlockCompressedBytesPerBlock(fmt);
	    bytes_per_block != 0) {
		const uint32_t blocks_w = std::max((pitch + 3u) / 4u, 1u);
		const uint32_t blocks_h = std::max((height + 3u) / 4u, 1u);
		return static_cast<uint64_t>(blocks_w) * blocks_h * bytes_per_block;
	}

	return 0;
}

uint64_t SetLinearUploadLevels(TileSizeOffset* level_sizes, uint32_t fmt, uint64_t height,
                               uint64_t levels, uint32_t base_pitch) {
	uint64_t offset = 0;
	auto     pitch  = base_pitch;
	auto     h      = static_cast<uint32_t>(height);

	for (uint32_t i = 0; i < levels; i++) {
		const auto size = CalcLinearUploadLevelSize(fmt, pitch, h);
		EXIT_NOT_IMPLEMENTED(size > 0xffffffffull);
		EXIT_NOT_IMPLEMENTED(offset > 0xffffffffull);

		level_sizes[i].size       = static_cast<uint32_t>(size);
		level_sizes[i].offset     = static_cast<uint32_t>(offset);
		level_sizes[i].src_size   = 0;
		level_sizes[i].src_offset = 0;
		level_sizes[i].x          = 0;
		level_sizes[i].y          = 0;

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

TextureUploadLayout TextureCalcUploadLayout(uint32_t fmt, uint64_t width, uint64_t height,
                                            uint64_t levels, uint32_t depth, uint64_t pitch,
                                            uint64_t tile, uint64_t upload_size,
                                            bool allow_depth_tile, bool volume_texture,
                                            const char* owner) {
	TextureUploadLayout layout {};
	layout.tile           = static_cast<uint32_t>(tile);
	layout.pitch          = static_cast<uint32_t>(pitch);
	layout.texel_block    = Prospero::BlockCompressedBytesPerBlock(fmt) != 0 ? 4u : 1u;
	layout.volume_texture = volume_texture;

	if (fmt != 0) {
		if (layout.tile != 0) {
			const auto tile_mode = static_cast<Prospero::TileMode>(layout.tile);
			switch (tile_mode) {
				case Prospero::TileMode::kStandard256B:
					if (TileIsStandard256BTextureSupported(fmt)) {
						layout.tile_family = TileBlockFamily::Standard256B;
					}
					break;
				case Prospero::TileMode::kStandard4KB:
					if (TileIsStandard4KBTextureSupported(fmt)) {
						layout.tile_family = TileBlockFamily::Standard4KB;
					}
					break;
				case Prospero::TileMode::kStandard64KB:
					if (TileIsStandard64KBTextureSupported(fmt)) {
						layout.tile_family = TileBlockFamily::Standard64KB;
					}
					break;
				case Prospero::TileMode::kPrt:
					if (TileIsStandard64KBTextureSupported(fmt)) {
						layout.tile_family = TileBlockFamily::Prt64KB;
					}
					break;
				case Prospero::TileMode::kRenderTarget:
					if (Prospero::RenderTargetBytesPerElement(fmt) != 0) {
						layout.tile_family = TileBlockFamily::RenderTarget64KB;
					}
					break;
				case Prospero::TileMode::kDepth:
					if (allow_depth_tile && Prospero::RenderTargetBytesPerElement(fmt) != 0) {
						layout.tile_family = TileBlockFamily::Depth64KB;
					}
					break;
				default: break;
			}
			if (layout.tile_family == TileBlockFamily::Count) {
				EXIT("%s: unsupported typed tiled upload: fmt=%u tile=%u "
				     "size=%" PRIu64 " extent=%" PRIu64 "x%" PRIu64 " pitch=%" PRIu64
				     " levels=%" PRIu64 "\n",
				     owner, static_cast<uint32_t>(fmt), layout.tile, upload_size, width, height,
				     pitch, levels);
			}
		}

		layout.pitch = TileGetTexturePitch(fmt, width, levels, layout.tile);

		TileGetTextureSize(fmt, width, height, layout.pitch, levels, layout.tile, nullptr,
		                   layout.level_sizes, layout.padded_sizes);

		if (static_cast<Prospero::TileMode>(layout.tile) != Prospero::TileMode::kLinear) {
			if (layout.volume_texture) {
				layout.slice_stride = SetLinearUploadLevels(layout.level_sizes, fmt, height, levels,
				                                            static_cast<uint32_t>(width));
			} else {
				TileSizeOffset tiled_levels[16] {};
				std::copy_n(layout.level_sizes, levels, tiled_levels);
				layout.source_slice_stride =
				    CalcTextureSliceStride(tiled_levels, levels, upload_size, depth);
				SetLinearUploadLevels(layout.level_sizes, fmt, height, levels, layout.pitch);
				for (uint32_t i = 0; i < levels; ++i) {
					if (tiled_levels[i].src_size > tiled_levels[i].size) {
						layout.first_tail_level = std::min(layout.first_tail_level, i);
					}
					layout.level_sizes[i].src_offset = GetLevelSrcOffset(tiled_levels[i]);
					layout.level_sizes[i].src_size   = GetLevelSrcSize(tiled_levels[i]);
					layout.level_sizes[i].x          = tiled_levels[i].x;
					layout.level_sizes[i].y          = tiled_levels[i].y;
				}
			}
		} else if (layout.volume_texture) {
			layout.slice_stride =
			    CalcTextureSliceStride(layout.level_sizes, levels, upload_size, depth);
		}
	} else {
		EXIT("%s: legacy texture upload format unsupported: fmt=0 tile=%u size=%" PRIu64
		     " extent=%" PRIu64 "x%" PRIu64 " pitch=%" PRIu64 " levels=%" PRIu64 "\n",
		     owner, layout.tile, upload_size, width, height, pitch, levels);
	}

	if (!layout.volume_texture) {
		layout.slice_stride =
		    CalcTextureSliceStride(layout.level_sizes, levels, upload_size, depth);
	}
	return layout;
}

std::vector<vk::BufferImageCopy>
TextureBuildImageCopies(const TextureUploadLayout& layout, uint32_t width, uint32_t height,
                        uint32_t depth, uint64_t levels, bool array_texture,
                        bool volume_texture) {
	uint32_t mip_width  = width;
	uint32_t mip_height = height;
	uint32_t mip_pitch  = volume_texture && static_cast<Prospero::TileMode>(layout.tile) !=
	                                            Prospero::TileMode::kLinear
	                          ? width
	                          : layout.pitch;

	std::vector<vk::BufferImageCopy> regions;
	regions.reserve(GetTextureRegionCount(depth, levels, volume_texture));
	for (uint32_t i = 0; i < levels; i++) {
		EXIT_NOT_IMPLEMENTED(layout.level_sizes[i].size == 0);

		const auto mip_depth = GetTextureLevelDepth(depth, i, volume_texture);

		for (uint32_t z = 0; z < mip_depth; z++) {
			const auto slice_offset = z * layout.slice_stride;
			vk::BufferImageCopy region {};
			region.bufferOffset =
			    layout.level_sizes[i].offset + slice_offset;
			region.imageSubresource = {vk::ImageAspectFlagBits::eColor, i,
			                           array_texture ? z : 0, 1};
			region.imageOffset.z = volume_texture ? static_cast<int>(z) : 0;
			region.imageExtent   = {mip_width, mip_height, 1};
			const bool linear =
			    static_cast<Prospero::TileMode>(layout.tile) == Prospero::TileMode::kLinear;
			if (linear) {
				region.bufferRowLength   = layout.padded_sizes[i].width;
				region.bufferImageHeight = layout.padded_sizes[i].height;
			} else {
				const auto align = [](uint32_t value, uint32_t block) {
					return ((value + block - 1u) / block) * block;
				};
				const auto pitch = align(mip_pitch, layout.texel_block);
				region.bufferRowLength =
				    pitch > align(mip_width, layout.texel_block) ? pitch : 0;
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

struct GpuTileElementLayout {
	uint32_t bytes = 0;
	uint32_t wide  = 1;
	uint32_t tall  = 1;
};

static bool GetGpuTileElementLayout(uint32_t fmt, GpuTileElementLayout& out) {
	if (const auto bytes = Prospero::NumBytesPerElement(fmt); bytes != 0) {
		out = {bytes, 1, 1};
		return true;
	}
	if (const auto bytes = Prospero::BlockCompressedBytesPerBlock(fmt); bytes != 0) {
		out = {bytes, 4, 4};
		return true;
	}
	return false;
}

static bool SetGpuTileSize(uint64_t offset, uint64_t length, uint64_t capacity, uint64_t& size) {
	if (offset > capacity || length > capacity - offset) {
		return false;
	}
	size = length;
	return true;
}

bool TextureBuildGpuTileInfos(uint64_t size,
                              const std::vector<vk::BufferImageCopy>& regions,
                              const TextureUploadLayout& layout, uint32_t fmt, uint32_t depth,
                              uint64_t levels, std::vector<GpuTileInfo>& out_infos) {
	if (size == 0 || levels == 0 || levels > 16 || depth == 0 ||
	    regions.size() != GetTextureRegionCount(depth, levels, layout.volume_texture) ||
	    Prospero::IsFmaskTextureFormat(fmt)) {
		return false;
	}

	GpuTileElementLayout element {};
	if (layout.tile_family == TileBlockFamily::RenderTarget64KB ||
	    layout.tile_family == TileBlockFamily::Depth64KB) {
		element.bytes = Prospero::RenderTargetBytesPerElement(fmt);
	} else if (!GetGpuTileElementLayout(fmt, element)) {
		return false;
	}
	if (element.bytes == 0) {
		return false;
	}

	std::vector<GpuTileInfo> infos;
	infos.reserve(regions.size());
	if (layout.volume_texture) {
		TileVolumeLayout volume {};
		if (!TileGetTextureVolumeLayout(fmt, regions[0].imageExtent.width,
		                                regions[0].imageExtent.height, depth,
		                                static_cast<uint32_t>(levels), layout.tile, volume)) {
			return false;
		}
		element = {volume.bytes_per_element, volume.texel_width, volume.texel_height};
		TileBlockLayout block {};
		if (!TileGetBlockLayout(volume.family, element.bytes, block)) return false;

		size_t region_base = 0;
		for (uint32_t level = 0; level < levels; ++level) {
			const uint32_t mip_depth     = GetTextureLevelDepth(depth, level, true);
			const bool     tail          = level >= volume.first_tail_level;
			const uint64_t linear_stride = layout.slice_stride;
			for (uint32_t z = 0; z < mip_depth; z += block.block_depth) {
				const uint32_t copy_depth = std::min(block.block_depth, mip_depth - z);
				const auto&    region     = regions[region_base + z];
				const auto     pitch = region.bufferRowLength != 0
				                           ? region.bufferRowLength
				                           : region.imageExtent.width;
				const auto logical_height = region.bufferImageHeight != 0
				                                ? region.bufferImageHeight
				                                : region.imageExtent.height;
				GpuTileInfo    info {};
				info.family            = block.family;
				info.bytes_per_element = block.bytes_per_element;
				info.linear_offset     = region.bufferOffset;
				info.tiled_offset =
				    static_cast<uint64_t>(z / block.block_depth) * volume.block_slice_size +
				    volume.level_offsets[level];
				const uint64_t linear_span =
				    static_cast<uint64_t>(copy_depth - 1u) * linear_stride +
				    layout.level_sizes[level].size;
				if (!SetGpuTileSize(info.linear_offset, linear_span, size, info.linear_size) ||
				    !SetGpuTileSize(info.tiled_offset, volume.level_sizes[level], size,
				                    info.tiled_size)) {
					return false;
				}
				info.linear_slice_stride = linear_stride;
				info.width = std::max(
				    (region.imageExtent.width + element.wide - 1u) / element.wide, 1u);
				info.height = std::max(
				    (logical_height + element.tall - 1u) / element.tall, 1u);
				info.depth       = copy_depth;
				info.surface_z = block.block_depth == 1
				                     ? static_cast<uint32_t>(region.imageOffset.z)
				                     : 0;
				info.pitch =
				    std::max((pitch + element.wide - 1u) / element.wide, 1u);
				info.tail_x      = tail ? volume.tail_x[level] : 0;
				info.tail_y      = tail ? volume.tail_y[level] : 0;
				info.tail        = tail;
				info.tiled_width = volume.level_widths[level];
				info.tiled_height = volume.level_heights[level];
				infos.push_back(info);
			}
			region_base += mip_depth;
		}
	} else {
		const auto base_family = layout.tile_family;
		if (base_family == TileBlockFamily::Count) {
			return false;
		}

		size_t region_index = 0;
		for (uint32_t level = 0; level < levels; level++) {
			const auto&     level_size = layout.level_sizes[level];
			const bool      tail       = level >= layout.first_tail_level;
			const auto      family     = base_family;
			TileBlockLayout block {};
			if (!TileGetBlockLayout(family, element.bytes, block)) {
				return false;
			}
			const auto level_depth = GetTextureLevelDepth(depth, level, layout.volume_texture);
			for (uint32_t z = 0; z < level_depth; z++) {
				const auto& region = regions[region_index++];
				const auto pitch = region.bufferRowLength != 0
				                       ? region.bufferRowLength
				                       : region.imageExtent.width;
				const auto logical_height = region.bufferImageHeight != 0
				                                ? region.bufferImageHeight
				                                : region.imageExtent.height;
				GpuTileInfo info {};
				info.family            = block.family;
				info.bytes_per_element = block.bytes_per_element;
				info.linear_offset     = region.bufferOffset;
				info.tiled_offset      = TextureUploadSliceSourceOffset(layout, level, z);
				if (!SetGpuTileSize(info.linear_offset, level_size.size, size, info.linear_size) ||
				    !SetGpuTileSize(info.tiled_offset, GetLevelSrcSize(level_size), size,
				                    info.tiled_size)) {
					return false;
				}
				info.width = std::max(
				    (region.imageExtent.width + element.wide - 1u) / element.wide, 1u);
				info.height = std::max(
				    (logical_height + element.tall - 1u) / element.tall, 1u);
				info.surface_z = base_family == TileBlockFamily::RenderTarget64KB ||
				                         base_family == TileBlockFamily::Depth64KB
				                     ? region.imageSubresource.baseArrayLayer
				                     : 0;
				info.pitch =
				    std::max((pitch + element.wide - 1u) / element.wide, 1u);
				info.tail      = tail;
				info.tail_x    = tail ? level_size.x : 0;
				info.tail_y    = tail ? level_size.y : 0;
				info.tiled_width =
				    layout.padded_sizes[level].width != 0
				        ? std::max((layout.padded_sizes[level].width + element.wide - 1u) /
				                       element.wide,
				                   1u)
				        : info.pitch;
				info.tiled_height =
				    layout.padded_sizes[level].height != 0
				        ? std::max((layout.padded_sizes[level].height + element.tall - 1u) /
				                       element.tall,
				                   1u)
				        : info.height;
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
