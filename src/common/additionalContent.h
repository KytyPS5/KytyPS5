#ifndef KYTY_COMMON_ADDITIONAL_CONTENT_H_
#define KYTY_COMMON_ADDITIONAL_CONTENT_H_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Common {

struct AddContentEntry {
	std::string           content_id;
	std::string           entitlement_label;
	std::filesystem::path mount_dir; // Empty for an add-on without extra data.
	std::string           title;
};

// Finds packages for this title, deduplicated by entitlement label.
std::vector<AddContentEntry> FindAddContent(const std::filesystem::path& dir,
                                            const std::string&           title_id);

// Parses a user add-on: "LABEL" without extra data, or "LABEL=DIR" with its data in DIR.
std::optional<AddContentEntry> ParseAddContent(std::string_view value);

} // namespace Common

#endif /* KYTY_COMMON_ADDITIONAL_CONTENT_H_ */
