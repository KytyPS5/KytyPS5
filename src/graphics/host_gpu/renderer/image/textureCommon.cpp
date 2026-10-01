#include "graphics/host_gpu/renderer/image/textureCommon.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/host_gpu/renderer/image/tiler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <array>
#include <cinttypes>

namespace Libs::Graphics {
namespace {

// Rows are channel order; columns are the number of physical components minus one. Unused
// selectors complete each entry to a permutation so logical write masks can be inverted.
constexpr Prospero::ColorComponentMapping kRenderTargetColorMappings[4][4] = {
    {Prospero::ColorMappingRgba, Prospero::ColorMappingRgba, Prospero::ColorMappingRgba,
     Prospero::ColorMappingRgba},
    {Prospero::ColorMappingGr, Prospero::ColorMappingRabg, Prospero::ColorMappingRgab,
     Prospero::ColorMappingBgra},
    {Prospero::ColorMappingBgra, Prospero::ColorMappingGr, Prospero::ColorMappingBgra,
     Prospero::ColorMappingAbgr},
    {Prospero::ColorMappingAgba, Prospero::ColorMappingArbg, Prospero::ColorMappingAgbr,
     Prospero::ColorMappingArgb},
};

struct HostFormatInfo {
	vk::Format                      format = vk::Format::eUndefined;
	Prospero::ColorComponentMapping host_to_storage;
};

HostFormatInfo ResolveHostFormat(Prospero::BufferFormat guest_format,
                                 Prospero::ChannelOrder order) {
	if (order == Prospero::ChannelOrder::kAlt) {
		switch (guest_format) {
			case Prospero::BufferFormat::k8_8_8_8UNorm:
				return {vk::Format::eB8G8R8A8Unorm, Prospero::ColorMappingBgra};
			case Prospero::BufferFormat::k8_8_8_8SNorm:
				return {vk::Format::eB8G8R8A8Snorm, Prospero::ColorMappingBgra};
			case Prospero::BufferFormat::k8_8_8_8Srgb:
				return {vk::Format::eB8G8R8A8Srgb, Prospero::ColorMappingBgra};
			case Prospero::BufferFormat::k10_10_10_2UNorm:
				return {vk::Format::eA2R10G10B10UnormPack32, Prospero::ColorMappingBgra};
			default: break;
		}
	}
	const auto format = VulkanFormat(guest_format);
	switch (guest_format) {
		case Prospero::BufferFormat::k5_5_5_1UNorm: return {format, Prospero::ColorMappingBgra};
		case Prospero::BufferFormat::k1_5_5_5UNorm:
		case Prospero::BufferFormat::k4_4_4_4UNorm: return {format, Prospero::ColorMappingAbgr};
		default: return {format, {}};
	}
}

uint32_t GetTextureLevelDepth(uint32_t depth, uint32_t level, bool volume_texture) {
	return volume_texture ? std::max(depth >> level, 1u) : depth;
}

size_t GetTextureRegionCount(uint32_t depth, uint32_t levels, bool volume_texture) {
	size_t count = 0;
	for (uint32_t level = 0; level < levels; level++) {
		count += GetTextureLevelDepth(depth, level, volume_texture);
	}
	return count;
}

uint64_t CalcTextureSliceStride(const TextureUploadMipLayout* mips, uint32_t levels,
                                uint64_t total_size, uint32_t depth) {
	uint64_t stride = 0;
	for (uint32_t mip_level = 0; mip_level < levels; ++mip_level) {
		stride = std::max(stride, mips[mip_level].offset + mips[mip_level].size);
	}

	if (depth > 1 && total_size != 0 && total_size % depth == 0) {
		stride = std::max(stride, total_size / depth);
	}

	return stride;
}

uint64_t CalcLinearUploadLevelSize(const TileTextureElementLayout& element, uint32_t pitch,
                                   uint32_t height) {
	const uint32_t element_columns =
	    std::max((pitch + element.texel_width - 1u) / element.texel_width, 1u);
	const uint32_t element_rows =
	    std::max((height + element.texel_height - 1u) / element.texel_height, 1u);
	return static_cast<uint64_t>(element_columns) * element_rows * element.bytes;
}

uint64_t SetLinearUploadLevels(TextureUploadMipLayout*         mips,
                               const TileTextureElementLayout& element, uint32_t height,
                               uint32_t levels, uint32_t base_pitch) {
	uint64_t offset     = 0;
	auto     mip_pitch  = base_pitch;
	auto     mip_height = height;

	for (uint32_t mip_level = 0; mip_level < levels; ++mip_level) {
		const auto size = CalcLinearUploadLevelSize(element, mip_pitch, mip_height);
		mips[mip_level] = {offset, size};

		offset += size;
		if (mip_pitch > 1) {
			mip_pitch /= 2;
		}
		if (mip_height > 1) {
			mip_height /= 2;
		}
	}

	return offset;
}

bool FitsBufferRange(uint64_t offset, uint64_t size, uint64_t capacity) {
	return offset <= capacity && size <= capacity - offset;
}

} // namespace

RenderTargetFormatInfo TextureGetRenderTargetFormat(Prospero::ChannelLayout layout,
                                                    Prospero::ChannelType   type,
                                                    Prospero::ChannelOrder  order) {
	const auto encoding = Prospero::ResolveRenderTargetFormat(layout, type);
	if (encoding.IsValid() && encoding.SupportsOrder(order)) {
		const auto host_format = ResolveHostFormat(encoding.buffer_format, order);
		const auto bytes       = Prospero::RenderTargetBytesPerElement(encoding.buffer_format);
		if (host_format.format != vk::Format::eUndefined && bytes != 0) {
			const auto order_mapping =
			    kRenderTargetColorMappings[static_cast<size_t>(order)][encoding.components - 1u];
			return {host_format.format, bytes, host_format.host_to_storage.Then(order_mapping)};
		}
	}
	EXIT("unsupported render-target format combination: layout=%u type=%u order=%u\n",
	     static_cast<uint32_t>(layout), static_cast<uint32_t>(type), static_cast<uint32_t>(order));
}

vk::ComponentMapping TextureGetComponentMapping(uint32_t                        swizzle,
                                                Prospero::ColorComponentMapping host_to_storage) {
	constexpr std::array host_components {vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eG,
	                                      vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eA};
	std::array<vk::ComponentSwizzle, 4> storage_components {};
	for (uint32_t host_component = 0; host_component < host_components.size(); ++host_component) {
		storage_components[host_to_storage.Map(host_component)] = host_components[host_component];
	}
	const auto resolve = [&](uint32_t component) {
		const auto selector = static_cast<Prospero::CompSwizzle>(GetDstSel(swizzle, component));
		switch (selector) {
			case Prospero::CompSwizzle::kZero: return vk::ComponentSwizzle::eZero;
			case Prospero::CompSwizzle::kOne: return vk::ComponentSwizzle::eOne;
			case Prospero::CompSwizzle::kRed: return storage_components[0];
			case Prospero::CompSwizzle::kGreen: return storage_components[1];
			case Prospero::CompSwizzle::kBlue: return storage_components[2];
			case Prospero::CompSwizzle::kAlpha: return storage_components[3];
			default: EXIT("unknown swizzle: %u\n", static_cast<uint32_t>(selector));
		}
	};
	return {resolve(0), resolve(1), resolve(2), resolve(3)};
}

SurfaceFormatInfo TextureGetSurfaceFormatInfo(Prospero::BufferFormat format) {
	const auto backing_format = Prospero::RemapTextureFormat(format);
	const auto host_format = ResolveHostFormat(backing_format, Prospero::ChannelOrder::kStandard);
	const auto conversion_format =
	    backing_format != format ? format : Prospero::BufferFormat::kInvalid;
	if (host_format.format != vk::Format::eUndefined) {
		return {host_format.format, conversion_format, host_format.host_to_storage};
	}
	EXIT("unknown format: fmt = %u\n", static_cast<uint32_t>(format));
}

TextureUploadLayout TextureCalcUploadLayout(Prospero::BufferFormat format, uint32_t width,
                                            uint32_t height, uint32_t levels, uint32_t depth,
                                            Prospero::TileMode tile_mode, uint64_t upload_size,
                                            bool allow_depth_tile, bool volume_texture,
                                            const char* owner) {
	TextureUploadLayout layout {};
	layout.surface.description = {
	    format,
	    tile_mode,
	    volume_texture ? TileSurfaceDimension::Dim3D : TileSurfaceDimension::Dim2D,
	    width,
	    height,
	    volume_texture ? depth : 1u,
	    levels,
	    volume_texture ? 1u : depth,
	};
	const auto&              description = layout.surface.description;
	TileTextureElementLayout element {};

	if (format == Prospero::BufferFormat::kInvalid) {
		EXIT("%s: legacy texture upload format unsupported: fmt=0 tile=%u size=%" PRIu64
		     " extent=%ux%u levels=%u\n",
		     owner, static_cast<uint32_t>(description.tile_mode), upload_size, width, height,
		     levels);
	}

	if (tile_mode == Prospero::TileMode::kLinear) {
		if (!TileGetTextureElementLayout(format, element)) {
			EXIT("%s: unsupported linear texture format: fmt=%u\n", owner,
			     static_cast<uint32_t>(format));
		}
	} else {
		if ((tile_mode == Prospero::TileMode::kDepth && !allow_depth_tile) ||
		    !TileGetTiledTextureLayout(description, layout.surface)) {
			EXIT("%s: unsupported typed tiled upload: fmt=%u tile=%u "
			     "size=%" PRIu64 " extent=%ux%u levels=%u\n",
			     owner, static_cast<uint32_t>(format), static_cast<uint32_t>(tile_mode),
			     upload_size, width, height, levels);
		}
		element = {layout.surface.texture.block.bytes_per_element,
		           layout.surface.texture.texel_width, layout.surface.texture.texel_height};
	}
	layout.pitch = TileGetTexturePitch(format, width, description.tile_mode);
	if (tile_mode == Prospero::TileMode::kLinear) {
		TileSizeOffset level_sizes[16] {};
		TilePaddedSize padded_sizes[16] {};
		TileGetTextureSize(format, width, height, levels, description.tile_mode, nullptr,
		                   level_sizes, padded_sizes);
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
	const bool  linear_texture = description.tile_mode == Prospero::TileMode::kLinear;
	const auto  depth          = volume_texture ? description.depth : description.layers;
	uint32_t    mip_width      = description.width;
	uint32_t    mip_height     = description.height;
	uint32_t    mip_pitch = volume_texture && !linear_texture ? description.width : layout.pitch;

	std::vector<vk::BufferImageCopy> regions;
	regions.reserve(GetTextureRegionCount(depth, description.levels, volume_texture));
	for (uint32_t mip_level = 0; mip_level < description.levels; ++mip_level) {
		EXIT_NOT_IMPLEMENTED(layout.mips[mip_level].size == 0);
		const auto          mip_depth = GetTextureLevelDepth(depth, mip_level, volume_texture);
		vk::BufferImageCopy region {};
		region.imageSubresource = {vk::ImageAspectFlagBits::eColor, mip_level, 0, 1};
		region.imageExtent      = {mip_width, mip_height, 1};
		if (linear_texture) {
			region.bufferRowLength   = layout.mips[mip_level].row_length;
			region.bufferImageHeight = layout.mips[mip_level].image_height;
		} else {
			const auto texel_width   = layout.surface.texture.texel_width;
			const auto aligned_pitch = Common::AlignUp(mip_pitch, texel_width);
			region.bufferRowLength =
			    aligned_pitch > Common::AlignUp(mip_width, texel_width) ? aligned_pitch : 0;
		}
		for (uint32_t slice_index = 0; slice_index < mip_depth; ++slice_index) {
			region.bufferOffset = layout.mips[mip_level].offset + slice_index * layout.slice_stride;
			region.imageSubresource.baseArrayLayer = volume_texture ? 0u : slice_index;
			region.imageOffset.z = volume_texture ? static_cast<int>(slice_index) : 0;
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

bool TextureDownloadCoversRange(const TextureUploadLayout&           layout,
                                std::span<const vk::BufferImageCopy> regions,
                                std::span<const GpuTileInfo> tiles, uint64_t offset,
                                uint64_t size) {
	if (size == 0 || offset > UINT64_MAX - size || regions.empty()) {
		return false;
	}
	const auto end = offset + size;
	if (layout.surface.description.tile_mode == Prospero::TileMode::kLinear) {
		if (!tiles.empty()) {
			return false;
		}
		TileTextureElementLayout element {};
		if (!TileGetTextureElementLayout(layout.surface.description.format, element) ||
		    element.bytes == 0 || element.texel_width == 0 || element.texel_height == 0) {
			return false;
		}
		size_t selected = regions.size();
		for (uint64_t cursor = offset; cursor < end;) {
			uint64_t covered_end = cursor;
			for (size_t index = 0; index < regions.size(); ++index) {
				if (selected != regions.size() && selected != index) {
					continue;
				}
				const auto& region = regions[index];
				if (region.imageExtent.depth != 1 || region.imageSubresource.layerCount != 1 ||
				    region.imageExtent.width == 0 || region.imageExtent.height == 0) {
					continue;
				}
				const uint64_t row_length =
				    region.bufferRowLength != 0 ? region.bufferRowLength : region.imageExtent.width;
				const uint64_t row_bytes =
				    ((row_length + element.texel_width - 1) / element.texel_width) * element.bytes;
				const uint64_t active_bytes =
				    ((static_cast<uint64_t>(region.imageExtent.width) + element.texel_width - 1) /
				     element.texel_width) *
				    element.bytes;
				const uint64_t active_rows =
				    (static_cast<uint64_t>(region.imageExtent.height) + element.texel_height - 1) /
				    element.texel_height;
				if (row_bytes == 0 || row_bytes < active_bytes || cursor < region.bufferOffset) {
					continue;
				}
				const auto relative = cursor - region.bufferOffset;
				const auto row      = relative / row_bytes;
				const auto column   = relative % row_bytes;
				if (row < active_rows && column < active_bytes) {
					selected    = index;
					covered_end = std::max(covered_end,
					                       cursor + std::min(active_bytes - column, end - cursor));
				}
			}
			if (covered_end == cursor) {
				return false;
			}
			cursor = std::min(covered_end, end);
		}
		return true;
	}

	if (tiles.size() != regions.size()) {
		return false;
	}
	size_t selected = tiles.size();
	for (uint64_t cursor = offset; cursor < end;) {
		uint64_t covered_end = cursor;
		for (size_t index = 0; index < tiles.size(); ++index) {
			if (selected != tiles.size() && selected != index) {
				continue;
			}
			const auto&     tile   = tiles[index];
			const auto&     region = regions[index];
			TileBlockLayout block {};
			if (tile.tail || tile.depth != 1 || region.imageExtent.depth != 1 ||
			    region.imageSubresource.layerCount != 1 || region.bufferImageHeight != 0 ||
			    tile.tiled_offset > UINT64_MAX - tile.tiled_size ||
			    !TileGetBlockLayout(tile.family, tile.bytes_per_element, block) ||
			    block.block_depth != 1 || block.block_size == 0 || cursor < tile.tiled_offset ||
			    cursor - tile.tiled_offset >= tile.tiled_size) {
				continue;
			}
			const auto texel_width  = layout.surface.texture.texel_width;
			const auto texel_height = layout.surface.texture.texel_height;
			if (texel_width == 0 || texel_height == 0 ||
			    static_cast<uint64_t>(tile.width) * texel_width != region.imageExtent.width ||
			    static_cast<uint64_t>(tile.height) * texel_height != region.imageExtent.height) {
				continue;
			}
			const uint64_t columns =
			    (static_cast<uint64_t>(tile.tiled_width) + block.block_width - 1) /
			    block.block_width;
			if (columns == 0) {
				continue;
			}
			const auto block_index = (cursor - tile.tiled_offset) / block.block_size;
			const auto block_x     = block_index % columns;
			const auto block_y     = block_index / columns;
			// The tiler writes only dispatched texels. A complete physical tile is safe;
			// partial edge tiles, packed mip tails, and allocation padding are not.
			if ((block_x + 1) * block.block_width > tile.width ||
			    (block_y + 1) * block.block_height > tile.height) {
				continue;
			}
			const auto block_end = tile.tiled_offset + (block_index + 1) * block.block_size;
			if (block_end <= tile.tiled_offset + tile.tiled_size) {
				selected    = index;
				covered_end = std::max(covered_end, block_end);
			}
		}
		if (covered_end == cursor) {
			return false;
		}
		cursor = std::min(covered_end, end);
	}
	return true;
}

bool TextureBuildGpuTileInfos(uint64_t tiled_size, const std::vector<vk::BufferImageCopy>& regions,
                              const TextureUploadLayout& layout, uint32_t levels,
                              std::vector<GpuTileInfo>& out_tile_infos) {
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

	std::vector<GpuTileInfo> tile_infos;
	tile_infos.reserve(regions.size());
	const auto slices_per_copy = volume_texture ? block.block_depth : 1u;
	size_t     region_base     = 0;
	for (uint32_t mip_level = 0; mip_level < levels; ++mip_level) {
		const auto& mip       = surface.mips[mip_level];
		const auto  mip_depth = GetTextureLevelDepth(depth, mip_level, volume_texture);
		for (uint32_t slice_index = 0; slice_index < mip_depth; slice_index += slices_per_copy) {
			const auto& region = regions[region_base + slice_index];
			GpuTileInfo tile_info {};
			tile_info.family            = block.family;
			tile_info.bytes_per_element = block.bytes_per_element;
			tile_info.linear_offset     = region.bufferOffset;
			tile_info.linear_size       = layout.mips[mip_level].size;
			tile_info.tiled_size        = mip.size;
			if (volume_texture) {
				tile_info.depth               = std::min(slices_per_copy, mip_depth - slice_index);
				tile_info.linear_slice_stride = layout.slice_stride;
				tile_info.linear_size += (tile_info.depth - 1u) * layout.slice_stride;
				tile_info.tiled_offset =
				    mip.offset +
				    static_cast<uint64_t>(slice_index / slices_per_copy) * surface.block_slice_size;
				tile_info.surface_z =
				    block.block_depth == 1 ? static_cast<uint32_t>(region.imageOffset.z) : 0;
			} else {
				const auto source_stride = layout.source_slice_stride != 0
				                               ? layout.source_slice_stride
				                               : surface.block_slice_size;
				if (slice_index > (UINT64_MAX - mip.offset) / source_stride) {
					return false;
				}
				tile_info.tiled_offset =
				    mip.offset + static_cast<uint64_t>(slice_index) * source_stride;
				tile_info.surface_z = block.family == TileBlockFamily::RenderTarget64KB ||
				                              block.family == TileBlockFamily::Depth64KB
				                          ? region.imageSubresource.baseArrayLayer
				                          : 0;
			}
			if (!FitsBufferRange(tile_info.linear_offset, tile_info.linear_size, UINT64_MAX) ||
			    !FitsBufferRange(tile_info.tiled_offset, tile_info.tiled_size, tiled_size)) {
				return false;
			}
			const auto row_length =
			    region.bufferRowLength != 0 ? region.bufferRowLength : region.imageExtent.width;
			const auto image_height = region.bufferImageHeight != 0 ? region.bufferImageHeight
			                                                        : region.imageExtent.height;
			tile_info.width         = std::max(
			    (region.imageExtent.width + texture.texel_width - 1u) / texture.texel_width, 1u);
			tile_info.height =
			    std::max((image_height + texture.texel_height - 1u) / texture.texel_height, 1u);
			tile_info.pitch =
			    std::max((row_length + texture.texel_width - 1u) / texture.texel_width, 1u);
			tile_info.tail         = mip_level >= surface.first_tail_level;
			tile_info.tail_x       = tile_info.tail ? mip.tail_x : 0;
			tile_info.tail_y       = tile_info.tail ? mip.tail_y : 0;
			tile_info.tiled_width  = mip.padded_width;
			tile_info.tiled_height = mip.padded_height;
			tile_infos.push_back(tile_info);
		}
		region_base += mip_depth;
	}

	out_tile_infos = std::move(tile_infos);
	return true;
}

} // namespace Libs::Graphics
