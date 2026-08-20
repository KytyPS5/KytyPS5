#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Graphics {

class Presenter;

[[nodiscard]] Presenter& WindowInit(uint32_t width, uint32_t height);
void                     WindowRun();
void                     WindowShutdown();

// Writes the driver pipeline cache to disk without destroying anything. The
// emulator's normal exit is std::quick_exit(), which runs no destructors, so
// this is what actually gets the cache written; WindowShutdown() still writes
// it on the paths that do unwind.
void                     WindowFlushCaches();

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_ */
