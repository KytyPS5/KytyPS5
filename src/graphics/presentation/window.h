#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Graphics {

class Presenter;

struct DisplayLuminance {
	float min_luminance            = 0.0f;
	float max_luminance            = 0.0f;
	float max_full_frame_luminance = 0.0f;
};

[[nodiscard]] Presenter& WindowInit(uint32_t width, uint32_t height);
void                     WindowRun();
void                     WindowShutdown();
// Luminance in nits of the monitor that shows the game window, as reported by the host OS.
// Returns false when the host does not report it.
[[nodiscard]] bool WindowGetDisplayLuminance(DisplayLuminance* luminance);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_ */
