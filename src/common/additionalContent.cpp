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

std::vector<AddContentEntry> ReadLocalEntitlements(const std::filesystem::path& file,
                                                  const std::string& title_id) {
	std::error_code ec;
	const auto size = std::filesystem::file_size(file, ec);
	if (title_id.size() != 9 || ec || size > 64 * 1024) {
		return {};
	}
	std::ifstream stream(file, std::ios::binary);
	if (!stream) {
		return {};
	}
	const auto trim = [](std::string value) {
		const auto first = value.find_first_not_of(" \t\r\n");
		return first == std::string::npos ? std::string {} :
		       value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
	};
	static const std::regex pattern(
	    "^[A-Z]{2}[0-9]{4}-[A-Z]{4}[0-9]{5}_[0-9]{2}-[A-Z0-9]{16}$");
	std::map<std::string, AddContentEntry> entries;
	std::map<std::string, std::string> fields;
	bool psal = false, invalid = false;
	const auto flush = [&] {
		const auto id = fields.find("content_id");
		const auto status = fields.find("download_status");
		const auto service = fields.find("service_label");
		if (psal && !invalid && id != fields.end() && status != fields.end() &&
		    status->second == "NO_EXTRA_DATA" &&
		    (service == fields.end() || service->second == "0") &&
		    std::regex_match(id->second, pattern) && id->second.compare(7, 9, title_id) == 0) {
			const auto label = id->second.substr(20);
			entries.try_emplace(label, AddContentEntry {id->second, label, {}, {}});
		}
	};
	std::string line;
	while (std::getline(stream, line)) {
		line = trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';') {
			continue;
		}
		if (line[0] == '[') {
			flush();
			fields.clear();
			invalid = false;
			psal = line == "[PSAL]";
			continue;
		}
		if (!psal) {
			continue;
		}
		const auto equals = line.find('=');
		if (equals == std::string::npos) {
			invalid = true;
			continue;
		}
		const auto key = trim(line.substr(0, equals));
		const auto value = trim(line.substr(equals + 1));
		// Unknown options may change ownership semantics. Do not silently ignore them.
		// Keys are not consumed by the entitlement-info APIs implemented here.
		if ((key != "content_id" && key != "download_status" && key != "service_label" &&
		     key != "entitlement_key") || !fields.emplace(key, value).second) {
			invalid = true;
		}
	}
	flush();
	std::vector<AddContentEntry> result;
	for (auto& [label, entry]: entries) {
		result.push_back(std::move(entry));
	}
	return result;
}

} // namespace Common
