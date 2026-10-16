#ifndef KYTY_COMMON_ADDITIONAL_CONTENT_H_
#define KYTY_COMMON_ADDITIONAL_CONTENT_H_

#include <filesystem>
#include <string>
#include <vector>

namespace Common {

struct AddContentEntry {
	std::string           content_id;
	std::string           entitlement_label;
	std::filesystem::path mount_dir;
	std::string           title;
};

// Finds packages for this title, deduplicated by entitlement label.
std::vector<AddContentEntry> FindAddContent(const std::filesystem::path& dir,
                                            const std::string&           title_id);

} // namespace Common

#endif /* KYTY_COMMON_ADDITIONAL_CONTENT_H_ */
