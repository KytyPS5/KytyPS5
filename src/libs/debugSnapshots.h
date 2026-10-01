#ifndef EMULATOR_INCLUDE_LIBS_DEBUG_SNAPSHOTS_H_
#define EMULATOR_INCLUDE_LIBS_DEBUG_SNAPSHOTS_H_

#include <cstdint>
#include <nlohmann/json_fwd.hpp>

// Live-state snapshots served to the launcher's debug tools. Every snapshot is one table:
//   { "summary": { label: value, ... }, "columns": [ name, ... ], "rows": [ [ cell, ... ], ... ] }
// Each provider takes its subsystem's own lock, so the debug server thread can call it at any
// time while the guest runs.

namespace Libs::LibKernel::Memory {
nlohmann::json DebugMemorySnapshot();
// Reads guest bytes without faulting. "hex" holds two characters per byte, "??" where the range is
// not backed.
nlohmann::json DebugMemoryRead(uint64_t vaddr, uint32_t size);
} // namespace Libs::LibKernel::Memory

namespace Libs::LibKernel {
nlohmann::json DebugThreadSnapshot();
} // namespace Libs::LibKernel

namespace Libs::Audio {
nlohmann::json DebugAudioSnapshot();
} // namespace Libs::Audio

namespace Libs::Graphics {
nlohmann::json DebugGpuSnapshot();
} // namespace Libs::Graphics

namespace Loader {
nlohmann::json DebugImportSnapshot();
} // namespace Loader

namespace Libs::DebugServer {
void Start(uint16_t port);
} // namespace Libs::DebugServer

#endif /* EMULATOR_INCLUDE_LIBS_DEBUG_SNAPSHOTS_H_ */
