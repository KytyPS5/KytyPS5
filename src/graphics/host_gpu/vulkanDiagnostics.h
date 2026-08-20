#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDIAGNOSTICS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDIAGNOSTICS_H_

#include <string>

namespace Libs::Graphics {

// The Vulkan half of the --diagnostics report: every physical device on this
// machine, its driver, its memory heaps, and which of the extensions this
// emulator actually looks for are present.
//
// Creates its own instance and destroys it again. It does not need a window, a
// surface or a device, so it works on a machine where the emulator itself does
// not start - which is exactly when someone needs to file a report.
//
// Never throws and never exits: a machine with no usable Vulkan gets a report
// saying so, because that is the single most useful thing it could say.
[[nodiscard]] std::string BuildVulkanReport();

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDIAGNOSTICS_H_ */
