#include "common/trophies.h"

#include "common/file.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <span>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace Common::Trophies {
namespace {

using Json  = nlohmann::json;
using Files = std::map<std::string_view, std::span<const std::byte>>;

constexpr uint64_t MaxUnlocksSize = uint64_t {1} << 20u;
constexpr uint64_t MaxPackageSize = uint64_t {1} << 30u;

uint64_t ReadBigEndian(const std::byte* data, size_t size) {
	uint64_t value = 0;
	for (size_t i = 0; i < size; ++i) {
		value = (value << 8u) | std::to_integer<uint8_t>(data[i]);
	}
	return value;
}

int ReadId(const Json& value) {
	if (value.is_number_integer()) {
		const auto number = value.get<int64_t>();
		return number >= 0 && number <= INT32_MAX ? static_cast<int>(number) : -1;
	}
	if (!value.is_string()) {
		return -1;
	}
	const auto& text        = value.get_ref<const std::string&>();
	int         id          = -1;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
	return error == std::errc {} && end == text.data() + text.size() && id >= 0 ? id : -1;
}

Json ReadJson(const Files& files, std::string_view name) {
	const auto entry = files.find(name);
	if (entry == files.end()) {
		return {};
	}
	return Json::parse(entry->second.begin(), entry->second.end(), nullptr, false);
}

Package ParsePackage(std::span<const std::byte> data, int console_language) {
	if (data.size() < 0x40 || data.size() > MaxPackageSize ||
	    ReadBigEndian(data.data(), 4) != 0xb228c60a || ReadBigEndian(data.data() + 4, 4) != 1) {
		return {};
	}
	const auto size  = ReadBigEndian(data.data() + 8, 8);
	const auto count = ReadBigEndian(data.data() + 0x10, 4);
	const auto toc   = ReadBigEndian(data.data() + 0x14, 4);
	if (size < 0x40 || size > data.size() || count > 4096 || toc > size ||
	    0x20 + count * 0x40 > size - toc) {
		return {};
	}
	Files files;
	for (uint64_t i = 0; i < count; ++i) {
		const auto*            entry = data.data() + toc + 0x20 + i * 0x40;
		const auto*            name  = reinterpret_cast<const char*>(entry);
		const std::string_view filename(name, std::find(name, name + 0x20, '\0') - name);
		const auto             offset = ReadBigEndian(entry + 0x20, 8);
		const auto             length = ReadBigEndian(entry + 0x28, 8);
		if (offset > size || length > size - offset) {
			return {};
		}
		files.emplace(filename, data.subspan(offset, length));
	}
	const auto conf = ReadJson(files, "tropconf.json");
	if (!conf.is_object() || !conf.contains("trophies") || !conf["trophies"].is_array() ||
	    conf["trophies"].size() > 1000 || !conf.contains("defaultLanguage") ||
	    !conf["defaultLanguage"].is_string()) {
		return {};
	}
	static constexpr std::array<std::string_view, 30> locales = {
	    "ja-JP", "en-US", "fr-FR",   "es-ES",   "de-DE",  "it-IT", "nl-NL", "pt-PT",
	    "ru-RU", "ko-KR", "zh-Hant", "zh-Hans", "fi-FI",  "sv-SE", "da-DK", "no-NO",
	    "pl-PL", "pt-BR", "en-GB",   "tr-TR",   "es-419", "ar-AE", "fr-CA", "cs-CZ",
	    "hu-HU", "el-GR", "ro-RO",   "th-TH",   "vi-VN",  "id-ID"};
	const auto locale =
	    locales[console_language >= 0 && console_language < locales.size() ? console_language : 1];
	auto meta = ReadJson(files, fmt::format("tropmeta_{}.json", locale));
	if (meta.is_null()) {
		meta = ReadJson(files, fmt::format("tropmeta_{}.json",
		                                   conf["defaultLanguage"].get_ref<const std::string&>()));
	}
	if (!meta.is_object() || !meta.contains("metadata") || !meta["metadata"].is_object()) {
		return {};
	}
	const auto& texts = meta["metadata"];
	if (!texts.contains("titleMetadata") || !texts["titleMetadata"].is_object() ||
	    !texts["titleMetadata"].contains("name") || !texts["titleMetadata"]["name"].is_string() ||
	    !texts.contains("trophyMetadata") || !texts["trophyMetadata"].is_array()) {
		return {};
	}
	Package package;
	package.title = texts["titleMetadata"]["name"].get<std::string>();
	package.groups.emplace(-1, package.title);
	uint64_t icon_bytes = 0;
	for (const auto& definition: conf["trophies"]) {
		if (!definition.is_object() || !definition.contains("id") ||
		    !definition.contains("grade") || !definition["grade"].is_string()) {
			return {};
		}
		Trophy trophy;
		trophy.id              = ReadId(definition["id"]);
		const auto grade       = definition["grade"].get<std::string>();
		const auto grade_index = std::string_view("PGSB").find(grade);
		if (trophy.id < 0 || grade.size() != 1 || grade_index == std::string_view::npos) {
			return {};
		}
		trophy.grade = static_cast<int>(grade_index) + 1;
		if (definition.contains("groupId")) {
			trophy.group_id = ReadId(definition["groupId"]);
			if (trophy.group_id < 0) {
				return {};
			}
		}
		if (definition.contains("platinumTrophyId")) {
			trophy.platinum_id = ReadId(definition["platinumTrophyId"]);
		}
		trophy.hidden     = definition.contains("hidden") && definition["hidden"] == true;
		trophy.has_reward = definition.contains("hasReward") && definition["hasReward"] == true;
		if (definition.contains("unlockCondition")) {
			const auto& condition = definition["unlockCondition"];
			if (condition.is_object() && condition.contains("progressive") &&
			    condition["progressive"] == true) {
				if (!condition.contains("targetValue") || !condition["targetValue"].is_string()) {
					return {};
				}
				const auto& value  = condition["targetValue"].get_ref<const std::string&>();
				uint64_t    target = 0;
				const auto [end, error] =
				    std::from_chars(value.data(), value.data() + value.size(), target);
				if (error != std::errc {} || end != value.data() + value.size()) {
					return {};
				}
				trophy.target = target;
			}
		}
		const auto icon = files.find(fmt::format("trop{:04}.png", trophy.id));
		if (icon != files.end()) {
			if (icon->second.size() > size - icon_bytes) {
				return {};
			}
			trophy.icon_png.assign(icon->second.begin(), icon->second.end());
			icon_bytes += icon->second.size();
		}
		package.groups.try_emplace(trophy.group_id);
		if (package.groups.size() > 50 ||
		    !package.trophies.emplace(trophy.id, std::move(trophy)).second) {
			return {};
		}
	}
	if (texts.contains("groupMetadata") && texts["groupMetadata"].is_array()) {
		for (const auto& group: texts["groupMetadata"]) {
			if (group.is_object() && group.contains("id") && group.contains("name") &&
			    group["name"].is_string()) {
				const auto found = package.groups.find(ReadId(group["id"]));
				if (found != package.groups.end()) {
					found->second = group["name"].get<std::string>();
				}
			}
		}
	}
	for (const auto& text: texts["trophyMetadata"]) {
		if (!text.is_object() || !text.contains("id")) {
			continue;
		}
		const auto found = package.trophies.find(ReadId(text["id"]));
		if (found == package.trophies.end()) {
			continue;
		}
		for (const auto& [key, field]: {std::pair {"name", &found->second.name},
		                                {"detail", &found->second.description},
		                                {"reward", &found->second.reward}}) {
			if (text.contains(key) && text[key].is_string()) {
				*field = text[key].get<std::string>();
			}
		}
	}
	return package;
}

} // namespace

