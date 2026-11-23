#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_PSSR_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_PSSR_H_

#include "graphics/guest_gpu/gpu_defs.h"

#include <cstdint>

namespace Libs::Graphics::Pssr {

enum Flags : uint32_t {
	kReverseDepth = 1u << 0, // depth 1 is near
	kAutoExposure = 1u << 1,
	kReset        = 1u << 2, // the history is discarded
	kSpatialOnly  = 1u << 3, // the inputs fit no temporal upscaler
};

// Zeroed depth, motion and exposure descriptors are absent inputs. Motion vectors times
// motion_scale are render-resolution pixels to the previous frame, y down, without jitter;
// jitter is the sample offset from the pixel center in pixels, y down.
struct Request {
	uint32_t output[8];
	uint32_t input[8];
	uint32_t width;
	uint32_t height;
	uint32_t depth[8];
	uint32_t motion[8];
	uint32_t exposure[8];
	uint64_t context; // owner of the history
	uint32_t x;
	uint32_t y;
	float    jitter[2];
	float    motion_scale[2];
	float    pre_exposure;
	uint32_t flags;
};

constexpr uint32_t RequestDwords = sizeof(Request) / sizeof(uint32_t);
static_assert(sizeof(Request) == 208);

inline bool IsColorFormat(Prospero::BufferFormat format) {
	switch (format) {
		case Prospero::BufferFormat::k11_11_10Float:
		case Prospero::BufferFormat::k10_11_11Float:
		case Prospero::BufferFormat::k16_16_16_16Float: return true;
		default: return false;
	}
}

} // namespace Libs::Graphics::Pssr

#endif // EMULATOR_SRC_GRAPHICS_GUEST_GPU_PSSR_H_
