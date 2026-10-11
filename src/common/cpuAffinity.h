#ifndef EMULATOR_SRC_COMMON_CPUAFFINITY_H_
#define EMULATOR_SRC_COMMON_CPUAFFINITY_H_

namespace Common {

// On a CPU with cores of different speeds, keeps the calling thread on the fastest cores. A no-op
// on CPUs whose cores are all alike and on macOS. `name` is for the log line.
void PreferPerformanceCores(const char* name);

} // namespace Common

#endif // EMULATOR_SRC_COMMON_CPUAFFINITY_H_
