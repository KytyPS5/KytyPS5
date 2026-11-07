// Tests for where the emulator keeps the pipeline and program lists of each game.

#include "common/cacheFolder.h"

#include <iostream>

namespace {

using Common::CacheEnvironment;
using Common::CacheHost;
using Common::PipelineCacheFolder;
using Path = std::filesystem::path;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

const CacheEnvironment kEnv {.home           = "/Users/me",
                             .xdg_cache_home = "/var/cache/me",
                             .local_app_data = "C:/Users/me/AppData/Local"};

void TestTheHostCacheFolderIsUsed() {
	Check(PipelineCacheFolder({}, CacheHost::MacOs, kEnv) ==
	          Path("/Users/me/Library/Caches/KytyPS5/PipelineCache"),
	      "macOS: the user's Library/Caches");
	Check(PipelineCacheFolder({}, CacheHost::Linux, kEnv) ==
	          Path("/var/cache/me/KytyPS5/PipelineCache"),
	      "Linux: XDG_CACHE_HOME first");
	Check(PipelineCacheFolder({}, CacheHost::Linux, {.home = "/home/me"}) ==
	          Path("/home/me/.cache/KytyPS5/PipelineCache"),
	      "Linux without XDG_CACHE_HOME: ~/.cache");
	Check(PipelineCacheFolder({}, CacheHost::Windows, kEnv) ==
	          Path("C:/Users/me/AppData/Local/KytyPS5/PipelineCache"),
	      "Windows: LOCALAPPDATA");
}

void TestTheWorkingDirectoryIsTheLastResort() {
	for (const auto host: {CacheHost::Windows, CacheHost::MacOs, CacheHost::Linux}) {
		Check(PipelineCacheFolder({}, host, {}) == Path("_PipelineCache"),
		      "no per-user folder named: the working directory as before");
	}
	Check(PipelineCacheFolder({}, CacheHost::MacOs, {.xdg_cache_home = "/x"}) ==
	          Path("_PipelineCache"),
	      "macOS does not read the Linux variable");
}

void TestTheCommandLineWins() {
	for (const auto host: {CacheHost::Windows, CacheHost::MacOs, CacheHost::Linux}) {
		Check(PipelineCacheFolder("/data/kyty", host, kEnv) == Path("/data/kyty"),
		      "a folder given on the command line is used as given");
	}
}

} // namespace

int main() {
	TestTheHostCacheFolderIsUsed();
	TestTheWorkingDirectoryIsTheLastResort();
	TestTheCommandLineWins();
	if (g_failures != 0) {
		std::cerr << g_failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "cache folder tests passed\n";
	return 0;
}
