#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"

#include <bit>

namespace Libs::Graphics {

struct TileSizeAlign {
	uint32_t size  = 0;
	uint32_t align = 0;
};

struct TileSizeOffset {
	uint32_t size   = 0;
	uint32_t offset = 0;
};

struct TilePaddedSize {
	uint32_t width  = 0;
	uint32_t height = 0;
};

enum class TileBlockFamily : uint32_t {
	Standard256B,
	Standard4KB,
	Standard4KB3D,
	Standard64KB,
	Standard64KB3D,
	Prt64KB,
	Prt64KB3D,
	RenderTarget64KB,
	Depth64KB,
	Count,
};

struct TileBlockLayout {
	TileBlockFamily family            = TileBlockFamily::Count;
	uint32_t        bytes_per_element = 0;
	uint32_t        block_size        = 0;
	uint32_t        block_width       = 0;
	uint32_t        block_height      = 0;
	uint32_t        block_depth       = 0;
};

struct TileTextureElementLayout {
	uint32_t bytes        = 0;
	uint32_t texel_width  = 1;
	uint32_t texel_height = 1;
};

struct TileTextureBlockLayout {
	TileBlockLayout block;
	uint32_t        texel_width  = 1;
	uint32_t        texel_height = 1;
};

enum class TileSurfaceDimension : uint32_t {
	Dim2D,
	Dim3D,
};

struct TileSurfaceDescription {
	Prospero::BufferFormat format    = Prospero::BufferFormat::kInvalid;
	Prospero::TileMode     tile_mode = Prospero::TileMode::kLinear;
	TileSurfaceDimension   dimension = TileSurfaceDimension::Dim2D;
	uint32_t               width     = 0;
	uint32_t               height    = 0;
	uint32_t               depth     = 1;
	uint32_t               levels    = 1;
	uint32_t               layers    = 1;
};

// Guest mip chains run backwards within each layer or volume block-slice. Tail levels share an
// offset and use tail_x/tail_y within that block. Pitch and height are padded dimensions; they and
// the tail coordinates count addressable elements (compressed blocks for BC formats).
struct TileMipLayout {
	uint64_t offset = 0;
	uint64_t size   = 0;
	uint32_t pitch  = 0;
	uint32_t height = 0;
	uint32_t tail_x = 0;
	uint32_t tail_y = 0;

	bool operator==(const TileMipLayout&) const = default;
};

struct TileSurfaceLayout {
	TileTextureBlockLayout texture;
	uint32_t               first_tail_level = 0;
	uint64_t               block_slice_size = 0;
	uint64_t               total_size       = 0;
};

bool TileGetBlockLayout(TileBlockFamily family, uint32_t bytes_per_element,
                        TileBlockLayout& layout);
// Inline, so callers keep the layout in registers instead of reloading it from memory.
inline bool TileGetTextureElementLayout(Prospero::BufferFormat format,
                                        TileTextureElementLayout& layout) {
	if (const auto bytes = Prospero::NumBytesPerElement(format); std::has_single_bit(bytes)) {
		layout = {bytes, 1, 1};
		return true;
	}
	if (const auto bytes = Prospero::BlockCompressedBytesPerBlock(format); bytes != 0) {
		layout = {bytes, 4, 4};
		return true;
	}
	return false;
}
bool TileGetTextureBlockLayout(Prospero::BufferFormat format, Prospero::TileMode tile, bool volume,
                               TileTextureBlockLayout& layout);
// mips receives the description.levels levels of one block slice.
bool TileGetTiledTextureLayout(const TileSurfaceDescription& description, TileSurfaceLayout& layout,
                               TileMipLayout* mips);
bool TileGetBlockOffset(const TileBlockLayout& layout, uint32_t x, uint32_t y, uint32_t z,
                        uint32_t& byte_offset);
bool TileGetBlockXor(const TileBlockLayout& layout, uint32_t block_x, uint32_t block_y,
                     uint32_t block_z, uint32_t& byte_offset);

bool     TileGetDepthSize(uint32_t width, uint32_t height, Prospero::DepthFormat z_format,
                          Prospero::StencilFormat stencil_format, bool htile,
                          TileSizeAlign& stencil_size, TileSizeAlign& htile_size,
                          TileSizeAlign& depth_size, uint32_t num_fragments_log2 = 0);
uint32_t TileGetRenderTargetPitch(uint32_t width, uint32_t bytes_per_element,
                                  uint32_t num_fragments_log2 = 0);
bool     TileGetRenderTargetSize(uint32_t width, uint32_t height, uint32_t pitch,
                                 uint32_t bytes_per_element, TileSizeAlign& total_size,
                                 uint32_t num_fragments_log2 = 0);
// mips, when given, receives the size and offset within a slice of each level that has metadata.
bool     TileGetDccSize(uint32_t width, uint32_t height, uint32_t slices,
                        uint32_t bytes_per_element, uint32_t levels, Prospero::TileMode tile,
                        TileSizeAlign& total_size, uint32_t num_fragments_log2 = 0,
                        TileSizeOffset* mips = nullptr);
bool     TileGetCmaskSize(uint32_t width, uint32_t height, uint32_t slices, uint32_t levels,
                          TileSizeAlign& total_size);
void     TileGetTextureSize(Prospero::BufferFormat format, uint32_t width, uint32_t height,
                            uint32_t levels, Prospero::TileMode tile, TileSizeAlign* total_size,
                            TileSizeOffset* level_sizes, TilePaddedSize* padded_size);
void     TileGetTextureTotalSize(Prospero::BufferFormat format, uint32_t width, uint32_t height,
                                 uint32_t depth, uint32_t levels, Prospero::TileMode tile,
                                 bool volume_texture, TileSizeAlign& total_size);
uint32_t TileGetTexturePitch(Prospero::BufferFormat format, uint32_t width,
                             Prospero::TileMode tile);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_ */
