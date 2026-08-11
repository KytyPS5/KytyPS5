#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/dateTime.h"
namespace Loader::Timer {

void         Start();
double       GetTimeMs();
Common::Time GetTime();
uint64_t     GetCounter();
uint64_t     GetFrequency();

struct Lifecycle {
	static constexpr const char* name       = "Timer";
	static constexpr auto        initialize = Loader::Timer::Start;
};

} // namespace Loader::Timer

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_ */
