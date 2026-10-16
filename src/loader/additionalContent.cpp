#include "loader/additionalContent.h"

#include "common/logging/log.h"
#include "common/stringUtils.h"

#include <algorithm>
#include <string_view>

namespace Loader {

static std::vector<AddContentEntry> g_add_content;

void AddContentLoad(const std::filesystem::path& dir, const std::string& title_id) {
	g_add_content = Common::FindAddContent(dir, title_id);
	for (const auto& entry: g_add_content) {
		LOGF("AddContent: registered %s -> %s\n", entry.content_id.c_str(),
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
	const std::string_view value(label, std::find(label, label + label_size, '\0') - label);
	const auto             it =
	    std::find_if(g_add_content.begin(), g_add_content.end(),
	                 [&](const auto& entry) { return entry.entitlement_label == value; });
	return it == g_add_content.end() ? nullptr : &*it;
}

} // namespace Loader
