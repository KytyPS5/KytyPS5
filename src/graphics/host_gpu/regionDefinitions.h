#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONDEFINITIONS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONDEFINITIONS_H_

#include "common/bitArray.h"
#include "common/common.h"

#include <compare>

namespace Libs::Graphics {

constexpr uint64_t TRACKER_PAGE_SIZE    = 4ull * 1024ull;
constexpr uint64_t TRACKER_REGION_SIZE  = 4ull * 1024ull * 1024ull;
constexpr uint64_t TRACKER_ADDRESS_SIZE = 1ull << 40u;
constexpr size_t   TRACKER_REGION_PAGES = TRACKER_REGION_SIZE / TRACKER_PAGE_SIZE;

struct GuestRange {
	uint64_t address = 0;
	uint64_t size    = 0;

	[[nodiscard]] constexpr bool Empty() const noexcept { return address == 0 && size == 0; }
	[[nodiscard]] constexpr bool Valid() const noexcept {
		return address != 0 && size != 0 && address < TRACKER_ADDRESS_SIZE &&
		       size <= TRACKER_ADDRESS_SIZE - address;
	}
	[[nodiscard]] constexpr bool     ValidOrEmpty() const noexcept { return Empty() || Valid(); }
	[[nodiscard]] constexpr uint64_t End() const noexcept { return address + size; }
	auto                             operator<=>(const GuestRange&) const = default;
};

enum class DirtySource { Cpu, Gpu };
using RegionBits = Common::BitArray<TRACKER_REGION_PAGES>;
static_assert(sizeof(RegionBits) == TRACKER_REGION_PAGES / 8);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONDEFINITIONS_H_
