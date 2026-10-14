#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORMETADATAFILL_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORMETADATAFILL_H_

#include <cstdint>
#include <optional>

namespace Libs::Graphics {

// A guest marks a colour target as fast-cleared by filling its DCC or CMask block with one code
// byte. The fill arrives as a 32-bit uniform-fill dispatch; it writes a code only when all four
// bytes are the same.
[[nodiscard]] constexpr std::optional<uint8_t> ColorMetadataFillCode(uint32_t fill_value) {
	const auto byte = static_cast<uint8_t>(fill_value);
	if (fill_value != byte * 0x01010101u) {
		return std::nullopt;
	}
	return byte;
}

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORMETADATAFILL_H_ */
