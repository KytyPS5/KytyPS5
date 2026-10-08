#ifndef KYTY_GRAPHICS_PRESENTATION_UPSCALE_RUNTIME_H_
#define KYTY_GRAPHICS_PRESENTATION_UPSCALE_RUNTIME_H_

#include <filesystem>
#include <optional>

namespace Libs::Graphics {
// Resolve an explicit UTF-8 path or OptiScaler.dll beside the executable.
// Invalid paths never fall back to loading DLLs from the working directory.
std::optional<std::filesystem::path> OptiScalerRuntimePath();
} // namespace Libs::Graphics

#endif
