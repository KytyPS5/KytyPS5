#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONDEFINITIONS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONDEFINITIONS_H_

#include "common/bitArray.h"
#include "common/common.h"

namespace Libs::Graphics {

constexpr uint64_t TRACKER_PAGE_SIZE    = 4ull * 1024ull;
constexpr uint64_t TRACKER_REGION_SIZE  = 4ull * 1024ull * 1024ull;
constexpr uint64_t TRACKER_ADDRESS_SIZE = 1ull << 40u;
constexpr size_t   TRACKER_REGION_PAGES = TRACKER_REGION_SIZE / TRACKER_PAGE_SIZE;

enum class DirtySource { Cpu, Gpu };
using RegionBits = Common::BitArray<TRACKER_REGION_PAGES>;
static_assert(sizeof(RegionBits) == TRACKER_REGION_PAGES / 8);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONDEFINITIONS_H_
