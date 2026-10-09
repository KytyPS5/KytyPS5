#include "common/file.h"
#include "graphics/host_gpu/renderer/pipeline/driverCachePolicy.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {
using Libs::Graphics::DriverCachePolicy::ReplaceFile;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "DriverCachePolicyTests: failed: %s\n", message);
		std::abort();
	}
}

class TempDirectory {
public:
	TempDirectory() {
		const auto prefix = "kyty_driver_cache_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
		for (uint32_t attempt = 0; attempt < 100; ++attempt) {
			m_path = std::filesystem::temp_directory_path() / (prefix + "_" + std::to_string(attempt));
			std::error_code error;
			if (std::filesystem::create_directory(m_path, error)) {
				return;
			}
			Check(!error, "create unique temporary directory");
		}
		Check(false, "unique temporary directory attempts exhausted");
	}
	~TempDirectory() {
		std::error_code error;
		std::filesystem::remove_all(m_path, error);
	}
	const std::filesystem::path& Path() const { return m_path; }
private:
	std::filesystem::path m_path;
};

std::string ReadAll(const std::filesystem::path& path) {
	Common::File input(path, Common::File::Mode::Read);
	Check(!input.IsInvalid(), "open cache fixture");
	const auto bytes = input.ReadWholeBuffer();
	return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void WriteAll(const std::filesystem::path& path, const std::string& text) {
	Common::File output;
	Check(output.Create(path), "create cache fixture");
	uint32_t written = 0;
	output.Write(text.data(), static_cast<uint32_t>(text.size()), &written);
	Check(written == text.size() && output.Flush(), "write complete cache fixture");
}

void TestReplaceKeepsOldFileOnFailure() {
	TempDirectory directory;
	const auto dst = directory.Path() / "cache.bin";
	const auto temp = directory.Path() / "cache.bin.tmp";
	WriteAll(dst, "old cache");
	Check(!ReplaceFile(temp, dst), "missing source unexpectedly replaced cache");
	Check(std::filesystem::is_regular_file(dst) && ReadAll(dst) == "old cache", "old cache lost when source is missing");
	std::filesystem::create_directories(temp / "child");
	Check(!ReplaceFile(temp, dst), "directory unexpectedly replaced cache file");
	Check(std::filesystem::is_regular_file(dst) && ReadAll(dst) == "old cache", "old cache lost when rename fails");
	std::puts("DriverCachePolicyTests: missing source and rename failure preserve old cache");
}

void TestReplaceSuccess() {
	TempDirectory directory;
	const auto dst = directory.Path() / "cache.bin";
	const auto temp = directory.Path() / "cache.bin.tmp";
	const std::string payload(1024 * 1024 + 37, 'N');
	WriteAll(temp, payload);
	Check(ReplaceFile(temp, dst) && ReadAll(dst) == payload && !std::filesystem::exists(temp), "create cache at an absent destination");
	WriteAll(temp, "complete new cache");
	Check(ReplaceFile(temp, dst) && ReadAll(dst) == "complete new cache" && !std::filesystem::exists(temp), "replace an existing cache completely");
	std::puts("DriverCachePolicyTests: absent and existing destinations receive complete cache");
}
}

int main(int argc, char** argv) {
	TestReplaceKeepsOldFileOnFailure();
	if (argc == 2 && std::string(argv[1]) == "--preservation-only") {
		return 0;
	}
	TestReplaceSuccess();
	std::puts("DriverCachePolicyTests: all cases passed");
	return 0;
}
