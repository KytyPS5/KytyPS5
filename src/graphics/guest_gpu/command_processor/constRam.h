#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_CONSTRAM_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_CONSTRAM_H_

#include <cstdint>

namespace Libs::Graphics {

// The constant engine's RAM, in dwords (48 KiB).
inline constexpr uint32_t CONST_RAM_DWORDS = 0x3000;

// Whether WRITE_CONST_RAM or DUMP_CONST_RAM stays inside the constant RAM: `offset` in bytes,
// dword aligned, and `dwords` long. A packet comes from the guest: the range is checked as a
// whole, not its offset and length apart.
[[nodiscard]] constexpr bool ConstRamRangeFits(uint32_t offset, uint32_t dwords) noexcept {
	const uint32_t first = offset / 4u;
	return (offset & 0x3u) == 0 && dwords < CONST_RAM_DWORDS && first < CONST_RAM_DWORDS &&
	       dwords <= CONST_RAM_DWORDS - first;
}

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_CONSTRAM_H_ */
