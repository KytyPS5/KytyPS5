#include "common/trophies.h"

#include "common/file.h"
#include "common/localization.h"

#include <algorithm>
#include <charconv>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <span>

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

std::optional<uint64_t> ReadUnsigned(const Json& value) {
	if (value.is_number_unsigned()) {
		return value.get<uint64_t>();
	}
	if (value.is_number_integer()) {
		const auto number = value.get<int64_t>();
		return number >= 0 ? std::optional<uint64_t>(static_cast<uint64_t>(number)) : std::nullopt;
	}
	if (!value.is_string()) {
		return std::nullopt;
	}
	const auto& text        = value.get_ref<const std::string&>();
	uint64_t    number      = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
	return error == std::errc {} && end == text.data() + text.size()
	           ? std::optional<uint64_t>(number)
	           : std::nullopt;
}

Json ReadJson(const Files& files, std::string_view name) {
	const auto entry = files.find(name);
	if (entry == files.end()) {
		return {};
	}
	return Json::parse(entry->second.begin(), entry->second.end(), nullptr, false);
}

bool ReadFiles(std::span<const std::byte> data, Files& files, uint64_t& size) {
	if (data.size() < 0x40 || data.size() > MaxPackageSize ||
	    ReadBigEndian(data.data(), 4) != 0xb228c60a || ReadBigEndian(data.data() + 4, 4) != 1) {
		return false;
	}
	size             = ReadBigEndian(data.data() + 8, 8);
	const auto count = ReadBigEndian(data.data() + 0x10, 4);
	const auto toc   = ReadBigEndian(data.data() + 0x14, 4);
	if (size < 0x40 || size > data.size() || count > 4096 || toc > size ||
	    0x20 + count * 0x40 > size - toc) {
		return false;
	}
	for (uint64_t i = 0; i < count; ++i) {
		const auto*            entry = data.data() + toc + 0x20 + i * 0x40;
		const auto*            name  = reinterpret_cast<const char*>(entry);
		const std::string_view filename(name, std::find(name, name + 0x20, '\0') - name);
		const auto             offset = ReadBigEndian(entry + 0x20, 8);
		const auto             length = ReadBigEndian(entry + 0x28, 8);
		if (offset > size || length > size - offset) {
			return false;
		}
		files.emplace(filename, data.subspan(offset, length));
	}
	return true;
}

