#ifndef KYTY_COMMON_TROPHIES_H_
#define KYTY_COMMON_TROPHIES_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Common::Trophies {

inline constexpr char PackageDirectory[] = "sce_sys/trophy2";

struct Trophy {
	int                     id          = 0;
	int                     group_id    = -1;
	int                     platinum_id = -1;
	int                     grade       = 0;
	std::optional<uint64_t> target;
	std::optional<uint64_t> uds_stat_id;
	bool                    progressive = false;
	std::string             name;
	std::string             description;
	std::string             reward;
	std::vector<std::byte>  icon_png;
	bool                    hidden     = false;
	bool                    has_reward = false;
};

struct Package {
	std::string                title;
	std::map<int, std::string> groups;
	std::map<int, Trophy>      trophies;
	// UDS event name -> stat ID, from uds00.ucp; empty when that file is unavailable.
	std::map<std::string, std::set<uint64_t>> event_stats;
};

struct UnlockData {
	std::set<int>              unlocked;
	std::map<int, std::string> dates;
};

[[nodiscard]] Package LoadPackage(const std::filesystem::path& path, int console_language);
[[nodiscard]] std::filesystem::path PackagePath(uint32_t service_label);
[[nodiscard]] std::filesystem::path UdsPackagePath(uint32_t service_label);
[[nodiscard]] std::map<std::string, std::set<uint64_t>> ParseUdsEventStats(std::span<const std::byte> data);
[[nodiscard]] std::map<std::string, std::set<uint64_t>> LoadUdsEventStats(const std::filesystem::path& path);
[[nodiscard]] std::vector<int> FindUdsTrophies(const Package& package, std::string_view event_name,
                                               uint64_t counter);
[[nodiscard]] std::filesystem::path UnlocksPath(const std::filesystem::path& root,
                                                std::string_view title_id, int user_id,
                                                uint32_t service_label);
[[nodiscard]] UnlockData            LoadUnlockData(const std::filesystem::path& path);
[[nodiscard]] bool SaveUnlockData(const std::filesystem::path& path, const UnlockData& unlocks);
[[nodiscard]] std::optional<int> FindUdsTrophy(const Package& package, std::string_view event_name,
                                               uint64_t counter);

} // namespace Common::Trophies

#endif // KYTY_COMMON_TROPHIES_H_
