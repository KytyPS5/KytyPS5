#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_PSSR_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_PSSR_H_

#include <cstdint>

namespace Libs::Graphics::Pssr {

// PS5 Pro upscaling (PSSR) requests, routed from libScePsml to the GPU command processor.
//
// On a PS5 Pro, sceAgc-based games reserve space in their command buffer and call the Psml
// dispatch function, which writes the upscaler's GPU work there. Kyty instead fills that space
// with one IT_NOP/R_PSSR packet carrying a copy of the request, so the request runs in GPU
// order. Layout of the request (observed in Astro Bot):
//   +0x00..+0x37  pointers to image descriptors (T#), 7 slots, some may be null
//   +0x60         float 0.0, +0x64 float 1.0
//   +0x68         float (250 in Astro Bot gameplay; not the far plane)
//   +0x6c..+0xa7  projection: x scale +0x6c, y scale +0x80, depth terms +0x94/+0xa4
//   +0xac..+0xcf  three orthonormal vectors (likely the camera rotation)
//   +0xe8, +0xec  near and far planes
//   +0xe8..+0x107 render size and jitter-related values
//   +0x110        float 1.0, +0x114 byte: reset history
// Slots seen in Astro Bot: 0 output, 1 colour, 2 motion vectors, 3 depth, 4 previous depth,
// 5 exposure (1x1), 6 reactive.

constexpr uint32_t Magic        = 0x52535350u; // "PSSR"
constexpr uint32_t Version      = 1;
constexpr uint32_t ParamsDwords = 0x50; // 320 bytes copied from the request
constexpr uint32_t TextureSlots = 7;

struct Request {
	uint32_t magic;
	uint32_t version;
	uint64_t params_address;            // guest address of the request (for logs)
	uint32_t params[ParamsDwords];      // the request itself
	uint32_t textures[TextureSlots][8]; // T# behind each pointer slot, copied at dispatch
	uint32_t texture_mask;              // bit i: slot i had a descriptor
};

constexpr uint32_t RequestDwords = sizeof(Request) / sizeof(uint32_t);
static_assert(sizeof(Request) % sizeof(uint32_t) == 0);

} // namespace Libs::Graphics::Pssr

#endif // EMULATOR_SRC_GRAPHICS_GUEST_GPU_PSSR_H_
