#include "graphics/host_gpu/renderer/image/refreshRegions.h"

#include "common/alignment.h"
#include "common/assert.h"

#include <algorithm>

namespace Libs::Graphics {
namespace {

// The tiler and the download path move 32-bit words, and the buffer cache hands out ranges in
// whole words. Rounding to 4 bytes keeps every range usable by all of them, even for 1 and 2 byte
// elements. Vulkan itself only needs a multiple of the texel size for color copies.
constexpr uint32_t MinimumRangeAlignment = 4;

// A block of cells in a grid that is read row by row.
struct CellRect {
	uint64_t row       = 0;
	uint64_t row_count = 0;
	uint64_t col       = 0;
	uint64_t col_count = 0;
};

// Covers the cells from (first_row, first_col) to (last_row, last_col) in reading order with at
// most three rectangles, being a partial first row, the full rows between and a partial last row.
// A first row that starts at column 0 or a last row that ends at the final column joins the
// middle rectangle, so the cover is never larger than the cells it was asked for.
template <typename Emit>
void SplitReadingOrder(uint64_t first_row, uint64_t first_col, uint64_t last_row,
                       uint64_t last_col, uint64_t columns, Emit&& emit) {
	// Callers keep both columns inside the row. A head starting past the last column would
	// otherwise wrap its unsigned width into a huge rectangle.
	EXIT_IF(first_col >= columns || last_col >= columns || first_row > last_row ||
	        (first_row == last_row && first_col > last_col));
	if (first_row == last_row) {
		emit(CellRect {first_row, 1, first_col, last_col - first_col + 1});
		return;
	}
	const bool head_full = first_col == 0;
	const bool tail_full = last_col + 1 == columns;
	if (!head_full) {
		emit(CellRect {first_row, 1, first_col, columns - first_col});
	}
	const uint64_t middle_begin = head_full ? first_row : first_row + 1;
	const uint64_t middle_end   = tail_full ? last_row : last_row - 1;
	if (middle_begin <= middle_end) {
		emit(CellRect {middle_begin, middle_end - middle_begin + 1, 0, columns});
	}
	if (!tail_full) {
		emit(CellRect {last_row, 1, 0, last_col + 1});
	}
}

// The aspect a copy of this image addresses, matching ImageInfo::BufferCopies.
vk::ImageAspectFlagBits CopyAspect(const ImageInfo& info) {
	if (info.IsDepth()) {
		return vk::ImageAspectFlagBits::eDepth;
	}
	return info.pixel_format == vk::Format::eS8Uint ? vk::ImageAspectFlagBits::eStencil
	                                                : vk::ImageAspectFlagBits::eColor;
}

// The size of a mip in storage elements.
struct ElementExtent {
	uint32_t width  = 0;
	uint32_t height = 0;
};

ElementExtent MipElementExtent(const ImageInfo& info, uint32_t level) {
	const auto extent = info.MipExtent(level);
	return {(extent.width + info.tiling.texel_width - 1u) / info.tiling.texel_width,
	        (extent.height + info.tiling.texel_height - 1u) / info.tiling.texel_height};
}

// Turns a rectangle of cells into elements and clips it to the mip, since the cells of the last
// row and column can hang over padding. Cells wholly in the padding add nothing.
void AppendClipped(std::vector<RefreshRegion>& out, uint32_t level, uint32_t layer,
                   const CellRect& cells, uint32_t cell_width, uint32_t cell_height,
                   const ElementExtent& mip) {
	const uint64_t x0 = cells.col * cell_width;
	const uint64_t y0 = cells.row * cell_height;
	if (x0 >= mip.width || y0 >= mip.height) {
		return;
	}
	const uint64_t width  = std::min<uint64_t>(cells.col_count * cell_width, mip.width - x0);
	const uint64_t height = std::min<uint64_t>(cells.row_count * cell_height, mip.height - y0);
	out.push_back({level, layer, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
	               static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
}

// Adds the regions of one tiled plane, which is one layer of one mip, that hold bytes of
// [lo, hi). Tail levels share a single block, so their addressed bytes cannot be told apart and
// the whole mip is taken.
void AppendTiledPlane(std::vector<RefreshRegion>& out, const ImageInfo& info, uint32_t level,
                      uint32_t layer, GuestRange plane, uint64_t lo, uint64_t hi) {
	const uint64_t begin = std::max(lo, plane.address);
	const uint64_t end   = std::min(hi, plane.End());
	if (begin >= end) {
		return;
	}
	const auto mip = MipElementExtent(info, level);
	if (level >= info.first_tail_level) {
		out.push_back({level, layer, 0, 0, mip.width, mip.height});
		return;
	}
	const auto&    block   = info.tiling.block;
	const uint64_t columns = (uint64_t {info.mip_layout[level].pitch} + block.block_width - 1u) /
	                         block.block_width;
	// The last byte is end - 1, because end is exclusive and may sit exactly on a block boundary.
	const uint64_t first_block = (begin - plane.address) / block.block_size;
	const uint64_t last_block  = (end - 1u - plane.address) / block.block_size;
	SplitReadingOrder(first_block / columns, first_block % columns, last_block / columns,
	                  last_block % columns, columns, [&](const CellRect& cells) {
		                  AppendClipped(out, level, layer, cells, block.block_width,
		                                block.block_height, mip);
	                  });
}

// Adds the regions of one linear slice of one mip that hold bytes of [lo, hi). slice_begin is
// the slice's offset into the image data.
void AppendLinearSlice(std::vector<RefreshRegion>& out, const ImageInfo& info, uint32_t level,
                       uint32_t layer, uint64_t slice_begin, uint64_t lo, uint64_t hi) {
	const auto&    mip       = info.mip_layout[level];
	const auto     extent    = MipElementExtent(info, level);
	const uint64_t element   = info.tiling.block.bytes_per_element;
	const uint64_t row_bytes = uint64_t {mip.linear_pitch} * element;
	if (row_bytes == 0 || extent.width == 0 || extent.height == 0) {
		return;
	}
	const uint64_t used  = (extent.height - 1u) * row_bytes + extent.width * element;
	const uint64_t begin = std::max(lo, slice_begin);
	const uint64_t end   = std::min(hi, slice_begin + used);
	if (begin >= end) {
		return;
	}
	uint64_t first_row = (begin - slice_begin) / row_bytes;
	uint64_t first_col = (begin - slice_begin) % row_bytes / element;
	uint64_t last_row  = (end - 1u - slice_begin) / row_bytes;
	uint64_t last_col  = (end - 1u - slice_begin) % row_bytes / element;
	// Bytes in the row padding hold no texels, so a first byte there starts the next row and a
	// last byte there ends at the final texel of its row.
	if (first_col >= extent.width) {
		++first_row;
		first_col = 0;
	}
	last_col = std::min<uint64_t>(last_col, extent.width - 1u);
	if (first_row > last_row || (first_row == last_row && first_col > last_col)) {
		return;
	}
	SplitReadingOrder(first_row, first_col, last_row, last_col, extent.width,
	                  [&](const CellRect& cells) {
		                  AppendClipped(out, level, layer, cells, 1, 1, extent);
	                  });
}

} // namespace

RefreshByteRange RoundRefreshRange(const ImageInfo& info, uint64_t lo, uint64_t hi) {
	const uint64_t alignment = std::max(info.tiling.block.bytes_per_element, MinimumRangeAlignment);
	lo                       = Common::AlignDown(lo, alignment);
	hi                       = std::min(Common::AlignUp(hi, alignment), info.data.size);
	return lo < hi ? RefreshByteRange {lo, hi} : RefreshByteRange {};
}

std::vector<RefreshRegion> TiledRefreshRegions(const ImageInfo& info, uint64_t lo, uint64_t hi) {
	std::vector<RefreshRegion> regions;
	const auto&                block = info.tiling.block;
	if (!info.IsTiled() || info.IsVolume() || block.block_size == 0 || block.block_width == 0 ||
	    block.block_height == 0 || info.resources.layers == 0 || info.tiling.texel_width == 0 ||
	    info.tiling.texel_height == 0) {
		return regions;
	}
	const auto range = RoundRefreshRange(info, lo, hi);
	for (uint32_t level = 0; level < info.resources.levels; ++level) {
		const auto&    mip        = info.mip_layout[level];
		const uint64_t plane_size = mip.size / info.resources.layers;
		for (uint32_t layer = 0; layer < info.resources.layers; ++layer) {
			// This is the plane Prepare gives the tiler for the same layer of the same mip.
			const GuestRange plane {mip.offset + uint64_t {layer} * info.tiled_slice_stride,
			                        plane_size};
			AppendTiledPlane(regions, info, level, layer, plane, range.lo, range.hi);
		}
	}
	return regions;
}

std::vector<RefreshRegion> LinearRefreshRegions(const ImageInfo& info, uint64_t lo, uint64_t hi) {
	std::vector<RefreshRegion> regions;
	if (info.IsVolume() || info.resources.layers == 0 || info.tiling.texel_width == 0 ||
	    info.tiling.texel_height == 0) {
		return regions;
	}
	const auto range = RoundRefreshRange(info, lo, hi);
	for (uint32_t level = 0; level < info.resources.levels; ++level) {
		const auto&    mip    = info.mip_layout[level];
		const uint64_t stride = info.linear_slice_stride != 0 ? info.linear_slice_stride
		                                                      : mip.linear_size;
		for (uint32_t layer = 0; layer < info.resources.layers; ++layer) {
			AppendLinearSlice(regions, info, level, layer,
			                  mip.linear_offset + uint64_t {layer} * stride, range.lo, range.hi);
		}
	}
	return regions;
}

std::vector<vk::BufferImageCopy>
RegionCopies(const ImageInfo& info, std::span<const RefreshRegion> regions, uint64_t buffer_base) {
	const auto aspect = CopyAspect(info);
	const uint64_t                   element = info.tiling.block.bytes_per_element;
	std::vector<vk::BufferImageCopy> copies;
	copies.reserve(regions.size());
	for (const auto& region: regions) {
		const auto&    mip       = info.mip_layout[region.level];
		const auto     extent    = info.MipExtent(region.level);
		const uint64_t row_bytes = uint64_t {mip.linear_pitch} * element;
		const uint64_t stride    = info.linear_slice_stride != 0 ? info.linear_slice_stride
		                                                         : mip.linear_size;
		const uint32_t x_texel   = region.x0 * info.tiling.texel_width;
		const uint32_t y_texel   = region.y0 * info.tiling.texel_height;
		vk::BufferImageCopy copy {};
		// The origin term is needed because bufferOffset addresses the first texel copied and is
		// not shifted by imageOffset.
		copy.bufferOffset = buffer_base + mip.linear_offset + uint64_t {region.layer} * stride +
		                    uint64_t {region.y0} * row_bytes + uint64_t {region.x0} * element;
		copy.bufferRowLength  = mip.linear_pitch * info.tiling.texel_width;
		copy.imageSubresource = {aspect, region.level, region.layer, 1};
		copy.imageOffset      = vk::Offset3D {static_cast<int32_t>(x_texel),
		                                      static_cast<int32_t>(y_texel), 0};
		// Block formats can pad a mip past its real size, so the extent is cut to the mip.
		copy.imageExtent = vk::Extent3D {
		    std::min(region.width * info.tiling.texel_width, extent.width - x_texel),
		    std::min(region.height * info.tiling.texel_height, extent.height - y_texel), 1};
		copies.push_back(copy);
	}
	return copies;
}

} // namespace Libs::Graphics
