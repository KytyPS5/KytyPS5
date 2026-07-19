#include "graphics/guest_gpu/tile.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/asyncJob.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fmt/format.h>
#include <vector>

namespace Libs::Graphics {

static uint32_t IntLog2(uint32_t i) {
	return 31 - __builtin_clz(i | 1u);
}

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
	// AGC linear surfaces store smaller mip records first; mip 0 is last in the block slice.
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

bool TileGetStandard4KBVolumeLayout(uint32_t format, uint32_t* bytes_per_element,
                                    uint32_t* texels_per_element_wide,
                                    uint32_t* texels_per_element_tall, uint32_t* block_width_log2,
                                    uint32_t* block_height_log2, uint32_t* block_depth_log2) {
	if (!Gen5Standard4KBLayout(format, bytes_per_element, texels_per_element_wide,
	                           texels_per_element_tall, block_width_log2, block_height_log2)) {
		return false;
	}

	switch (*bytes_per_element) {
		case 1:
			*block_width_log2  = 4;
			*block_height_log2 = 4;
			*block_depth_log2  = 4;
			return true;
		case 2:
			*block_width_log2  = 3;
			*block_height_log2 = 4;
			*block_depth_log2  = 4;
			return true;
		case 4:
			*block_width_log2  = 3;
			*block_height_log2 = 4;
			*block_depth_log2  = 3;
			return true;
		case 8:
			*block_width_log2  = 3;
			*block_height_log2 = 3;
			*block_depth_log2  = 3;
			return true;
		case 16:
			*block_width_log2  = 2;
			*block_height_log2 = 3;
			*block_depth_log2  = 3;
			return true;
		default: break;
	}

	return false;
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
	// AGC thin 64 KiB block table, shared by depth and render-target tiles.
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

struct Uint256 {
	Uint128 n[2];
};

template <typename T>
static void DetileStandard4KBTyped(T* dst, const T* src, uint32_t row_elements,
                                   uint32_t width_elements, uint32_t height_elements,
                                   uint32_t padded_width, uint32_t block_width_log2,
                                   uint32_t block_height_log2, uint64_t dst_size, uint64_t src_size,
                                   uint32_t src_x, uint32_t src_y) {
	constexpr auto bytes_per_element = static_cast<uint32_t>(sizeof(T));

	std::array<uint32_t, 64> x_elements {};
	std::array<uint32_t, 64> y_elements {};

	const uint32_t block_width        = 1u << block_width_log2;
	const uint32_t block_height       = 1u << block_height_log2;
	const uint32_t elements_per_block = 4096u / bytes_per_element;
	const uint32_t blocks_per_row     = padded_width >> block_width_log2;

	for (uint32_t x = 0; x < block_width; x++) {
		x_elements[x] = Gen5Standard4KBOffsetInBlock(x, 0, bytes_per_element) / bytes_per_element;
	}

	for (uint32_t y = 0; y < block_height; y++) {
		y_elements[y] = Gen5Standard4KBOffsetInBlock(0, y, bytes_per_element) / bytes_per_element;
	}

	const uint64_t dst_count = dst_size / bytes_per_element;
	const uint64_t src_count = src_size / bytes_per_element;

	for (uint32_t block_y = 0; block_y < height_elements; block_y += block_height) {
		const uint32_t copy_height = std::min(block_height, height_elements - block_y);

		for (uint32_t block_x = 0; block_x < width_elements; block_x += block_width) {
			const uint32_t copy_width = std::min(block_width, width_elements - block_x);
			const uint64_t block_index =
			    (static_cast<uint64_t>(block_y >> block_height_log2) * blocks_per_row) +
			    (block_x >> block_width_log2);
			const uint64_t block_base = block_index * elements_per_block;

			for (uint32_t local_y = 0; local_y < copy_height; local_y++) {
				const uint64_t dst_row =
				    (static_cast<uint64_t>(block_y + local_y) * row_elements) + block_x;
				const uint64_t src_row = block_base + y_elements[src_y + local_y];

				for (uint32_t local_x = 0; local_x < copy_width; local_x++) {
					const uint64_t dst_index = dst_row + local_x;
					const uint64_t src_index = src_row + x_elements[src_x + local_x];

					if (src_index < src_count && dst_index < dst_count) {
						dst[dst_index] = src[src_index];
					}
				}
			}
		}
	}
}

template <typename T>
static void DetileStandard4KBVolumeTyped(T* dst, const T* src, uint32_t row_elements,
                                         uint32_t width_elements, uint32_t height_elements,
                                         uint32_t depth_elements, uint32_t padded_width,
                                         uint32_t padded_height, uint32_t block_width_log2,
                                         uint32_t block_height_log2, uint32_t block_depth_log2,
                                         uint64_t dst_slice_stride, uint64_t dst_size,
                                         uint64_t src_size) {
	constexpr auto bytes_per_element = static_cast<uint32_t>(sizeof(T));

	const uint32_t block_width        = 1u << block_width_log2;
	const uint32_t block_height       = 1u << block_height_log2;
	const uint32_t block_depth        = 1u << block_depth_log2;
	const uint32_t elements_per_block = 4096u / bytes_per_element;
	const uint32_t blocks_per_row     = padded_width >> block_width_log2;
	const uint32_t blocks_per_column  = padded_height >> block_height_log2;
	const uint64_t blocks_per_slice =
	    static_cast<uint64_t>(blocks_per_row) * static_cast<uint64_t>(blocks_per_column);
	const uint64_t dst_count          = dst_size / bytes_per_element;
	const uint64_t src_count          = src_size / bytes_per_element;
	const uint64_t dst_slice_elements = dst_slice_stride / bytes_per_element;

	for (uint32_t block_z = 0; block_z < depth_elements; block_z += block_depth) {
		const uint32_t copy_depth = std::min(block_depth, depth_elements - block_z);

		for (uint32_t block_y = 0; block_y < height_elements; block_y += block_height) {
			const uint32_t copy_height = std::min(block_height, height_elements - block_y);

			for (uint32_t block_x = 0; block_x < width_elements; block_x += block_width) {
				const uint32_t copy_width = std::min(block_width, width_elements - block_x);
				const uint64_t block_index =
				    ((static_cast<uint64_t>(block_z >> block_depth_log2) * blocks_per_slice) +
				     (static_cast<uint64_t>(block_y >> block_height_log2) * blocks_per_row) +
				     (block_x >> block_width_log2));
				const uint64_t block_base = block_index * elements_per_block;

				for (uint32_t local_z = 0; local_z < copy_depth; local_z++) {
					for (uint32_t local_y = 0; local_y < copy_height; local_y++) {
						const uint64_t dst_row =
						    (static_cast<uint64_t>(block_z + local_z) * dst_slice_elements) +
						    (static_cast<uint64_t>(block_y + local_y) * row_elements) + block_x;

						for (uint32_t local_x = 0; local_x < copy_width; local_x++) {
							const uint64_t dst_index = dst_row + local_x;
							const uint64_t src_index =
							    block_base + (Gen5Standard4KBVolumeOffsetInBlock(
							                      local_x, local_y, local_z, bytes_per_element) /
							                  bytes_per_element);

							if (src_index < src_count && dst_index < dst_count) {
								dst[dst_index] = src[src_index];
							}
						}
					}
				}
			}
		}
	}
}

static uint32_t Gen5Standard256BOffsetInBlock(uint32_t x, uint32_t y, uint32_t bytes_per_element) {
	return Gen5Standard4KBOffsetInBlock(x, y, bytes_per_element) & 0xffu;
}

template <typename T>
static void
DetileStandard256BTyped(T* dst, const T* src, uint32_t row_elements, uint32_t width_elements,
                        uint32_t height_elements, uint32_t padded_width, uint32_t block_width_log2,
                        uint32_t block_height_log2, uint64_t dst_size, uint64_t src_size) {
	constexpr auto bytes_per_element = static_cast<uint32_t>(sizeof(T));

	std::array<uint32_t, 16> x_elements {};
	std::array<uint32_t, 16> y_elements {};

	const uint32_t block_width        = 1u << block_width_log2;
	const uint32_t block_height       = 1u << block_height_log2;
	const uint32_t elements_per_block = 256u / bytes_per_element;
	const uint32_t blocks_per_row     = padded_width >> block_width_log2;

	for (uint32_t x = 0; x < block_width; x++) {
		x_elements[x] = Gen5Standard256BOffsetInBlock(x, 0, bytes_per_element) / bytes_per_element;
	}

	for (uint32_t y = 0; y < block_height; y++) {
		y_elements[y] = Gen5Standard256BOffsetInBlock(0, y, bytes_per_element) / bytes_per_element;
	}

	const uint64_t dst_count = dst_size / bytes_per_element;
	const uint64_t src_count = src_size / bytes_per_element;

	for (uint32_t block_y = 0; block_y < height_elements; block_y += block_height) {
		const uint32_t copy_height = std::min(block_height, height_elements - block_y);

		for (uint32_t block_x = 0; block_x < width_elements; block_x += block_width) {
			const uint32_t copy_width = std::min(block_width, width_elements - block_x);
			const uint64_t block_index =
			    (static_cast<uint64_t>(block_y >> block_height_log2) * blocks_per_row) +
			    (block_x >> block_width_log2);
			const uint64_t block_base = block_index * elements_per_block;

			for (uint32_t local_y = 0; local_y < copy_height; local_y++) {
				const uint64_t dst_row =
				    (static_cast<uint64_t>(block_y + local_y) * row_elements) + block_x;
				const uint64_t src_row = block_base + y_elements[local_y];

				for (uint32_t local_x = 0; local_x < copy_width; local_x++) {
					const uint64_t dst_index = dst_row + local_x;
					const uint64_t src_index = src_row + x_elements[local_x];

					if (src_index < src_count && dst_index < dst_count) {
						dst[dst_index] = src[src_index];
					}
				}
			}
		}
	}
}

class Tiler {
public:
	Tiler() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	~Tiler() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(Tiler);

	Common::Mutex m_mutex;

	AsyncJob m_job1;
};

class Tiler32 {
public:
	uint32_t m_macro_tile_height = 0;
	uint32_t m_bank_height       = 0;
	uint32_t m_num_banks         = 0;
	uint32_t m_num_pipes         = 0;
	uint32_t m_padded_width      = 0;
	uint32_t m_padded_height     = 0;
	uint32_t m_pipe_bits         = 0;
	uint32_t m_bank_bits         = 0;

	void Init(uint32_t width, uint32_t height) {
		m_macro_tile_height = 128;
		m_bank_height       = 2;
		m_num_banks         = 8;
		m_num_pipes         = 16;
		m_padded_width      = AlignUp(width, 128);
		m_padded_height     = AlignUp(height, m_macro_tile_height);
		m_pipe_bits         = 4;
		m_bank_bits         = 3;
	}

	static uint32_t GetElementIndex(uint32_t x, uint32_t y) {
		uint32_t elem = 0;
		elem |= ((x >> 0u) & 0x1u) << 0u;
		elem |= ((x >> 1u) & 0x1u) << 1u;
		elem |= ((y >> 0u) & 0x1u) << 2u;
		elem |= ((x >> 2u) & 0x1u) << 3u;
		elem |= ((y >> 1u) & 0x1u) << 4u;
		elem |= ((y >> 2u) & 0x1u) << 5u;

		return elem;
	}

	static uint32_t GetPipeIndex(uint32_t x, uint32_t y) {
		uint32_t pipe = 0;

		pipe |= (((x >> 3u) ^ (y >> 3u) ^ (x >> 4u)) & 0x1u) << 0u;
		pipe |= (((x >> 4u) ^ (y >> 4u)) & 0x1u) << 1u;
		pipe |= (((x >> 5u) ^ (y >> 5u)) & 0x1u) << 2u;
		pipe |= (((x >> 6u) ^ (y >> 5u)) & 0x1u) << 3u;

		return pipe;
	}

	static uint32_t GetBankIndex(uint32_t x, uint32_t y, uint32_t bank_width, uint32_t bank_height,
	                             uint32_t num_banks, uint32_t num_pipes) {
		const uint32_t x_shift_offset = IntLog2(bank_width * num_pipes);
		const uint32_t y_shift_offset = IntLog2(bank_height);
		const uint32_t xs             = x >> x_shift_offset;
		const uint32_t ys             = y >> y_shift_offset;
		uint32_t       bank           = 0;
		switch (num_banks) {
			case 8:
				bank |= (((xs >> 3u) ^ (ys >> 5u)) & 0x1u) << 0u;
				bank |= (((xs >> 4u) ^ (ys >> 4u) ^ (ys >> 5u)) & 0x1u) << 1u;
				bank |= (((xs >> 5u) ^ (ys >> 3u)) & 0x1u) << 2u;
				break;
			case 16:
				bank |= (((xs >> 3u) ^ (ys >> 6u)) & 0x1u) << 0u;
				bank |= (((xs >> 4u) ^ (ys >> 5u) ^ (ys >> 6u)) & 0x1u) << 1u;
				bank |= (((xs >> 5u) ^ (ys >> 4u)) & 0x1u) << 2u;
				bank |= (((xs >> 6u) ^ (ys >> 3u)) & 0x1u) << 3u;
				break;
			default:;
		}

		return bank;
	}

	[[nodiscard]] uint64_t GetTiledOffset(uint32_t x, uint32_t y) const {
		return GetTiledOffset(x, y, 32);
	}

	[[nodiscard]] uint64_t GetTiledOffset(uint32_t x, uint32_t y, uint32_t bits_per_element) const {
		uint64_t element_index = GetElementIndex(x, y);

		uint32_t xh             = x;
		uint32_t yh             = y;
		uint64_t pipe           = GetPipeIndex(xh, yh);
		uint64_t bank           = GetBankIndex(xh, yh, 1, m_bank_height, m_num_banks, m_num_pipes);
		uint32_t tile_bytes     = (8 * 8 * bits_per_element + 7) / 8;
		uint64_t element_offset = (element_index * bits_per_element);
		uint64_t tile_split_slice = 0;

		if (tile_bytes > 512) {
			tile_split_slice = element_offset / (static_cast<uint64_t>(512) * 8);
			element_offset %= (static_cast<uint64_t>(512) * 8);
			tile_bytes = 512;
		}

		uint64_t macro_tile_bytes =
		    (128 / 8) * (m_macro_tile_height / 8) * tile_bytes / (m_num_pipes * m_num_banks);
		uint64_t macro_tiles_per_row     = m_padded_width / 128;
		uint64_t macro_tile_row_index    = y / m_macro_tile_height;
		uint64_t macro_tile_column_index = x / 128;
		uint64_t macro_tile_index =
		    (macro_tile_row_index * macro_tiles_per_row) + macro_tile_column_index;
		uint64_t macro_tile_offset = macro_tile_index * macro_tile_bytes;
		uint64_t macro_tiles_per_slice =
		    macro_tiles_per_row * (m_padded_height / m_macro_tile_height);
		uint64_t slice_bytes    = macro_tiles_per_slice * macro_tile_bytes;
		uint64_t slice_offset   = tile_split_slice * slice_bytes;
		uint64_t tile_row_index = (y / 8) % m_bank_height;
		uint64_t tile_index     = tile_row_index;
		uint64_t tile_offset    = tile_index * tile_bytes;

		uint64_t tile_split_slice_rotation = ((m_num_banks / 2) + 1) * tile_split_slice;
		bank ^= tile_split_slice_rotation;
		bank &= (m_num_banks - 1);

		uint64_t total_offset =
		    (slice_offset + macro_tile_offset + tile_offset) * 8 + element_offset;
		uint64_t bit_offset = total_offset & 0x7u;
		total_offset /= 8;

		uint64_t pipe_interleave_offset = total_offset & 0xffu;
		uint64_t offset                 = total_offset >> 8u;
		uint64_t byte_offset            = pipe_interleave_offset | (pipe << (8u)) |
		                                  (bank << (8u + m_pipe_bits)) |
		                                  (offset << (8u + m_pipe_bits + m_bank_bits));

		return ((byte_offset << 3u) | bit_offset) / 8;
	}
};

struct FastTiler32XInfo {
	uint64_t macro_offset  = 0;
	uint16_t element_bytes = 0;
	uint8_t  pipe          = 0;
	uint8_t  bank          = 0;
};

struct FastTiler32YInfo {
	uint64_t macro_offset  = 0;
	uint16_t tile_offset   = 0;
	uint16_t element_bytes = 0;
	uint8_t  pipe          = 0;
	uint8_t  bank          = 0;
};

class FastTiler32 {
public:
	void Init(const Tiler32& t, uint32_t width, uint32_t height) {
		constexpr uint32_t bytes_per_element = 4;
		const uint32_t     tile_bytes        = 8u * 8u * bytes_per_element;
		const uint64_t macro_tile_bytes = (128u / 8u) * (t.m_macro_tile_height / 8u) * tile_bytes /
		                                  (t.m_num_pipes * t.m_num_banks);
		const uint64_t macro_tiles_per_row = t.m_padded_width / 128u;

		m_x.resize(width);
		m_y.resize(height);
		m_pipe_bits = t.m_pipe_bits;
		m_bank_bits = t.m_bank_bits;

		for (uint32_t x = 0; x < width; x++) {
			auto& info = m_x[x];

			info.macro_offset = (x / 128u) * macro_tile_bytes;
			info.element_bytes =
			    static_cast<uint16_t>(Tiler32::GetElementIndex(x, 0) * bytes_per_element);
			info.pipe = static_cast<uint8_t>(Tiler32::GetPipeIndex(x, 0));
			info.bank = static_cast<uint8_t>(
			    Tiler32::GetBankIndex(x, 0, 1, t.m_bank_height, t.m_num_banks, t.m_num_pipes));
		}

		for (uint32_t y = 0; y < height; y++) {
			auto& info = m_y[y];

			info.macro_offset = (static_cast<uint64_t>(y) / t.m_macro_tile_height) *
			                    macro_tiles_per_row * macro_tile_bytes;
			info.tile_offset  = static_cast<uint16_t>(((y / 8u) % t.m_bank_height) * tile_bytes);
			info.element_bytes =
			    static_cast<uint16_t>(Tiler32::GetElementIndex(0, y) * bytes_per_element);
			info.pipe = static_cast<uint8_t>(Tiler32::GetPipeIndex(0, y));
			info.bank = static_cast<uint8_t>(
			    Tiler32::GetBankIndex(0, y, 1, t.m_bank_height, t.m_num_banks, t.m_num_pipes));
		}
	}

	[[nodiscard]] uint64_t GetTiledOffset(uint32_t x, uint32_t y) const {
		const auto& x_info = m_x[x];
		const auto& y_info = m_y[y];

		const uint64_t base = y_info.macro_offset + x_info.macro_offset + y_info.tile_offset +
		                      y_info.element_bytes + x_info.element_bytes;
		const uint64_t pipe = x_info.pipe ^ y_info.pipe;
		const uint64_t bank = x_info.bank ^ y_info.bank;

		return (base & 0xffu) | (pipe << 8u) | (bank << (8u + m_pipe_bits)) |
		       ((base >> 8u) << (8u + m_pipe_bits + m_bank_bits));
	}

private:
	std::vector<FastTiler32XInfo> m_x;
	std::vector<FastTiler32YInfo> m_y;
	uint32_t                      m_pipe_bits = 0;
	uint32_t                      m_bank_bits = 0;
};

static Tiler* g_tiler = nullptr;

void TileInit() {
	EXIT_IF(g_tiler != nullptr);

	g_tiler = new Tiler;
}

// NOLINTNEXTLINE(readability-non-const-parameter)
static void Detile32(const Tiler32& t, uint32_t width, uint32_t height, uint32_t dst_pitch,
                     uint8_t* dst, const uint8_t* src) {
	EXIT_IF(g_tiler == nullptr);

	Common::LockGuard lock(g_tiler->m_mutex);

	FastTiler32 fast_tiler;
	fast_tiler.Init(t, width, height);

	struct DetileParams {
		const FastTiler32* t;
		uint32_t           start_y;
		uint32_t           width;
		uint32_t           height;
		uint32_t           dst_pitch;
		uint8_t*           dst;
		const uint8_t*     src;
	};

	auto func = [](void* args) {
		auto* p = static_cast<DetileParams*>(args);

		auto*              dst       = p->dst;
		const auto*        src       = p->src;
		const FastTiler32* t         = p->t;
		uint32_t           start_y   = p->start_y;
		uint32_t           width     = p->width;
		uint32_t           height    = p->height;
		uint64_t           dst_pitch = p->dst_pitch;

		for (uint32_t y = start_y; y < height; y++) {
			uint32_t x             = 0;
			uint64_t linear_offset = y * dst_pitch * 4;

			for (; x + 1 < width; x += 2) {
				auto tiled_offset = t->GetTiledOffset(x, y);

				*reinterpret_cast<uint64_t*>(dst + linear_offset) =
				    *reinterpret_cast<const uint64_t*>(src + tiled_offset);
				linear_offset += 8;
			}
			if (x < width) {
				auto tiled_offset = t->GetTiledOffset(x, y);

				*reinterpret_cast<uint32_t*>(dst + linear_offset) =
				    *reinterpret_cast<const uint32_t*>(src + tiled_offset);
			}
		}
	};

	const uint32_t split = height / 2;
	DetileParams   p1 {&fast_tiler, 0, width, split, dst_pitch, dst, src};
	DetileParams   p2 {&fast_tiler, split, width, height, dst_pitch, dst, src};

	g_tiler->m_job1.Execute([func, &p2] { func(&p2); });
	func(&p1);
	g_tiler->m_job1.Wait();
}

static constexpr uint32_t GetStandard64KB32XPart(uint32_t x) {
	// FIXME: Temporary PS5 AGC TileMode::kStandard64KB 32bpp detiler.
	// AGC Standard64KB is block-linear: 32bpp surfaces use 128x128-element
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
		offset ^= (y << 3u) & 0x0008u;
		offset ^= (y << 4u) & 0x0020u;
		offset ^= (y << 5u) & 0x0f80u;
		offset ^= (y << 9u) & 0x1000u;
		offset ^= (y << 8u) & 0x4000u;

		offset ^= (x << 2u) & 0x0004u;
		offset ^= (x << 3u) & 0x0010u;
		offset ^= (x << 4u) & 0x0440u;
		offset ^= (x << 5u) & 0x0300u;
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
	} else {
		EXIT("unsupported render-target element size: %u\n", static_cast<uint32_t>(sizeof(T)));
	}

	return offset;
}

template <typename T, bool tiled_to_linear>
static void ConvertRenderTargetTyped(uint32_t width, uint32_t height, uint32_t pitch, T* dst,
                                     const T* src, uint64_t size) {
	EXIT_IF(g_tiler == nullptr);

	uint32_t block_width  = 0;
	uint32_t block_height = 0;
	EXIT_NOT_IMPLEMENTED(!Gen5Thin64KBBlockSizeFromElementBytes(static_cast<uint32_t>(sizeof(T)),
	                                                            &block_width, &block_height));

	const uint64_t blocks_per_row = (static_cast<uint64_t>(pitch) + block_width - 1u) / block_width;
	const uint32_t block_columns  = (width == 0 ? 0 : 1u + (width - 1u) / block_width);
	const uint64_t elements_per_block = 65536u / sizeof(T);
	const uint64_t count              = size / sizeof(T);

	std::array<uint32_t, 256> x_elements {};
	std::array<uint32_t, 256> y_elements {};
	for (uint32_t x = 0; x < block_width; x++) {
		x_elements[x] = Gen5RenderTargetOffsetInBlock<T>(x, 0) / sizeof(T);
	}
	for (uint32_t y = 0; y < block_height; y++) {
		y_elements[y] = Gen5RenderTargetOffsetInBlock<T>(0, y) / sizeof(T);
	}

	struct ConvertParams {
		uint32_t        start_block_row;
		uint32_t        end_block_row;
		uint32_t        width;
		uint32_t        height;
		uint32_t        pitch;
		uint32_t        block_width;
		uint32_t        block_height;
		uint32_t        block_columns;
		uint64_t        blocks_per_row;
		uint64_t        elements_per_block;
		const uint32_t* x_elements;
		const uint32_t* y_elements;
		T*              dst;
		const T*        src;
		uint64_t        count;
	};

	auto func = [](void* args) {
		auto* p = static_cast<ConvertParams*>(args);

		for (uint32_t block_row = p->start_block_row; block_row < p->end_block_row; block_row++) {
			const uint32_t block_y     = block_row * p->block_height;
			const uint32_t copy_height = std::min(p->block_height, p->height - block_y);
			const uint32_t block_y_element =
			    Gen5RenderTargetOffsetInBlock<T>(0, block_y) / sizeof(T);

			for (uint32_t block_column = 0; block_column < p->block_columns; block_column++) {
				const uint32_t block_x =
				    static_cast<uint32_t>(static_cast<uint64_t>(block_column) * p->block_width);
				const uint32_t copy_width = std::min(p->block_width, p->width - block_x);
				const uint64_t block_base =
				    (static_cast<uint64_t>(block_row) * p->blocks_per_row + block_column) *
				    p->elements_per_block;

				for (uint32_t y = 0; y < copy_height; y++) {
					const uint64_t linear_row =
					    static_cast<uint64_t>(block_y + y) * p->pitch + block_x;
					for (uint32_t x = 0; x < copy_width; x++) {
						const uint64_t linear_index = linear_row + x;
						const uint64_t tiled_index =
						    block_base + (p->x_elements[x] ^ p->y_elements[y] ^ block_y_element);
						if (linear_index < p->count && tiled_index < p->count) {
							if constexpr (tiled_to_linear) {
								p->dst[linear_index] = p->src[tiled_index];
							} else {
								p->dst[tiled_index] = p->src[linear_index];
							}
						}
					}
				}
			}
		}
	};

	const uint32_t block_rows = (height == 0 ? 0 : 1u + (height - 1u) / block_height);
	const uint32_t split      = block_rows / 2;
	ConvertParams  p1 {0,
	                   split,
	                   width,
	                   height,
	                   pitch,
	                   block_width,
	                   block_height,
	                   block_columns,
	                   blocks_per_row,
	                   elements_per_block,
	                   x_elements.data(),
	                   y_elements.data(),
	                   dst,
	                   src,
	                   count};
	ConvertParams  p2 {split,
	                   block_rows,
	                   width,
	                   height,
	                   pitch,
	                   block_width,
	                   block_height,
	                   block_columns,
	                   blocks_per_row,
	                   elements_per_block,
	                   x_elements.data(),
	                   y_elements.data(),
	                   dst,
	                   src,
	                   count};

	Common::LockGuard lock(g_tiler->m_mutex);
	g_tiler->m_job1.Execute([func, &p2] { func(&p2); });
	func(&p1);
	g_tiler->m_job1.Wait();
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

template <typename T, uint32_t BlockWidth, uint32_t BlockHeight, uint32_t CopyBatch,
          bool ExactSurfaceRequiresFullWidth, typename Tables>
static void ConvertTiledToLinearStandard64KBElements(void* dst, const void* src, uint32_t width,
                                                     uint32_t height, uint32_t pitch, uint64_t size,
                                                     uint64_t src_size, uint32_t src_x,
                                                     uint32_t src_y, const Tables& tables) {
	static_assert(BlockWidth * BlockHeight * sizeof(T) == 64u * 1024u);
	static_assert(CopyBatch != 0 && BlockWidth % CopyBatch == 0);

	EXIT_IF(dst == nullptr || src == nullptr);
	EXIT_NOT_IMPLEMENTED(pitch == 0);
	const bool tail_block = (src_size != 0 && (src_x != 0 || src_y != 0 || size < src_size));
	EXIT_NOT_IMPLEMENTED(width > pitch);
	EXIT_NOT_IMPLEMENTED(tail_block &&
	                     (src_x + width > BlockWidth || src_y + height > BlockHeight));

	auto*       dst_elements      = static_cast<T*>(dst);
	const auto* src_elements      = static_cast<const T*>(src);
	const auto  element_count     = size / sizeof(T);
	const auto  src_element_count = (src_size != 0 ? src_size : size) / sizeof(T);
	const auto  blocks_per_row =
	    tail_block ? 1u : static_cast<uint64_t>(AlignUp(pitch, BlockWidth) / BlockWidth);
	const auto  dst_element_count = static_cast<uint64_t>(pitch) * height;
	const bool  exact_surface = !tail_block && (!ExactSurfaceRequiresFullWidth || width == pitch) &&
	                            size % sizeof(T) == 0 && element_count == dst_element_count;
	const auto* x_elements    = tables.x_words.data();
	const auto* y_elements    = tables.y_words.data();

	if (!exact_surface) {
		std::memset(dst_elements, 0, static_cast<size_t>(size));
	}

	constexpr uint64_t ElementsPerBlock = BlockWidth * BlockHeight;
	for (uint32_t block_y = 0; block_y < height; block_y += BlockHeight) {
		const uint32_t copy_height = std::min(BlockHeight, height - block_y);

		for (uint32_t block_x = 0; block_x < width; block_x += BlockWidth) {
			const uint32_t copy_width = std::min(BlockWidth, width - block_x);
			const uint64_t block_base =
			    tail_block ? 0u
			               : ((static_cast<uint64_t>(block_y / BlockHeight) * blocks_per_row) +
			                  block_x / BlockWidth) *
			                     ElementsPerBlock;
			const bool full_block = !tail_block && copy_width == BlockWidth &&
			                        copy_height == BlockHeight &&
			                        block_base + ElementsPerBlock <= src_element_count &&
			                        static_cast<uint64_t>(block_y + BlockHeight - 1u) * pitch +
			                                block_x + BlockWidth - 1u <
			                            element_count;

			if (full_block) {
				for (uint32_t local_y = 0; local_y < BlockHeight; local_y++) {
					const uint64_t dst_row =
					    static_cast<uint64_t>(block_y + local_y) * pitch + block_x;
					const uint64_t src_row = block_base + y_elements[src_y + local_y];

					for (uint32_t local_x = 0; local_x < BlockWidth; local_x += CopyBatch) {
						for (uint32_t i = 0; i < CopyBatch; i++) {
							dst_elements[dst_row + local_x + i] =
							    src_elements[src_row + x_elements[src_x + local_x + i]];
						}
					}
				}
				continue;
			}

			for (uint32_t local_y = 0; local_y < copy_height; local_y++) {
				const uint64_t dst_row = static_cast<uint64_t>(block_y + local_y) * pitch + block_x;
				const uint64_t src_row = block_base + y_elements[src_y + local_y];

				for (uint32_t local_x = 0; local_x < copy_width; local_x++) {
					const uint64_t dst_index = dst_row + local_x;
					const uint64_t src_index = src_row + x_elements[src_x + local_x];

					if (src_index < src_element_count && dst_index < element_count) {
						dst_elements[dst_index] = src_elements[src_index];
					}
				}
			}
		}
	}
}

void TileConvertTiledToLinearStandard64KB32(void* dst, const void* src, uint32_t width,
                                            uint32_t height, uint32_t pitch, uint64_t size,
                                            uint64_t src_size, uint32_t src_x, uint32_t src_y) {
	KYTY_PROFILER_FUNCTION();

	ConvertTiledToLinearStandard64KBElements<uint32_t, 128u, 128u, 4u, true>(
	    dst, src, width, height, pitch, size, src_size, src_x, src_y, GetStandard64KB32Tables());
}

void TileConvertLinearToTiledStandard64KB32(void* dst, const void* src, uint32_t width,
                                            uint32_t height, uint32_t pitch, uint64_t size) {
	KYTY_PROFILER_FUNCTION();

	const auto rows           = static_cast<uint64_t>(height == 0 ? 0 : height - 1u);
	const auto expected_pitch = (static_cast<uint64_t>(width) + 127u) & ~uint64_t {127u};
	if (pitch != expected_pitch || size < (rows * pitch + width) * sizeof(uint32_t)) {
		EXIT("invalid linear-to-Standard64KB32 conversion, dst=%p src=%p extent=%ux%u "
		     "pitch=%u size=0x%016" PRIx64 "\n",
		     dst, src, width, height, pitch, size);
	}

	auto*       dst32          = static_cast<uint32_t*>(dst);
	const auto* src32          = static_cast<const uint32_t*>(src);
	const auto& tables         = GetStandard64KB32Tables();
	const auto  blocks_per_row = (static_cast<uint64_t>(pitch) + 127u) >> 7u;
	const auto  block_rows     = (static_cast<uint64_t>(height) + 127u) >> 7u;
	if (blocks_per_row > UINT64_MAX / block_rows / 65536u ||
	    size != blocks_per_row * block_rows * 65536u) {
		EXIT("invalid Standard64KB32 allocation, extent=%ux%u pitch=%u size=0x%016" PRIx64
		     " blocks=%" PRIu64 "x%" PRIu64 "\n",
		     width, height, pitch, size, blocks_per_row, block_rows);
	}
	const auto* x_words = tables.x_words.data();
	const auto* y_words = tables.y_words.data();

	std::memset(dst32, 0, static_cast<size_t>(size));
	for (uint32_t block_y = 0; block_y < height; block_y += 128u) {
		const uint32_t block_height = std::min(128u, height - block_y);
		for (uint32_t block_x = 0; block_x < width; block_x += 128u) {
			const uint32_t block_width = std::min(128u, width - block_x);
			const uint64_t block_base =
			    (((static_cast<uint64_t>(block_y) >> 7u) * blocks_per_row) + (block_x >> 7u))
			    << 14u;
			if (block_width == 128u && block_height == 128u) {
				for (uint32_t local_y = 0; local_y < 128u; local_y++) {
					const uint64_t src_row =
					    (static_cast<uint64_t>(block_y + local_y) * pitch) + block_x;
					const uint64_t dst_row = block_base + y_words[local_y];
					for (uint32_t local_x = 0; local_x < 128u; local_x += 4u) {
						dst32[dst_row + x_words[local_x + 0u]] = src32[src_row + local_x + 0u];
						dst32[dst_row + x_words[local_x + 1u]] = src32[src_row + local_x + 1u];
						dst32[dst_row + x_words[local_x + 2u]] = src32[src_row + local_x + 2u];
						dst32[dst_row + x_words[local_x + 3u]] = src32[src_row + local_x + 3u];
					}
				}
				continue;
			}

			for (uint32_t local_y = 0; local_y < block_height; local_y++) {
				const uint64_t src_row =
				    (static_cast<uint64_t>(block_y + local_y) * pitch) + block_x;
				const uint64_t dst_row = block_base + y_words[local_y];
				for (uint32_t local_x = 0; local_x < block_width; local_x++) {
					dst32[dst_row + x_words[local_x]] = src32[src_row + local_x];
				}
			}
		}
	}
}

void TileConvertTiledToLinearStandard64KB16(void* dst, const void* src, uint32_t width,
                                            uint32_t height, uint32_t pitch, uint64_t size,
                                            uint64_t src_size, uint32_t src_x, uint32_t src_y) {
	KYTY_PROFILER_FUNCTION();

	ConvertTiledToLinearStandard64KBElements<uint16_t, 256u, 128u, 4u, true>(
	    dst, src, width, height, pitch, size, src_size, src_x, src_y, GetStandard64KB16Tables());
}

static void TileConvertTiledToLinearStandard64KB8Elements(
    void* dst, const void* src, uint32_t width_elements, uint32_t height_elements,
    uint32_t pitch_elements, uint64_t size, uint64_t src_size, uint32_t src_x, uint32_t src_y) {
	KYTY_PROFILER_FUNCTION();

	ConvertTiledToLinearStandard64KBElements<uint8_t, 256u, 256u, 4u, false>(
	    dst, src, width_elements, height_elements, pitch_elements, size, src_size, src_x, src_y,
	    GetStandard64KB8Tables());
}

static void TileConvertTiledToLinearStandard64KB64Elements(
    void* dst, const void* src, uint32_t width_elements, uint32_t height_elements,
    uint32_t pitch_elements, uint64_t size, uint64_t src_size, uint32_t src_x, uint32_t src_y) {
	KYTY_PROFILER_FUNCTION();

	ConvertTiledToLinearStandard64KBElements<uint64_t, 128u, 64u, 8u, false>(
	    dst, src, width_elements, height_elements, pitch_elements, size, src_size, src_x, src_y,
	    GetStandard64KB64Tables());
}

static void TileConvertLinearToTiledStandard64KB64Elements(
    void* dst, const void* src, uint32_t width_elements, uint32_t height_elements,
    uint32_t pitch_elements, uint64_t size, uint64_t dst_size, uint32_t dst_x, uint32_t dst_y) {
	if (width_elements > pitch_elements) {
		EXIT("invalid linear-to-Standard64KB64 tail conversion, extent=%ux%u pitch=%u "
		     "size=0x%016" PRIx64 " dst_size=0x%016" PRIx64 " dst=%u,%u\n",
		     width_elements, height_elements, pitch_elements, size, dst_size, dst_x, dst_y);
	}
	auto*       dst64          = static_cast<uint64_t*>(dst);
	const auto* src64          = static_cast<const uint64_t*>(src);
	const auto& tables         = GetStandard64KB64Tables();
	const auto  size_words     = size >> 3u;
	const auto  dst_size_words = dst_size >> 3u;
	for (uint32_t y = 0; y < height_elements; y++) {
		const uint64_t src_row = static_cast<uint64_t>(y) * pitch_elements;
		const uint64_t dst_row = tables.y_words[dst_y + y];
		for (uint32_t x = 0; x < width_elements; x++) {
			const uint64_t src_index = src_row + x;
			const uint64_t dst_index = dst_row + tables.x_words[dst_x + x];
			if (src_index < size_words && dst_index < dst_size_words) {
				dst64[dst_index] = src64[src_index];
			}
		}
	}
}

static void TileConvertTiledToLinearStandard64KB128Elements(
    void* dst, const void* src, uint32_t width_elements, uint32_t height_elements,
    uint32_t pitch_elements, uint64_t size, uint64_t src_size, uint32_t src_x, uint32_t src_y) {
	KYTY_PROFILER_FUNCTION();

	ConvertTiledToLinearStandard64KBElements<Uint128, 64u, 64u, 4u, false>(
	    dst, src, width_elements, height_elements, pitch_elements, size, src_size, src_x, src_y,
	    GetStandard64KB128Tables());
}

void TileConvertTiledToLinearStandard64KB(void* dst, const void* src, uint32_t format,
                                          uint32_t width, uint32_t height, uint32_t pitch,
                                          uint64_t size, uint64_t src_size, uint32_t src_x,
                                          uint32_t src_y) {
	uint32_t bytes_per_element       = 0;
	uint32_t texels_per_element_wide = 0;
	uint32_t texels_per_element_tall = 0;
	uint32_t block_width_log2        = 0;
	uint32_t block_height_log2       = 0;

	EXIT_NOT_IMPLEMENTED(!Gen5Standard64KBLayout(format, &bytes_per_element,
	                                             &texels_per_element_wide, &texels_per_element_tall,
	                                             &block_width_log2, &block_height_log2));

	const uint32_t width_elements =
	    std::max((width + texels_per_element_wide - 1u) / texels_per_element_wide, 1u);
	const uint32_t height_elements =
	    std::max((height + texels_per_element_tall - 1u) / texels_per_element_tall, 1u);
	const uint32_t pitch_elements =
	    std::max((pitch + texels_per_element_wide - 1u) / texels_per_element_wide, 1u);

	switch (bytes_per_element) {
		case 1:
			TileConvertTiledToLinearStandard64KB8Elements(dst, src, width_elements, height_elements,
			                                              pitch_elements, size, src_size, src_x,
			                                              src_y);
			break;
		case 2:
			TileConvertTiledToLinearStandard64KB16(dst, src, width_elements, height_elements,
			                                       pitch_elements, size, src_size, src_x, src_y);
			break;
		case 4:
			TileConvertTiledToLinearStandard64KB32(dst, src, width_elements, height_elements,
			                                       pitch_elements, size, src_size, src_x, src_y);
			break;
		case 8:
			TileConvertTiledToLinearStandard64KB64Elements(dst, src, width_elements,
			                                               height_elements, pitch_elements, size,
			                                               src_size, src_x, src_y);
			break;
		case 16:
			TileConvertTiledToLinearStandard64KB128Elements(dst, src, width_elements,
			                                                height_elements, pitch_elements, size,
			                                                src_size, src_x, src_y);
			break;
		default: EXIT("unsupported Standard64KB element size: %u\n", bytes_per_element);
	}
}

void TileConvertTiledToLinearStandard256B(void* dst, const void* src, uint32_t format,
                                          uint32_t width, uint32_t height, uint32_t pitch,
                                          uint64_t dst_size, uint64_t src_size) {
	uint32_t bytes_per_element       = 0;
	uint32_t texels_per_element_wide = 0;
	uint32_t texels_per_element_tall = 0;
	uint32_t block_width_log2        = 0;
	uint32_t block_height_log2       = 0;

	EXIT_NOT_IMPLEMENTED(!Gen5Standard256BLayout(format, &bytes_per_element,
	                                             &texels_per_element_wide, &texels_per_element_tall,
	                                             &block_width_log2, &block_height_log2));

	const uint32_t row_elements =
	    std::max((pitch + texels_per_element_wide - 1u) / texels_per_element_wide, 1u);
	const uint32_t width_elements =
	    std::max((width + texels_per_element_wide - 1u) / texels_per_element_wide, 1u);
	const uint32_t height_elements =
	    std::max((height + texels_per_element_tall - 1u) / texels_per_element_tall, 1u);
	const uint32_t padded_width = AlignUp(row_elements, 1u << block_width_log2);

	if (src_size == 0) {
		src_size = dst_size;
	}

	switch (bytes_per_element) {
		case 1:
			DetileStandard256BTyped(static_cast<uint8_t*>(dst), static_cast<const uint8_t*>(src),
			                        row_elements, width_elements, height_elements, padded_width,
			                        block_width_log2, block_height_log2, dst_size, src_size);
			break;
		case 2:
			DetileStandard256BTyped(static_cast<uint16_t*>(dst), static_cast<const uint16_t*>(src),
			                        row_elements, width_elements, height_elements, padded_width,
			                        block_width_log2, block_height_log2, dst_size, src_size);
			break;
		case 4:
			DetileStandard256BTyped(static_cast<uint32_t*>(dst), static_cast<const uint32_t*>(src),
			                        row_elements, width_elements, height_elements, padded_width,
			                        block_width_log2, block_height_log2, dst_size, src_size);
			break;
		case 8:
			DetileStandard256BTyped(static_cast<uint64_t*>(dst), static_cast<const uint64_t*>(src),
			                        row_elements, width_elements, height_elements, padded_width,
			                        block_width_log2, block_height_log2, dst_size, src_size);
			break;
		case 16:
			DetileStandard256BTyped(static_cast<Uint128*>(dst), static_cast<const Uint128*>(src),
			                        row_elements, width_elements, height_elements, padded_width,
			                        block_width_log2, block_height_log2, dst_size, src_size);
			break;
		default: EXIT("unsupported Standard256B element size: %u\n", bytes_per_element);
	}
}

// Prospero depth/stencil-tile address table for 2D, 1xAA surfaces by element size.
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

static_assert(Depth64KB8XOffsetBytes(2) == 0x0004u);
static_assert(Depth64KB8YOffsetBytes(1) == 0x0002u);
static_assert((Depth64KB8XOffsetBytes(3) ^ Depth64KB8YOffsetBytes(5)) == 0x0027u);
static_assert(Depth64KB16XOffsetBytes(2) == 0x0008u);
static_assert(Depth64KB16YOffsetBytes(1) == 0x0004u);
static_assert((Depth64KB16XOffsetBytes(3) ^ Depth64KB16YOffsetBytes(5)) == 0x004eu);
static_assert(Depth64KB32XOffsetBytes(2) == 0x0010u);
static_assert(Depth64KB32YOffsetBytes(1) == 0x0008u);
static_assert((Depth64KB32XOffsetBytes(3) ^ Depth64KB32YOffsetBytes(5)) == 0x009cu);

struct Depth64KB8Tables {
	std::array<uint32_t, 256> x_words {};
	std::array<uint32_t, 256> y_words {};

	constexpr Depth64KB8Tables() {
		for (uint32_t x = 0; x < x_words.size(); x++) {
			x_words[x] = Depth64KB8XOffsetBytes(x);
		}
		for (uint32_t y = 0; y < y_words.size(); y++) {
			y_words[y] = Depth64KB8YOffsetBytes(y);
		}
	}
};

struct Depth64KB16Tables {
	std::array<uint32_t, 256> x_words {};
	std::array<uint32_t, 128> y_words {};

	constexpr Depth64KB16Tables() {
		for (uint32_t x = 0; x < x_words.size(); x++) {
			x_words[x] = Depth64KB16XOffsetBytes(x) / sizeof(uint16_t);
		}
		for (uint32_t y = 0; y < y_words.size(); y++) {
			y_words[y] = Depth64KB16YOffsetBytes(y) / sizeof(uint16_t);
		}
	}
};

struct Depth64KB32Tables {
	std::array<uint32_t, 128> x_words {};
	std::array<uint32_t, 128> y_words {};

	constexpr Depth64KB32Tables() {
		for (uint32_t x = 0; x < x_words.size(); x++) {
			x_words[x] = Depth64KB32XOffsetBytes(x) / sizeof(uint32_t);
		}
		for (uint32_t y = 0; y < y_words.size(); y++) {
			y_words[y] = Depth64KB32YOffsetBytes(y) / sizeof(uint32_t);
		}
	}
};

template <bool tiled_to_linear, typename T, size_t x_count, size_t y_count>
static void TileConvertDepthTyped(void* dst, const void* src, uint32_t width, uint32_t height,
                                  uint32_t pitch, uint64_t size,
                                  const std::array<uint32_t, x_count>& x_words,
                                  const std::array<uint32_t, y_count>& y_words) {
	constexpr uint32_t block_width    = static_cast<uint32_t>(x_count);
	constexpr uint32_t block_height   = static_cast<uint32_t>(y_count);
	const uint32_t     block_columns  = 1u + (width - 1u) / block_width;
	const uint64_t     blocks_per_row = pitch / block_width;
	const uint32_t     block_rows     = 1u + (height - 1u) / block_height;

	struct ConvertParams {
		uint32_t        start_block_row;
		uint32_t        end_block_row;
		uint32_t        width;
		uint32_t        height;
		uint32_t        pitch;
		uint32_t        block_columns;
		uint64_t        blocks_per_row;
		const uint32_t* x_words;
		const uint32_t* y_words;
		T*              dst;
		const T*        src;
	};

	auto convert = [](void* args) {
		auto* p = static_cast<ConvertParams*>(args);
		for (uint32_t block_row = p->start_block_row; block_row < p->end_block_row; block_row++) {
			const uint32_t block_y = block_row * static_cast<uint32_t>(y_count);
			const uint32_t copy_height =
			    std::min(static_cast<uint32_t>(y_count), p->height - block_y);
			for (uint32_t block_column = 0; block_column < p->block_columns; block_column++) {
				const uint32_t block_x = block_column * static_cast<uint32_t>(x_count);
				const uint32_t copy_width =
				    std::min(static_cast<uint32_t>(x_count), p->width - block_x);
				const uint64_t block_base =
				    (static_cast<uint64_t>(block_row) * p->blocks_per_row + block_column) *
				    (65536u / sizeof(T));
				for (uint32_t y = 0; y < copy_height; y++) {
					const uint64_t linear_row =
					    static_cast<uint64_t>(block_y + y) * p->pitch + block_x;
					const uint64_t tiled_row = block_base + p->y_words[y];
					for (uint32_t x = 0; x < copy_width; x++) {
						const uint64_t linear_index = linear_row + x;
						const uint64_t tiled_index  = tiled_row ^ p->x_words[x];
						if constexpr (tiled_to_linear) {
							p->dst[linear_index] = p->src[tiled_index];
						} else {
							p->dst[tiled_index] = p->src[linear_index];
						}
					}
				}
			}
		}
	};

	auto*       dst_typed = static_cast<T*>(dst);
	const auto* src_typed = static_cast<const T*>(src);
	std::memset(dst_typed, 0, static_cast<size_t>(size));
	const uint32_t split = block_rows / 2u;
	ConvertParams  first {0,
	                      split,
	                      width,
	                      height,
	                      pitch,
	                      block_columns,
	                      blocks_per_row,
	                      x_words.data(),
	                      y_words.data(),
	                      dst_typed,
	                      src_typed};
	ConvertParams  second {split,          block_rows,    width,          height,
	                       pitch,          block_columns, blocks_per_row, x_words.data(),
	                       y_words.data(), dst_typed,     src_typed};
	if (split == 0) {
		convert(&second);
		return;
	}
	EXIT_IF(g_tiler == nullptr);
	Common::LockGuard lock(g_tiler->m_mutex);
	g_tiler->m_job1.Execute([convert, &second] { convert(&second); });
	convert(&first);
	g_tiler->m_job1.Wait();
}

template <bool tiled_to_linear>
static void TileConvertDepth(void* dst, const void* src, uint32_t format, uint32_t width,
                             uint32_t height, uint32_t pitch, uint64_t size) {
	KYTY_PROFILER_FUNCTION();
	const uint32_t bytes_per_element = Prospero::NumBytesPerElement(format);
	const bool     supported_bpe =
	    bytes_per_element == 1 || bytes_per_element == 2 || bytes_per_element == 4;
	const uint32_t block_width  = supported_bpe && bytes_per_element <= 2 ? 256u : 128u;
	const uint32_t block_height = supported_bpe ? 65536u / (block_width * bytes_per_element) : 0;
	if (dst == nullptr || src == nullptr || width == 0 || height == 0 || pitch < width ||
	    !supported_bpe || pitch % block_width != 0 || size == 0 || size % 65536u != 0) {
		EXIT("unsupported depth conversion, dst=%p src=%p format=%u "
		     "extent=%ux%u pitch=%u size=0x%016" PRIx64 "\n",
		     dst, src, format, width, height, pitch, size);
	}
	const uint64_t block_rows = (static_cast<uint64_t>(height) + block_height - 1u) / block_height;
	const uint64_t blocks_per_row = pitch / block_width;
	if (blocks_per_row > UINT64_MAX / block_rows ||
	    blocks_per_row * block_rows > UINT64_MAX / 65536u ||
	    size != blocks_per_row * block_rows * 65536u) {
		EXIT("depth storage disagrees with 64 KiB block layout, "
		     "extent=%ux%u pitch=%u bpe=%u size=0x%016" PRIx64 "\n",
		     width, height, pitch, bytes_per_element, size);
	}
	if (bytes_per_element == 1) {
		static constexpr Depth64KB8Tables tables;
		TileConvertDepthTyped<tiled_to_linear, uint8_t>(dst, src, width, height, pitch, size,
		                                                tables.x_words, tables.y_words);
	} else if (bytes_per_element == 2) {
		static constexpr Depth64KB16Tables tables;
		TileConvertDepthTyped<tiled_to_linear, uint16_t>(dst, src, width, height, pitch, size,
		                                                 tables.x_words, tables.y_words);
	} else {
		static constexpr Depth64KB32Tables tables;
		TileConvertDepthTyped<tiled_to_linear, uint32_t>(dst, src, width, height, pitch, size,
		                                                 tables.x_words, tables.y_words);
	}
}

void TileConvertTiledToLinearDepth(void* dst, const void* src, uint32_t format, uint32_t width,
                                   uint32_t height, uint32_t pitch, uint64_t size) {
	TileConvertDepth<true>(dst, src, format, width, height, pitch, size);
}

void TileConvertLinearToTiledDepth(void* dst, const void* src, uint32_t format, uint32_t width,
                                   uint32_t height, uint32_t pitch, uint64_t size) {
	TileConvertDepth<false>(dst, src, format, width, height, pitch, size);
}

void TileConvertTiledToLinearStandard4KB(void* dst, const void* src, uint32_t format,
                                         uint32_t width, uint32_t height, uint32_t pitch,
                                         uint64_t dst_size, uint64_t src_size, uint32_t src_x,
                                         uint32_t src_y) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_NOT_IMPLEMENTED(pitch == 0);
	EXIT_NOT_IMPLEMENTED(width > pitch);

	uint32_t bytes_per_element       = 0;
	uint32_t texels_per_element_wide = 0;
	uint32_t texels_per_element_tall = 0;
	uint32_t block_width_log2        = 0;
	uint32_t block_height_log2       = 0;

	EXIT_NOT_IMPLEMENTED(!Gen5Standard4KBLayout(format, &bytes_per_element,
	                                            &texels_per_element_wide, &texels_per_element_tall,
	                                            &block_width_log2, &block_height_log2));

	const uint32_t row_elements =
	    std::max((pitch + texels_per_element_wide - 1u) / texels_per_element_wide, 1u);
	const uint32_t width_elements =
	    std::max((width + texels_per_element_wide - 1u) / texels_per_element_wide, 1u);
	const uint32_t height_elements =
	    std::max((height + texels_per_element_tall - 1u) / texels_per_element_tall, 1u);
	const uint32_t block_width   = 1u << block_width_log2;
	const uint32_t block_height  = 1u << block_height_log2;
	const uint32_t padded_width  = (row_elements + block_width - 1u) & ~(block_width - 1u);
	const uint32_t padded_height = (height_elements + block_height - 1u) & ~(block_height - 1u);

	EXIT_IF((padded_width & (block_width - 1u)) != 0);
	EXIT_IF((padded_height & (block_height - 1u)) != 0);
	EXIT_NOT_IMPLEMENTED(src_x >= block_width || src_y >= block_height);
	EXIT_NOT_IMPLEMENTED(src_x != 0 && src_x + width_elements > block_width);
	EXIT_NOT_IMPLEMENTED(src_y != 0 && src_y + height_elements > block_height);

	auto* dst8 = static_cast<uint8_t*>(dst);

	std::memset(dst8, 0, static_cast<size_t>(dst_size));

	switch (bytes_per_element) {
		case 1:
			DetileStandard4KBTyped(static_cast<uint8_t*>(dst), static_cast<const uint8_t*>(src),
			                       row_elements, width_elements, height_elements, padded_width,
			                       block_width_log2, block_height_log2, dst_size, src_size, src_x,
			                       src_y);
			break;
		case 2:
			DetileStandard4KBTyped(static_cast<uint16_t*>(dst), static_cast<const uint16_t*>(src),
			                       row_elements, width_elements, height_elements, padded_width,
			                       block_width_log2, block_height_log2, dst_size, src_size, src_x,
			                       src_y);
			break;
		case 4:
			DetileStandard4KBTyped(static_cast<uint32_t*>(dst), static_cast<const uint32_t*>(src),
			                       row_elements, width_elements, height_elements, padded_width,
			                       block_width_log2, block_height_log2, dst_size, src_size, src_x,
			                       src_y);
			break;
		case 8:
			DetileStandard4KBTyped(static_cast<uint64_t*>(dst), static_cast<const uint64_t*>(src),
			                       row_elements, width_elements, height_elements, padded_width,
			                       block_width_log2, block_height_log2, dst_size, src_size, src_x,
			                       src_y);
			break;
		case 16:
			DetileStandard4KBTyped(static_cast<Uint128*>(dst), static_cast<const Uint128*>(src),
			                       row_elements, width_elements, height_elements, padded_width,
			                       block_width_log2, block_height_log2, dst_size, src_size, src_x,
			                       src_y);
			break;
		default: EXIT("unsupported Standard4KB element size: %u\n", bytes_per_element);
	}
}

void TileConvertTiledToLinearStandard4KB3D(void* dst, const void* src, uint32_t format,
                                           uint32_t width, uint32_t height, uint32_t depth,
                                           uint32_t pitch, uint64_t dst_slice_stride,
                                           uint64_t dst_size, uint64_t src_size, bool clear_dst) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_NOT_IMPLEMENTED(pitch == 0);
	EXIT_NOT_IMPLEMENTED(width > pitch);
	EXIT_NOT_IMPLEMENTED(depth == 0);

