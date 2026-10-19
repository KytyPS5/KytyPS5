#include "loader/additionalContent.h"

#include "common/logging/log.h"
#include "common/stringUtils.h"

#include <algorithm>
#include <string_view>

namespace Loader {

static std::vector<AddContentEntry> g_add_content;
static std::vector<AddContentEntry> g_local_entitlements;

void AddContentLoad(const std::filesystem::path& dir, const std::string& title_id,
                    const std::filesystem::path& app0_dir) {
	g_add_content = Common::FindAddContent(dir, title_id);
	g_local_entitlements = app0_dir.empty() ? std::vector<AddContentEntry> {} :
	    Common::ReadLocalEntitlements(app0_dir / "dlc_emu.ini", title_id);
	std::erase_if(g_local_entitlements, [](const auto& local) {
		return std::ranges::any_of(g_add_content, [&](const auto& installed) {
			return installed.entitlement_label == local.entitlement_label;
		});
	});
	for (const auto& entry: g_local_entitlements) {
		LOGF("Local entitlement: %s (PSAL, NO_EXTRA_DATA)\n", entry.content_id.c_str());
	}
	for (const auto& entry: g_add_content) {
		LOGF("AddContent: registered %s -> %s\n", entry.content_id.c_str(),
		     Common::PathToString(entry.mount_dir).c_str());
	}
}

const std::vector<AddContentEntry>& AddContentGetAll() {
	return g_add_content;
}

const std::vector<AddContentEntry>& LocalEntitlementsGetAll() {
	return g_local_entitlements;
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
