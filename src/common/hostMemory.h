#ifndef EMULATOR_SRC_COMMON_HOSTMEMORY_H_
#define EMULATOR_SRC_COMMON_HOSTMEMORY_H_

#include "common/common.h"

namespace Common {

bool HostMemoryQueryReadable(uint64_t addr, uint64_t requested_size, uint64_t& readable_size);
bool HostMemoryRangeIsReadable(uint64_t addr, uint64_t size);

} // namespace Common

#endif // EMULATOR_SRC_COMMON_HOSTMEMORY_H_
