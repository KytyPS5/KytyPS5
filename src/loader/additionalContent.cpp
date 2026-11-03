#include "loader/additionalContent.h"

#include "common/logging/log.h"
#include "common/stringUtils.h"

#include <algorithm>
#include <string_view>

namespace Loader {

static std::vector<AddContentEntry> g_add_content;

static auto FindLabel(std::string_view label) {
	return std::find_if(g_add_content.begin(), g_add_content.end(),
	                    [&](const auto& entry) { return entry.entitlement_label == label; });
}

void AddContentLoad(const std::filesystem::path& dir, const std::vector<AddContentEntry>& user,
                    const std::string& title_id) {
	g_add_content = Common::FindAddContent(dir, title_id);
	for (const auto& entry: user) {
		const auto it = FindLabel(entry.entitlement_label);
		if (it == g_add_content.end()) {
			g_add_content.push_back(entry);
		} else {
			*it = entry;
		}
	}
	for (const auto& entry: g_add_content) {
		LOGF("AddContent: registered %s -> %s\n", entry.entitlement_label.c_str(),
		     Common::PathToString(entry.mount_dir).c_str());
	}
}

const std::vector<AddContentEntry>& AddContentGetAll() {
	return g_add_content;
}

const AddContentEntry* AddContentFind(const char* label, size_t label_size) {
	if (label == nullptr || label_size == 0) {
		return nullptr;
	}
	const auto it = FindLabel({label, std::find(label, label + label_size, '\0')});
	return it == g_add_content.end() ? nullptr : &*it;
}

} // namespace Loader