	uint32_t bytes_per_element       = 0;
	uint32_t texels_per_element_wide = 0;
	uint32_t texels_per_element_tall = 0;
	uint32_t block_width_log2        = 0;
	uint32_t block_height_log2       = 0;
	uint32_t block_depth_log2        = 0;

	EXIT_NOT_IMPLEMENTED(!TileGetStandard4KBVolumeLayout(
	    format, &bytes_per_element, &texels_per_element_wide, &texels_per_element_tall,
	    &block_width_log2, &block_height_log2, &block_depth_log2));
	EXIT_NOT_IMPLEMENTED(dst_slice_stride == 0);
	EXIT_NOT_IMPLEMENTED((dst_slice_stride % bytes_per_element) != 0);

	const uint32_t row_elements =
	    std::max((pitch + texels_per_element_wide - 1u) / texels_per_element_wide, 1u);
	const uint32_t width_elements =
	    std::max((width + texels_per_element_wide - 1u) / texels_per_element_wide, 1u);
	const uint32_t height_elements =
	    std::max((height + texels_per_element_tall - 1u) / texels_per_element_tall, 1u);
	const uint32_t block_width   = 1u << block_width_log2;
	const uint32_t block_height  = 1u << block_height_log2;
	const uint32_t padded_width  = AlignUp(row_elements, block_width);
	const uint32_t padded_height = AlignUp(height_elements, block_height);