Package ParsePackage(std::span<const std::byte> data, int console_language) {
	Files    files;
	uint64_t size = 0;
	if (!ReadFiles(data, files, size)) {
		return {};
	}
	const auto conf = ReadJson(files, "tropconf.json");
	if (!conf.is_object() || !conf.contains("trophies") || !conf["trophies"].is_array() ||
	    conf["trophies"].size() > 1000 || !conf.contains("defaultLanguage") ||
	    !conf["defaultLanguage"].is_string()) {
		return {};
	}
	const auto locale = Localization::LocaleName(console_language);
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
			if (condition.is_object()) {
				trophy.progressive =
				    condition.contains("progressive") && condition["progressive"] == true;
				if (condition.contains("udsStatId")) {
					trophy.uds_stat_id = ReadUnsigned(condition["udsStatId"]);
					if (!trophy.uds_stat_id) {
						return {};
					}
				}
				if (condition.contains("targetValue")) {
					trophy.target = ReadUnsigned(condition["targetValue"]);
				}
				const auto comparison = condition.find("comparator");
				if (comparison != condition.end()) {
					if (*comparison == "ge")
						trophy.comparison = Comparison::GreaterEqual;
					else if (*comparison == "gt")
						trophy.comparison = Comparison::Greater;
					else if (*comparison == "le")
						trophy.comparison = Comparison::LessEqual;
					else if (*comparison == "lt")
						trophy.comparison = Comparison::Less;
				}
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
	return root / "_Trophies" / title_id /
	       fmt::format("trophies_{}_{}.json", user_id, service_label);
}

UnlockData LoadUnlockData(const std::filesystem::path& path) {
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
	UnlockData unlocks;
	for (const auto& value: json["unlockedTrophies"]) {
		const auto id = ReadId(value);
		if (id >= 0) {
			unlocks.unlocked.insert(id);
		}
	}
	if (json.contains("unlockedAt") && json["unlockedAt"].is_object()) {
		for (auto it = json["unlockedAt"].begin(); it != json["unlockedAt"].end(); ++it) {
			const auto id = ReadId(Json(it.key()));
			if (id >= 0 && unlocks.unlocked.contains(id)) {
				const auto tick = ReadUnsigned(it.value());
				if (tick && *tick >= UnixEpochTick && *tick <= 315537897599999999ULL) {
					unlocks.timestamps.emplace(id, *tick);
				}
			}
		}
	}
	if (json.contains("maxStats") && json["maxStats"].is_object()) {
		for (auto it = json["maxStats"].begin(); it != json["maxStats"].end(); ++it) {
			const auto id    = ReadUnsigned(Json(it.key()));
			const auto value = ReadUnsigned(it.value());
			if (id && value) unlocks.max_stats.emplace(*id, *value);
		}
	}
	if (json.contains("progressValues") && json["progressValues"].is_object()) {
		for (auto it = json["progressValues"].begin(); it != json["progressValues"].end(); ++it) {
			const auto id    = ReadId(Json(it.key()));
			const auto value = ReadUnsigned(it.value());
			if (id >= 0 && value) unlocks.progress.emplace(id, *value);
		}
	}
	return unlocks;
}

bool SaveUnlockData(const std::filesystem::path& path, const UnlockData& unlocks) {
	if (path.empty() || !File::CreateDirectories(path.parent_path())) {
		return false;
	}
	Json timestamps = Json::object();
	for (const auto& [id, tick]: unlocks.timestamps) {
		if (unlocks.unlocked.contains(id)) {
			timestamps[std::to_string(id)] = tick;
		}
	}
	Json stats = Json::object();
	for (const auto& [id, value]: unlocks.max_stats) {
		stats[std::to_string(id)] = value;
	}
	Json progress = Json::object();
	for (const auto& [id, value]: unlocks.progress) {
		progress[std::to_string(id)] = value;
	}
	const auto text = Json {{"unlockedTrophies", unlocks.unlocked},
	                        {"unlockedAt", timestamps},
	                        {"maxStats", stats},
	                        {"progressValues", progress}}.dump();
	auto temporary = path;
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

UdsRules LoadUdsRules(const std::filesystem::path& path) {
	File file(path, File::Mode::Read);
	if (file.IsInvalid() || file.Size() > MaxPackageSize) {
		return {};
	}
	const auto bytes = file.ReadWholeBuffer();
	Files      files;
	uint64_t   size = 0;
	if (!ReadFiles(bytes, files, size)) {
		return {};
	}
	const auto definitions = ReadJson(files, "stats_definition.json");
	const auto extraction  = ReadJson(files, "stats_extraction.json");
	if (!definitions.is_object() || !definitions.contains("statDefinitionArray") ||
	    !definitions["statDefinitionArray"].is_array() || !extraction.is_object() ||
	    !extraction.contains("statsExtractionRuleArray") ||
	    !extraction["statsExtractionRuleArray"].is_array()) {
		return {};
	}
	// Support direct unsigned latest values and nonnegative integer maxima.
	std::map<uint64_t, UdsRule> stats;
	for (const auto& definition: definitions["statDefinitionArray"]) {
		if (!definition.is_object() || !definition.contains("statId") ||
		    !definition.contains("aggregation") ||
		    (definition["aggregation"] != "latest" && definition["aggregation"] != "max") ||
		    !definition.contains("dataType")) {
			continue;
		}
		const auto  id   = ReadUnsigned(definition["statId"]);
		const auto& type = definition["dataType"];
		const bool  maximum = definition["aggregation"] == "max";
		if (!id || (type != "uint64" && type != "uint32" && type != "int64") ||
		    (type == "int64" && (!maximum || !definition.contains("minValue")))) {
			continue;
		}
		const uint64_t limit   = type == "uint32"  ? UINT32_MAX
		                         : type == "int64" ? INT64_MAX
		                                           : UINT64_MAX;
		const auto     initial = definition.contains("initialValue")
		                             ? ReadUnsigned(definition["initialValue"])
		                             : std::optional<uint64_t>(0);
		if ((definition.contains("minValue") && ReadUnsigned(definition["minValue"]) != 0) ||
		    (definition.contains("maxValue") && ReadUnsigned(definition["maxValue"]) != limit) ||
		    !initial || *initial > limit) {
			continue;
		}
		const auto value_type = type == "uint32"  ? UdsRule::Type::Uint32
		                        : type == "int64" ? UdsRule::Type::Int64
		                                          : UdsRule::Type::Uint64;
		stats.emplace(*id, UdsRule {*id, {}, value_type, maximum, *initial});
	}
	UdsRules events;
	for (const auto& rule: extraction["statsExtractionRuleArray"]) {
		if (!rule.is_object() || !rule.contains("condition") || !rule.contains("action")) {
			continue;
		}
		const auto& condition = rule["condition"];
		const auto& action    = rule["action"];
		if (!condition.is_object() || condition.size() != 1 || !condition.contains("eventName") ||
		    !condition["eventName"].is_string() || !action.is_object() ||
		    !action.contains("input") || !action["input"].is_string() ||
		    !action.contains("output") || !action["output"].is_object() ||
		    !action["output"].contains("statId")) {
			continue;
		}
		const auto  id    = ReadUnsigned(action["output"]["statId"]);
		const auto& input = action["input"].get_ref<const std::string&>();
		if (!id || !stats.contains(*id) || !input.starts_with("$.") || input.size() == 2 ||
		    input.find_first_not_of(
		        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_", 2) !=
		        std::string::npos) {
			continue;
		}
		auto parsed  = stats.at(*id);
		parsed.input = input.substr(2);
		events[condition["eventName"].get<std::string>()].push_back(std::move(parsed));
	}
	return events;
}

std::filesystem::path UdsPackagePath(uint32_t service_label) {
	return std::filesystem::path("sce_sys/uds") / fmt::format("uds{:02}.ucp", service_label);
}

UdsResult ApplyUdsEvent(const Package& package, UnlockData& unlocks, std::string_view event_name,
                        const std::map<std::string, UdsInteger>& properties) {
	const auto rules = package.event_rules.find(std::string(event_name));
	if (rules == package.event_rules.end()) {
		return {};
	}
	UdsResult                                  result;
	std::vector<std::pair<uint64_t, uint64_t>> values;
	for (const auto& rule: rules->second) {
		const auto value = properties.find(rule.input);
		if (value == properties.end()) continue;
		std::optional<uint64_t> number;
		switch (rule.type) {
			case UdsRule::Type::Uint32:
				if (const auto* input = std::get_if<uint32_t>(&value->second)) number = *input;
				break;
			case UdsRule::Type::Uint64:
				if (const auto* input = std::get_if<uint64_t>(&value->second)) number = *input;
				break;
			case UdsRule::Type::Int64:
				if (const auto* input = std::get_if<int64_t>(&value->second); input && *input >= 0)
					number = static_cast<uint64_t>(*input);
				break;
		}
		if (!number) continue;
		if (rule.maximum) {
			auto [stat, inserted] = unlocks.max_stats.try_emplace(rule.stat_id, rule.initial_value);
			const auto next       = std::max({stat->second, rule.initial_value, *number});
			result.stats_changed |= inserted || next != stat->second;
			stat->second = next;
			number       = next;
		}
		values.emplace_back(rule.stat_id, *number);
	}
	for (const auto& [id, trophy]: package.trophies) {
		if (trophy.grade == 1 || unlocks.unlocked.contains(id) || !trophy.uds_stat_id ||
		    !trophy.target) {
			continue;
		}
		for (const auto& [stat_id, number]: values) {
			if (stat_id != *trophy.uds_stat_id) continue;
			bool matched = false;
			switch (trophy.comparison) {
				case Comparison::GreaterEqual: matched = number >= *trophy.target; break;
				case Comparison::Greater: matched = number > *trophy.target; break;
				case Comparison::LessEqual: matched = number <= *trophy.target; break;
				case Comparison::Less: matched = number < *trophy.target; break;
				case Comparison::None: break;
			}
			if (matched) {
				result.trophies.push_back(id);
				break;
			}
		}
	}
	return result;
}

static bool IsProgressTrophy(const Trophy& trophy) {
	return trophy.grade != 1 && trophy.progressive && trophy.target;
}

bool UpdateTrophyProgress(const Package& package, int trophy_id, uint64_t value,
                          UnlockData& unlocks, bool& changed) {
	changed          = false;
	const auto found = package.trophies.find(trophy_id);
	if (found == package.trophies.end() || !IsProgressTrophy(found->second) ||
	    unlocks.unlocked.contains(trophy_id)) {
		return false;
	}
	auto [current, inserted] = unlocks.progress.try_emplace(trophy_id, value);
	if (inserted || current->second < value) {
		current->second = value;
		changed         = true;
	}
	return current->second >= *found->second.target;
}

std::optional<uint64_t> TrophyProgress(const Trophy& trophy, const UnlockData& unlocks) {
	if (!IsProgressTrophy(trophy)) {
		return std::nullopt;
	}
	std::optional<uint64_t> progress;
	if (trophy.uds_stat_id) {
		if (const auto stat = unlocks.max_stats.find(*trophy.uds_stat_id);
		    stat != unlocks.max_stats.end()) {
			progress = stat->second;
		}
	}
	if (const auto reported = unlocks.progress.find(trophy.id);
	    reported != unlocks.progress.end()) {
		progress = std::max(progress.value_or(0), reported->second);
	}
	if (progress) {
		progress = std::min(*progress, *trophy.target);
	}
	return progress;
}

uint32_t Progress::Percentage() const {
	const auto points        = total_grade[2] * 6 + total_grade[3] * 2 + total_grade[4];
	const auto earned_points = earned_grade[2] * 6 + earned_grade[3] * 2 + earned_grade[4];
	return points == 0 ? 0 : earned_points * 100 / points;
}

Progress GetProgress(const Package& package, const UnlockData& unlocks, std::optional<int> group) {
	Progress progress;
	for (const auto& [id, trophy]: package.trophies) {
		if (group && trophy.group_id != *group) {
			continue;
		}
		++progress.total;
		++progress.total_grade[trophy.grade];
		if (unlocks.unlocked.contains(id)) {
			++progress.earned;
			++progress.earned_grade[trophy.grade];
		}
	}
	return progress;
}

} // namespace Common::Trophies
