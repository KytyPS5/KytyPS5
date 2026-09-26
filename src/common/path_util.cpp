#include "path_util.h"

#include <cstdlib>
#include <filesystem>

namespace PathUtil {

// String definitions for used folders
static constexpr auto save_dir           = "_SaveData/";
static constexpr auto temp_dir           = "_TempData/";
static constexpr auto pipeline_cache_dir = "_PipelineCache/";
static constexpr auto download_dir       = "_DownloadData/";
static constexpr auto texture_dir        = "_Textures/";

static inline std::filesystem::path GetUserPath() {
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	return std::filesystem::path(std::getenv("HOME")) / ".local/share/kyty";
#endif
	return "";
}

std::filesystem::path GetPath(PathType path) {
	switch (path) {
		case SAVE_DIR: return GetUserPath() / save_dir;

		case TEMP_DIR:
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
			return "/tmp/kyty/";
#endif
			return temp_dir;

		case PIPELINE_CACHE_DIR: return GetUserPath() / pipeline_cache_dir;

		case DOWNLOAD_DIR: return GetUserPath() / download_dir;

		case TEXTURE_DIR: return GetUserPath() / texture_dir;
	}
}

} // namespace PathUtil
