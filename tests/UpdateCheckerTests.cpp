#include "updateChecker.h"

#include <QCoreApplication>
#include <cstdlib>
#include <iostream>

namespace {

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "Assertion failed: " << message << std::endl;
		std::abort();
	}
}

#define REQUIRE(expr) Check((expr), #expr)

} // namespace

void TestIsNewerRelease() {
	// 1. Same tag: should NOT be newer
	REQUIRE(!UpdateChecker::IsNewerRelease(QStringLiteral("KytyPS5-2026-09-04-d96d0ee"),
	                                      QStringLiteral("KytyPS5-2026-09-04-d96d0ee")));

	// 2. Older remote tag: should NOT be newer (prevents offering downgrade on stale feed)
	REQUIRE(!UpdateChecker::IsNewerRelease(QStringLiteral("KytyPS5-2026-09-03-c2189de"),
	                                      QStringLiteral("KytyPS5-2026-09-04-d96d0ee")));

	// 3. Newer remote tag: SHOULD be newer
	REQUIRE(UpdateChecker::IsNewerRelease(QStringLiteral("KytyPS5-2026-09-05-a1b2c3d"),
	                                     QStringLiteral("KytyPS5-2026-09-04-d96d0ee")));

	// 4. Same day but different hash: returns true so the launcher verifies with GitHub
	REQUIRE(UpdateChecker::IsNewerRelease(QStringLiteral("KytyPS5-2026-09-04-040b2b9"),
	                                     QStringLiteral("KytyPS5-2026-09-04-d96d0ee")));

	// 5. Empty tag
	REQUIRE(!UpdateChecker::IsNewerRelease(QString(), QStringLiteral("KytyPS5-2026-09-04-d96d0ee")));

	// 6. Non-standard tags without date format: returns true if tags differ
	REQUIRE(UpdateChecker::IsNewerRelease(QStringLiteral("v2.0.0"), QStringLiteral("v1.0.0")));

	std::cout << "[PASS] TestIsNewerRelease" << std::endl;
}

void TestParseUpdateInfo() {
	// 1. Website feed JSON (has "tag" and "html_url")
	const QByteArray website_json = R"({
		"tag": "KytyPS5-2026-09-04-d96d0ee",
		"html_url": "https://github.com/KytyPS5/KytyPS5/releases/tag/KytyPS5-2026-09-04-d96d0ee"
	})";
	const auto info1 = UpdateChecker::ParseUpdateInfo(website_json);
	REQUIRE(info1.error.isEmpty());
	REQUIRE(info1.tag == QStringLiteral("KytyPS5-2026-09-04-d96d0ee"));
	REQUIRE(info1.page_url == QUrl(QStringLiteral("https://github.com/KytyPS5/KytyPS5/releases/tag/KytyPS5-2026-09-04-d96d0ee")));

	// 2. GitHub Releases API JSON (has "tag_name" and "html_url")
	const QByteArray github_json = R"({
		"tag_name": "KytyPS5-2026-09-04-d96d0ee",
		"html_url": "https://github.com/KytyPS5/KytyPS5/releases/tag/KytyPS5-2026-09-04-d96d0ee"
	})";
	const auto info2 = UpdateChecker::ParseUpdateInfo(github_json);
	REQUIRE(info2.error.isEmpty());
	REQUIRE(info2.tag == QStringLiteral("KytyPS5-2026-09-04-d96d0ee"));

	// 3. Invalid / malformed JSON
	const auto info_bad = UpdateChecker::ParseUpdateInfo("not a json document");
	REQUIRE(!info_bad.error.isEmpty());

	// 4. Incomplete JSON (missing tag)
	const auto info_incomplete = UpdateChecker::ParseUpdateInfo(R"({"html_url": "https://example.com"})");
	REQUIRE(!info_incomplete.error.isEmpty());

	// 5. Incomplete JSON (non-https page_url)
	const auto info_insecure = UpdateChecker::ParseUpdateInfo(R"({"tag": "v1.0", "html_url": "http://example.com"})");
	REQUIRE(!info_insecure.error.isEmpty());

	std::cout << "[PASS] TestParseUpdateInfo" << std::endl;
}

int main(int argc, char* argv[]) {
	QCoreApplication app(argc, argv);

	TestIsNewerRelease();
	TestParseUpdateInfo();

	std::cout << "All UpdateChecker tests passed successfully!" << std::endl;
	return 0;
}
