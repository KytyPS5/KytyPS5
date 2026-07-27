#include "graphics/guest_gpu/tile.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"

#include <algorithm>
#include <array>
#include <bit>
#include <fmt/format.h>
#include <vector>

namespace Libs::Graphics {

static uint32_t AlignUp(uint32_t value, uint32_t alignment) {
	return (value + alignment - 1u) & ~(alignment - 1u);
}

static uint32_t ShiftCeil(uint32_t value, uint32_t shift) {
	return static_cast<uint32_t>((static_cast<uint64_t>(value) + (1ull << shift) - 1ull) >> shift);
}

static uint32_t CalcLinearBlockWidth(uint32_t bytes_per_element) {
	return std::max(1u, 256u / bytes_per_element);
}

static uint32_t CalcLinearAlignedLevelPitch(uint32_t base_width, uint32_t base_height,
                                            uint32_t level, uint32_t bytes_per_element,
                                            uint32_t* padded_height, uint32_t* level_size) {
	const uint32_t level_width  = ShiftCeil(base_width, level);
	const uint32_t level_height = ShiftCeil(base_height, level);
	const uint32_t padded_width =
	    AlignUp(std::max(level_width, 1u), CalcLinearBlockWidth(bytes_per_element));
	const uint64_t size =
	    static_cast<uint64_t>(padded_width) * std::max(level_height, 1u) * bytes_per_element;
	EXIT_NOT_IMPLEMENTED(size > 0xffffffffull);
	*padded_height = std::max(level_height, 1u);
	*level_size    = static_cast<uint32_t>(size);
	return padded_width;
}

static uint32_t SetLinearMipChainLayout(uint32_t levels, const uint32_t* mip_pitch,
                                        const uint32_t* mip_height, const uint32_t* mip_size,
                                        TileSizeOffset* level_sizes, TilePaddedSize* padded_size) {
	uint32_t offset = 0;
	// Smaller mip records come first; mip 0 is last in the block slice.
	for (int32_t l = static_cast<int32_t>(levels) - 1; l >= 0; l--) {
		const auto level = static_cast<uint32_t>(l);
		if (level_sizes != nullptr) {
			level_sizes[level].size   = mip_size[level];
			level_sizes[level].offset = offset;
		}
		if (padded_size != nullptr) {
			padded_size[level].width  = mip_pitch[level];
			padded_size[level].height = mip_height[level];
		}

		offset += mip_size[level];
	}

	return AlignUp(offset, 256u);
}

static bool Gen5Standard4KBLayout(uint32_t format, uint32_t* bytes_per_element,
                                  uint32_t* texels_per_element_wide,
                                  uint32_t* texels_per_element_tall, uint32_t* block_width_log2,
                                  uint32_t* block_height_log2) {
	const auto bytes = Prospero::NumBytesPerElement(format);
	switch (bytes) {
		case 1:
			*bytes_per_element       = 1;
			*texels_per_element_wide = 1;
			*texels_per_element_tall = 1;
			*block_width_log2        = 6;
			*block_height_log2       = 6;
			return true;
		case 2:
			*bytes_per_element       = 2;
			*texels_per_element_wide = 1;
			*texels_per_element_tall = 1;
			*block_width_log2        = 6;
			*block_height_log2       = 5;
			return true;
		case 4:
			*bytes_per_element       = 4;
			*texels_per_element_wide = 1;
			*texels_per_element_tall = 1;
			*block_width_log2        = 5;
			*block_height_log2       = 5;
			return true;
		case 8:
			*bytes_per_element       = 8;
			*texels_per_element_wide = 1;
			*texels_per_element_tall = 1;
			*block_width_log2        = 5;
			*block_height_log2       = 4;
			return true;
		case 16:
			*bytes_per_element       = 16;
			*texels_per_element_wide = 1;
			*texels_per_element_tall = 1;
			*block_width_log2        = 4;
			*block_height_log2       = 4;
			return true;
		default: break;
	}

	switch (Prospero::BlockCompressedBytesPerBlock(format)) {
		case 8:
			*bytes_per_element       = 8;
			*texels_per_element_wide = 4;
			*texels_per_element_tall = 4;
			*block_width_log2        = 5;
			*block_height_log2       = 4;
			return true;
		case 16:
			*bytes_per_element       = 16;
			*texels_per_element_wide = 4;
			*texels_per_element_tall = 4;
			*block_width_log2        = 4;
			*block_height_log2       = 4;
			return true;
		default: return false;
	}
}

static bool Gen5Standard256BLayout(uint32_t format, uint32_t* bytes_per_element,
                                   uint32_t* texels_per_element_wide,
                                   uint32_t* texels_per_element_tall, uint32_t* block_width_log2,
                                   uint32_t* block_height_log2) {
	const auto bytes = Prospero::NumBytesPerElement(format);
	switch (bytes) {
		case 1:
			*bytes_per_element       = 1;
			*texels_per_element_wide = 1;
			*texels_per_element_tall = 1;
			*block_width_log2        = 4;
			*block_height_log2       = 4;
			return true;
		case 2:
			*bytes_per_element       = 2;
			*texels_per_element_wide = 1;
			*texels_per_element_tall = 1;
			*block_width_log2        = 4;
			*block_height_log2       = 3;
			return true;
		case 4:
			*bytes_per_element       = 4;
			*texels_per_element_wide = 1;
			*texels_per_element_tall = 1;
			*block_width_log2        = 3;
			*block_height_log2       = 3;
			return true;
		case 8:
			*bytes_per_element       = 8;
			*texels_per_element_wide = 1;
			*texels_per_element_tall = 1;
			*block_width_log2        = 3;
			*block_height_log2       = 2;
			return true;
		case 16:
			*bytes_per_element       = 16;
			*texels_per_element_wide = 1;
			*texels_per_element_tall = 1;
			*block_width_log2        = 2;
			*block_height_log2       = 2;
			return true;
		default: break;
	}

	switch (Prospero::BlockCompressedBytesPerBlock(format)) {
		case 8:
			*bytes_per_element       = 8;
			*texels_per_element_wide = 4;
			*texels_per_element_tall = 4;
			*block_width_log2        = 3;
			*block_height_log2       = 2;
			return true;
		case 16:
			*bytes_per_element       = 16;
			*texels_per_element_wide = 4;
			*texels_per_element_tall = 4;
			*block_width_log2        = 2;
			*block_height_log2       = 2;
			return true;
		default: return false;
	}
}

static bool Gen5Standard64KBLayout(uint32_t format, uint32_t* bytes_per_element,
                                   uint32_t* texels_per_element_wide,
                                   uint32_t* texels_per_element_tall, uint32_t* block_width_log2,
                                   uint32_t* block_height_log2) {
	switch (Prospero::NumBytesPerElement(format)) {
		case 1:
			*bytes_per_element       = 1;
			*texels_per_element_wide = 1;
			*texels_per_element_tall = 1;
			*block_width_log2        = 8;
			*block_height_log2       = 8;
			return true;
		case 2:
			*bytes_per_element       = 2;
			*texels_per_element_wide = 1;
			*texels_per_element_tall = 1;
			*block_width_log2        = 8;
			*block_height_log2       = 7;
			return true;
		case 4:
			*bytes_per_element       = 4;
			*texels_per_element_wide = 1;
			*texels_per_element_tall = 1;
			*block_width_log2        = 7;
			*block_height_log2       = 7;
			return true;
		case 8:
			*bytes_per_element       = 8;
			*texels_per_element_wide = 1;
			*texels_per_element_tall = 1;
			*block_width_log2        = 7;
			*block_height_log2       = 6;
			return true;
		case 16:
			*bytes_per_element       = 16;
			*texels_per_element_wide = 1;
			*texels_per_element_tall = 1;
			*block_width_log2        = 6;
			*block_height_log2       = 6;
			return true;
		default: break;
	}

	switch (Prospero::BlockCompressedBytesPerBlock(format)) {
		case 8:
			*bytes_per_element       = 8;
			*texels_per_element_wide = 4;
			*texels_per_element_tall = 4;
			*block_width_log2        = 7;
			*block_height_log2       = 6;
			return true;
		case 16:
			*bytes_per_element       = 16;
			*texels_per_element_wide = 4;
			*texels_per_element_tall = 4;
			*block_width_log2        = 6;
			*block_height_log2       = 6;
			return true;
		default: return false;
	}
}

static bool Gen5Thin64KBBlockSizeFromElementBytes(uint32_t bytes_per_element, uint32_t* block_width,
                                                  uint32_t* block_height) {
	// Thin 64 KiB block dimensions shared by depth and render-target tiles.
	switch (bytes_per_element) {
		case 1:
			*block_width  = 256;
			*block_height = 256;
			return true;
		case 2:
			*block_width  = 256;
			*block_height = 128;
			return true;
		case 4:
			*block_width  = 128;
			*block_height = 128;
			return true;
		case 8:
			*block_width  = 128;
			*block_height = 64;
			return true;
		case 16:
			*block_width  = 64;
			*block_height = 64;
			return true;
		default: break;
	}

	return false;
}

static bool Gen5Msaa64KBBlockSizeFromElementBytes(uint32_t  bytes_per_element,
                                                  uint32_t  num_fragments_log2,
                                                  uint32_t* block_width, uint32_t* block_height) {
	if (num_fragments_log2 == 0) {
		return Gen5Thin64KBBlockSizeFromElementBytes(bytes_per_element, block_width, block_height);
	}
	if (num_fragments_log2 > 3 || !std::has_single_bit(bytes_per_element) ||
	    bytes_per_element > 16) {
		return false;
	}
	// Row zero is the ordinary thin layout; rows 1..3 are 2x, 4x and 8x blocks.
	static constexpr uint8_t LOG2_BLOCK[4][5][2] = {
	    {{8, 8}, {8, 7}, {7, 7}, {7, 6}, {6, 6}},
	    {{7, 8}, {7, 7}, {6, 7}, {6, 6}, {5, 6}},
	    {{7, 7}, {7, 6}, {6, 6}, {6, 5}, {5, 5}},
	    {{6, 7}, {6, 6}, {5, 6}, {5, 5}, {4, 5}},
	};
	const auto bytes_log2 = std::countr_zero(bytes_per_element);
	*block_width          = 1u << LOG2_BLOCK[num_fragments_log2][bytes_log2][0];
	*block_height         = 1u << LOG2_BLOCK[num_fragments_log2][bytes_log2][1];
	return true;
}

struct Gen5MipTailLocation {
	uint32_t x;
	uint32_t y;
};

static constexpr Gen5MipTailLocation GEN5_MIP_TAIL_LOCATIONS_THIN_4KB[5][8] = {
    {{32, 0}, {16, 32}, {0, 48}, {0, 32}, {16, 16}, {16, 0}, {0, 16}, {0, 0}},
    {{32, 0}, {16, 16}, {0, 24}, {0, 16}, {16, 8}, {16, 0}, {0, 8}, {0, 0}},
    {{16, 0}, {8, 16}, {0, 24}, {0, 16}, {8, 8}, {8, 0}, {0, 8}, {0, 0}},
    {{16, 0}, {8, 8}, {0, 12}, {0, 8}, {8, 4}, {8, 0}, {0, 4}, {0, 0}},
    {{8, 0}, {4, 8}, {0, 12}, {0, 8}, {4, 4}, {4, 0}, {0, 4}, {0, 0}},
};

static constexpr Gen5MipTailLocation GEN5_MIP_TAIL_LOCATIONS_THIN_64KB[5][12] = {
    {{128, 0},
     {0, 128},
     {64, 0},
     {0, 64},
     {32, 0},
     {16, 32},
     {0, 48},
     {0, 32},
     {16, 16},
     {16, 0},
     {0, 16},
     {0, 0}},
    {{128, 0},
     {0, 64},
     {64, 0},
     {0, 32},
     {32, 0},
     {16, 16},
     {0, 24},
     {0, 16},
     {16, 8},
     {16, 0},
     {0, 8},
     {0, 0}},
    {{64, 0},
     {0, 64},
     {32, 0},
     {0, 32},
     {16, 0},
     {8, 16},
     {0, 24},
     {0, 16},
     {8, 8},
     {8, 0},
     {0, 8},
     {0, 0}},
    {{64, 0},
     {0, 32},
     {32, 0},
     {0, 16},
     {16, 0},
     {8, 8},
     {0, 12},
     {0, 8},
     {8, 4},
     {8, 0},
     {0, 4},
     {0, 0}},
    {{32, 0},
     {0, 32},
     {16, 0},
     {0, 16},
     {8, 0},
     {4, 8},
     {0, 12},
     {0, 8},
     {4, 4},
     {4, 0},
     {0, 4},
     {0, 0}},
};

static constexpr Gen5MipTailLocation GEN5_MIP_TAIL_LOCATIONS_THICK_64KB[5][10] = {
    {{32, 0}, {0, 16}, {16, 0}, {8, 8}, {0, 12}, {0, 8}, {8, 4}, {8, 0}, {0, 4}, {0, 0}},
    {{16, 0}, {0, 16}, {8, 0}, {4, 8}, {0, 12}, {0, 8}, {4, 4}, {4, 0}, {0, 4}, {0, 0}},
    {{16, 0}, {0, 16}, {8, 0}, {4, 8}, {0, 12}, {0, 8}, {4, 4}, {4, 0}, {0, 4}, {0, 0}},
    {{16, 0}, {0, 8}, {8, 0}, {4, 4}, {0, 6}, {0, 4}, {4, 2}, {4, 0}, {0, 2}, {0, 0}},
    {{8, 0}, {0, 8}, {4, 0}, {2, 4}, {0, 6}, {0, 4}, {2, 2}, {2, 0}, {0, 2}, {0, 0}},
};

static constexpr Gen5MipTailLocation GEN5_MIP_TAIL_LOCATIONS_THICK_4KB[5][5] = {
    {{0, 8}, {8, 4}, {8, 0}, {0, 4}, {0, 0}}, {{0, 8}, {4, 4}, {4, 0}, {0, 4}, {0, 0}},
    {{0, 8}, {4, 4}, {4, 0}, {0, 4}, {0, 0}}, {{0, 4}, {4, 2}, {4, 0}, {0, 2}, {0, 0}},
    {{0, 4}, {2, 2}, {2, 0}, {0, 2}, {0, 0}},
};

struct TextureBlockLayout {
	uint32_t bytes;
	uint32_t texel_width;
	uint32_t texel_height;
	uint32_t block_width;
	uint32_t block_height;
	uint32_t block_size;
};

static bool GetTextureBlockLayout(uint32_t format, uint32_t tile, TextureBlockLayout& out) {
	uint32_t width_log2 = 0, height_log2 = 0;
	if (tile == 1 && Gen5Standard256BLayout(format, &out.bytes, &out.texel_width, &out.texel_height,
	                                        &width_log2, &height_log2)) {
		out.block_size = 256;
	} else if (tile == 5 && Gen5Standard4KBLayout(format, &out.bytes, &out.texel_width,
	                                              &out.texel_height, &width_log2, &height_log2)) {
		out.block_size = 4096;
	} else if ((tile == 9 || tile == 17) &&
	           Gen5Standard64KBLayout(format, &out.bytes, &out.texel_width, &out.texel_height,
	                                  &width_log2, &height_log2)) {
		out.block_size = 65536;
	} else if (tile == 24 || tile == 27) {
		out.bytes       = Prospero::NumBytesPerElement(format);
		out.texel_width = out.texel_height = 1;
		if (out.bytes == 0 || (tile == 24 && out.bytes > 8) ||
		    !Gen5Thin64KBBlockSizeFromElementBytes(out.bytes, &out.block_width,
		                                           &out.block_height)) {
			return false;
		}
		out.block_size = 65536;
		return true;
	} else {
		return false;
	}
	out.block_width  = 1u << width_log2;
	out.block_height = 1u << height_log2;
	return true;
}

static void SetMicroMipLayout(const TextureBlockLayout& block, uint32_t width, uint32_t height,
                              uint32_t levels, TileSizeAlign* total_size,
                              TileSizeOffset* level_sizes, TilePaddedSize* padded_size) {
	const uint32_t width0  = (width + block.texel_width - 1u) / block.texel_width;
	const uint32_t height0 = (height + block.texel_height - 1u) / block.texel_height;
	uint32_t       offset  = 0;
	for (int32_t l = static_cast<int32_t>(levels) - 1; l >= 0; --l) {
		const auto     level = static_cast<uint32_t>(l);
		const uint32_t padded_width =
		    AlignUp(std::max(ShiftCeil(width0, level), 1u), block.block_width);
		const uint32_t padded_height =
		    AlignUp(std::max(ShiftCeil(height0, level), 1u), block.block_height);
		const uint32_t size = padded_width * padded_height * block.bytes;
		if (level_sizes != nullptr) level_sizes[level] = {size, offset};
		if (padded_size != nullptr) {
			padded_size[level] = {padded_width * block.texel_width,
			                      padded_height * block.texel_height};
		}
		offset += size;
	}
	if (total_size != nullptr) *total_size = {offset, block.block_size};
}

static void SetMacroMipLayout(const TextureBlockLayout& block, uint32_t tile, uint32_t width,
                              uint32_t height, uint32_t levels, TileSizeAlign* total_size,
                              TileSizeOffset* level_sizes, TilePaddedSize* padded_size) {
	const uint32_t width0     = (width + block.texel_width - 1u) / block.texel_width;
	const uint32_t height0    = (height + block.texel_height - 1u) / block.texel_height;
	const uint32_t bytes_log2 = std::countr_zero(block.bytes);
	const uint32_t max_tail   = block.block_size == 4096 ? 8u : 12u;
	uint32_t       tail_width = block.block_width >> 1u, tail_height = block.block_height;
	if (tile == 24 && block.bytes < 4) {
		tail_width  = 64;
		tail_height = 128;
	}

	uint32_t first_tail = levels;
	if (levels > 1) {
		for (uint32_t level = 0; level < levels; ++level) {
			if (ShiftCeil(width0, level) <= tail_width &&
			    ShiftCeil(height0, level) <= tail_height && levels - level <= max_tail) {
				first_tail = level;
				break;
			}
		}
	}

	uint32_t offset = first_tail < levels ? block.block_size : 0;
	for (int32_t l = static_cast<int32_t>(first_tail) - 1; l >= 0; --l) {
		const auto     level = static_cast<uint32_t>(l);
		const uint32_t padded_width =
		    AlignUp(std::max(ShiftCeil(width0, level), 1u), block.block_width);
		const uint32_t padded_height =
		    AlignUp(std::max(ShiftCeil(height0, level), 1u), block.block_height);
		const uint32_t size = padded_width * padded_height * block.bytes;
		if (level_sizes != nullptr) level_sizes[level] = {size, offset, size, offset};
		if (padded_size != nullptr) {
			padded_size[level] = {padded_width * block.texel_width,
			                      padded_height * block.texel_height};
		}
		offset += size;
	}

	uint32_t linear_offset = 0;
	for (uint32_t level = first_tail; level < levels; ++level) {
		const uint32_t mip_width =
		    std::max(((width >> level) + block.texel_width - 1u) / block.texel_width, 1u);
		const uint32_t mip_height =
		    std::max(((height >> level) + block.texel_height - 1u) / block.texel_height, 1u);
		const auto tail = block.block_size == 4096
		                      ? GEN5_MIP_TAIL_LOCATIONS_THIN_4KB[bytes_log2][level - first_tail]
		                      : GEN5_MIP_TAIL_LOCATIONS_THIN_64KB[bytes_log2][level - first_tail];
		if (level_sizes != nullptr) {
			level_sizes[level] = {mip_width * mip_height * block.bytes,
			                      linear_offset,
			                      block.block_size,
			                      0,
			                      tail.x,
			                      tail.y};
		}
		if (padded_size != nullptr) {
			padded_size[level] = {block.block_width * block.texel_width,
			                      block.block_height * block.texel_height};
		}
		linear_offset += mip_width * mip_height * block.bytes;
	}
	if (total_size != nullptr) *total_size = {offset, block.block_size};
}

bool TileGetTextureVolumeLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t depth,
                                uint32_t levels, uint32_t tile, TileVolumeLayout& out) {
	if (width == 0 || height == 0 || depth == 0 || levels == 0 || levels > 16) {
		return false;
	}
	TextureBlockLayout element {};
	if (!GetTextureBlockLayout(format, tile, element)) return false;

	TileBlockFamily family = TileBlockFamily::Count;
	switch (tile) {
		case 5: family = TileBlockFamily::Standard4KB3D; break;
		case 9: family = TileBlockFamily::Standard64KB3D; break;
		case 17: family = TileBlockFamily::Prt64KB3D; break;
		case 24: family = TileBlockFamily::Depth64KB; break;
		case 27: family = TileBlockFamily::RenderTarget64KB; break;
		default: return false;
	}
	TileBlockLayout block {};
	if (!TileGetBlockLayout(family, element.bytes, block)) return false;

	out                    = {};
	out.family             = family;
	out.bytes_per_element  = element.bytes;
	out.texel_width        = element.texel_width;
	out.texel_height       = element.texel_height;
	out.block_depth        = block.block_depth;
	out.first_tail_level   = levels;
	const uint32_t width0  = (width + element.texel_width - 1u) / element.texel_width;
	const uint32_t height0 = (height + element.texel_height - 1u) / element.texel_height;
	const bool     thick4  = family == TileBlockFamily::Standard4KB3D;
	const bool     thick64 =
	    family == TileBlockFamily::Standard64KB3D || family == TileBlockFamily::Prt64KB3D;
	uint32_t max_tail = 12u;
	if (thick4) {
		max_tail = 5u;
	} else if (thick64) {
		max_tail = 10u;
	}
	uint32_t tail_width  = thick4 ? block.block_width : block.block_width >> 1u;
	uint32_t tail_height = thick4 ? block.block_height >> 1u : block.block_height;
	if (family == TileBlockFamily::Depth64KB && element.bytes < 4) {
		tail_width  = 64;
		tail_height = 128;
	}

	for (uint32_t level = 0; level < levels; ++level) {
		const uint32_t mip_width  = std::max(ShiftCeil(width0, level), 1u);
		const uint32_t mip_height = std::max(ShiftCeil(height0, level), 1u);
		if (levels > 1 && mip_width <= tail_width && mip_height <= tail_height &&
		    levels - level <= max_tail) {
			out.first_tail_level = level;
			out.block_slice_size += block.block_size;
			break;
		}
		out.level_widths[level]  = AlignUp(mip_width, block.block_width);
		out.level_heights[level] = AlignUp(mip_height, block.block_height);
		out.level_sizes[level] = static_cast<uint64_t>(block.block_depth) *
		                         out.level_widths[level] * out.level_heights[level] * element.bytes;
		out.block_slice_size += out.level_sizes[level];
	}

	const auto                 bytes_log2     = std::countr_zero(element.bytes);
	const Gen5MipTailLocation* tail_locations = nullptr;
	if (thick4) {
		tail_locations = GEN5_MIP_TAIL_LOCATIONS_THICK_4KB[bytes_log2];
	} else if (thick64) {
		tail_locations = GEN5_MIP_TAIL_LOCATIONS_THICK_64KB[bytes_log2];
	} else {
		tail_locations = GEN5_MIP_TAIL_LOCATIONS_THIN_64KB[bytes_log2];
	}
	for (uint32_t level = out.first_tail_level; level < levels; ++level) {
		const auto index         = level - out.first_tail_level;
		const auto tail          = tail_locations[index];
		out.level_sizes[level]   = block.block_size;
		out.level_widths[level]  = block.block_width;
		out.level_heights[level] = block.block_height;
		out.tail_x[level]        = tail.x;
		out.tail_y[level]        = tail.y;
	}
	uint64_t offset = out.first_tail_level < levels ? block.block_size : 0;
	for (int32_t l = static_cast<int32_t>(out.first_tail_level) - 1; l >= 0; --l) {
		out.level_offsets[l] = offset;
		offset += out.level_sizes[l];
	}
	out.total_size = out.block_slice_size * ShiftCeil(depth, std::countr_zero(block.block_depth));
	return offset == out.block_slice_size;
}

