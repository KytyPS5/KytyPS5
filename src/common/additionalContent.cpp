#include "common/additionalContent.h"

#include "common/stringUtils.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <system_error>

namespace Common {

namespace {

std::string GetString(const nlohmann::json& object, const std::string& key) {
	const auto it = object.find(key);
	return it != object.end() && it->is_string() ? it->get<std::string>() : std::string {};
}

std::string GetTitle(const nlohmann::json& param) {
	const auto localized = param.find("localizedParameters");
	if (localized != param.end() && localized->is_object()) {
		const auto preferred = localized->find(GetString(*localized, "defaultLanguage"));
		if (preferred != localized->end()) {
			if (auto title = GetString(*preferred, "titleName"); !title.empty()) {
				return title;
			}
		}
		for (const auto& language: *localized) {
			if (auto title = GetString(language, "titleName"); !title.empty()) {
				return title;
			}
		}
	}
	return GetString(param, "titleName");
}

} // namespace

std::vector<AddContentEntry> FindAddContent(const std::filesystem::path& dir,
                                            const std::string&           title_id) {
	if (dir.empty() || title_id.empty()) {
		return {};
	}

	std::error_code ec;
	const auto      root = std::filesystem::absolute(dir, ec);
	if (ec) {
		return {};
	}

	// Scan directories separately so an unreadable child does not discard its siblings.
	std::vector<std::filesystem::path> pending {root};
	std::vector<std::filesystem::path> params;
	while (!pending.empty()) {
		const auto current = std::move(pending.back());
		pending.pop_back();
		std::filesystem::directory_iterator it(
		    current, std::filesystem::directory_options::skip_permission_denied, ec);
		const std::filesystem::directory_iterator end;
		for (; !ec && it != end; it.increment(ec)) {
			std::error_code entry_ec;
			if (it->is_directory(entry_ec) && !it->is_symlink(entry_ec) && !entry_ec) {
				pending.push_back(it->path());
			} else if (it->is_regular_file(entry_ec) && !entry_ec &&
			           EqualNoCase(PathToString(it->path().filename()), "param.json") &&
			           EqualNoCase(PathToString(it->path().parent_path().filename()), "sce_sys")) {
				params.push_back(it->path());
			}
		}
	}
	std::sort(params.begin(), params.end());

	static const std::regex content_id_pattern(
	    "^[A-Z]{2}[0-9]{4}-[A-Z]{4}[0-9]{5}_[0-9]{2}-[A-Z0-9]{16}$");
	std::map<std::string, AddContentEntry> by_label;
	for (const auto& file: params) {
		const auto size = std::filesystem::file_size(file, ec);
		if (ec || size > 1024 * 1024) {
			continue;
		}
		std::ifstream stream(file, std::ios::binary);
		if (!stream) {
			continue;
		}
		const auto param = nlohmann::json::parse(stream, nullptr, false);
		// applicationCategoryType is required for applications, not additional content.
		if (!param.is_object() || param.contains("applicationCategoryType")) {
			continue;
		}
		const auto content_id = GetString(param, "contentId");
		if (!std::regex_match(content_id, content_id_pattern) ||
		    content_id.compare(7, 9, title_id) != 0) {
			continue;
		}

		const auto mount_dir = file.parent_path().parent_path();
		const auto label     = content_id.substr(20);
		by_label.try_emplace(label,
		                     AddContentEntry {content_id, label, mount_dir, GetTitle(param)});
	}

	std::vector<AddContentEntry> entries;
	entries.reserve(by_label.size());
	for (auto& [label, entry]: by_label) {
		entries.push_back(std::move(entry));
	}
	return entries;
}

} // namespace Common
