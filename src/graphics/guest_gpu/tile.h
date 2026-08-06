#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Graphics {

struct TileSizeAlign {
	uint32_t size  = 0;
	uint32_t align = 0;
};

struct TileSizeOffset {
	uint32_t size       = 0;
	uint32_t offset     = 0;
	uint32_t src_size   = 0;
	uint32_t src_offset = 0;
	uint32_t x          = 0;
	uint32_t y          = 0;
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
	uint32_t             format    = 0;
	uint32_t             tile_mode = 0;
	TileSurfaceDimension dimension = TileSurfaceDimension::Dim2D;
	uint32_t             width     = 0;
	uint32_t             height    = 0;
	uint32_t             depth     = 1;
	uint32_t             levels    = 1;
	uint32_t             layers    = 1;
};

// Width/height fields are measured in addressable elements, not texture texels.
struct TileMipLayout {
	uint64_t offset        = 0;
	uint64_t size          = 0;
	uint32_t width         = 0;
	uint32_t height        = 0;
	uint32_t padded_width  = 0;
	uint32_t padded_height = 0;
	uint32_t tail_x        = 0;
	uint32_t tail_y        = 0;
};

struct TileSurfaceLayout {
	TileSurfaceDescription description;
	TileTextureBlockLayout texture;
	uint32_t               first_tail_level = 0;
	uint64_t               block_slice_size = 0;
	uint64_t               total_size       = 0;
	TileMipLayout          mips[16]         = {};
};

bool TileGetBlockLayout(TileBlockFamily family, uint32_t bytes_per_element,
                        TileBlockLayout& layout);
bool TileGetTextureElementLayout(uint32_t format, TileTextureElementLayout& layout);
bool TileGetTextureBlockLayout(uint32_t format, uint32_t tile, bool volume,
                               TileTextureBlockLayout& layout);
bool TileGetTiledTextureLayout(const TileSurfaceDescription& description,
                               TileSurfaceLayout&            layout);
bool TileGetBlockOffset(const TileBlockLayout& layout, uint32_t x, uint32_t y, uint32_t z,
                        uint32_t& byte_offset);
bool TileGetBlockXor(const TileBlockLayout& layout, uint32_t block_x, uint32_t block_y,
                     uint32_t& byte_offset);
bool TileGetBlockXor(const TileBlockLayout& layout, uint32_t block_x, uint32_t block_y,
                     uint32_t block_z, uint32_t& byte_offset);

bool     TileGetDepthSize(uint32_t width, uint32_t height, uint32_t pitch, uint32_t z_format,
                          uint32_t stencil_format, bool htile, TileSizeAlign& stencil_size,
                          TileSizeAlign& htile_size, TileSizeAlign& depth_size,
                          uint32_t num_fragments_log2 = 0);
uint32_t TileGetRenderTargetPitch(uint32_t width, uint32_t bytes_per_element,
                                  uint32_t num_fragments_log2 = 0);
uint32_t TileGetDepthPitch(uint32_t width, uint32_t bytes_per_element,
                           uint32_t num_fragments_log2 = 0);
bool     TileGetRenderTargetSize(uint32_t width, uint32_t height, uint32_t pitch,
                                 uint32_t bytes_per_element, TileSizeAlign& total_size,
                                 uint32_t num_fragments_log2 = 0);
bool     TileGetRenderTargetMipLayout(uint32_t width, uint32_t height, uint32_t pitch,
                                      uint32_t bytes_per_element, uint32_t levels,
                                      TileSizeAlign& total_size, TileSizeOffset* level_sizes,
                                      TilePaddedSize* padded_size);
void     TileGetTextureSize(uint32_t format, uint32_t width, uint32_t height, uint32_t levels,
                            uint32_t tile, TileSizeAlign* total_size, TileSizeOffset* level_sizes,
                            TilePaddedSize* padded_size);
void     TileGetTextureTotalSize(uint32_t format, uint32_t width, uint32_t height, uint32_t depth,
                                 uint32_t levels, uint32_t tile, bool volume_texture,
                                 TileSizeAlign& total_size);
uint32_t TileGetTexturePitch(uint32_t format, uint32_t width, uint32_t tile);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_ */