static uint32_t Gen5Standard4KBOffsetInBlock(uint32_t x, uint32_t y, uint32_t bytes_per_element) {
	uint32_t offset = 0;

	switch (bytes_per_element) {
		case 1:
			offset ^= (y << 4u) & 0x1f0u;
			offset ^= (y << 5u) & 0x400u;
			offset ^= x & 0x00fu;
			offset ^= (x << 5u) & 0x200u;
			offset ^= (x << 6u) & 0x800u;
			return offset;
		case 2:
			offset ^= (y << 4u) & 0x070u;
			offset ^= (y << 5u) & 0x100u;
			offset ^= (y << 6u) & 0x400u;
			offset ^= (x << 1u) & 0x00eu;
			offset ^= (x << 4u) & 0x080u;
			offset ^= (x << 5u) & 0x200u;
			offset ^= (x << 6u) & 0x800u;
			return offset;
		case 4:
			offset ^= (y << 4u) & 0x070u;
			offset ^= (y << 5u) & 0x100u;
			offset ^= (y << 6u) & 0x400u;
			offset ^= (x << 2u) & 0x00cu;
			offset ^= (x << 5u) & 0x080u;
			offset ^= (x << 6u) & 0x200u;
			offset ^= (x << 7u) & 0x800u;
			return offset;
		case 8:
			offset ^= (y << 4u) & 0x030u;
			offset ^= (y << 6u) & 0x100u;
			offset ^= (y << 7u) & 0x400u;
			offset ^= (x << 3u) & 0x008u;
			offset ^= (x << 5u) & 0x0c0u;
			offset ^= (x << 6u) & 0x200u;
			offset ^= (x << 7u) & 0x800u;
			return offset;
		case 16:
			offset ^= (y << 4u) & 0x030u;
			offset ^= (y << 6u) & 0x100u;
			offset ^= (y << 7u) & 0x400u;
			offset ^= (x << 6u) & 0x0c0u;
			offset ^= (x << 7u) & 0x200u;
			offset ^= (x << 8u) & 0x800u;
			return offset;
		default: EXIT("unsupported Standard4KB element size: %u\n", bytes_per_element);
	}
	return 0;
}

