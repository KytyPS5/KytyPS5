#include "libs/npWebApi2Entitlements.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <vector>

namespace Libs::Network {

namespace {

constexpr std::string_view ENTITLEMENT_SEGMENT = "/entitlements/";
constexpr std::string_view ENTITLEMENT_IDS     = "entitlementIds=";
constexpr std::string_view ESCAPED_COMMA       = "%2C";

bool IsEntitlementId(const std::string& id) {
	return !id.empty() && std::all_of(id.begin(), id.end(), [](char c) {
		return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
	});
}

// Adds the IDs of a comma-separated list (the comma may be escaped) to `ids`.
void AddIdList(std::string list, std::vector<std::string>& ids) {
	for (size_t at = 0; (at = list.find(ESCAPED_COMMA, at)) != std::string::npos;) {
		list.replace(at, ESCAPED_COMMA.size(), ",");
	}
	size_t begin = 0;
	while (begin <= list.size()) {
		const auto end = std::min(list.find(',', begin), list.size());
		auto       id  = list.substr(begin, end - begin);
		if (IsEntitlementId(id)) ids.push_back(std::move(id));
		begin = end + 1;
	}
}

} // namespace

std::string OwnedEntitlementsJson(const std::string& path) {
	std::vector<std::string> ids;
	const auto               query = path.find('?');
	const auto               route = path.substr(0, query);
	if (const auto at = route.find(ENTITLEMENT_SEGMENT); at != std::string::npos) {
		const auto begin = at + ENTITLEMENT_SEGMENT.size();
		AddIdList(route.substr(begin, route.find('/', begin) - begin), ids);
	}
	if (query != std::string::npos) {
		if (const auto at = path.find(ENTITLEMENT_IDS, query); at != std::string::npos) {
			const auto begin = at + ENTITLEMENT_IDS.size();
			AddIdList(path.substr(begin, path.find('&', begin) - begin), ids);
		}
	}
	std::string items;
	for (const auto& id: ids) {
		if (!items.empty()) items += ",";
		items += R"({"id":")" + id +
		         R"(","entitlementType":"unified","activeFlag":true,"isConsumable":false,"useCount":0,"useLimit":0})";
	}
	const auto count = std::to_string(ids.size());
	return R"({"start":0,"size":)" + count + R"(,"totalResults":)" + count + R"(,"entitlements":[)" + items + "]}";
}

} // namespace Libs::Network
