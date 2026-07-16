#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/dateTime.h"
#include "common/subsystems.h"

namespace Loader::Timer {

KYTY_SUBSYSTEM_DEFINE(Timer);

void         Start();
double       GetTimeMs();
Common::Time GetTime();
uint64_t     GetCounter();
uint64_t     GetFrequency();

} // namespace Loader::Timer

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_ */