static uint32_t Gen5Standard4KBVolumeOffsetInBlock(uint32_t x, uint32_t y, uint32_t z,
                                                   uint32_t bytes_per_element) {
	uint32_t offset = 0;

	switch (bytes_per_element) {
		case 1:
			offset ^= x & 0x3u;
			offset ^= (x << 4u) & 0x40u;
			offset ^= (x << 6u) & 0x200u;
			offset ^= (y << 3u) & 0x8u;
			offset ^= (y << 4u) & 0x20u;
			offset ^= (y << 6u) & 0x100u;
			offset ^= (y << 8u) & 0x800u;
			offset ^= (z << 2u) & 0x4u;
			offset ^= (z << 3u) & 0x10u;
			offset ^= (z << 5u) & 0x80u;
			offset ^= (z << 7u) & 0x400u;
			return offset;
		case 2:
			offset ^= (x << 1u) & 0x2u;
			offset ^= (x << 5u) & 0x40u;
			offset ^= (x << 7u) & 0x200u;
			offset ^= (y << 3u) & 0x8u;
			offset ^= (y << 4u) & 0x20u;
			offset ^= (y << 6u) & 0x100u;
			offset ^= (y << 8u) & 0x800u;
			offset ^= (z << 2u) & 0x4u;
			offset ^= (z << 3u) & 0x10u;
			offset ^= (z << 5u) & 0x80u;
			offset ^= (z << 7u) & 0x400u;
			return offset;
		case 4:
			offset ^= (x << 2u) & 0x4u;
			offset ^= (x << 5u) & 0x40u;
			offset ^= (x << 7u) & 0x200u;
			offset ^= (y << 3u) & 0x8u;
			offset ^= (y << 4u) & 0x20u;
			offset ^= (y << 6u) & 0x100u;
			offset ^= (y << 8u) & 0x800u;
			offset ^= (z << 4u) & 0x10u;
			offset ^= (z << 6u) & 0x80u;
			offset ^= (z << 8u) & 0x400u;
			return offset;
		case 8:
			offset ^= (x << 3u) & 0x8u;
			offset ^= (x << 5u) & 0x40u;
			offset ^= (x << 7u) & 0x200u;
			offset ^= (y << 5u) & 0x20u;
			offset ^= (y << 7u) & 0x100u;
			offset ^= (y << 9u) & 0x800u;
			offset ^= (z << 4u) & 0x10u;
			offset ^= (z << 6u) & 0x80u;
			offset ^= (z << 8u) & 0x400u;
			return offset;
		case 16:
			offset ^= (x << 6u) & 0x40u;
			offset ^= (x << 8u) & 0x200u;
			offset ^= (y << 5u) & 0x20u;
			offset ^= (y << 7u) & 0x100u;
			offset ^= (y << 9u) & 0x800u;
			offset ^= (z << 4u) & 0x10u;
			offset ^= (z << 6u) & 0x80u;
			offset ^= (z << 8u) & 0x400u;
			return offset;
		default: EXIT("unsupported Standard4KB volume element size: %u\n", bytes_per_element);
	}
	return 0;
}