	src_size = (src_size != 0 ? src_size : dst_size);
	if (clear_dst) {
		std::memset(dst, 0, static_cast<size_t>(dst_size));
	}

	switch (bytes_per_element) {
		case 1:
			DetileStandard4KBVolumeTyped(static_cast<uint8_t*>(dst),
			                             static_cast<const uint8_t*>(src), row_elements,
			                             width_elements, height_elements, depth, padded_width,
			                             padded_height, block_width_log2, block_height_log2,
			                             block_depth_log2, dst_slice_stride, dst_size, src_size);
			break;
		case 2:
			DetileStandard4KBVolumeTyped(static_cast<uint16_t*>(dst),
			                             static_cast<const uint16_t*>(src), row_elements,
			                             width_elements, height_elements, depth, padded_width,
			                             padded_height, block_width_log2, block_height_log2,
			                             block_depth_log2, dst_slice_stride, dst_size, src_size);
			break;
		case 4:
			DetileStandard4KBVolumeTyped(static_cast<uint32_t*>(dst),
			                             static_cast<const uint32_t*>(src), row_elements,
			                             width_elements, height_elements, depth, padded_width,
			                             padded_height, block_width_log2, block_height_log2,
			                             block_depth_log2, dst_slice_stride, dst_size, src_size);
			break;
		case 8:
			DetileStandard4KBVolumeTyped(static_cast<uint64_t*>(dst),
			                             static_cast<const uint64_t*>(src), row_elements,
			                             width_elements, height_elements, depth, padded_width,
			                             padded_height, block_width_log2, block_height_log2,
			                             block_depth_log2, dst_slice_stride, dst_size, src_size);
			break;
		case 16:
			DetileStandard4KBVolumeTyped(static_cast<Uint128*>(dst),
			                             static_cast<const Uint128*>(src), row_elements,
			                             width_elements, height_elements, depth, padded_width,
			                             padded_height, block_width_log2, block_height_log2,
			                             block_depth_log2, dst_slice_stride, dst_size, src_size);
			break;
		default: EXIT("unsupported Standard4KB volume element size: %u\n", bytes_per_element);
	}
}

