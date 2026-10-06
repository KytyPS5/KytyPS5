#ifndef KYTY_COMMON_TROPHIES_H_
#define KYTY_COMMON_TROPHIES_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
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
};

[[nodiscard]] Package LoadPackage(const std::filesystem::path& path, int console_language);
[[nodiscard]] std::filesystem::path PackagePath(uint32_t service_label);
[[nodiscard]] std::filesystem::path UnlocksPath(const std::filesystem::path& root,
                                                std::string_view title_id, int user_id,
                                                uint32_t service_label);
[[nodiscard]] std::set<int>         LoadUnlocks(const std::filesystem::path& path);
[[nodiscard]] bool SaveUnlocks(const std::filesystem::path& path, const std::set<int>& unlocked);

} // namespace Common::Trophies

#endif // KYTY_COMMON_TROPHIES_H_