static uint32_t Bit(uint32_t value, uint32_t source, uint32_t destination) {
	return ((value >> source) & 1u) << destination;
}

static uint32_t Gen5Standard64KBVolumeOffsetInBlock(uint32_t x, uint32_t y, uint32_t z,
                                                    uint32_t bytes_per_element) {
	static constexpr uint8_t SOURCES[5][4] = {
	    {4, 4, 4, 5}, {3, 4, 4, 4}, {3, 3, 4, 4}, {3, 3, 3, 4}, {2, 3, 3, 3}};
	const auto* bits = SOURCES[std::countr_zero(bytes_per_element)];
	return Gen5Standard4KBVolumeOffsetInBlock(x, y, z, bytes_per_element) ^ Bit(x, bits[0], 12) ^
	       Bit(z, bits[1], 13) ^ Bit(y, bits[2], 14) ^ Bit(x, bits[3], 15);
}

bool TileIsStandard4KBTextureSupported(uint32_t format) {
	uint32_t bytes_per_element       = 0;
	uint32_t texels_per_element_wide = 0;
	uint32_t texels_per_element_tall = 0;
	uint32_t block_width_log2        = 0;
	uint32_t block_height_log2       = 0;

	return Gen5Standard4KBLayout(format, &bytes_per_element, &texels_per_element_wide,
	                             &texels_per_element_tall, &block_width_log2, &block_height_log2);
}

bool TileIsStandard256BTextureSupported(uint32_t format) {
	uint32_t bytes_per_element       = 0;
	uint32_t texels_per_element_wide = 0;
	uint32_t texels_per_element_tall = 0;
	uint32_t block_width_log2        = 0;
	uint32_t block_height_log2       = 0;

	return Gen5Standard256BLayout(format, &bytes_per_element, &texels_per_element_wide,
	                              &texels_per_element_tall, &block_width_log2, &block_height_log2);
}

bool TileIsStandard64KBTextureSupported(uint32_t format) {
	uint32_t bytes_per_element       = 0;
	uint32_t texels_per_element_wide = 0;
	uint32_t texels_per_element_tall = 0;
	uint32_t block_width_log2        = 0;
	uint32_t block_height_log2       = 0;

	return Gen5Standard64KBLayout(format, &bytes_per_element, &texels_per_element_wide,
	                              &texels_per_element_tall, &block_width_log2, &block_height_log2);
}

struct Uint128 {
	uint64_t n[2];
};

static uint32_t Gen5Standard256BOffsetInBlock(uint32_t x, uint32_t y, uint32_t bytes_per_element) {
	return Gen5Standard4KBOffsetInBlock(x, y, bytes_per_element) & 0xffu;
}

static constexpr uint32_t GetStandard64KB32XPart(uint32_t x) {
	// Standard64KB is block-linear: 32bpp surfaces use 128x128-element
	// 64KB blocks, with fixed x/y bit interleaving inside each block.
	uint32_t element_offset = 0;
	element_offset ^= (x << 2u) & 0x0cu;
	element_offset ^= (x << 5u) & 0x80u;
	element_offset ^= (x << 6u) & 0x200u;
	element_offset ^= (x << 7u) & 0x800u;
	element_offset ^= (x << 8u) & 0x2000u;
	// X6 selects the upper 32KB of each 128x128x32bpp block.
	element_offset ^= (x << 9u) & 0x8000u;

	return element_offset;
}

static constexpr uint32_t GetStandard64KB32YPart(uint32_t y) {
	uint32_t element_offset = 0;
	element_offset ^= (y << 4u) & 0x70u;
	element_offset ^= (y << 5u) & 0x100u;
	element_offset ^= (y << 6u) & 0x400u;
	element_offset ^= (y << 7u) & 0x1000u;
	element_offset ^= (y << 8u) & 0x4000u;

	return element_offset;
}

struct Standard64KB32Tables {
	std::array<uint32_t, 128> x_words {};
	std::array<uint32_t, 128> y_words {};

	constexpr Standard64KB32Tables() {
		for (uint32_t i = 0; i < 128; i++) {
			uint32_t x_part = 0;
			x_part ^= (i << 2u) & 0x0cu;
			x_part ^= (i << 5u) & 0x80u;
			x_part ^= (i << 6u) & 0x200u;
			x_part ^= (i << 7u) & 0x800u;
			x_part ^= (i << 8u) & 0x2000u;
			x_part ^= (i << 9u) & 0x8000u;

			uint32_t y_part = 0;
			y_part ^= (i << 4u) & 0x70u;
			y_part ^= (i << 5u) & 0x100u;
			y_part ^= (i << 6u) & 0x400u;
			y_part ^= (i << 7u) & 0x1000u;
			y_part ^= (i << 8u) & 0x4000u;

			x_words[i] = x_part >> 2u;
			y_words[i] = y_part >> 2u;
		}
	}
};

static_assert(GetStandard64KB32XPart(127) < (64u * 1024u));
static_assert(GetStandard64KB32YPart(127) < (64u * 1024u));

static const Standard64KB32Tables& GetStandard64KB32Tables() {
	static constexpr Standard64KB32Tables tables;
	return tables;
}

struct Standard64KB16Tables {
	std::array<uint32_t, 256> x_words {};
	std::array<uint32_t, 128> y_words {};

	constexpr Standard64KB16Tables() {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t x_part = 0;
			x_part ^= (i << 1u) & 0x000eu;
			x_part ^= (i << 4u) & 0x0080u;
			x_part ^= (i << 5u) & 0x0200u;
			x_part ^= (i << 6u) & 0x0800u;
			x_part ^= (i << 7u) & 0x2000u;
			x_part ^= (i << 8u) & 0x8000u;

			x_words[i] = x_part >> 1u;
		}