void TileConvertTiledToLinear(void* dst, const void* src, TileMode mode, uint32_t width,
                              uint32_t height) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(mode != TileMode::VideoOutTiled);

	Tiler32 t;
	t.Init(width, height);

	Detile32(t, width, height, width, static_cast<uint8_t*>(dst), static_cast<const uint8_t*>(src));
}

void TileConvertTiledToLinearRenderTarget(void* dst, const void* src, uint32_t width,
                                          uint32_t height, uint32_t pitch,
                                          uint32_t bytes_per_element, uint64_t size,
                                          uint64_t src_size, uint32_t src_x, uint32_t src_y) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(dst == nullptr || src == nullptr);
	EXIT_NOT_IMPLEMENTED(width > pitch);
	EXIT_NOT_IMPLEMENTED(pitch == 0);

	src_size              = (src_size != 0 ? src_size : size);
	const bool tail_block = (src_size != 0 && (src_x != 0 || src_y != 0 || size < src_size));
	if (!tail_block && bytes_per_element <= 8) {
		switch (bytes_per_element) {
			case 1:
				ConvertRenderTargetTyped<uint8_t, true>(width, height, pitch,
				                                        static_cast<uint8_t*>(dst),
				                                        static_cast<const uint8_t*>(src), size);
				break;
			case 2:
				ConvertRenderTargetTyped<uint16_t, true>(width, height, pitch,
				                                         static_cast<uint16_t*>(dst),
				                                         static_cast<const uint16_t*>(src), size);
				break;
			case 4:
				ConvertRenderTargetTyped<uint32_t, true>(width, height, pitch,
				                                         static_cast<uint32_t*>(dst),
				                                         static_cast<const uint32_t*>(src), size);
				break;
			case 8:
				ConvertRenderTargetTyped<uint64_t, true>(width, height, pitch,
				                                         static_cast<uint64_t*>(dst),
				                                         static_cast<const uint64_t*>(src), size);
				break;
			default: EXIT("unsupported render-target element size: %u\n", bytes_per_element);
		}
		return;
	}

	switch (bytes_per_element) {
		case 1:
			TileConvertTiledToLinearStandard64KB8Elements(dst, src, width, height, pitch, size,
			                                              src_size, src_x, src_y);
			break;
		case 2:
			TileConvertTiledToLinearStandard64KB16(dst, src, width, height, pitch, size, src_size,
			                                       src_x, src_y);
			break;
		case 4:
			TileConvertTiledToLinearStandard64KB32(dst, src, width, height, pitch, size, src_size,
			                                       src_x, src_y);
			break;
		case 8:
			TileConvertTiledToLinearStandard64KB64Elements(dst, src, width, height, pitch, size,
			                                               src_size, src_x, src_y);
			break;
		case 16:
			TileConvertTiledToLinearStandard64KB128Elements(dst, src, width, height, pitch, size,
			                                                src_size, src_x, src_y);
			break;
		default: EXIT("unsupported render-target element size: %u\n", bytes_per_element);
	}
}

