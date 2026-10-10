#pragma once

#include <filesystem>
#include <string>

namespace Kyty::Updater {
struct Result {
	bool        success;
	std::string message;
};

// Only release files are replaced. Files absent from the release are preserved.
// Successful installation consumes staged files; stage on the target filesystem.
Result Validate(const std::filesystem::path& source, const std::filesystem::path& target,
                const std::filesystem::path& launcher_relative);
Result Install(const std::filesystem::path& source, const std::filesystem::path& target,
               const std::filesystem::path& launcher_relative);
} // namespace Kyty::Updater
