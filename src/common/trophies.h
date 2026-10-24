#ifndef KYTY_COMMON_TROPHIES_H_
#define KYTY_COMMON_TROPHIES_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Common::Trophies {

inline constexpr char     PackageDirectory[] = "sce_sys/trophy2";
inline constexpr uint64_t UnixEpochTick      = 62135596800000000ULL;

enum class Comparison { None, GreaterEqual, Greater, LessEqual, Less };

struct Trophy {
	int                     id          = 0;
	int                     group_id    = -1;
	int                     platinum_id = -1;
	int                     grade       = 0;
	std::optional<uint64_t> target;
	std::optional<uint64_t> uds_stat_id;
	Comparison              comparison  = Comparison::None;
	bool                    progressive = false;
	std::string             name;
	std::string             description;
	std::string             reward;
	std::vector<std::byte>  icon_png;
	bool                    hidden     = false;
	bool                    has_reward = false;
};

using UdsInteger = std::variant<int32_t, uint32_t, int64_t, uint64_t>;
// Aggregated stat value: signed stats hold int64_t, unsigned ones uint64_t.
using UdsValue = std::variant<int64_t, uint64_t>;

enum class Aggregation { Latest, Max, Min, Sum, Count };

struct UdsRule {
	uint64_t    stat_id;
	std::string input; // Event property name; empty for count stats.
	size_t      type;  // UdsInteger alternative index the event property must match.
	Aggregation aggregation;
};

using UdsRules = std::map<std::string, std::vector<UdsRule>>;
using UdsStats = std::map<uint64_t, UdsValue>;

struct Package {
	std::string                title;
	std::map<int, std::string> groups;
	std::map<int, Trophy>      trophies;
	UdsRules                   event_rules;
};

struct UnlockData {
	std::set<int>           unlocked;
	std::map<int, uint64_t> timestamps;
	UdsStats                stats;
};

struct Progress {
	std::array<uint32_t, 5> total_grade {};
	std::array<uint32_t, 5> earned_grade {};
	uint32_t                total  = 0;
	uint32_t                earned = 0;

	[[nodiscard]] uint32_t Percentage() const;
};

[[nodiscard]] Progress GetProgress(const Package& package, const UnlockData& unlocks,
                                   std::optional<int> group = std::nullopt);

[[nodiscard]] Package LoadPackage(const std::filesystem::path& path, int console_language);
[[nodiscard]] std::filesystem::path PackagePath(uint32_t service_label);
[[nodiscard]] std::filesystem::path UdsPackagePath(uint32_t service_label);
[[nodiscard]] UdsRules              LoadUdsRules(const std::filesystem::path& path);
// Folds the event into the stats it feeds and returns trophies whose condition now holds.
// Only stats of still-locked trophies are updated; `changed` reports whether `stats` was modified.
[[nodiscard]] std::vector<int> ApplyUdsEvent(const Package& package, std::string_view event_name,
                                             const std::map<std::string, UdsInteger>& properties,
                                             UnlockData& unlocks, bool& changed);
[[nodiscard]] std::optional<uint64_t> StatProgress(const Trophy& trophy, const UdsStats& stats);
[[nodiscard]] std::filesystem::path   UnlocksPath(const std::filesystem::path& root,
                                                  std::string_view title_id, int user_id,
                                                  uint32_t service_label);
[[nodiscard]] UnlockData              LoadUnlockData(const std::filesystem::path& path);
[[nodiscard]] bool SaveUnlockData(const std::filesystem::path& path, const UnlockData& unlocks);

} // namespace Common::Trophies

#endif // KYTY_COMMON_TROPHIES_H_