void TileConvertLinearToTiledRenderTarget(void* dst, const void* src, uint32_t width,
                                          uint32_t height, uint32_t pitch,
                                          uint32_t bytes_per_element, uint64_t size,
                                          uint64_t dst_size, uint32_t dst_x, uint32_t dst_y) {
	KYTY_PROFILER_FUNCTION();
	const bool tail_block = dst_size != 0 && (dst_x != 0 || dst_y != 0 || size < dst_size);
	if (tail_block) {
		// Not sure about this at all
		if (bytes_per_element != 8) {
			EXIT("unsupported linear-to-tiled render-target tail element size: %u\n",
			     bytes_per_element);
		}
		TileConvertLinearToTiledStandard64KB64Elements(dst, src, width, height, pitch, size,
		                                               dst_size, dst_x, dst_y);
		return;
	}

	if (dst == nullptr || src == nullptr || width == 0 || height == 0 || pitch == 0 ||
	    width > pitch || bytes_per_element == 0 || size == 0 || size % bytes_per_element != 0) {
		EXIT("invalid linear-to-tiled render-target conversion, dst=%p src=%p extent=%ux%u "
		     "pitch=%u bpe=%u size=0x%016" PRIx64 "\n",
		     dst, src, width, height, pitch, bytes_per_element, size);
	}
	const auto rows = static_cast<uint64_t>(height - 1);
	if (rows > (UINT64_MAX - width) / pitch ||
	    (rows * pitch + width) > UINT64_MAX / bytes_per_element ||
	    size < (rows * pitch + width) * bytes_per_element) {
		EXIT("linear-to-tiled render-target storage is too small, extent=%ux%u pitch=%u "
		     "bpe=%u size=0x%016" PRIx64 "\n",
		     width, height, pitch, bytes_per_element, size);
	}

	std::memset(dst, 0, size);
	switch (bytes_per_element) {
		case 1:
			ConvertRenderTargetTyped<uint8_t, false>(width, height, pitch,
			                                         static_cast<uint8_t*>(dst),
			                                         static_cast<const uint8_t*>(src), size);
			break;
		case 2:
			ConvertRenderTargetTyped<uint16_t, false>(width, height, pitch,
			                                          static_cast<uint16_t*>(dst),
			                                          static_cast<const uint16_t*>(src), size);
			break;
		case 4:
			ConvertRenderTargetTyped<uint32_t, false>(width, height, pitch,
			                                          static_cast<uint32_t*>(dst),
			                                          static_cast<const uint32_t*>(src), size);
			break;
		case 8:
			ConvertRenderTargetTyped<uint64_t, false>(width, height, pitch,
			                                          static_cast<uint64_t*>(dst),
			                                          static_cast<const uint64_t*>(src), size);
			break;
		default: EXIT("unsupported render-target element size: %u\n", bytes_per_element);
	}
}

