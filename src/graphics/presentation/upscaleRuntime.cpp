#include "graphics/presentation/upscaleRuntime.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"

#include <SDL3/SDL.h>

namespace Libs::Graphics {
std::optional<std::filesystem::path> OptiScalerRuntimePath() {
	const auto& configured = Config::GetOptiScalerPath();
	if (!configured.empty()) {
		std::error_code error;
		auto path = std::filesystem::absolute(std::filesystem::u8path(configured), error);
		if (!error) return path;
		LOGF("OptiScaler unavailable: invalid runtime path: %s\n", error.message().c_str());
		return std::nullopt;
	}
	const char* base = SDL_GetBasePath();
	if (base != nullptr) return std::filesystem::u8path(base) / "OptiScaler.dll";
	LOGF("OptiScaler unavailable: cannot resolve executable directory: %s\n", SDL_GetError());
	return std::nullopt;
}
} // namespace Libs::Graphics
