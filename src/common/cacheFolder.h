#ifndef KYTY_COMMON_CACHEFOLDER_H_
#define KYTY_COMMON_CACHEFOLDER_H_

#include <filesystem>
#include <string>

namespace Common {

enum class CacheHost { Windows, MacOs, Linux };

struct CacheEnvironment {
	std::string home;           // HOME
	std::string xdg_cache_home; // XDG_CACHE_HOME
	std::string local_app_data; // LOCALAPPDATA
};

// Where the emulator keeps what it builds for itself and builds again when lost: the pipeline and
// program lists of each game. Not in the working directory: an emulator started from another
// folder, or a new copy of it, would start each game as if it had never run. The host's per-user
// cache folder is shared by every copy, and the host may clear it.
//
// `configured`: the folder given on the command line, if any. When the environment names no
// per-user folder, the working directory as before.
[[nodiscard]] inline std::filesystem::path PipelineCacheFolder(
    const std::filesystem::path& configured, CacheHost host, const CacheEnvironment& env) {
	if (!configured.empty()) {
		return configured;
	}
	switch (host) {
		case CacheHost::Windows:
			if (!env.local_app_data.empty()) {
				return std::filesystem::path(env.local_app_data) / "KytyPS5" / "PipelineCache";
			}
			break;
		case CacheHost::MacOs:
			if (!env.home.empty()) {
				return std::filesystem::path(env.home) / "Library" / "Caches" / "KytyPS5" /
				       "PipelineCache";
			}
			break;
		case CacheHost::Linux:
			if (!env.xdg_cache_home.empty()) {
				return std::filesystem::path(env.xdg_cache_home) / "KytyPS5" / "PipelineCache";
			}
			if (!env.home.empty()) {
				return std::filesystem::path(env.home) / ".cache" / "KytyPS5" / "PipelineCache";
			}
			break;
	}
	return "_PipelineCache";
}

} // namespace Common

#endif /* KYTY_COMMON_CACHEFOLDER_H_ */
