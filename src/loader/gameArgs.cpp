#include "loader/gameArgs.h"

#include "common/file.h"
#include "common/hackNotice.h"
#include "common/logging/log.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Loader::GameArgs {

namespace {

bool IsDemonsSouls() {
	static const bool demons_souls = [] {
		std::string title;
		return SystemContentParamSfoGetString("TITLE_ID", &title) &&
		       (title == "PPSA01340" || title == "PPSA01341" || title == "PPSA01342");
	}();
	return demons_souls;
}

// One "+name=value" line per console variable: Demon's Souls' defaults, then those of
// KYTY_GAME_CONVARS, whose names replace the defaults'.
// Demon's Souls' default: no index-buffer culling. Before drawing an object the engine culls its
// triangles in a compute pass of its own (~2,770 dispatches a frame at Boletaria, ~11 ms a frame
// of the execution thread); without it the host GPU draws the triangles the pass would have
// culled, the image unchanged (doIndexBufferCulling=true: back).
const std::string& ExtraLines() {
	static const std::string lines = [] {
		std::vector<std::pair<std::string, std::string>> entries; // (name, "name=value")
		const auto                                       add = [&](std::string_view list) {
			for (size_t at = 0; at < list.size();) {
				auto end = list.find_first_of(";,", at);
				if (end == std::string_view::npos) {
					end = list.size();
				}
				const auto first = list.find_first_not_of(' ', at);
				const auto last  = list.find_last_not_of(' ', end - 1);
				if (first != std::string_view::npos && first < end && last >= first) {
					std::string entry(list.substr(first, last - first + 1));
					const auto  name = entry.substr(0, entry.find('='));
					std::erase_if(entries, [&](const auto& held) { return held.first == name; });
					entries.emplace_back(name, std::move(entry));
				}
				at = end + 1;
			}
		};
		const char* value = std::getenv("KYTY_GAME_CONVARS");
		if (value != nullptr && std::string_view(value) == "0") {
			return std::string();
		}
		if (IsDemonsSouls()) {
			add("doIndexBufferCulling=false");
		}
		if (value != nullptr) {
			add(value);
		}
		std::string out;
		for (const auto& [name, entry]: entries) {
			out += "+" + entry + "\n";
		}
		return out;
	}();
	return lines;
}

// The engine opens it as "$/packagecmdlineargs.txt" (lower case): the name is compared without
// case.
bool IsArgsFile(const std::filesystem::path& real) {
	if (ExtraLines().empty()) {
		return false;
	}
	auto name = real.filename().string();
	std::ranges::transform(name, name.begin(), [](char c) {
		return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	});
	return name == "packagecmdlineargs.txt";
}

std::string Contents(const std::filesystem::path& real) {
	std::string  text;
	Common::File in; // (also a member of a game archive)
	if (in.Open(real, Common::File::Mode::Read)) {
		text.resize(static_cast<size_t>(in.Size()));
		uint32_t read = 0;
		in.Read(text.data(), static_cast<uint32_t>(text.size()), &read);
		text.resize(read);
		in.Close();
	}
	if (!text.empty() && text.back() != '\n') {
		text += '\n';
	}
	return text + ExtraLines();
}

} // namespace

std::filesystem::path RedirectRead(const std::filesystem::path& real) {
	if (!IsArgsFile(real)) {
		return {};
	}
	static std::mutex mutex;
	std::lock_guard   lock(mutex);
	// The copy goes next to the temporary files; one that cannot be written leaves the file as it
	// is.
	std::error_code error;
	const auto copy = std::filesystem::temp_directory_path(error) / "kyty-PackageCmdLineArgs.txt";
	if (error) {
		return {};
	}
	const auto    text = Contents(real);
	std::ofstream out(copy, std::ios::binary | std::ios::trunc);
	out.write(text.data(), static_cast<std::streamsize>(text.size()));
	out.close();
	if (!out) {
		return {};
	}
	Log::WriteToConsoleAndLog("Game console variables added to the game's command-line file:\n" +
	                          ExtraLines());
	if (ExtraLines().find("+doIndexBufferCulling=false\n") != std::string::npos) {
		Common::HackNotice::Report("Demon's Souls index-buffer culling pass turned off",
		                           "game console variables", 0, 0, "+doIndexBufferCulling=false");
	}
	return copy;
}

std::optional<uint64_t> RedirectedSize(const std::filesystem::path& real) {
	if (!IsArgsFile(real)) {
		return std::nullopt;
	}
	return Contents(real).size();
}

} // namespace Loader::GameArgs