		for (uint32_t i = 0; i < 128; i++) {
			uint32_t y_part = 0;
			y_part ^= (i << 4u) & 0x0070u;
			y_part ^= (i << 5u) & 0x0100u;
			y_part ^= (i << 6u) & 0x0400u;
			y_part ^= (i << 7u) & 0x1000u;
			y_part ^= (i << 8u) & 0x4000u;

			y_words[i] = y_part >> 1u;
		}
	}
};

template <typename T>
static uint32_t Gen5RenderTargetOffsetInBlock(uint32_t x, uint32_t y) {
	uint32_t offset = 0;

	if constexpr (sizeof(T) == 1) {
		offset ^= (y << 2u) & 0x0008u;
		offset ^= (y << 4u) & 0x0010u;
		offset ^= (y << 3u) & 0x00a0u;
		offset ^= (y << 5u) & 0x0f00u;
		offset ^= (y << 6u) & 0x1000u;
		offset ^= (y << 7u) & 0x4000u;

		offset ^= x & 0x0007u;
		offset ^= (x << 3u) & 0x0040u;
		offset ^= (x << 5u) & 0x0300u;
		offset ^= (x << 4u) & 0x0400u;
		offset ^= (x << 6u) & 0x0800u;
		offset ^= (x << 7u) & 0x2000u;
		offset ^= (x << 8u) & 0x8000u;
	} else if constexpr (sizeof(T) == 2) {
		offset ^= (y << 4u) & 0x0070u;
		offset ^= (y << 5u) & 0x0f00u;
		offset ^= (y << 8u) & 0x5000u;

		offset ^= (x << 1u) & 0x000eu;
		offset ^= (x << 4u) & 0x0480u;
		offset ^= (x << 5u) & 0x0300u;
		offset ^= (x << 6u) & 0x0800u;
		offset ^= (x << 7u) & 0x2000u;
		offset ^= (x << 8u) & 0x8000u;
	} else if constexpr (sizeof(T) == 4) {
		offset ^= (y << 4u) & 0x0070u;
		offset ^= (y << 5u) & 0x0f00u;
		offset ^= (y << 9u) & 0x1000u;
		offset ^= (y << 8u) & 0x4000u;

		offset ^= (x << 2u) & 0x000cu;
		offset ^= (x << 5u) & 0x0380u;
		offset ^= (x << 4u) & 0x0400u;
		offset ^= (x << 6u) & 0x0800u;
		offset ^= (x << 9u) & 0xa000u;
	} else if constexpr (sizeof(T) == 8) {
		offset ^= (y << 4u) & 0x0010u;
		offset ^= (y << 6u) & 0x0080u;
		offset ^= (y << 5u) & 0x0f00u;
		offset ^= (y << 10u) & 0x5000u;

		offset ^= (x << 3u) & 0x0008u;
		offset ^= (x << 4u) & 0x0460u;
		offset ^= (x << 5u) & 0x0300u;
		offset ^= (x << 6u) & 0x0800u;
		offset ^= (x << 10u) & 0x2000u;
		offset ^= (x << 9u) & 0x8000u;
	} else if constexpr (sizeof(T) == 16) {
		offset ^= (x << 4u) & 0x0410u;
		offset ^= (x << 5u) & 0x0340u;
		offset ^= (x << 6u) & 0x0800u;
		offset ^= (x << 11u) & 0xa000u;

		offset ^= (y << 5u) & 0x0f20u;
		offset ^= (y << 6u) & 0x0080u;
		offset ^= (y << 10u) & 0x1000u;
		offset ^= (y << 11u) & 0x4000u;
	} else {
		EXIT("unsupported render-target element size: %u\n", static_cast<uint32_t>(sizeof(T)));
	}

	return offset;
}

static const Standard64KB16Tables& GetStandard64KB16Tables() {
	static constexpr Standard64KB16Tables tables;
	return tables;
}

struct Standard64KB8Tables {
	std::array<uint32_t, 256> x_words {};
	std::array<uint32_t, 256> y_words {};

	constexpr Standard64KB8Tables() {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t x_part = 0;
			x_part ^= i & 0x000fu;
			x_part ^= (i << 5u) & 0x0200u;
			x_part ^= (i << 6u) & 0x0800u;
			x_part ^= (i << 7u) & 0x2000u;
			x_part ^= (i << 8u) & 0x8000u;

			uint32_t y_part = 0;
			y_part ^= (i << 4u) & 0x01f0u;
			y_part ^= (i << 5u) & 0x0400u;
			y_part ^= (i << 6u) & 0x1000u;
			y_part ^= (i << 7u) & 0x4000u;

			x_words[i] = x_part;
			y_words[i] = y_part;
		}
	}
};

static const Standard64KB8Tables& GetStandard64KB8Tables() {
	static constexpr Standard64KB8Tables tables;
	return tables;
}

struct Standard64KB64Tables {
	std::array<uint32_t, 128> x_words {};
	std::array<uint32_t, 64>  y_words {};

	constexpr Standard64KB64Tables() {
		for (uint32_t i = 0; i < 128; i++) {
			uint32_t x_part = 0;
			x_part ^= (i << 3u) & 0x0008u;
			x_part ^= (i << 5u) & 0x00c0u;
			x_part ^= (i << 6u) & 0x0200u;
			x_part ^= (i << 7u) & 0x0800u;
			x_part ^= (i << 8u) & 0x2000u;
			x_part ^= (i << 9u) & 0x8000u;

			x_words[i] = x_part >> 3u;
		}

		for (uint32_t i = 0; i < 64; i++) {
			uint32_t y_part = 0;
			y_part ^= (i << 4u) & 0x0030u;
			y_part ^= (i << 6u) & 0x0100u;
			y_part ^= (i << 7u) & 0x0400u;
			y_part ^= (i << 8u) & 0x1000u;
			y_part ^= (i << 9u) & 0x4000u;

			y_words[i] = y_part >> 3u;
		}
	}
};

static const Standard64KB64Tables& GetStandard64KB64Tables() {
	static constexpr Standard64KB64Tables tables;
	return tables;
}

struct Standard64KB128Tables {
	std::array<uint32_t, 64> x_words {};
	std::array<uint32_t, 64> y_words {};

	constexpr Standard64KB128Tables() {
		for (uint32_t i = 0; i < 64; i++) {
			uint32_t x_part = 0;
			x_part ^= (i << 6u) & 0x00c0u;
			x_part ^= (i << 7u) & 0x0200u;
			x_part ^= (i << 8u) & 0x0800u;
			x_part ^= (i << 9u) & 0x2000u;
			x_part ^= (i << 10u) & 0x8000u;

			uint32_t y_part = 0;
			y_part ^= (i << 4u) & 0x0030u;
			y_part ^= (i << 6u) & 0x0100u;
			y_part ^= (i << 7u) & 0x0400u;
			y_part ^= (i << 8u) & 0x1000u;
			y_part ^= (i << 9u) & 0x4000u;

			x_words[i] = x_part >> 4u;
			y_words[i] = y_part >> 4u;
		}
	}
};

static const Standard64KB128Tables& GetStandard64KB128Tables() {
	static constexpr Standard64KB128Tables tables;
	return tables;
}

static constexpr uint32_t Depth64KB8XOffsetBytes(uint32_t x);
static constexpr uint32_t Depth64KB8YOffsetBytes(uint32_t y);
static constexpr uint32_t Depth64KB16XOffsetBytes(uint32_t x);
static constexpr uint32_t Depth64KB16YOffsetBytes(uint32_t y);
static constexpr uint32_t Depth64KB32XOffsetBytes(uint32_t x);
static constexpr uint32_t Depth64KB32YOffsetBytes(uint32_t y);
static constexpr uint32_t Depth64KB64XOffsetBytes(uint32_t x);
static constexpr uint32_t Depth64KB64YOffsetBytes(uint32_t y);

