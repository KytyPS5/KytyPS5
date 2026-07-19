#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Graphics {

enum class TileMode {
	VideoOutLinear,
	VideoOutTiled,
	TextureLinear,
	TextureTiled,
	// RenderTextureLinear,
	// RenderTextureTiled,
};

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

void TileInit();
void TileConvertTiledToLinear(void* dst, const void* src, TileMode mode, uint32_t width,
                              uint32_t height);
void TileConvertTiledToLinearRenderTarget(void* dst, const void* src, uint32_t width,
                                          uint32_t height, uint32_t pitch,
                                          uint32_t bytes_per_element, uint64_t size,
                                          uint64_t src_size = 0, uint32_t src_x = 0,
                                          uint32_t src_y = 0);
void TileConvertLinearToTiledRenderTarget(void* dst, const void* src, uint32_t width,
                                          uint32_t height, uint32_t pitch,
                                          uint32_t bytes_per_element, uint64_t size,
                                          uint64_t dst_size = 0, uint32_t dst_x = 0,
                                          uint32_t dst_y = 0);
void TileConvertTiledToLinearStandard64KB(void* dst, const void* src, uint32_t format,
                                          uint32_t width, uint32_t height, uint32_t pitch,
                                          uint64_t size, uint64_t src_size = 0, uint32_t src_x = 0,
                                          uint32_t src_y = 0);
void TileConvertTiledToLinearStandard64KB32(void* dst, const void* src, uint32_t width,
                                            uint32_t height, uint32_t pitch, uint64_t size,
                                            uint64_t src_size = 0, uint32_t src_x = 0,
                                            uint32_t src_y = 0);
void TileConvertLinearToTiledStandard64KB32(void* dst, const void* src, uint32_t width,
                                            uint32_t height, uint32_t pitch, uint64_t size);
void TileConvertTiledToLinearStandard64KB16(void* dst, const void* src, uint32_t width,
                                            uint32_t height, uint32_t pitch, uint64_t size,
                                            uint64_t src_size = 0, uint32_t src_x = 0,
                                            uint32_t src_y = 0);
void TileConvertTiledToLinearDepth(void* dst, const void* src, uint32_t format, uint32_t width,
                                   uint32_t height, uint32_t pitch, uint64_t size);
void TileConvertLinearToTiledDepth(void* dst, const void* src, uint32_t format, uint32_t width,
                                   uint32_t height, uint32_t pitch, uint64_t size);
void TileConvertTiledToLinearStandard4KB(void* dst, const void* src, uint32_t format,
                                         uint32_t width, uint32_t height, uint32_t pitch,
                                         uint64_t dst_size, uint64_t src_size, uint32_t src_x = 0,
                                         uint32_t src_y = 0);
void TileConvertTiledToLinearStandard256B(void* dst, const void* src, uint32_t format,
                                          uint32_t width, uint32_t height, uint32_t pitch,
                                          uint64_t dst_size, uint64_t src_size);
bool TileIsStandard256BTextureSupported(uint32_t format);
bool TileIsStandard4KBTextureSupported(uint32_t format);
bool TileIsStandard64KBTextureSupported(uint32_t format);
bool TileGetStandard4KBVolumeLayout(uint32_t format, uint32_t* bytes_per_element,
                                    uint32_t* texels_per_element_wide,
                                    uint32_t* texels_per_element_tall, uint32_t* block_width_log2,
                                    uint32_t* block_height_log2, uint32_t* block_depth_log2);

bool     TileGetDepthSize(uint32_t width, uint32_t height, uint32_t pitch, uint32_t z_format,
                          uint32_t stencil_format, bool htile, TileSizeAlign* stencil_size,
                          TileSizeAlign* htile_size, TileSizeAlign* depth_size);
uint32_t TileGetRenderTargetPitch(uint32_t width, uint32_t bytes_per_element);
bool     TileGetRenderTargetSize(uint32_t width, uint32_t height, uint32_t pitch,
                                 uint32_t bytes_per_element, TileSizeAlign* total_size);
bool     TileGetRenderTargetMipLayout(uint32_t width, uint32_t height, uint32_t pitch,
                                      uint32_t bytes_per_element, uint32_t levels,
                                      TileSizeAlign* total_size, TileSizeOffset* level_sizes,
                                      TilePaddedSize* padded_size);
void     TileGetTextureSize(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                            uint32_t levels, uint32_t tile, TileSizeAlign* total_size,
                            TileSizeOffset* level_sizes, TilePaddedSize* padded_size);
void TileGetTextureTotalSize(uint32_t format, uint32_t width, uint32_t height, uint32_t depth,
                             uint32_t pitch, uint32_t levels, uint32_t tile, bool volume_texture,
                             TileSizeAlign* total_size);
uint32_t TileGetTexturePitch(uint32_t format, uint32_t width, uint32_t levels, uint32_t tile);
void     TileConvertTiledToLinearStandard4KB3D(void* dst, const void* src, uint32_t format,
                                               uint32_t width, uint32_t height, uint32_t depth,
                                               uint32_t pitch, uint64_t dst_slice_stride,
                                               uint64_t dst_size, uint64_t src_size,
                                               bool clear_dst = true);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_ */
