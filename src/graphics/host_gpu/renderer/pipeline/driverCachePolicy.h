#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DRIVERCACHEPOLICY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DRIVERCACHEPOLICY_H_

#include <filesystem>
#include <system_error>

namespace Libs::Graphics::DriverCachePolicy {

// Keep the old cache visible until rename succeeds; never delete the destination first.
inline bool ReplaceCacheFile(const std::filesystem::path& temp, const std::filesystem::path& dst) {
	std::error_code error;
	if (!std::filesystem::is_regular_file(temp, error)) {
		return false;
	}
	std::filesystem::rename(temp, dst, error);
	if (error) {
		std::filesystem::remove(temp, error);
		return false;
	}
	return true;
}

} // namespace Libs::Graphics::DriverCachePolicy

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DRIVERCACHEPOLICY_H_
