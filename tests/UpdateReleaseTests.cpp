#include "updateRelease.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <cstdlib>

namespace {

const QUrl EXPECTED_PAGE_URL(
    QStringLiteral("https://github.com/KytyPS5/KytyPS5/releases/tag/KytyPS5-2026-10-10-abcdef0"));
const QByteArray EXPECTED_DOWNLOAD_URL =
    "https://github.com/KytyPS5/KytyPS5/releases/download/v1%2Fstable%2Btest/"
    "v1%2Fstable%2Btest-Linux-x86_64.tar.gz";

const QString TAG    = QStringLiteral("KytyPS5-2026-10-10-abcdef0");
const QString SUFFIX = QStringLiteral("-Linux-x86_64.tar.gz");

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "UpdateReleaseTests: %s\n", message);
		std::abort();
	}
}

QJsonObject Release(const QString& suffix = SUFFIX, const QString& tag = TAG) {
	const QJsonObject asset {
	    {QStringLiteral("name"), tag + suffix},
	    {QStringLiteral("size"), 1234},
	    {QStringLiteral("digest"), QStringLiteral("sha256:") + QString(64, QLatin1Char('a'))}};
	return {{QStringLiteral("tag_name"), tag},
	        {QStringLiteral("published_at"), QStringLiteral("2026-10-10T12:00:00Z")},
	        {QStringLiteral("draft"), false},
	        {QStringLiteral("prerelease"), false},
	        {QStringLiteral("assets"), QJsonArray {asset}}};
}

UpdateRelease::Info Parse(const QJsonObject& root, const QString& suffix = SUFFIX) {
	return UpdateRelease::Parse(QJsonDocument(root).toJson(QJsonDocument::Compact), suffix);
}

void CheckInvalidAsset(const QString& key, const QJsonValue& value) {
	auto root  = Release();
	auto asset = root.value(QStringLiteral("assets")).toArray().first().toObject();
	asset.insert(key, value);
	root.insert(QStringLiteral("assets"), QJsonArray {asset});
	const auto info = Parse(root);
	Check(!info.error.isEmpty() && info.download_url.isEmpty() && info.sha256.isEmpty(),
	      "invalid asset was accepted");
	Check(info.tag == TAG && !info.page_url.isEmpty() && info.published_at.isValid(),
	      "invalid package discarded valid release metadata");
}

void TestParsing() {
	for (const auto& suffix:
	     {SUFFIX, QStringLiteral("-Windows-x64.zip"), QStringLiteral("-macOS-x86_64.zip")}) {
		const auto info = Parse(Release(suffix), suffix);
		Check(info.error.isEmpty() && info.tag == TAG && info.size == 1234,
		      "valid platform package was rejected");
		Check(info.sha256 == QByteArray(32, static_cast<char>(0xaa)), "checksum was not decoded");
		Check(info.download_url.fileName() == TAG + suffix, "wrong asset was selected");
		Check(info.page_url == EXPECTED_PAGE_URL,
		      "release page was not derived from the official repository");
	}
	const auto encoded = Parse(Release(SUFFIX, QStringLiteral("v1/stable+test")));
	Check(encoded.download_url.toEncoded() == EXPECTED_DOWNLOAD_URL,
	      "release tag and asset name were not encoded as URL segments");
	Check(!UpdateRelease::Parse("not JSON", SUFFIX).error.isEmpty(), "malformed JSON was accepted");
	Check(!UpdateRelease::Parse("[]", SUFFIX).error.isEmpty(), "JSON array was accepted");
	Check(!Parse(Release(), {}).error.isEmpty(), "unsupported platform was accepted");
	Check(!Parse(Release(), QStringLiteral("-Windows-x64.zip")).error.isEmpty(),
	      "missing asset was accepted");

	for (const auto& key: {QStringLiteral("draft"), QStringLiteral("prerelease")}) {
		auto root = Release();
		root.insert(key, true);
		Check(!Parse(root).error.isEmpty(), "draft or prerelease was accepted");
	}
	for (const auto& field: {QStringLiteral("tag_name"), QStringLiteral("published_at")}) {
		auto root = Release();
		root.remove(field);
		Check(!Parse(root).error.isEmpty(), "missing release metadata was accepted");
	}

	for (const auto& digest: {QString(), QStringLiteral("sha256:abc"),
	                          QStringLiteral("sha256:") + QString(64, QLatin1Char('z'))}) {
		CheckInvalidAsset(QStringLiteral("digest"), digest);
	}
	for (const double size: {-1., 0., 1.5}) {
		CheckInvalidAsset(QStringLiteral("size"), size);
	}
	CheckInvalidAsset(QStringLiteral("name"), QStringLiteral("another-release") + SUFFIX);
}

void TestOrdering() {
	using UpdateRelease::Compare;
	using UpdateRelease::VersionRelation;
	const auto latest  = Parse(Release());
	auto       current = latest;
	Check(Compare(latest, current) == VersionRelation::Current, "matching tags were not current");
	current.tag = QStringLiteral("KytyPS5-2026-10-10-1234567");
	Check(Compare(latest, current) == VersionRelation::Unknown,
	      "equal timestamps implied an update");
	current.published_at = latest.published_at.addSecs(-60);
	Check(Compare(latest, current) == VersionRelation::Newer, "same-day newer release was missed");
	current.published_at = latest.published_at.addSecs(60);
	Check(Compare(latest, current) == VersionRelation::Older,
	      "older release would cause a downgrade");
	current.published_at = {};
	Check(Compare(latest, current) == VersionRelation::Unknown,
	      "missing current metadata implied an update");
	Check(Compare(latest, {}) == VersionRelation::Unknown,
	      "empty current release implied an update");
}

} // namespace

int main() {
	TestParsing();
	TestOrdering();
	return 0;
}