bool TileGetBlockLayout(TileBlockFamily family, uint32_t bytes_per_element,
                        TileBlockLayout& layout) {
	if (!std::has_single_bit(bytes_per_element) || bytes_per_element > 16) {
		return false;
	}

	TileBlockLayout result {family, bytes_per_element, 0, 0, 0, 1};
	auto            thin = [&] {
		return Gen5Thin64KBBlockSizeFromElementBytes(bytes_per_element, &result.block_width,
		                                             &result.block_height);
	};
	auto thick64 = [&] {
		static constexpr uint8_t LOG2_DIMS[5][3] = {
		    {6, 5, 5}, {5, 5, 5}, {5, 5, 4}, {5, 4, 4}, {4, 4, 4}};
		const auto index    = std::countr_zero(bytes_per_element);
		result.block_width  = 1u << LOG2_DIMS[index][0];
		result.block_height = 1u << LOG2_DIMS[index][1];
		result.block_depth  = 1u << LOG2_DIMS[index][2];
	};
	switch (family) {
		case TileBlockFamily::Standard256B:
			result.block_size = 256;
			if (bytes_per_element <= 2) {
				result.block_width = 16;
			} else if (bytes_per_element <= 8) {
				result.block_width = 8;
			} else {
				result.block_width = 4;
			}
			result.block_height = result.block_size / (result.block_width * bytes_per_element);
			break;
		case TileBlockFamily::Standard4KB:
			result.block_size = 4096;
			if (bytes_per_element <= 2) {
				result.block_width = 64;
			} else if (bytes_per_element <= 8) {
				result.block_width = 32;
			} else {
				result.block_width = 16;
			}
			result.block_height = result.block_size / (result.block_width * bytes_per_element);
			break;
		case TileBlockFamily::Standard4KB3D: {
			static constexpr uint8_t LOG2_DIMS[5][3] = {
			    {4, 4, 4}, {3, 4, 4}, {3, 4, 3}, {3, 3, 3}, {2, 3, 3}};
			const auto index    = std::countr_zero(bytes_per_element);
			result.block_size   = 4096;
			result.block_width  = 1u << LOG2_DIMS[index][0];
			result.block_height = 1u << LOG2_DIMS[index][1];
			result.block_depth  = 1u << LOG2_DIMS[index][2];
			break;
		}
		case TileBlockFamily::Standard64KB:
		case TileBlockFamily::Prt64KB:
			result.block_size = 65536;
			if (!thin()) {
				return false;
			}
			break;
		case TileBlockFamily::Standard64KB3D:
		case TileBlockFamily::Prt64KB3D:
			result.block_size = 65536;
			thick64();
			break;
		case TileBlockFamily::RenderTarget64KB:
			result.block_size = 65536;
			if (!thin()) {
				return false;
			}
			break;
		case TileBlockFamily::Depth64KB:
			if (bytes_per_element > 8) {
				return false;
			}
			result.block_size = 65536;
			if (!thin()) {
				return false;
			}
			break;
		case TileBlockFamily::Count: return false;
	}

	if (static_cast<uint64_t>(result.block_width) * result.block_height * result.block_depth *
	        bytes_per_element !=
	    result.block_size) {
		return false;
	}
	layout = result;
	return true;
}

bool TileGetBlockOffset(const TileBlockLayout& layout, uint32_t x, uint32_t y, uint32_t z,
                        uint32_t& byte_offset) {
	TileBlockLayout expected {};
	if (!TileGetBlockLayout(layout.family, layout.bytes_per_element, expected) ||
	    layout.block_size != expected.block_size || layout.block_width != expected.block_width ||
	    layout.block_height != expected.block_height ||
	    layout.block_depth != expected.block_depth || x >= layout.block_width ||
	    y >= layout.block_height || z >= layout.block_depth) {
		return false;
	}

	uint32_t offset = 0;
	switch (layout.family) {
		case TileBlockFamily::Standard256B:
			offset = Gen5Standard256BOffsetInBlock(x, y, layout.bytes_per_element);
			break;
		case TileBlockFamily::Standard4KB:
			offset = Gen5Standard4KBOffsetInBlock(x, y, layout.bytes_per_element);
			break;
		case TileBlockFamily::Standard4KB3D:
			offset = Gen5Standard4KBVolumeOffsetInBlock(x, y, z, layout.bytes_per_element);
			break;
		case TileBlockFamily::Standard64KB3D:
		case TileBlockFamily::Prt64KB3D: {
			offset = Gen5Standard64KBVolumeOffsetInBlock(x, y, z, layout.bytes_per_element);
			if (layout.family == TileBlockFamily::Prt64KB3D) {
				static constexpr uint8_t SOURCES[5][4] = {
				    {4, 5, 4, 4}, {4, 4, 3, 4}, {4, 4, 3, 3}, {3, 4, 3, 3}, {3, 3, 2, 3}};
				const auto* bits = SOURCES[std::countr_zero(layout.bytes_per_element)];
				offset ^= Bit(y, bits[0], 10) ^ Bit(x, bits[1], 10) ^ Bit(x, bits[2], 11) ^
				          Bit(z, bits[3], 11);
			}
			break;
		}
		case TileBlockFamily::Standard64KB:
		case TileBlockFamily::Prt64KB:
			switch (layout.bytes_per_element) {
				case 1:
					offset =
					    GetStandard64KB8Tables().x_words[x] ^ GetStandard64KB8Tables().y_words[y];
					break;
				case 2:
					offset = (GetStandard64KB16Tables().x_words[x] ^
					          GetStandard64KB16Tables().y_words[y]) *
					         2u;
					break;
				case 4:
					offset = (GetStandard64KB32Tables().x_words[x] ^
					          GetStandard64KB32Tables().y_words[y]) *
					         4u;
					break;
				case 8:
					offset = (GetStandard64KB64Tables().x_words[x] ^
					          GetStandard64KB64Tables().y_words[y]) *
					         8u;
					break;
				case 16:
					offset = (GetStandard64KB128Tables().x_words[x] ^
					          GetStandard64KB128Tables().y_words[y]) *
					         16u;
					break;
				default: return false;
			}
			if (layout.family == TileBlockFamily::Prt64KB) {
				static constexpr uint8_t SOURCES[5][4] = {
				    {7, 7, 6, 6}, {7, 6, 6, 5}, {6, 6, 5, 5}, {6, 5, 5, 4}, {5, 5, 4, 4}};
				const auto* bits = SOURCES[std::countr_zero(layout.bytes_per_element)];
				offset ^= Bit(x, bits[0], 8) ^ Bit(y, bits[1], 9) ^ Bit(x, bits[2], 10) ^
				          Bit(y, bits[3], 11);
			}
			break;
		case TileBlockFamily::RenderTarget64KB:
			switch (layout.bytes_per_element) {
				case 1: offset = Gen5RenderTargetOffsetInBlock<uint8_t>(x, y); break;
				case 2: offset = Gen5RenderTargetOffsetInBlock<uint16_t>(x, y); break;
				case 4: offset = Gen5RenderTargetOffsetInBlock<uint32_t>(x, y); break;
				case 8: offset = Gen5RenderTargetOffsetInBlock<uint64_t>(x, y); break;
				case 16: offset = Gen5RenderTargetOffsetInBlock<Uint128>(x, y); break;
				default: return false;
			}
			break;
		case TileBlockFamily::Depth64KB:
			switch (layout.bytes_per_element) {
				case 1: offset = Depth64KB8XOffsetBytes(x) ^ Depth64KB8YOffsetBytes(y); break;
				case 2: offset = Depth64KB16XOffsetBytes(x) ^ Depth64KB16YOffsetBytes(y); break;
				case 4: offset = Depth64KB32XOffsetBytes(x) ^ Depth64KB32YOffsetBytes(y); break;
				case 8: offset = Depth64KB64XOffsetBytes(x) ^ Depth64KB64YOffsetBytes(y); break;
				default: return false;
			}
			break;
		case TileBlockFamily::Count: return false;
	}

	if (offset >= layout.block_size || offset % layout.bytes_per_element != 0) {
		return false;
	}
	byte_offset = offset;
	return true;
}

bool TileGetBlockXor(const TileBlockLayout& layout, uint32_t block_x, uint32_t block_y,
                     uint32_t& byte_offset) {
	return TileGetBlockXor(layout, block_x, block_y, 0, byte_offset);
}

bool TileGetBlockXor(const TileBlockLayout& layout, uint32_t block_x, uint32_t block_y,
                     uint32_t block_z, uint32_t& byte_offset) {
	TileBlockLayout expected {};
	if (!TileGetBlockLayout(layout.family, layout.bytes_per_element, expected) ||
	    layout.block_size != expected.block_size || layout.block_width != expected.block_width ||
	    layout.block_height != expected.block_height ||
	    layout.block_depth != expected.block_depth) {
		return false;
	}
	if (layout.family == TileBlockFamily::Depth64KB && layout.bytes_per_element == 8) {
		if (block_x > UINT32_MAX / layout.block_width ||
		    block_y > UINT32_MAX / layout.block_height) {
			return false;
		}
		byte_offset = Depth64KB64XOffsetBytes(block_x * layout.block_width) ^
		              Depth64KB64YOffsetBytes(block_y * layout.block_height);
	} else if (layout.family == TileBlockFamily::RenderTarget64KB) {
		if (block_x > UINT32_MAX / layout.block_width ||
		    block_y > UINT32_MAX / layout.block_height) {
			return false;
		}
		const auto x = block_x * layout.block_width;
		const auto y = block_y * layout.block_height;
		switch (layout.bytes_per_element) {
			case 1: byte_offset = Gen5RenderTargetOffsetInBlock<uint8_t>(x, y); break;
			case 2: byte_offset = Gen5RenderTargetOffsetInBlock<uint16_t>(x, y); break;
			case 4: byte_offset = Gen5RenderTargetOffsetInBlock<uint32_t>(x, y); break;
			case 8: byte_offset = Gen5RenderTargetOffsetInBlock<uint64_t>(x, y); break;
			case 16: byte_offset = Gen5RenderTargetOffsetInBlock<Uint128>(x, y); break;
			default: return false;
		}
	} else {
		byte_offset = 0;
	}
	if (layout.family == TileBlockFamily::RenderTarget64KB ||
	    layout.family == TileBlockFamily::Depth64KB) {
		byte_offset ^= ((block_z & 8u) << 5u) ^ ((block_z & 4u) << 7u) ^ ((block_z & 2u) << 9u) ^
		               ((block_z & 1u) << 11u);
	}
	return byte_offset < layout.block_size && byte_offset % layout.bytes_per_element == 0;
}

