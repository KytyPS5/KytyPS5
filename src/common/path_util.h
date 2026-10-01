#pragma once

#include <filesystem>

namespace PathUtil {

enum PathType { SAVE_DIR, TEMP_DIR, PIPELINE_CACHE_DIR, DOWNLOAD_DIR, TEXTURE_DIR };

std::filesystem::path GetPath(PathType path);
}; // namespace PathUtil
