#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_REFRESHREGIONS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_REFRESHREGIONS_H_

#include "graphics/host_gpu/renderer/image/imageInfo.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

// A rectangle of one mip level and one array layer, measured in storage elements. Compressed
// formats count blocks, so x0, y0, width and height are not texels for them.
struct RefreshRegion {
	uint32_t level  = 0;
	uint32_t layer  = 0;
	uint32_t x0     = 0;
	uint32_t y0     = 0;
	uint32_t width  = 0;
	uint32_t height = 0;
	auto     operator<=>(const RefreshRegion&) const = default;
};

// A half-open byte range [lo, hi) relative to the start of ImageInfo::data.
struct RefreshByteRange {
	uint64_t lo = 0;
	uint64_t hi = 0;
	auto     operator<=>(const RefreshByteRange&) const = default;
};

// Rounds a byte range outward to whole elements and to 4 bytes, whichever is larger, and clamps
// it to the image. TiledRefreshRegions and LinearRefreshRegions apply this themselves, so callers
// only need it to size the buffer they fetch for the same range.
[[nodiscard]] RefreshByteRange RoundRefreshRange(const ImageInfo& info, uint64_t lo, uint64_t hi);

// The rectangles of a tiled 2D image that hold any byte of [lo, hi) of the guest data. Whole
// macro blocks are returned, clipped to each mip, and tail levels are returned whole. Volumes
// give an empty result because they are never refreshed in parts.
[[nodiscard]] std::vector<RefreshRegion> TiledRefreshRegions(const ImageInfo& info, uint64_t lo,
                                                             uint64_t hi);

// The rectangles of a guest-linear image that hold any byte of [lo, hi). Each mip and slice gives
// at most a partial first row, the full middle rows and a partial last row.
[[nodiscard]] std::vector<RefreshRegion> LinearRefreshRegions(const ImageInfo& info, uint64_t lo,
                                                              uint64_t hi);

// Buffer to image copies for regions whose texels sit in the linear layout BufferCopies uses,
// starting at buffer_base. Vulkan does not offset buffer addressing by imageOffset, so every
// bufferOffset includes the rectangle origin.
[[nodiscard]] std::vector<vk::BufferImageCopy>
RegionCopies(const ImageInfo& info, std::span<const RefreshRegion> regions, uint64_t buffer_base);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_REFRESHREGIONS_H_