static constexpr uint32_t Depth64KB8XOffsetBytes(uint32_t x) {
	uint32_t offset = 0;
	offset ^= x & 0x0001u;
	offset ^= (x << 1u) & 0x0004u;
	offset ^= (x << 2u) & 0x0010u;
	offset ^= (x << 3u) & 0x0040u;
	offset ^= (x << 5u) & 0x0300u;
	offset ^= (x << 4u) & 0x0400u;
	offset ^= (x << 6u) & 0x0800u;
	offset ^= (x << 7u) & 0x2000u;
	offset ^= (x << 8u) & 0x8000u;
	return offset;
}

static constexpr uint32_t Depth64KB8YOffsetBytes(uint32_t y) {
	uint32_t offset = 0;
	offset ^= (y << 1u) & 0x0002u;
	offset ^= (y << 2u) & 0x0008u;
	offset ^= (y << 3u) & 0x00a0u;
	offset ^= (y << 5u) & 0x0f00u;
	offset ^= (y << 6u) & 0x1000u;
	offset ^= (y << 7u) & 0x4000u;
	return offset;
}

static constexpr uint32_t Depth64KB16XOffsetBytes(uint32_t x) {
	uint32_t offset = 0;
	offset ^= (x << 1u) & 0x0002u;
	offset ^= (x << 2u) & 0x0008u;
	offset ^= (x << 3u) & 0x0020u;
	offset ^= (x << 4u) & 0x0480u;
	offset ^= (x << 5u) & 0x0300u;
	offset ^= (x << 6u) & 0x0800u;
	offset ^= (x << 7u) & 0x2000u;
	offset ^= (x << 8u) & 0x8000u;
	return offset;
}

static constexpr uint32_t Depth64KB16YOffsetBytes(uint32_t y) {
	uint32_t offset = 0;
	offset ^= (y << 2u) & 0x0004u;
	offset ^= (y << 3u) & 0x0010u;
	offset ^= (y << 4u) & 0x0040u;
	offset ^= (y << 5u) & 0x0f00u;
	offset ^= (y << 8u) & 0x5000u;
	return offset;
}

static constexpr uint32_t Depth64KB32XOffsetBytes(uint32_t x) {
	uint32_t offset = 0;
	offset ^= (x << 2u) & 0x0004u;
	offset ^= (x << 3u) & 0x0010u;
	offset ^= (x << 4u) & 0x0440u;
	offset ^= (x << 5u) & 0x0300u;
	offset ^= (x << 6u) & 0x0800u;
	offset ^= (x << 9u) & 0xa000u;
	return offset;
}

static constexpr uint32_t Depth64KB32YOffsetBytes(uint32_t y) {
	uint32_t offset = 0;
	offset ^= (y << 3u) & 0x0008u;
	offset ^= (y << 4u) & 0x0020u;
	offset ^= (y << 5u) & 0x0f80u;
	offset ^= (y << 9u) & 0x1000u;
	offset ^= (y << 8u) & 0x4000u;
	return offset;
}

static constexpr uint32_t Depth64KB64XOffsetBytes(uint32_t x) {
	return ((x << 3u) & 0x0008u) ^ ((x << 4u) & 0x0420u) ^ ((x << 5u) & 0x0380u) ^
	       ((x << 6u) & 0x0800u) ^ ((x << 10u) & 0x2000u) ^ ((x << 9u) & 0x8000u);
}

static constexpr uint32_t Depth64KB64YOffsetBytes(uint32_t y) {
	return ((y << 4u) & 0x0010u) ^ ((y << 5u) & 0x0f40u) ^ ((y << 10u) & 0x5000u);
}

static_assert(Depth64KB8XOffsetBytes(2) == 0x0004u);
static_assert(Depth64KB8YOffsetBytes(1) == 0x0002u);
static_assert((Depth64KB8XOffsetBytes(3) ^ Depth64KB8YOffsetBytes(5)) == 0x0027u);
static_assert(Depth64KB16XOffsetBytes(2) == 0x0008u);
static_assert(Depth64KB16YOffsetBytes(1) == 0x0004u);
static_assert((Depth64KB16XOffsetBytes(3) ^ Depth64KB16YOffsetBytes(5)) == 0x004eu);
static_assert(Depth64KB32XOffsetBytes(2) == 0x0010u);
static_assert(Depth64KB32YOffsetBytes(1) == 0x0008u);
static_assert((Depth64KB32XOffsetBytes(3) ^ Depth64KB32YOffsetBytes(5)) == 0x009cu);

bool TileGetHtileSize(uint32_t width, uint32_t height, TileSizeAlign& htile_size) {
	htile_size = {};
	if (width == 0 || width > 16384 || height == 0 || height > 16384) {
		return false;
	}
	// Prospero HTile stores one DWORD per depth tile. Its 32 KiB allocation blocks cover
	// 1024x512 pixels, independently of the attachment's fragment count.
	const uint64_t size = static_cast<uint64_t>(AlignUp(width, 1024u) / 1024u) *
	                      (AlignUp(height, 512u) / 512u) * 32768u;
	if (size == 0 || size > UINT32_MAX) {
		return false;
	}
	htile_size = {static_cast<uint32_t>(size), 32768};
	return true;
}

bool TileGetDepthSize(uint32_t width, uint32_t height, uint32_t pitch, uint32_t z_format,
                      uint32_t stencil_format, bool htile, TileSizeAlign& stencil_size,
                      TileSizeAlign& htile_size, TileSizeAlign& depth_size,
                      uint32_t num_fragments_log2) {
	EXIT_IF(pitch != 0);
	// Prospero derives uncompressed depth/stencil as independent 64 KiB block surfaces.
	if (width > 0 && width <= 16384 && height > 0 && height <= 16384 &&
	    (z_format == 1 || z_format == 3) && stencil_format <= 1 && num_fragments_log2 <= 3) {
		const uint32_t depth_bytes          = z_format == 1 ? 2u : 4u;
		uint32_t       depth_block_width    = 0;
		uint32_t       depth_block_height   = 0;
		uint32_t       stencil_block_width  = 0;
		uint32_t       stencil_block_height = 0;
		const bool     valid_blocks =
		    Gen5Msaa64KBBlockSizeFromElementBytes(depth_bytes, num_fragments_log2,
		                                          &depth_block_width, &depth_block_height) &&
		    (stencil_format == 0 ||
		     Gen5Msaa64KBBlockSizeFromElementBytes(1, num_fragments_log2, &stencil_block_width,
		                                           &stencil_block_height));
		const uint32_t fragments = 1u << num_fragments_log2;
		const uint64_t depth_bytes_total =
		    valid_blocks ? static_cast<uint64_t>(AlignUp(width, depth_block_width)) *
		                       AlignUp(height, depth_block_height) * depth_bytes * fragments
		                 : 0;
		const uint64_t stencil_bytes_total =
		    stencil_format == 1 && valid_blocks
		        ? static_cast<uint64_t>(AlignUp(width, stencil_block_width)) *
		              AlignUp(height, stencil_block_height) * fragments
		        : 0;
		TileSizeAlign calculated_htile {};
		const bool    htile_valid = !htile || TileGetHtileSize(width, height, calculated_htile);
		if (depth_bytes_total <= UINT32_MAX && stencil_bytes_total <= UINT32_MAX && htile_valid) {
			depth_size   = {static_cast<uint32_t>(depth_bytes_total), 65536};
			stencil_size = stencil_format == 1
			                   ? TileSizeAlign {static_cast<uint32_t>(stencil_bytes_total), 65536}
			                   : TileSizeAlign {};
			htile_size   = calculated_htile;
			return true;
		}
	}
	depth_size   = TileSizeAlign();
	htile_size   = TileSizeAlign();
	stencil_size = TileSizeAlign();
	return false;
}

uint32_t TileGetRenderTargetPitch(uint32_t width, uint32_t bytes_per_element,
                                  uint32_t num_fragments_log2) {
	uint32_t block_width  = 0;
	uint32_t block_height = 0;
	if (width == 0 || !Gen5Msaa64KBBlockSizeFromElementBytes(bytes_per_element, num_fragments_log2,
	                                                         &block_width, &block_height)) {
		return 0;
	}
	const uint64_t pitch = (static_cast<uint64_t>(width) + block_width - 1u) &
	                       ~static_cast<uint64_t>(block_width - 1u);
	return pitch <= UINT32_MAX ? static_cast<uint32_t>(pitch) : 0;
}