bool TileGetDepthSize(uint32_t width, uint32_t height, uint32_t pitch, uint32_t z_format,
                      uint32_t stencil_format, bool htile, TileSizeAlign* stencil_size,
                      TileSizeAlign* htile_size, TileSizeAlign* depth_size) {
	EXIT_IF(pitch != 0);
	// Prospero derives uncompressed depth/stencil as independent 64 KiB block surfaces and HTile
	// as 32 KiB metadata blocks covering 1024x512 pixels for a single-mip, single-slice target.
	if (width > 0 && width <= 16384 && height > 0 && height <= 16384 &&
	    (z_format == 1 || z_format == 3) && stencil_format <= 1) {
		const uint32_t depth_bytes       = z_format == 1 ? 2u : 4u;
		const uint32_t depth_block_width = z_format == 1 ? 256u : 128u;
		const uint64_t depth_bytes_total =
		    static_cast<uint64_t>(AlignUp(width, depth_block_width)) * AlignUp(height, 128u) *
		    depth_bytes;
		const uint64_t stencil_bytes_total =
		    stencil_format == 1
		        ? static_cast<uint64_t>(AlignUp(width, 256u)) * AlignUp(height, 256u)
		        : 0;
		const uint64_t htile_bytes_total =
		    htile ? static_cast<uint64_t>(AlignUp(width, 1024u) / 1024u) *
		                (AlignUp(height, 512u) / 512u) * 32768u
		          : 0;
		if (depth_bytes_total <= UINT32_MAX && stencil_bytes_total <= UINT32_MAX &&
		    htile_bytes_total <= UINT32_MAX) {
			*depth_size   = {static_cast<uint32_t>(depth_bytes_total), 65536};
			*stencil_size = stencil_format == 1
			                    ? TileSizeAlign {static_cast<uint32_t>(stencil_bytes_total), 65536}
			                    : TileSizeAlign {};
			*htile_size   = htile ? TileSizeAlign {static_cast<uint32_t>(htile_bytes_total), 32768}
			                      : TileSizeAlign {};
			return true;
		}
	}
	*depth_size   = TileSizeAlign();
	*htile_size   = TileSizeAlign();
	*stencil_size = TileSizeAlign();
	return false;
}

uint32_t TileGetRenderTargetPitch(uint32_t width, uint32_t bytes_per_element) {
	uint32_t block_width  = 0;
	uint32_t block_height = 0;
	if (width == 0 ||
	    !Gen5Thin64KBBlockSizeFromElementBytes(bytes_per_element, &block_width, &block_height)) {
		return 0;
	}
	const uint64_t pitch = (static_cast<uint64_t>(width) + block_width - 1u) &
	                       ~static_cast<uint64_t>(block_width - 1u);
	return pitch <= UINT32_MAX ? static_cast<uint32_t>(pitch) : 0;
}

bool TileGetRenderTargetSize(uint32_t width, uint32_t height, uint32_t pitch,
                             uint32_t bytes_per_element, TileSizeAlign* total_size) {
	*total_size           = {};
	uint32_t block_width  = 0;
	uint32_t block_height = 0;
	if (height == 0 || pitch == 0 ||
	    !Gen5Thin64KBBlockSizeFromElementBytes(bytes_per_element, &block_width, &block_height) ||
	    pitch != TileGetRenderTargetPitch(width, bytes_per_element)) {
		return false;
	}
	const uint64_t padded_height = (static_cast<uint64_t>(height) + block_height - 1u) &
	                               ~static_cast<uint64_t>(block_height - 1u);
	const uint64_t size          = static_cast<uint64_t>(pitch) * padded_height * bytes_per_element;
	if (size == 0 || size > UINT32_MAX) {
		return false;
	}
	total_size->size  = static_cast<uint32_t>(size);
	total_size->align = 65536;
	return true;
}

