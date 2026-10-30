// Tests for the PSN entitlements answer of a signed-in account.

#include "libs/npWebApi2Entitlements.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace {

using Libs::Network::OwnedEntitlementsJson;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

// The IDs of the answer, or one "invalid" entry when it is not the expected JSON.
std::vector<std::string> Ids(const std::string& path) {
	const auto answer = nlohmann::json::parse(OwnedEntitlementsJson(path), nullptr, false);
	if (answer.is_discarded() || !answer.contains("entitlements") ||
	    answer["totalResults"] != answer["entitlements"].size()) {
		return {"invalid"};
	}
	std::vector<std::string> ids;
	for (const auto& entry: answer["entitlements"]) {
		if (entry["activeFlag"] != true) return {"invalid"};
		ids.push_back(entry["id"]);
	}
	return ids;
}

void TestTheIdsOfTheQueryAreOwned() {
	Check(Ids("/v1/users/me/entitlements?entitlementIds=AAA111,BBB_222") ==
	          std::vector<std::string> {"AAA111", "BBB_222"},
	      "both IDs of the list are owned");
	Check(Ids("/v1/users/me/entitlements?limit=5&entitlementIds=AAA111%2CBBB-222&fields=x") ==
	          std::vector<std::string> {"AAA111", "BBB-222"},
	      "an escaped comma splits the list, and other parameters are not IDs");
}

void TestTheIdOfThePathIsOwned() {
	Check(Ids("/v1/users/me/entitlements/CCC333") == std::vector<std::string> {"CCC333"}, "the path names one");
	Check(Ids("/v1/users/me/entitlements/CCC333/details?x=1") == std::vector<std::string> {"CCC333"},
	      "the segment ends at the next slash");
}

void TestARequestThatNamesNoneGetsAnEmptyList() {
	Check(Ids("/v1/users/me/entitlements").empty(), "no IDs, no entitlements");
	Check(Ids("/v1/users/me/entitlements?entitlementIds=").empty(), "an empty list, no entitlements");
	Check(Ids("").empty(), "an empty path, no entitlements");
}

void TestANameThatIsNotAnIdIsSkipped() {
	Check(Ids(R"(/v1/users/me/entitlements?entitlementIds=GOOD1,bad"x,,%22quoted%22)") ==
	          std::vector<std::string> {"GOOD1"},
	      "quotes and escapes are not copied into the answer");
}

} // namespace

int main() {
	TestTheIdsOfTheQueryAreOwned();
	TestTheIdOfThePathIsOwned();
	TestARequestThatNamesNoneGetsAnEmptyList();
	TestANameThatIsNotAnIdIsSkipped();
	if (g_failures != 0) {
		std::cerr << g_failures << " NP WebApi2 entitlement check(s) failed\n";
		return 1;
	}
	std::cout << "NP WebApi2 entitlement tests passed\n";
	return 0;
}