uint32_t TileGetDepthPitch(uint32_t width, uint32_t bytes_per_element,
                           uint32_t num_fragments_log2) {
	return TileGetRenderTargetPitch(width, bytes_per_element, num_fragments_log2);
}

bool TileGetRenderTargetSize(uint32_t width, uint32_t height, uint32_t pitch,
                             uint32_t bytes_per_element, TileSizeAlign& total_size,
                             uint32_t num_fragments_log2) {
	total_size            = {};
	uint32_t block_width  = 0;
	uint32_t block_height = 0;
	if (height == 0 || pitch == 0 ||
	    !Gen5Msaa64KBBlockSizeFromElementBytes(bytes_per_element, num_fragments_log2, &block_width,
	                                           &block_height) ||
	    pitch != TileGetRenderTargetPitch(width, bytes_per_element, num_fragments_log2)) {
		return false;
	}
	const uint64_t padded_height = (static_cast<uint64_t>(height) + block_height - 1u) &
	                               ~static_cast<uint64_t>(block_height - 1u);
	const uint64_t size = static_cast<uint64_t>(pitch) * padded_height * bytes_per_element *
	                      (1u << num_fragments_log2);
	if (size == 0 || size > UINT32_MAX) {
		return false;
	}
	total_size.size  = static_cast<uint32_t>(size);
	total_size.align = 65536;
	return true;
}

bool TileGetRenderTargetMipLayout(uint32_t width, uint32_t height, uint32_t pitch,
                                  uint32_t bytes_per_element, uint32_t levels,
                                  TileSizeAlign& total_size, TileSizeOffset* level_sizes,
                                  TilePaddedSize* padded_size) {
	total_size = {};
	if (width == 0 || height == 0 || levels == 0 || levels > 16 ||
	    pitch != TileGetRenderTargetPitch(width, bytes_per_element)) {
		return false;
	}
	uint32_t max_levels    = 1;
	uint32_t max_dimension = std::max(width, height);
	while (max_dimension > 1) {
		max_dimension >>= 1u;
		max_levels++;
	}
	if (levels > max_levels) {
		return false;
	}
	uint32_t format = 0;
	switch (bytes_per_element) {
		case 1: format = Prospero::GpuEnumValue(Prospero::BufferFormat::k8UNorm); break;
		case 2: format = Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm); break;
		case 4: format = Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float); break;
		case 8: format = Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float); break;
		case 16: format = Prospero::GpuEnumValue(Prospero::BufferFormat::k32_32_32_32Float); break;
		default: return false;
	}
	TileGetTextureSize(format, width, height, pitch, levels,
	                   Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget), &total_size,
	                   level_sizes, padded_size);
	return total_size.size != 0 && total_size.align == 65536;
}

void TileGetTextureSize(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                        uint32_t levels, uint32_t tile, TileSizeAlign* total_size,
                        TileSizeOffset* level_sizes, TilePaddedSize* padded_size) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(levels == 0 || levels > 16);

	if (const uint32_t bytes_per_element = Prospero::NumBytesPerElement(format);
	    bytes_per_element != 0 && tile == 0) {
		uint32_t mip_pitch[16] {};
		uint32_t mip_height[16] {};
		uint32_t mip_size[16] {};

		for (uint32_t l = 0; l < levels; l++) {
			mip_pitch[l] = CalcLinearAlignedLevelPitch(width, height, l, bytes_per_element,
			                                           &mip_height[l], &mip_size[l]);
		}

		const uint32_t total = SetLinearMipChainLayout(levels, mip_pitch, mip_height, mip_size,
		                                               level_sizes, padded_size);

		if (total_size != nullptr) {
			total_size->size  = total;
			total_size->align = 256;
		}

		return;
	}

	TextureBlockLayout block {};
	if (GetTextureBlockLayout(format, tile, block)) {
		if (tile == 1) {
			SetMicroMipLayout(block, width, height, levels, total_size, level_sizes, padded_size);
		} else {
			SetMacroMipLayout(block, tile, width, height, levels, total_size, level_sizes,
			                  padded_size);
		}
		return;
	}

	if (auto bytes_per_block = Prospero::BlockCompressedBytesPerBlock(format);
	    bytes_per_block != 0 && tile == 0) {
		uint32_t mip_pitch[16] {};
		uint32_t mip_height[16] {};
		uint32_t mip_size[16] {};

		const uint32_t blocks_w0 = std::max((width + 3u) / 4u, 1u);
		const uint32_t blocks_h0 = std::max((height + 3u) / 4u, 1u);
		for (uint32_t l = 0; l < levels; l++) {
			uint32_t       padded_blocks_h  = 0;
			const uint32_t aligned_blocks_w = CalcLinearAlignedLevelPitch(
			    blocks_w0, blocks_h0, l, bytes_per_block, &padded_blocks_h, &mip_size[l]);

			mip_pitch[l]  = std::max(aligned_blocks_w * 4u, 32u);
			mip_height[l] = std::max(padded_blocks_h * 4u, 32u);
		}

		const uint32_t total = SetLinearMipChainLayout(levels, mip_pitch, mip_height, mip_size,
		                                               level_sizes, padded_size);

		if (total_size != nullptr) {
			total_size->size  = total;
			total_size->align = 256;
		}

		return;
	}
	if (total_size != nullptr && total_size->size == 0) {
		std::vector<std::string> list;
		list.push_back(fmt::format("format = {}", format));
		list.push_back(fmt::format("width  = {}", width));
		list.push_back(fmt::format("height = {}", height));
		list.push_back(fmt::format("pitch  = {}", pitch));
		list.push_back(fmt::format("levels = {}", levels));
		list.push_back(fmt::format("tile   = {}", tile));
		EXIT("unknown format:\n%s\n", Common::Concat(list, '\n').c_str());
	}
}

void TileGetTextureTotalSize(uint32_t format, uint32_t width, uint32_t height, uint32_t depth,
                             uint32_t pitch, uint32_t levels, uint32_t tile, bool volume_texture,
                             TileSizeAlign& total_size) {
	EXIT_NOT_IMPLEMENTED(depth == 0);
	if (volume_texture) {
		TileVolumeLayout volume {};
		if (TileGetTextureVolumeLayout(format, width, height, depth, levels, tile, volume)) {
			EXIT_NOT_IMPLEMENTED(volume.total_size > UINT32_MAX);
			total_size.size  = static_cast<uint32_t>(volume.total_size);
			total_size.align = tile == 5 ? 4096u : 65536u;
			return;
		}
	}

	TileSizeAlign slice_size {};
	TileGetTextureSize(format, width, height, pitch, levels, tile, &slice_size, nullptr, nullptr);
	total_size           = slice_size;
	const uint64_t total = static_cast<uint64_t>(slice_size.size) * depth;
	EXIT_NOT_IMPLEMENTED(total > 0xffffffffull);
	total_size.size = static_cast<uint32_t>(total);
}

uint32_t TileGetTexturePitch(uint32_t format, uint32_t width, uint32_t levels, uint32_t tile) {
	uint32_t pitch = width;

	if (tile == 27) {
		if (const uint32_t bytes_per_element = Prospero::NumBytesPerElement(format);
		    bytes_per_element != 0) {
			uint32_t block_width  = 0;
			uint32_t block_height = 0;
			EXIT_NOT_IMPLEMENTED(!Gen5Thin64KBBlockSizeFromElementBytes(
			    bytes_per_element, &block_width, &block_height));
			pitch = AlignUp(pitch, block_width);
		}
	}

	if (tile == 0) {
		if (const auto bytes_per_element = Prospero::NumBytesPerElement(format);
		    bytes_per_element != 0) {
			pitch = AlignUp(pitch * bytes_per_element, 256u) / bytes_per_element;
		}
	}
	uint32_t bytes_per_element       = 0;
	uint32_t texels_per_element_wide = 0;
	uint32_t texels_per_element_tall = 0;
	uint32_t block_width_log2        = 0;
	uint32_t block_height_log2       = 0;
	if ((tile == 9 || tile == 17) &&
	    Gen5Standard64KBLayout(format, &bytes_per_element, &texels_per_element_wide,
	                           &texels_per_element_tall, &block_width_log2, &block_height_log2)) {
		pitch = AlignUp(pitch, (1u << block_width_log2) * texels_per_element_wide);
	}
	if (tile == 24) {
		const uint32_t bytes_per_element = Prospero::NumBytesPerElement(format);
		uint32_t       block_width       = 0;
		uint32_t       block_height      = 0;
		if (bytes_per_element <= 8 &&
		    Gen5Thin64KBBlockSizeFromElementBytes(bytes_per_element, &block_width, &block_height)) {
			pitch = AlignUp(pitch, block_width);
		}
	}

	return pitch;
}

} // namespace Libs::Graphics