bool TileGetRenderTargetMipLayout(uint32_t width, uint32_t height, uint32_t pitch,
                                  uint32_t bytes_per_element, uint32_t levels,
                                  TileSizeAlign* total_size, TileSizeOffset* level_sizes,
                                  TilePaddedSize* padded_size) {
	*total_size = {};
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
	                   Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget), total_size,
	                   level_sizes, padded_size);
	return total_size->size != 0 && total_size->align == 65536;
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

	uint32_t std256_bytes_per_element       = 0;
	uint32_t std256_texels_per_element_wide = 0;
	uint32_t std256_texels_per_element_tall = 0;
	uint32_t std256_block_width_log2        = 0;
	uint32_t std256_block_height_log2       = 0;
	if (tile == 1 &&
	    Gen5Standard256BLayout(format, &std256_bytes_per_element, &std256_texels_per_element_wide,
	                           &std256_texels_per_element_tall, &std256_block_width_log2,
	                           &std256_block_height_log2)) {
		uint32_t offset     = 0;
		uint32_t mip_pitch  = pitch;
		uint32_t mip_height = height;
		for (uint32_t l = 0; l < levels; l++) {
			const uint32_t row_elements = std::max(
			    (mip_pitch + std256_texels_per_element_wide - 1u) / std256_texels_per_element_wide,
			    1u);
			const uint32_t height_elements = std::max(
			    (mip_height + std256_texels_per_element_tall - 1u) / std256_texels_per_element_tall,
			    1u);
			const uint32_t padded_width  = AlignUp(row_elements, 1u << std256_block_width_log2);
			const uint32_t padded_height = AlignUp(height_elements, 1u << std256_block_height_log2);
			const uint32_t size          = padded_width * padded_height * std256_bytes_per_element;

			if (level_sizes != nullptr) {
				level_sizes[l].size   = size;
				level_sizes[l].offset = offset;
			}
			if (padded_size != nullptr) {
				padded_size[l].width  = padded_width * std256_texels_per_element_wide;
				padded_size[l].height = padded_height * std256_texels_per_element_tall;
			}

			offset += size;
			mip_pitch  = std::max(mip_pitch / 2u, 1u);
			mip_height = std::max(mip_height / 2u, 1u);
		}

		if (total_size != nullptr) {
			total_size->size  = AlignUp(offset, 256u);
			total_size->align = 256;
		}

		return;
	}

	if (const uint32_t bytes_per_element = Prospero::NumBytesPerElement(format);
	    bytes_per_element != 0 && tile == 27 && levels == 1) {
		uint32_t block_width  = 0;
		uint32_t block_height = 0;
		EXIT_NOT_IMPLEMENTED(
		    !Gen5Thin64KBBlockSizeFromElementBytes(bytes_per_element, &block_width, &block_height));

		const uint32_t padded_width  = AlignUp(pitch, block_width);
		const uint32_t padded_height = AlignUp(height, block_height);
		const uint32_t size          = padded_width * padded_height * bytes_per_element;

		if (total_size != nullptr) {
			total_size->size  = size;
			total_size->align = 65536;
		}

		if (level_sizes != nullptr) {
			level_sizes[0].size   = size;
			level_sizes[0].offset = 0;
		}

		if (padded_size != nullptr) {
			padded_size[0].width  = padded_width;
			padded_size[0].height = padded_height;
		}

		return;
	}

	if (const uint32_t bytes_per_element = Prospero::NumBytesPerElement(format);
	    bytes_per_element != 0 && tile == 27 && levels > 1) {
		uint32_t block_width  = 0;
		uint32_t block_height = 0;
		EXIT_NOT_IMPLEMENTED(
		    !Gen5Thin64KBBlockSizeFromElementBytes(bytes_per_element, &block_width, &block_height));

		const uint32_t     bytes_log2        = IntLog2(bytes_per_element);
		const uint32_t     tail_width_limit  = block_width >> 1u;
		const uint32_t     tail_height_limit = block_height;
		constexpr uint32_t max_tail_levels   = 12u;

		uint32_t first_tail_level = levels;
		for (uint32_t l = 0; l < levels; l++) {
			const uint32_t mip_pitch  = std::max(ShiftCeil(pitch, l), 1u);
			const uint32_t mip_height = std::max(ShiftCeil(height, l), 1u);
			if (mip_pitch <= tail_width_limit && mip_height <= tail_height_limit &&
			    levels - l <= max_tail_levels) {
				first_tail_level = l;
				break;
			}
		}

		uint32_t offset = (first_tail_level < levels ? 65536u : 0u);

		for (int32_t l = static_cast<int32_t>(first_tail_level) - 1; l >= 0; l--) {
			const uint32_t level         = static_cast<uint32_t>(l);
			const uint32_t mip_pitch     = std::max(ShiftCeil(pitch, level), 1u);
			const uint32_t mip_height    = std::max(ShiftCeil(height, level), 1u);
			const uint32_t padded_width  = AlignUp(mip_pitch, block_width);
			const uint32_t padded_height = AlignUp(mip_height, block_height);
			const uint32_t size          = padded_width * padded_height * bytes_per_element;

			if (level_sizes != nullptr) {
				level_sizes[level].size       = size;
				level_sizes[level].offset     = offset;
				level_sizes[level].src_size   = size;
				level_sizes[level].src_offset = offset;
				level_sizes[level].x          = 0;
				level_sizes[level].y          = 0;
			}
			if (padded_size != nullptr) {
				padded_size[level].width  = padded_width;
				padded_size[level].height = padded_height;
			}

			offset += size;
		}

		uint32_t tail_linear_offset = 0;
		for (uint32_t l = first_tail_level; l < levels; l++) {
			const uint32_t mip_pitch   = std::max(ShiftCeil(pitch, l), 1u);
			const uint32_t mip_height  = std::max(ShiftCeil(height, l), 1u);
			const uint32_t linear_size = mip_pitch * mip_height * bytes_per_element;
			const auto&    tail_location =
			    GEN5_MIP_TAIL_LOCATIONS_THIN_64KB[bytes_log2][l - first_tail_level];
			if (level_sizes != nullptr) {
				level_sizes[l].size       = linear_size;
				level_sizes[l].offset     = tail_linear_offset;
				level_sizes[l].src_size   = 65536u;
				level_sizes[l].src_offset = 0;
				level_sizes[l].x          = tail_location.x;
				level_sizes[l].y          = tail_location.y;
			}
			if (padded_size != nullptr) {
				padded_size[l].width  = block_width;
				padded_size[l].height = block_height;
			}
			tail_linear_offset += AlignUp(linear_size, bytes_per_element);
		}
		EXIT_NOT_IMPLEMENTED(first_tail_level < levels && tail_linear_offset > 65536u);

		if (total_size != nullptr) {
			total_size->size  = AlignUp(offset, 65536u);
			total_size->align = 65536;
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
	uint32_t bytes_per_element       = 0;
	uint32_t texels_per_element_wide = 0;
	uint32_t texels_per_element_tall = 0;
	uint32_t block_width_log2        = 0;
	uint32_t block_height_log2       = 0;
	if (tile == 5 && levels == 1 &&
	    Gen5Standard4KBLayout(format, &bytes_per_element, &texels_per_element_wide,
	                          &texels_per_element_tall, &block_width_log2, &block_height_log2)) {
		const uint32_t row_elements =
		    std::max((pitch + texels_per_element_wide - 1u) / texels_per_element_wide, 1u);
		const uint32_t height_elements =
		    std::max((height + texels_per_element_tall - 1u) / texels_per_element_tall, 1u);
		const uint32_t block_width   = 1u << block_width_log2;
		const uint32_t block_height  = 1u << block_height_log2;
		const uint32_t padded_width  = (row_elements + block_width - 1u) & ~(block_width - 1u);
		const uint32_t padded_height = (height_elements + block_height - 1u) & ~(block_height - 1u);
		const uint32_t size          = padded_width * padded_height * bytes_per_element;

		if (total_size != nullptr) {
			total_size->size  = size;
			total_size->align = 4096;
		}

		if (level_sizes != nullptr) {
			level_sizes[0].size   = size;
			level_sizes[0].offset = 0;
		}

		if (padded_size != nullptr) {
			padded_size[0].width  = padded_width * texels_per_element_wide;
			padded_size[0].height = padded_height * texels_per_element_tall;
		}

		return;
	}

	if (tile == 5 && levels > 1 &&
	    Gen5Standard4KBLayout(format, &bytes_per_element, &texels_per_element_wide,
	                          &texels_per_element_tall, &block_width_log2, &block_height_log2)) {
		const uint32_t row_elements0 =
		    std::max((pitch + texels_per_element_wide - 1u) / texels_per_element_wide, 1u);
		const uint32_t height_elements0 =
		    std::max((height + texels_per_element_tall - 1u) / texels_per_element_tall, 1u);
		const uint32_t block_width       = 1u << block_width_log2;
		const uint32_t block_height      = 1u << block_height_log2;
		const uint32_t bytes_log2        = IntLog2(bytes_per_element);
		const uint32_t tail_width_limit  = block_width >> 1u;
		const uint32_t tail_height_limit = block_height;

		uint32_t first_tail_level = levels;
		for (uint32_t l = 0; l < levels; l++) {
			const uint32_t row_elements    = std::max(ShiftCeil(row_elements0, l), 1u);
			const uint32_t height_elements = std::max(ShiftCeil(height_elements0, l), 1u);
			if (row_elements <= tail_width_limit && height_elements <= tail_height_limit &&
			    levels - l <= 8) {
				first_tail_level = l;
				break;
			}
		}

		uint32_t offset = (first_tail_level < levels ? 4096u : 0u);

		for (int32_t l = static_cast<int32_t>(first_tail_level) - 1; l >= 0; l--) {
			const uint32_t level           = static_cast<uint32_t>(l);
			const uint32_t row_elements    = std::max(ShiftCeil(row_elements0, level), 1u);
			const uint32_t height_elements = std::max(ShiftCeil(height_elements0, level), 1u);
			const uint32_t padded_width    = AlignUp(row_elements, block_width);
			const uint32_t padded_height   = AlignUp(height_elements, block_height);
			const uint32_t size            = padded_width * padded_height * bytes_per_element;

			if (level_sizes != nullptr) {
				level_sizes[level].size       = size;
				level_sizes[level].offset     = offset;
				level_sizes[level].src_size   = size;
				level_sizes[level].src_offset = offset;
				level_sizes[level].x          = 0;
				level_sizes[level].y          = 0;
			}
			if (padded_size != nullptr) {
				padded_size[level].width  = padded_width * texels_per_element_wide;
				padded_size[level].height = padded_height * texels_per_element_tall;
			}

			offset += size;
		}

		uint32_t tail_linear_offset = 0;
		for (uint32_t l = first_tail_level; l < levels; l++) {
			const uint32_t row_elements    = std::max(ShiftCeil(row_elements0, l), 1u);
			const uint32_t height_elements = std::max(ShiftCeil(height_elements0, l), 1u);
			const uint32_t linear_size     = row_elements * height_elements * bytes_per_element;
			const auto&    tail_location =
			    GEN5_MIP_TAIL_LOCATIONS_THIN_4KB[bytes_log2][l - first_tail_level];
			if (level_sizes != nullptr) {
				level_sizes[l].size       = linear_size;
				level_sizes[l].offset     = tail_linear_offset;
				level_sizes[l].src_size   = 4096u;
				level_sizes[l].src_offset = 0;
				level_sizes[l].x          = tail_location.x;
				level_sizes[l].y          = tail_location.y;
			}
			if (padded_size != nullptr) {
				padded_size[l].width  = block_width * texels_per_element_wide;
				padded_size[l].height = block_height * texels_per_element_tall;
			}
			tail_linear_offset += AlignUp(linear_size, bytes_per_element);
		}
		EXIT_NOT_IMPLEMENTED(first_tail_level < levels && tail_linear_offset > 4096u);

		if (total_size != nullptr) {
			total_size->size  = AlignUp(offset, 4096u);
			total_size->align = 4096;
		}

		return;
	}

	if (tile == 24 && levels == 1) {
		const uint32_t bytes_per_element = Prospero::NumBytesPerElement(format);
		if (bytes_per_element != 0) {
			uint32_t   block_width  = 0;
			uint32_t   block_height = 0;
			const bool supported =
			    bytes_per_element <= 4 && Gen5Thin64KBBlockSizeFromElementBytes(
			                                  bytes_per_element, &block_width, &block_height);

			if (supported) {
				const uint32_t padded_width  = (pitch + block_width - 1u) & ~(block_width - 1u);
				const uint32_t padded_height = AlignUp(height, block_height);
				const uint32_t size          = padded_width * padded_height * bytes_per_element;

				if (total_size != nullptr) {
					total_size->size  = size;
					total_size->align = 65536;
				}

				if (level_sizes != nullptr) {
					level_sizes[0].size   = size;
					level_sizes[0].offset = 0;
				}

				if (padded_size != nullptr) {
					padded_size[0].width  = padded_width;
					padded_size[0].height = padded_height;
				}

				return;
			}
		}
	}

	if (tile == 24 && levels > 1) {
		const uint32_t bytes_per_element = Prospero::NumBytesPerElement(format);
		if (bytes_per_element != 0) {
			uint32_t   block_width  = 0;
			uint32_t   block_height = 0;
			const bool supported =
			    bytes_per_element <= 4 && Gen5Thin64KBBlockSizeFromElementBytes(
			                                  bytes_per_element, &block_width, &block_height);

			if (supported) {
				static bool logged = false;
				if (!logged) {
					LOGF("\t temporary: sizing PS5 depth tiled texture with mips, format = %u, "
					     "levels = %u\n",
					     format, levels);
					logged = true;
				}

				uint32_t offset     = 0;
				uint32_t mip_pitch  = pitch;
				uint32_t mip_height = height;
				for (uint32_t l = 0; l < levels; l++) {
					offset = AlignUp(offset, 65536u);

					const uint32_t padded_width  = AlignUp(mip_pitch, block_width);
					const uint32_t padded_height = AlignUp(mip_height, block_height);
					const uint32_t size          = padded_width * padded_height * bytes_per_element;

					if (level_sizes != nullptr) {
						level_sizes[l].size   = size;
						level_sizes[l].offset = offset;
					}
					if (padded_size != nullptr) {
						padded_size[l].width  = padded_width;
						padded_size[l].height = padded_height;
					}

					offset += size;
					mip_pitch  = std::max(mip_pitch / 2u, 1u);
					mip_height = std::max(mip_height / 2u, 1u);
				}

				if (total_size != nullptr) {
					total_size->size  = AlignUp(offset, 65536u);
					total_size->align = 65536;
				}

				return;
			}
		}
	}

	uint32_t std64_bytes_per_element       = 0;
	uint32_t std64_texels_per_element_wide = 0;
	uint32_t std64_texels_per_element_tall = 0;
	uint32_t std64_block_width_log2        = 0;
	uint32_t std64_block_height_log2       = 0;
	if (tile == 9 && levels > 1 &&
	    Gen5Standard64KBLayout(format, &std64_bytes_per_element, &std64_texels_per_element_wide,
	                           &std64_texels_per_element_tall, &std64_block_width_log2,
	                           &std64_block_height_log2)) {
		const uint32_t row_elements0 = std::max(
		    (pitch + std64_texels_per_element_wide - 1u) / std64_texels_per_element_wide, 1u);
		const uint32_t height_elements0 = std::max(
		    (height + std64_texels_per_element_tall - 1u) / std64_texels_per_element_tall, 1u);
		const uint32_t     block_width       = 1u << std64_block_width_log2;
		const uint32_t     block_height      = 1u << std64_block_height_log2;
		const uint32_t     bytes_log2        = IntLog2(std64_bytes_per_element);
		const uint32_t     tail_width_limit  = block_width >> 1u;
		const uint32_t     tail_height_limit = block_height;
		constexpr uint32_t max_tail_levels   = 12u;

		uint32_t first_tail_level = levels;
		for (uint32_t l = 0; l < levels; l++) {
			const uint32_t row_elements    = std::max(ShiftCeil(row_elements0, l), 1u);
			const uint32_t height_elements = std::max(ShiftCeil(height_elements0, l), 1u);
			if (row_elements <= tail_width_limit && height_elements <= tail_height_limit &&
			    levels - l <= max_tail_levels) {
				first_tail_level = l;
				break;
			}
		}

		uint32_t offset = (first_tail_level < levels ? 65536u : 0u);

		for (int32_t l = static_cast<int32_t>(first_tail_level) - 1; l >= 0; l--) {
			const uint32_t level           = static_cast<uint32_t>(l);
			const uint32_t row_elements    = std::max(ShiftCeil(row_elements0, level), 1u);
			const uint32_t height_elements = std::max(ShiftCeil(height_elements0, level), 1u);
			const uint32_t padded_width    = AlignUp(row_elements, block_width);
			const uint32_t padded_height   = AlignUp(height_elements, block_height);
			const uint32_t size            = padded_width * padded_height * std64_bytes_per_element;

			if (level_sizes != nullptr) {
				level_sizes[level].size       = size;
				level_sizes[level].offset     = offset;
				level_sizes[level].src_size   = size;
				level_sizes[level].src_offset = offset;
				level_sizes[level].x          = 0;
				level_sizes[level].y          = 0;
			}
			if (padded_size != nullptr) {
				padded_size[level].width  = padded_width * std64_texels_per_element_wide;
				padded_size[level].height = padded_height * std64_texels_per_element_tall;
			}

			offset += size;
		}

		uint32_t tail_linear_offset = 0;
		for (uint32_t l = first_tail_level; l < levels; l++) {
			const uint32_t row_elements    = std::max(ShiftCeil(row_elements0, l), 1u);
			const uint32_t height_elements = std::max(ShiftCeil(height_elements0, l), 1u);
			const uint32_t linear_size = row_elements * height_elements * std64_bytes_per_element;
			const auto&    tail_location =
			    GEN5_MIP_TAIL_LOCATIONS_THIN_64KB[bytes_log2][l - first_tail_level];
			if (level_sizes != nullptr) {
				level_sizes[l].size       = linear_size;
				level_sizes[l].offset     = tail_linear_offset;
				level_sizes[l].src_size   = 65536u;
				level_sizes[l].src_offset = 0;
				level_sizes[l].x          = tail_location.x;
				level_sizes[l].y          = tail_location.y;
			}
			if (padded_size != nullptr) {
				padded_size[l].width  = block_width * std64_texels_per_element_wide;
				padded_size[l].height = block_height * std64_texels_per_element_tall;
			}
			tail_linear_offset += AlignUp(linear_size, std64_bytes_per_element);
		}
		EXIT_NOT_IMPLEMENTED(first_tail_level < levels && tail_linear_offset > 65536u);

		if (total_size != nullptr) {
			total_size->size  = AlignUp(offset, 65536u);
			total_size->align = 65536;
		}

		return;
	}
	if (tile == 9 && levels == 1 &&
	    Gen5Standard64KBLayout(format, &std64_bytes_per_element, &std64_texels_per_element_wide,
	                           &std64_texels_per_element_tall, &std64_block_width_log2,
	                           &std64_block_height_log2)) {
		const uint32_t row_elements = std::max(
		    (pitch + std64_texels_per_element_wide - 1u) / std64_texels_per_element_wide, 1u);
		const uint32_t height_elements = std::max(
		    (height + std64_texels_per_element_tall - 1u) / std64_texels_per_element_tall, 1u);
		const uint32_t block_width   = 1u << std64_block_width_log2;
		const uint32_t block_height  = 1u << std64_block_height_log2;
		const uint32_t padded_width  = AlignUp(row_elements, block_width);
		const uint32_t padded_height = AlignUp(height_elements, block_height);
		const uint32_t size          = padded_width * padded_height * std64_bytes_per_element;

		if (total_size != nullptr) {
			total_size->size  = size;
			total_size->align = 65536;
		}

		if (level_sizes != nullptr) {
			level_sizes[0].size   = size;
			level_sizes[0].offset = 0;
		}

		if (padded_size != nullptr) {
			padded_size[0].width  = padded_width * std64_texels_per_element_wide;
			padded_size[0].height = padded_height * std64_texels_per_element_tall;
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
                             TileSizeAlign* total_size) {
	EXIT_NOT_IMPLEMENTED(depth == 0);

	TileSizeAlign slice_size {};
	TileGetTextureSize(format, width, height, pitch, levels, tile, &slice_size, nullptr, nullptr);

	*total_size    = slice_size;
	uint64_t total = static_cast<uint64_t>(slice_size.size) * depth;

	uint32_t bytes_per_element       = 0;
	uint32_t texels_per_element_wide = 0;
	uint32_t texels_per_element_tall = 0;
	uint32_t block_width_log2        = 0;
	uint32_t block_height_log2       = 0;
	uint32_t block_depth_log2        = 0;

	if (volume_texture && depth > 1 && tile == 5 &&
	    TileGetStandard4KBVolumeLayout(format, &bytes_per_element, &texels_per_element_wide,
	                                   &texels_per_element_tall, &block_width_log2,
	                                   &block_height_log2, &block_depth_log2)) {
		const uint32_t row_elements0 =
		    std::max((pitch + texels_per_element_wide - 1u) / texels_per_element_wide, 1u);
		const uint32_t height_elements0 =
		    std::max((height + texels_per_element_tall - 1u) / texels_per_element_tall, 1u);
		const uint32_t block_width      = 1u << block_width_log2;
		const uint32_t block_height     = 1u << block_height_log2;
		const uint32_t block_depth      = 1u << block_depth_log2;
		uint64_t       block_slice_size = 0;

		if (levels > 1) {
			const uint32_t     tail_width_limit  = block_width;
			const uint32_t     tail_height_limit = block_height >> 1u;
			constexpr uint32_t max_tail_levels   = 5u;

			for (uint32_t l = 0; l < levels; l++) {
				const uint32_t row_elements    = std::max(ShiftCeil(row_elements0, l), 1u);
				const uint32_t height_elements = std::max(ShiftCeil(height_elements0, l), 1u);
				if (row_elements <= tail_width_limit && height_elements <= tail_height_limit &&
				    levels - l <= max_tail_levels) {
					block_slice_size += 4096u;
					break;
				}

				block_slice_size += static_cast<uint64_t>(block_depth) *
				                    AlignUp(row_elements, block_width) *
				                    AlignUp(height_elements, block_height) * bytes_per_element;
			}
		} else {
			block_slice_size = static_cast<uint64_t>(block_depth) *
			                   AlignUp(row_elements0, block_width) *
			                   AlignUp(height_elements0, block_height) * bytes_per_element;
		}

		total             = block_slice_size * ShiftCeil(depth, block_depth_log2);
		total_size->align = 4096;
	}

	EXIT_NOT_IMPLEMENTED(total > 0xffffffffull);
	total_size->size = static_cast<uint32_t>(total);
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
	if (tile == 9 &&
	    Gen5Standard64KBLayout(format, &bytes_per_element, &texels_per_element_wide,
	                           &texels_per_element_tall, &block_width_log2, &block_height_log2)) {
		pitch = AlignUp(pitch, (1u << block_width_log2) * texels_per_element_wide);
	}
	if (tile == 24) {
		const uint32_t bytes_per_element = Prospero::NumBytesPerElement(format);
		uint32_t       block_width       = 0;
		uint32_t       block_height      = 0;
		if (bytes_per_element <= 4 &&
		    Gen5Thin64KBBlockSizeFromElementBytes(bytes_per_element, &block_width, &block_height)) {
			pitch = AlignUp(pitch, block_width);
		}
	}

	return pitch;
}

} // namespace Libs::Graphics
