#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_PSSR_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_PSSR_H_

#include <cstdint>

namespace Libs::Graphics::Pssr {

struct Request {
	uint32_t output[8];
	uint32_t input[8];
	uint32_t width;
	uint32_t height;
};

constexpr uint32_t RequestDwords = sizeof(Request) / sizeof(uint32_t);
static_assert(sizeof(Request) == 72);

} // namespace Libs::Graphics::Pssr

#endif // EMULATOR_SRC_GRAPHICS_GUEST_GPU_PSSR_H_
