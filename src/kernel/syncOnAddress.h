#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNC_ON_ADDRESS_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNC_ON_ADDRESS_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::LibKernel::SyncOnAddress {

using signal_poll_func_t = void (*)();

int Wait32(volatile uint32_t* address, uint32_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll = nullptr);
int Wait64(volatile uint64_t* address, uint64_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll = nullptr);
int Wake(volatile void* address, int32_t count);

} // namespace Libs::LibKernel::SyncOnAddress

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNC_ON_ADDRESS_H_ */
