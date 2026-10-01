#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_

namespace Loader::Timer {

void   Start();
double GetTimeMs();

struct Lifecycle {
	static constexpr const char* name       = "Timer";
	static constexpr auto        initialize = Loader::Timer::Start;
};

} // namespace Loader::Timer

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_ */