Package LoadPackage(const std::filesystem::path& path, int console_language) {
	File file(path, File::Mode::Read);
	if (file.IsInvalid() || file.Size() > MaxPackageSize) {
		return {};
	}
	return ParsePackage(file.ReadWholeBuffer(), console_language);
}

std::filesystem::path PackagePath(uint32_t service_label) {
	return std::filesystem::path(PackageDirectory) / fmt::format("trophy{:02}.ucp", service_label);
}

std::filesystem::path UnlocksPath(const std::filesystem::path& root, std::string_view title_id,
                                  int user_id, uint32_t service_label) {
	if (title_id.empty() || !std::all_of(title_id.begin(), title_id.end(), [](char c) {
		    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		           c == '_' || c == '-';
	    })) {
		return {};
	}
	return root / "_SaveData" / title_id /
	       fmt::format("trophies_{}_{}.json", user_id, service_label);
}

std::set<int> LoadUnlocks(const std::filesystem::path& path) {
	File file(path, File::Mode::Read);
	if (file.IsInvalid() || file.Size() > MaxUnlocksSize) {
		return {};
	}
	const auto bytes = file.ReadWholeBuffer();
	const auto json  = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
	if (!json.is_object() || !json.contains("unlockedTrophies") ||
	    !json["unlockedTrophies"].is_array()) {
		return {};
	}
	std::set<int> unlocked;
	for (const auto& value: json["unlockedTrophies"]) {
		const auto id = ReadId(value);
		if (id >= 0) {
			unlocked.insert(id);
		}
	}
	return unlocked;
}

bool SaveUnlocks(const std::filesystem::path& path, const std::set<int>& unlocked) {
	if (path.empty() || !File::CreateDirectories(path.parent_path())) {
		return false;
	}
	const auto text      = Json {{"unlockedTrophies", unlocked}}.dump();
	auto       temporary = path;
	temporary += ".tmp";
	File file;
	if (text.size() > MaxUnlocksSize || !file.Create(temporary)) {
		return false;
	}
	uint32_t written = 0;
	file.Write(text.data(), static_cast<uint32_t>(text.size()), &written);
	const bool complete = written == text.size() && file.Flush();
	file.Close();
	std::error_code error;
	if (complete) {
		std::filesystem::rename(temporary, path, error);
		if (!error) {
			return true;
		}
	}
	std::filesystem::remove(temporary, error);
	return false;
}

} // namespace Common::Trophies
