#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_IMAGEREADBACK_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_IMAGEREADBACK_H_

#include <cstdint>

namespace Libs::Graphics {

// Which images the GPU writes are copied back to guest memory after the submission that wrote
// them, so that the game's CPU can read them.
//
// A game reads back results the GPU computes: Unreal Engine renders the average brightness of a
// sky light into a 1x1 render target and reads that pixel with the CPU. In Crash Bandicoot 4 that
// target is tiled. Guest memory kept its old bytes, the CPU read a brightness of 0, the reflections
// were divided by it, and leaves and walls came out white.
//
// So every image of at most READBACK_TINY_EXTENT pixels on each side, with one layer and one
// slice, is copied back, tiled or linear: such images are read back far more often than sampled
// again, and each copy is a few bytes. Block-compressed images are left out; the GPU does not
// render them. Larger linear images are copied back only when the setting asks for it
// (--readback-linear-images).
inline constexpr uint32_t READBACK_TINY_EXTENT = 4;

struct ReadbackImage {
	uint32_t width            = 1;
	uint32_t height           = 1;
	uint32_t depth            = 1;
	uint32_t layers           = 1;
	bool     tiled            = false;
	bool     block_compressed = false;
};

[[nodiscard]] constexpr bool ReadsBackToGuest(const ReadbackImage& image,
                                              bool readback_linear_images) noexcept {
	const bool tiny = image.width <= READBACK_TINY_EXTENT &&
	                  image.height <= READBACK_TINY_EXTENT && image.depth == 1 &&
	                  image.layers == 1 && !image.block_compressed;
	return tiny || (readback_linear_images && !image.tiled);
}

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_IMAGEREADBACK_H_ */
