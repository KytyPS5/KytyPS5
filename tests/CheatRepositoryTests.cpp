#include "cheatFile.h"
#include "cheatRepository.h"
#include "configuration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

extern "C" {
#include <libavutil/aes.h>
#include <libavutil/mem.h>
}

#include <cstdio>
#include <cstdlib>
#include <exception>

namespace {

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "CheatRepositoryTests FAILED: %s\n", message);
		std::abort();
	}
}

QByteArray EncryptMc4(const QByteArray& plaintext) {
	static const uint8_t key[] = "304c6528f659c766110239a51cl5dd9c";
	uint8_t              iv[]  = "u@}kzW2u[u(8DWar";
	auto padded = plaintext;
	while (padded.size() % 16 != 0) {
		padded.append('\0');
	}
	auto* aes = av_aes_alloc();
	if (aes == nullptr) {
		return {};
	}
	if (av_aes_init(aes, key, 256, 0) < 0) {
		av_free(aes);
		return {};
	}
	QByteArray ciphertext(padded.size(), '\0');
	av_aes_crypt(aes, reinterpret_cast<uint8_t*>(ciphertext.data()),
	             reinterpret_cast<const uint8_t*>(padded.constData()),
	             static_cast<int>(padded.size() / 16), iv, 0);
	av_free(aes);
	return ciphertext.toBase64();
}

void TestVersionsAndTitles() {
	// NormalizeVersion
	Check(Cheats::NormalizeVersion(QStringLiteral("01.018.000")) == QStringLiteral("1.18"),
	      "NormalizeVersion trailing/leading zeros failed");
	Check(Cheats::NormalizeVersion(QStringLiteral("v1.018.000")) == QStringLiteral("1.18"),
	      "NormalizeVersion lowercase 'v' prefix failed");
	Check(Cheats::NormalizeVersion(QStringLiteral("V01.018.000")) == QStringLiteral("1.18"),
	      "NormalizeVersion uppercase 'V' prefix failed");
	Check(Cheats::NormalizeVersion(QStringLiteral("01_018_000")) == QStringLiteral("1.18"),
	      "NormalizeVersion underscore failed");
	Check(Cheats::NormalizeVersion(QStringLiteral("01-018-000")) == QStringLiteral("1.18"),
	      "NormalizeVersion hyphen failed");
	Check(Cheats::NormalizeVersion(QStringLiteral("   01.000   ")) == QStringLiteral("1"),
	      "NormalizeVersion whitespace and zeroes failed");
	Check(Cheats::NormalizeVersion(QStringLiteral("")).isEmpty(),
	      "NormalizeVersion empty should be empty");
	Check(Cheats::NormalizeVersion(QStringLiteral("   ")).isEmpty(),
	      "NormalizeVersion whitespace should be empty");

	// VersionsMatch
	Check(Cheats::VersionsMatch(QStringLiteral("01.018.000"), QStringLiteral("01.018.000")),
	      "exact version match failed");
	Check(Cheats::VersionsMatch(QStringLiteral("01.018.000"), QStringLiteral("01.018")),
	      "trailing zero version match failed");
	Check(Cheats::VersionsMatch(QStringLiteral("01.018.000"), QStringLiteral("1.018")),
	      "leading zero version match failed");
	Check(Cheats::VersionsMatch(QStringLiteral("01_018_000"), QStringLiteral("01.018.000")),
	      "underscore vs dot version match failed");
	Check(Cheats::VersionsMatch(QStringLiteral("v1.018.000"), QStringLiteral("01.018.000")),
	      "prefix 'v' version match failed");
	Check(Cheats::VersionsMatch(QStringLiteral("01-018-000"), QStringLiteral("01.018.000")),
	      "hyphenated version match failed");
	Check(!Cheats::VersionsMatch(QStringLiteral("01.018.001"), QStringLiteral("01.018.000")),
	      "mismatched non-zero trailing component should not match");
	Check(!Cheats::VersionsMatch(QStringLiteral("01.018.000"), QStringLiteral("01.007.000")),
	      "different versions should not match");
	Check(!Cheats::VersionsMatch(QStringLiteral(""), QStringLiteral("01.018.000")),
	      "empty version should not match");
	Check(!Cheats::VersionsMatch(QStringLiteral("01.018.000"), QStringLiteral("")),
	      "empty version should not match");
	Check(!Cheats::VersionsMatch(QStringLiteral("   "), QStringLiteral("   ")),
	      "whitespace versions should not match");

	// IsSupportedTitleId
	Check(Cheats::IsSupportedTitleId(QStringLiteral("PPSA21567")), "valid PPSA should be supported");
	Check(Cheats::IsSupportedTitleId(QStringLiteral("ppsa21567")), "lowercase PPSA should be supported");
	Check(Cheats::IsSupportedTitleId(QStringLiteral("CUSA00123")), "valid CUSA should be supported");
	Check(Cheats::IsSupportedTitleId(QStringLiteral("cusa00123")), "lowercase CUSA should be supported");
	Check(!Cheats::IsSupportedTitleId(QStringLiteral("PPSA1234")), "short title ID should be rejected");
	Check(!Cheats::IsSupportedTitleId(QStringLiteral("PPSA123456")), "long title ID should be rejected");
	Check(!Cheats::IsSupportedTitleId(QStringLiteral("ABCD12345")), "non-PPSA/CUSA should be rejected");
	Check(!Cheats::IsSupportedTitleId(QStringLiteral("PPSA-12345")), "hyphenated title ID should be rejected");
	Check(!Cheats::IsSupportedTitleId(QStringLiteral("")), "empty title ID should be rejected");

	// PlanPath
	Check(Cheats::PlanPath(QStringLiteral("PPSA21567")).endsWith(QStringLiteral("PPSA21567.json")),
	      "PlanPath should end with title_id.json");
	Check(Cheats::PlanPath(QStringLiteral("ABCD12345")).isEmpty(),
	      "PlanPath for invalid Title ID should be empty");

	// ModuleTypeToString
	Check(Cheats::ModuleTypeToString(Cheats::ModuleType::Patch) == QStringLiteral("Patch"),
	      "ModuleTypeToString Patch failed");
	Check(Cheats::ModuleTypeToString(Cheats::ModuleType::Cheat) == QStringLiteral("Cheat"),
	      "ModuleTypeToString Cheat failed");
	Check(Cheats::ModuleTypeToString(Cheats::ModuleType::Config) == QStringLiteral("Config"),
	      "ModuleTypeToString Config failed");

	// RemoteFile::Url() fallback test (DEFECT-04)
	Cheats::RemoteFile explicit_file;
	explicit_file.url = QUrl(QStringLiteral("https://example.com/custom.json"));
	Check(explicit_file.Url() == explicit_file.url, "explicit RemoteFile url mismatch");

	Cheats::RemoteFile hen_fallback_file;
	hen_fallback_file.format = QStringLiteral("json");
	hen_fallback_file.name   = QStringLiteral("PPSA21567_01.018.000_Astro.json");
	Check(hen_fallback_file.Url().toString().startsWith(QStringLiteral("https://raw.githubusercontent.com/")),
	      "RemoteFile fallback url should point to HEN Cheats repository");
}

void TestParseModulesCatalog() {
	const QByteArray catalog_json = R"({
		"version": 1,
		"patches": {
			"PPSA21567": [
				{
					"id": "PPSA21567",
					"version": "01.018.000",
					"gameName": "Astro Bot",
					"index": 1,
					"title": "Astro Bot non-RT",
					"author": "Community",
					"path": "patches/json/PPSA21567_01_018_000_Astro_Bot_1.json",
					"sha256": "abc123"
				}
			],
			"PPSA99999": [
				{
					"id": "PPSA99999",
					"version": "01.000.000",
					"title": "Other Game Patch",
					"path": "patches/json/PPSA99999_01_000_000.json"
				}
			]
		},
		"cheats": {
			"PPSA21567": [
				{
					"id": "PPSA21567",
					"version": "01.018.000",
					"title": "Infinite Lives",
					"author": "ModderX",
					"path": "cheats/json/PPSA21567_01_018_000_Astro_Bot_1.json",
					"sha256": "def456"
				}
			]
		},
		"configs": {
			"PPSA21567": [
				{
					"id": "PPSA21567",
					"version": "01.018.000",
					"title": "60 FPS Preset",
					"author": "KytyPS5 Team",
					"path": "configs/json/PPSA21567_01_018_000_Astro_Bot_1.json",
					"sha256": "789ghi"
				}
			]
		}
	})";

	// Test valid catalog lookup with uppercase Title ID
	auto result = Cheats::ParseModulesCatalog(catalog_json, QStringLiteral("PPSA21567"));
	Check(result.error.isEmpty(), "ParseModulesCatalog returned an error");
	Check(result.files.size() == 3, "ParseModulesCatalog should find exactly 3 files for PPSA21567");

	Check(result.files[0].type == Cheats::ModuleType::Patch, "expected Patch type");
	Check(result.files[0].source == QStringLiteral("KytyPS5-Modules"), "expected KytyPS5-Modules source");
	Check(result.files[0].author == QStringLiteral("Community"), "expected Community author");
	Check(result.files[0].url.toString() ==
	          QStringLiteral("https://kytyps5.github.io/KytyPS5-Modules/patches/json/PPSA21567_01_018_000_Astro_Bot_1.json"),
	      "unexpected download URL");

	Check(result.files[1].type == Cheats::ModuleType::Cheat, "expected Cheat type");
	Check(result.files[1].author == QStringLiteral("ModderX"), "expected ModderX author");

	Check(result.files[2].type == Cheats::ModuleType::Config, "expected Config type");
	Check(result.files[2].author == QStringLiteral("KytyPS5 Team"), "expected KytyPS5 Team author");

	// Test case insensitivity (lowercase title_id query)
	auto lower_result = Cheats::ParseModulesCatalog(catalog_json, QStringLiteral("ppsa21567"));
	Check(lower_result.error.isEmpty(), "ParseModulesCatalog case insensitive query failed");
	Check(lower_result.files.size() == 3, "ParseModulesCatalog should find 3 files with lowercase query");

	// Test Title ID filtering: searching for other title ID only returns its entries
	auto other_result = Cheats::ParseModulesCatalog(catalog_json, QStringLiteral("PPSA99999"));
	Check(other_result.files.size() == 1, "PPSA99999 should only return 1 file");
	Check(other_result.files[0].title == QStringLiteral("Other Game Patch"), "mismatched file for PPSA99999");

	// Test Security & Path Traversal Protections
	const QByteArray security_catalog = R"({
		"version": 1,
		"patches": {
			"PPSA21567": [
				{"title": "Traversal 1", "path": "patches/../../etc/passwd.json"},
				{"title": "Traversal 2", "path": "patches/..\\..\\windows\\win.ini.json"},
				{"title": "Absolute Scheme", "path": "http://evil.com/payload.json"},
				{"title": "File Scheme", "path": "file:///C:/secret.json"},
				{"title": "Leading Slash", "path": "/root/patch.json"},
				{"title": "URL Fragment", "path": "patches/test.json#frag"},
				{"title": "URL Query", "path": "patches/test.json?token=123"},
				{"title": "Non-Json Executable", "path": "patches/payload.exe"},
				{"title": "Non-Json Script", "path": "patches/script.sh"},
				{"title": "Empty Path", "path": ""},
				{"title": "Valid File", "path": "patches/json/safe_patch.json"}
			]
		}
	})";
	auto sec_result = Cheats::ParseModulesCatalog(security_catalog, QStringLiteral("PPSA21567"));
	Check(sec_result.error.isEmpty(), "ParseModulesCatalog security catalog should parse without error");
	Check(sec_result.files.size() == 1, "Only the single safe_patch.json should pass security filtering");
	Check(sec_result.files[0].title == QStringLiteral("Valid File"), "Safe patch was not properly preserved");

	// Test missing sections
	const QByteArray partial_catalog = R"({
		"version": 1,
		"configs": {
			"PPSA21567": [
				{"title": "Preset Only", "path": "configs/json/preset.json"}
			]
		}
	})";
	auto part_result = Cheats::ParseModulesCatalog(partial_catalog, QStringLiteral("PPSA21567"));
	Check(part_result.files.size() == 1, "Partial catalog should parse present sections cleanly");

	// Test empty catalog
	auto empty_result = Cheats::ParseModulesCatalog(QByteArray("{}"), QStringLiteral("PPSA21567"));
	Check(empty_result.files.isEmpty() && empty_result.error.isEmpty(), "Empty catalog should return empty list");

	// Test boundary & error conditions (SEC4-MED-01)
	Check(!Cheats::ParseModulesCatalog(catalog_json, QString()).error.isEmpty(),
	      "ParseModulesCatalog should reject empty title ID");
	Check(!Cheats::ParseModulesCatalog(QByteArray(), QStringLiteral("PPSA21567")).error.isEmpty(),
	      "ParseModulesCatalog should reject empty payload");
	const QByteArray huge_catalog(3 * 1024 * 1024, ' ');
	Check(!Cheats::ParseModulesCatalog(huge_catalog, QStringLiteral("PPSA21567")).error.isEmpty(),
	      "ParseModulesCatalog should reject oversized payload");

	// Test malformed JSON
	auto malformed_result = Cheats::ParseModulesCatalog(QByteArray("{ malformed"), QStringLiteral("PPSA21567"));
	Check(!malformed_result.error.isEmpty(), "Malformed catalog should report an error");

	// Test non-object JSON array
	auto array_result = Cheats::ParseModulesCatalog(QByteArray("[1, 2, 3]"), QStringLiteral("PPSA21567"));
	Check(!array_result.error.isEmpty(), "Non-object JSON catalog should report an error");
}

void TestParseIndex() {
	// Test HEN Cheats text catalog parsing
	const QByteArray index_data =
	    "# Comment line to skip\n"
	    "// Another comment line\n"
	    "\n"
	    "PPSA21567_01.018.000_Astro_Bot.json = Astro Bot Infinite Lives\n"
	    "PPSA21567_01.018.000_Astro_Bot_2.json = Astro Bot Invincible\n"
	    "PPSA99999_01.000.000_Other_Game.json = Other Game Mod\n"
	    "PPSA21567_01.018.000_Astro_Bot.mc4 = Astro Bot MC4 Trainer\n"
	    "PPSA21567_01.018.000_Astro_Bot.shn = Astro Bot SHN Cheat\n"
	    "InvalidLineWithoutSeparator.json\n"
	    "NotAValidTitleId_01.000.000.json = Invalid Title ID\n";

	auto json_result = Cheats::ParseIndex(index_data, QStringLiteral("json"), QStringLiteral("PPSA21567"));
	Check(json_result.error.isEmpty(), "ParseIndex json returned error");
	Check(json_result.files.size() == 2, "ParseIndex should find 2 JSON files for PPSA21567");
	Check(json_result.files[0].title == QStringLiteral("Astro Bot Infinite Lives"), "title mismatch");
	Check(json_result.files[0].author == QStringLiteral("HEN Cheats Contributors"), "author mismatch");
	Check(json_result.files[0].source == QStringLiteral("HEN Cheats Collection"), "source mismatch");
	Check(json_result.files[0].type == Cheats::ModuleType::Cheat, "type should be Cheat");
	Check(json_result.files[0].format == QStringLiteral("json"), "format mismatch");

	// Test MC4 format parsing
	auto mc4_result = Cheats::ParseIndex(index_data, QStringLiteral("mc4"), QStringLiteral("PPSA21567"));
	Check(mc4_result.files.size() == 1, "ParseIndex should find 1 MC4 file for PPSA21567");
	Check(mc4_result.files[0].format == QStringLiteral("mc4"), "format mismatch");

	// Test SHN format parsing
	auto shn_result = Cheats::ParseIndex(index_data, QStringLiteral("shn"), QStringLiteral("PPSA21567"));
	Check(shn_result.files.size() == 1, "ParseIndex should find 1 SHN file for PPSA21567");
	Check(shn_result.files[0].format == QStringLiteral("shn"), "format mismatch");

	// Test UTF-8 BOM stripping
	QByteArray bom_index = "\xef\xbb\xbf";
	bom_index += "PPSA21567_01.018.000_Astro_Bot.json = Astro Bot BOM\n";
	auto bom_result = Cheats::ParseIndex(bom_index, QStringLiteral("json"), QStringLiteral("PPSA21567"));
	Check(bom_result.files.size() == 1, "ParseIndex should handle UTF-8 BOM cleanly");
	Check(bom_result.files[0].title == QStringLiteral("Astro Bot BOM"), "BOM line parsed incorrectly");

	// Test negative paths: empty title ID, unsupported format, empty data
	auto no_title = Cheats::ParseIndex(index_data, QStringLiteral("json"), QStringLiteral(""));
	Check(!no_title.error.isEmpty(), "ParseIndex should reject empty Title ID");

	// Test mixed case format and title ID (SEC4-LOW-01)
	auto mixed_result = Cheats::ParseIndex(index_data, QStringLiteral("JSON"), QStringLiteral("ppsa21567"));
	Check(mixed_result.files.size() == 2, "ParseIndex should be case-insensitive for format and title ID");

	auto bad_fmt = Cheats::ParseIndex(index_data, QStringLiteral("exe"), QStringLiteral("PPSA21567"));
	Check(!bad_fmt.error.isEmpty(), "ParseIndex should reject invalid format");

	const QByteArray huge_index(3 * 1024 * 1024, ' ');
	Check(!Cheats::ParseIndex(huge_index, QStringLiteral("json"), QStringLiteral("PPSA21567")).error.isEmpty(),
	      "ParseIndex should reject oversized payload");

	auto empty_data = Cheats::ParseIndex(QByteArray(), QStringLiteral("json"), QStringLiteral("PPSA21567"));
	Check(empty_data.files.isEmpty() && empty_data.error.isEmpty(), "ParseIndex with empty data should return empty");
}

void TestConfigPreset() {
	const QByteArray preset_json = R"({
		"id": "PPSA21567",
		"version": "01.018.000",
		"name": "60 FPS Preset",
		"author": "KytyPS5 Team",
		"description": "High performance preset",
		"config": {
			"graphics": {
				"targetFps": 60,
				"vsync": true,
				"resolutionScale": 1.33
			},
			"cpu": {
				"threadCount": 12
			}
		}
	})";

	const auto doc = QJsonDocument::fromJson(preset_json);
	auto summary = Cheats::SummarizeConfigPreset(doc);
	Check(summary.title == QStringLiteral("60 FPS Preset"), "summary title mismatch");
	Check(summary.author == QStringLiteral("KytyPS5 Team"), "summary author mismatch");
	Check(!summary.settings.isEmpty(), "summary settings should not be empty");

	// Verify formatted settings strings in summary
	bool found_fps = false;
	bool found_vsync = false;
	bool found_scale = false;
	for (const auto& pair: summary.settings) {
		if (pair.first == QStringLiteral("Target FPS (Vblank)") && pair.second == QStringLiteral("60 Hz")) {
			found_fps = true;
		}
		if (pair.first == QStringLiteral("VSync (Present Mode)") && pair.second == QStringLiteral("Enabled (Fifo)")) {
			found_vsync = true;
		}
		if (pair.first == QStringLiteral("Resolution Scale") && pair.second == QStringLiteral("1.33x")) {
			found_scale = true;
		}
	}
	Check(found_fps, "summary settings missing Target FPS");
	Check(found_vsync, "summary settings missing V-Sync");
	Check(found_scale, "summary settings missing Resolution Scale");

	Configuration config;
	QString error;
	bool ok = Cheats::ApplyConfigPreset(&config, doc, &error);
	Check(ok, "ApplyConfigPreset failed");
	Check(config.custom_settings == true, "custom_settings should be enabled");
	Check(config.vblank_frequency == 60, "vblank_frequency should be 60");
	Check(config.present_mode == Configuration::PresentMode::Fifo, "present_mode should be Fifo for vsync");
	Check(config.screen_resolution == QStringLiteral("2560x1440"), "screen_resolution should be 1440p for 1.33x scale");

	// Test Resolution Scaling Buckets
	auto test_scale = [&](double scale, const QString& expected_res) {
		const QByteArray json = QString(R"({
			"name": "Scale Test",
			"config": {"graphics": {"resolutionScale": %1}}
		})").arg(scale).toUtf8();
		Configuration c;
		bool s_ok = Cheats::ApplyConfigPreset(&c, QJsonDocument::fromJson(json), &error);
		Check(s_ok, "ApplyConfigPreset scale failed");
		Check(c.screen_resolution == expected_res, QStringLiteral("expected %1 for scale %2, got %3")
		      .arg(expected_res).arg(scale).arg(c.screen_resolution).toUtf8().constData());
	};
	test_scale(2.0, QStringLiteral("3840x2160"));
	test_scale(1.4, QStringLiteral("2560x1440"));
	test_scale(1.0, QStringLiteral("1920x1080"));
	test_scale(0.5, QStringLiteral("1280x720"));

	// Test Boolean variations (strings "true"/"false", "enabled"/"disabled", "1"/"0", numbers 1/0)
	const QByteArray bool_variants_json = R"({
		"name": "Boolean Variants",
		"config": {
			"graphics": {
				"hdr_enabled": "true",
				"hide_cursor_enabled": "enabled",
				"stretch_enabled": "1",
				"flush_draws": 1,
				"tessellation_enabled": "false",
				"native_rt": "disabled",
				"shader_cache_enabled": "0",
				"perf_overlay_enabled": 0
			},
			"cpu": {
				"amd_cpu_enabled": "true"
			}
		}
	})";
	Configuration bool_config;
	ok = Cheats::ApplyConfigPreset(&bool_config, QJsonDocument::fromJson(bool_variants_json), &error);
	Check(ok, QStringLiteral("Boolean variants preset failed: %1").arg(error).toUtf8().constData());
	Check(bool_config.hdr_enabled == true, "hdr_enabled string 'true' failed");
	Check(bool_config.hide_cursor_enabled == true, "hide_cursor_enabled string 'enabled' failed");
	Check(bool_config.stretch_enabled == true, "stretch_enabled string '1' failed");
	Check(bool_config.flush_draws == 1, "flush_draws integer 1 failed");
	Check(bool_config.tessellation_enabled == false, "tessellation_enabled string 'false' failed");
	Check(bool_config.native_rt == false, "native_rt string 'disabled' failed");
	Check(bool_config.shader_cache_enabled == false, "shader_cache_enabled string '0' failed");
	Check(bool_config.perf_overlay_enabled == false, "perf_overlay_enabled integer 0 failed");
	Check(bool_config.amd_cpu_enabled == true, "amd_cpu_enabled string 'true' failed");

	// Test Present Modes (DEFECT-05: numeric 0/1/2 and string fifo/relaxedfifo/mailbox/immediate)
	auto test_pm = [&](const QJsonValue& val, Configuration::PresentMode expected) {
		QJsonObject grp;
		grp.insert(QStringLiteral("present_mode"), val);
		QJsonObject root;
		root.insert(QStringLiteral("config"), QJsonObject{{QStringLiteral("graphics"), grp}});
		Configuration c;
		QString err;
		bool pm_ok = Cheats::ApplyConfigPreset(&c, QJsonDocument(root), &err);
		Check(pm_ok, "ApplyConfigPreset for present_mode failed");
		Check(c.present_mode == expected, "present_mode mismatch");
	};
	test_pm(0, Configuration::PresentMode::Fifo);
	test_pm(1, Configuration::PresentMode::Mailbox);
	test_pm(2, Configuration::PresentMode::Immediate);
	test_pm(QStringLiteral("fifo"), Configuration::PresentMode::Fifo);
	test_pm(QStringLiteral("relaxedfifo"), Configuration::PresentMode::Fifo);
	test_pm(QStringLiteral("mailbox"), Configuration::PresentMode::Mailbox);
	test_pm(QStringLiteral("immediate"), Configuration::PresentMode::Immediate);

	// Test FPS bounds validation: < 30 and > 360 must be rejected
	const QByteArray low_fps_json = R"({"name": "Low FPS", "config": {"graphics": {"targetFps": 20}}})";
	ok = Cheats::ApplyConfigPreset(&config, QJsonDocument::fromJson(low_fps_json), &error);
	Check(!ok && !error.isEmpty(), "targetFps < 30 should be rejected");

	const QByteArray high_fps_json = R"({"name": "High FPS", "config": {"graphics": {"targetFps": 500}}})";
	ok = Cheats::ApplyConfigPreset(&config, QJsonDocument::fromJson(high_fps_json), &error);
	Check(!ok && !error.isEmpty(), "targetFps > 360 should be rejected");

	// Test invalid screen_resolution bounds/format (SEC4-LOW-02)
	const QByteArray invalid_res_json = R"({"name": "Invalid Res", "config": {"graphics": {"screen_resolution": "99999x99999"}}})";
	ok = Cheats::ApplyConfigPreset(&config, QJsonDocument::fromJson(invalid_res_json), &error);
	Check(!ok && !error.isEmpty(), "invalid screen_resolution should be rejected");

	// Test negative preset inputs: null config, non-object JSON, missing config object
	Check(!Cheats::ApplyConfigPreset(nullptr, doc, &error), "null config pointer should return false");
	Check(!Cheats::ApplyConfigPreset(&config, QJsonDocument::fromJson(QByteArray("[1, 2]")), &error),
	      "non-object JSON should return false");
	Check(!Cheats::ApplyConfigPreset(&config, QJsonDocument::fromJson(QByteArray(R"({"name": "No Config"})")), &error),
	      "missing config object should return false");
	Check(Cheats::SummarizeConfigPreset(QJsonDocument()).settings.isEmpty(),
	      "SummarizeConfigPreset on invalid document should return empty");
}

void TestMergeDocumentsAndPrepare() {
	const QByteArray base_json = R"({
		"id": "PPSA21567",
		"version": "01.018.000",
		"process": "eboot.bin",
		"mods": [
			{
				"name": "Base Patch",
				"source": "KytyPS5-Modules",
				"author": "Dev1",
				"enabled": true,
				"memory": [{"offset": "0x1000", "on": "9090", "off": "0000"}]
			}
		]
	})";

	const QByteArray new_json = R"({
		"id": "PPSA21567",
		"version": "01.018.000",
		"process": "eboot.bin",
		"mods": [
			{
				"name": "Infinite Health",
				"memory": [{"offset": "0x2000", "on": "cccc", "off": "1111"}]
			}
		]
	})";

	const auto base_doc = QJsonDocument::fromJson(base_json);
	const auto new_doc  = QJsonDocument::fromJson(new_json);

	const auto merged = Cheats::MergeDocuments(base_doc, new_doc, QStringLiteral("HEN Cheats Collection"),
	                                           QStringLiteral("TeeKay87"), QStringLiteral("eboot.bin"));
	const auto mods = merged.object().value(QStringLiteral("mods")).toArray();
	Check(mods.size() == 2, "merged mods should contain both base patch and new cheat");
	Check(mods[0].toObject().value(QStringLiteral("enabled")).toBool() == true,
	      "existing mod should retain enabled state");
	Check(mods[0].toObject().value(QStringLiteral("source")).toString() == QStringLiteral("KytyPS5-Modules"),
	      "existing mod should retain original source");
	Check(mods[0].toObject().value(QStringLiteral("author")).toString() == QStringLiteral("Dev1"),
	      "existing mod should retain original author");
	Check(mods[1].toObject().value(QStringLiteral("enabled")).toBool() == false,
	      "new mod should be disabled by default");
	Check(mods[1].toObject().value(QStringLiteral("source")).toString() == QStringLiteral("HEN Cheats Collection"),
	      "new mod should have source tagged");
	Check(mods[1].toObject().value(QStringLiteral("author")).toString() == QStringLiteral("TeeKay87"),
	      "new mod should have author tagged");

	// Test Mod Deduplication & Conflict Resolution
	const QByteArray dup_mod_json = R"({
		"id": "PPSA21567",
		"version": "01.018.000",
		"process": "eboot.bin",
		"mods": [
			{
				"name": "base patch",
				"memory": [{"offset": "0x9999", "on": "ffff", "off": "0000"}]
			},
			{
				"name": "Infinite Health",
				"memory": [{"offset": "0x2000", "on": "cccc", "off": "1111"}]
			}
		]
	})";
	const auto dup_merged = Cheats::MergeDocuments(base_doc, QJsonDocument::fromJson(dup_mod_json),
	                                               QStringLiteral("Source"), QStringLiteral("Author"),
	                                               QStringLiteral("eboot.bin"));
	const auto dup_mods = dup_merged.object().value(QStringLiteral("mods")).toArray();
	Check(dup_mods.size() == 2, "duplicate mod names should be deduplicated");
	Check(dup_mods[0].toObject().value(QStringLiteral("name")).toString() == QStringLiteral("Base Patch"),
	      "original base mod should be retained during deduplication");

	// Test Title ID mismatch protection
	const QByteArray diff_id_json = R"({
		"id": "PPSA99999",
		"version": "01.018.000",
		"mods": []
	})";
	QString merge_err;
	const auto rejected_merge = Cheats::MergeDocuments(base_doc, QJsonDocument::fromJson(diff_id_json),
	                                                   QStringLiteral("Source"), QStringLiteral("Author"),
	                                                   QStringLiteral("eboot.bin"), {}, &merge_err);
	Check(!rejected_merge.isObject(), "merge should reject mismatching Title IDs");
	Check(!merge_err.isEmpty(), "merge should report mismatch error");

	// Test process mismatch protection
	const QByteArray diff_proc_json = R"({
		"id": "PPSA21567",
		"version": "01.018.000",
		"process": "other.bin",
		"mods": [{"name": "Test", "memory": [{"offset": "0x1000", "on": "90", "off": "00"}]}]
	})";
	const auto rej_proc = Cheats::MergeDocuments(base_doc, QJsonDocument::fromJson(diff_proc_json),
	                                             QStringLiteral("Source"), QStringLiteral("Author"),
	                                             QStringLiteral("eboot.bin"), {}, &merge_err);
	Check(!rej_proc.isObject(), "merge should reject mismatching process names");

	// Test version stamping (with allow_version_mismatch = true for cross-version merge)
	const auto versioned_merge = Cheats::MergeDocuments(base_doc, new_doc, QStringLiteral("Source"),
	                                                    QStringLiteral("Author"), QStringLiteral("eboot.bin"),
	                                                    QStringLiteral("01.018.001"), nullptr, {}, true);
	Check(versioned_merge.object().value(QStringLiteral("version")).toString() == QStringLiteral("01.018.001"),
	      "merge should update version to installed game version");
	Check(versioned_merge.object().value(QStringLiteral("mods")).toArray()[0].toObject().value(QStringLiteral("enabled")).toBool() == false,
	      "cross-version merge should safely disable older base mods");

	// Test cross-version replacement of duplicate mod name with updated version
	const QByteArray update_patch_json = R"({
		"id": "PPSA21567",
		"version": "01.018.001",
		"process": "eboot.bin",
		"mods": [
			{
				"name": "Base Patch",
				"memory": [{"offset": "0x1050", "on": "9090", "off": "0000"}]
			}
		]
	})";
	const auto updated_merge = Cheats::MergeDocuments(base_doc, QJsonDocument::fromJson(update_patch_json),
	                                                  QStringLiteral("Source"), QStringLiteral("Author"),
	                                                  QStringLiteral("eboot.bin"), QStringLiteral("01.018.001"),
	                                                  nullptr, {}, true);
	const auto upd_mods = updated_merge.object().value(QStringLiteral("mods")).toArray();
	Check(upd_mods.size() == 1, "updated version should replace stale mod of same name");
	Check(upd_mods[0].toObject().value(QStringLiteral("memory")).toArray()[0].toObject().value(QStringLiteral("offset")).toString() == QStringLiteral("0x1050"),
	      "updated mod offset should take precedence on version upgrade");

	// Test PrepareForImport
	const QByteArray raw_import_json = R"({
		"id": "PPSA21567",
		"version": "01.000.000",
		"process": "eboot.bin",
		"mods": [
			{"name": "Mod 1", "memory": [{"offset": "0x10", "on": "90", "off": "00"}]},
			{"name": "Mod 2", "memory": [{"offset": "0x20", "on": "90", "off": "00"}]}
		]
	})";
	const auto prep_doc = Cheats::PrepareForImport(QJsonDocument::fromJson(raw_import_json),
	                                               QStringLiteral("KytyPS5-Modules"), QStringLiteral("ModderA"),
	                                               QStringLiteral("eboot.bin"), QStringLiteral("01.018.000"),
	                                               QStringLiteral("PPSA21567"));
	Check(prep_doc.isObject(), "PrepareForImport returned invalid document");
	Check(prep_doc.object().value(QStringLiteral("version")).toString() == QStringLiteral("01.018.000"),
	      "PrepareForImport should stamp installed version");
	const auto prep_mods = prep_doc.object().value(QStringLiteral("mods")).toArray();
	Check(prep_mods.size() == 2, "PrepareForImport should retain both sanitized mods");
	Check(prep_mods[0].toObject().value(QStringLiteral("source")).toString() == QStringLiteral("KytyPS5-Modules"),
	      "PrepareForImport should stamp source");
	Check(prep_mods[0].toObject().value(QStringLiteral("author")).toString() == QStringLiteral("ModderA"),
	      "PrepareForImport should stamp author");
	Check(prep_mods[0].toObject().value(QStringLiteral("enabled")).toBool() == false,
	      "PrepareForImport should set enabled to false by default");

	// Test WithSelection multi-mask and mask size mismatch
	const QByteArray three_mods_json = R"({
		"id": "PPSA21567",
		"version": "01.018.000",
		"process": "eboot.bin",
		"mods": [
			{"name": "M1", "enabled": false, "memory": [{"offset": "0x10", "on": "90", "off": "00"}]},
			{"name": "M2", "enabled": true, "memory": [{"offset": "0x20", "on": "90", "off": "00"}]},
			{"name": "M3", "enabled": false, "memory": [{"offset": "0x30", "on": "90", "off": "00"}]}
		]
	})";
	const auto three_doc = QJsonDocument::fromJson(three_mods_json);
	const auto selected_doc = Cheats::WithSelection(three_doc, {true, false, true}, QStringLiteral("01.018.000"));
	Check(selected_doc.isObject(), "WithSelection should return valid document");
	const auto sel_mods = selected_doc.object().value(QStringLiteral("mods")).toArray();
	Check(sel_mods[0].toObject().value(QStringLiteral("enabled")).toBool() == true, "M1 should be enabled");
	Check(sel_mods[1].toObject().value(QStringLiteral("enabled")).toBool() == false, "M2 should be disabled");
	Check(sel_mods[2].toObject().value(QStringLiteral("enabled")).toBool() == true, "M3 should be enabled");

	// Mask size mismatch must return empty doc
	const auto mismatched_sel = Cheats::WithSelection(three_doc, {true, false});
	Check(!mismatched_sel.isObject(), "WithSelection should reject mask size mismatch");
}

void TestSuccessiveCrossVersionMerge() {
	const QByteArray v1_json = R"({
		"id": "PPSA21567",
		"version": "01.00",
		"process": "eboot.bin",
		"mods": [
			{
				"name": "Mod A",
				"source": "KytyPS5-Modules",
				"author": "Author1",
				"enabled": true,
				"memory": [{"offset": "0x1000", "on": "9090", "off": "0000"}]
			},
			{
				"name": "Mod B",
				"source": "KytyPS5-Modules",
				"author": "Author2",
				"enabled": true,
				"memory": [{"offset": "0x2000", "on": "9090", "off": "0000"}]
			}
		]
	})";

	// Import 1: Target game version is upgraded to 01.01, but the remote module only provides Mod A for 01.01
	const QByteArray v2_modA_json = R"({
		"id": "PPSA21567",
		"version": "01.01",
		"process": "eboot.bin",
		"mods": [
			{
				"name": "Mod A",
				"memory": [{"offset": "0x1100", "on": "9090", "off": "0000"}]
			}
		]
	})";

	const auto doc_v1 = QJsonDocument::fromJson(v1_json);
	const auto doc_v2_a = QJsonDocument::fromJson(v2_modA_json);

	const auto merge1 = Cheats::MergeDocuments(doc_v1, doc_v2_a, QStringLiteral("KytyPS5-Modules"),
	                                           QStringLiteral("Author1"), QStringLiteral("eboot.bin"),
	                                           QStringLiteral("01.01"), nullptr, {}, true);
	Check(merge1.isObject(), "merge1 should succeed");
	Check(merge1.object().value(QStringLiteral("version")).toString() == QStringLiteral("01.01"),
	      "merge1 should update root version to 01.01");
	const auto merge1_mods = merge1.object().value(QStringLiteral("mods")).toArray();
	Check(merge1_mods.size() == 2, "merge1 should retain both Mod A and Mod B");

	QJsonObject mod_a1, mod_b1;
	for (const auto& v : merge1_mods) {
		const auto obj = v.toObject();
		if (obj.value(QStringLiteral("name")).toString() == QStringLiteral("Mod A")) mod_a1 = obj;
		if (obj.value(QStringLiteral("name")).toString() == QStringLiteral("Mod B")) mod_b1 = obj;
	}
	Check(!mod_a1.isEmpty(), "Mod A should be present in merge1");
	Check(!mod_b1.isEmpty(), "Mod B should be present in merge1");
	Check(mod_a1.value(QStringLiteral("memory")).toArray()[0].toObject().value(QStringLiteral("offset")).toString() == QStringLiteral("0x1100"),
	      "Mod A should have updated offset 0x1100 in merge1");
	Check(mod_b1.value(QStringLiteral("enabled")).toBool() == false,
	      "retained Mod B should be disabled because it was from version 01.00");
	Check(mod_b1.value(QStringLiteral("version")).toString() == QStringLiteral("01.00"),
	      "retained Mod B must preserve its origin version 01.00");

	// Import 2: Successive import where base_doc already has version 01.01, now importing Mod B for 01.01
	const QByteArray v2_modB_json = R"({
		"id": "PPSA21567",
		"version": "01.01",
		"process": "eboot.bin",
		"mods": [
			{
				"name": "Mod B",
				"memory": [{"offset": "0x2200", "on": "9090", "off": "0000"}]
			}
		]
	})";
	const auto doc_v2_b = QJsonDocument::fromJson(v2_modB_json);

	const auto merge2 = Cheats::MergeDocuments(merge1, doc_v2_b, QStringLiteral("KytyPS5-Modules"),
	                                           QStringLiteral("Author2"), QStringLiteral("eboot.bin"),
	                                           QStringLiteral("01.01"), nullptr, {}, true);
	Check(merge2.isObject(), "merge2 should succeed");
	const auto merge2_mods = merge2.object().value(QStringLiteral("mods")).toArray();
	Check(merge2_mods.size() == 2, "merge2 should still have 2 mods");

	QJsonObject mod_a2, mod_b2;
	for (const auto& v : merge2_mods) {
		const auto obj = v.toObject();
		if (obj.value(QStringLiteral("name")).toString() == QStringLiteral("Mod A")) mod_a2 = obj;
		if (obj.value(QStringLiteral("name")).toString() == QStringLiteral("Mod B")) mod_b2 = obj;
	}
	Check(!mod_a2.isEmpty(), "Mod A should still be present in merge2");
	Check(!mod_b2.isEmpty(), "Mod B should be present in merge2");
	Check(mod_b2.value(QStringLiteral("memory")).toArray()[0].toObject().value(QStringLiteral("offset")).toString() == QStringLiteral("0x2200"),
	      "successive import must replace stale 01.00 Mod B with new 01.01 offset 0x2200");
	Check(mod_b2.value(QStringLiteral("version")).toString() == QStringLiteral("01.01"),
	      "Mod B version must now be 01.01");

	// Import 3: Importing Mod B for 01.01 again should preserve existing Mod B (conflict deduplication)
	const auto merge3 = Cheats::MergeDocuments(merge2, doc_v2_b, QStringLiteral("KytyPS5-Modules"),
	                                           QStringLiteral("Author2"), QStringLiteral("eboot.bin"),
	                                           QStringLiteral("01.01"), nullptr, {}, true);
	Check(merge3.isObject(), "merge3 should succeed");
	const auto merge3_mods = merge3.object().value(QStringLiteral("mods")).toArray();
	Check(merge3_mods.size() == 2, "merge3 should not duplicate Mod B");
}

void TestValidateImportAndSave() {
	const QByteArray valid_json = R"({
		"id": "PPSA21567",
		"version": "01.018.000",
		"process": "eboot.bin",
		"master": false,
		"mods": [
			{
				"name": "Speed Mod",
				"memory": [{"offset": "0x1234", "on": "9090", "off": "0000", "section": 0}]
			}
		]
	})";
	const auto valid_doc = QJsonDocument::fromJson(valid_json);
	const auto err = Cheats::ValidateImport(valid_doc, QStringLiteral("PPSA21567"), QStringLiteral("01.018"),
	                                        QStringLiteral("eboot.bin"), false);
	Check(err.isEmpty(), QStringLiteral("ValidateImport should succeed: %1").arg(err).toUtf8().constData());

	// Test ParseDocument exported API (DEFECT-04)
	QJsonDocument parsed_doc;
	QString parse_err = Cheats::ParseDocument(valid_json, &parsed_doc);
	Check(parse_err.isEmpty(), "ParseDocument valid json failed");
	Check(parsed_doc.isObject(), "ParseDocument doc should be valid object");

	parse_err = Cheats::ParseDocument(valid_json, nullptr);
	Check(!parse_err.isEmpty(), "ParseDocument should reject nullptr");

	parse_err = Cheats::ParseDocument(QByteArray("{ malformed"), &parsed_doc);
	Check(!parse_err.isEmpty(), "ParseDocument should reject malformed json");

	parse_err = Cheats::ParseDocument(QByteArray("[1, 2, 3]"), &parsed_doc);
	Check(!parse_err.isEmpty(), "ParseDocument should reject non-object JSON array");

	// Test missing required headers via ValidateImport
	const auto missing_headers_doc = QJsonDocument::fromJson(QByteArray(R"({"id": "PPSA21567"})"));
	const auto missing_headers_err = Cheats::ValidateImport(missing_headers_doc, QStringLiteral("PPSA21567"),
	                                                        QStringLiteral("01.018.000"), QStringLiteral("eboot.bin"));
	Check(!missing_headers_err.isEmpty(), "ValidateImport should reject document missing version, process, or mods");

	// Negative Suite: Malformed Hex, Odd Bytes, Invalid Offsets, and Unsupported Features
	auto test_invalid = [](const char* json_str, const char* expected_desc) {
		const auto doc = QJsonDocument::fromJson(QByteArray(json_str));
		const auto e = Cheats::ValidateImport(doc, QStringLiteral("PPSA21567"), QStringLiteral("01.018.000"),
		                                      QStringLiteral("eboot.bin"), false);
		Check(!e.isEmpty(), QStringLiteral("ValidateImport should reject: %1").arg(expected_desc).toUtf8().constData());
	};

	// Odd-length hex in "on"
	test_invalid(R"({"id":"PPSA21567","version":"01.018.000","process":"eboot.bin","mods":[{"name":"A","memory":[{"offset":"0x10","on":"909","off":"0000"}]}]})",
	             "odd-length on bytes");
	// Odd-length hex in "off"
	test_invalid(R"({"id":"PPSA21567","version":"01.018.000","process":"eboot.bin","mods":[{"name":"A","memory":[{"offset":"0x10","on":"9090","off":"000"}]}]})",
	             "odd-length off bytes");
	// Non-hex chars in "on"
	test_invalid(R"({"id":"PPSA21567","version":"01.018.000","process":"eboot.bin","mods":[{"name":"A","memory":[{"offset":"0x10","on":"ZZ00","off":"0000"}]}]})",
	             "non-hex on bytes");
	// Non-hex chars in "off"
	test_invalid(R"({"id":"PPSA21567","version":"01.018.000","process":"eboot.bin","mods":[{"name":"A","memory":[{"offset":"0x10","on":"9090","off":"GG00"}]}]})",
	             "non-hex off bytes");
	// Mismatched on vs off byte lengths
	test_invalid(R"({"id":"PPSA21567","version":"01.018.000","process":"eboot.bin","mods":[{"name":"A","memory":[{"offset":"0x10","on":"9090","off":"00"}]}]})",
	             "mismatched on/off byte lengths");
	// Invalid offset
	test_invalid(R"({"id":"PPSA21567","version":"01.018.000","process":"eboot.bin","mods":[{"name":"A","memory":[{"offset":"0xZZG","on":"90","off":"00"}]}]})",
	             "invalid hex offset");
	// Non-zero section
	test_invalid(R"({"id":"PPSA21567","version":"01.018.000","process":"eboot.bin","mods":[{"name":"A","memory":[{"offset":"0x10","on":"90","off":"00","section":1}]}]})",
	             "non-zero section");
	// Fractional section
	test_invalid(R"({"id":"PPSA21567","version":"01.018.000","process":"eboot.bin","mods":[{"name":"A","memory":[{"offset":"0x10","on":"90","off":"00","section":0.5}]}]})",
	             "fractional section 0.5");
	// Unsupported memory addressing: absolute
	test_invalid(R"({"id":"PPSA21567","version":"01.018.000","process":"eboot.bin","mods":[{"name":"A","memory":[{"offset":"0x10","on":"90","off":"00","absolute":true}]}]})",
	             "absolute memory addressing");
	// Unsupported memory addressing: sectionName (DEFECT-07)
	test_invalid(R"({"id":"PPSA21567","version":"01.018.000","process":"eboot.bin","mods":[{"name":"A","memory":[{"offset":"0x10","on":"90","off":"00","sectionName":".text"}]}]})",
	             "sectionName addressing");
	// Unsupported memory addressing: logicalOffset (DEFECT-07)
	test_invalid(R"({"id":"PPSA21567","version":"01.018.000","process":"eboot.bin","mods":[{"name":"A","memory":[{"offset":"0x10","on":"90","off":"00","logicalOffset":"0x100"}]}]})",
	             "logicalOffset addressing");
	// Master codes
	test_invalid(R"({"id":"PPSA21567","version":"01.018.000","process":"eboot.bin","master":true,"mods":[{"name":"A","memory":[{"offset":"0x10","on":"90","off":"00"}]}]})",
	             "master code flag");
	// Module-specific cheats
	test_invalid(R"({"id":"PPSA21567","version":"01.018.000","process":"eboot.bin","mods":[{"name":"A","module_name":"libfoo.prx","memory":[{"offset":"0x10","on":"90","off":"00"}]}]})",
	             "module_name PRX patch");
	// Process mismatch
	test_invalid(R"({"id":"PPSA21567","version":"01.018.000","process":"other.bin","mods":[{"name":"A","memory":[{"offset":"0x10","on":"90","off":"00"}]}]})",
	             "mismatched process");
	// Title ID mismatch
	test_invalid(R"({"id":"PPSA99999","version":"01.018.000","process":"eboot.bin","mods":[{"name":"A","memory":[{"offset":"0x10","on":"90","off":"00"}]}]})",
	             "mismatched title id");
	// Empty mods
	test_invalid(R"({"id":"PPSA21567","version":"01.018.000","process":"eboot.bin","mods":[]})",
	             "empty mods list");

	// Strict vs non-strict version matching
	const QByteArray diff_ver_json = R"({
		"id": "PPSA21567",
		"version": "01.000.000",
		"process": "eboot.bin",
		"mods": [{"name": "A", "memory": [{"offset": "0x10", "on": "90", "off": "00"}]}]
	})";
	const auto diff_ver_doc = QJsonDocument::fromJson(diff_ver_json);
	const auto strict_err = Cheats::ValidateImport(diff_ver_doc, QStringLiteral("PPSA21567"), QStringLiteral("01.018.000"),
	                                               QStringLiteral("eboot.bin"), false);
	Check(!strict_err.isEmpty(), "strict version check should reject different version");

	const auto non_strict_err = Cheats::ValidateImport(diff_ver_doc, QStringLiteral("PPSA21567"), QStringLiteral("01.018.000"),
	                                                   QStringLiteral("eboot.bin"), true);
	Check(non_strict_err.isEmpty(), "allow_version_mismatch=true should accept different version");

	// Test Cheats::SaveDocument
	QTemporaryDir temp_dir;
	Check(temp_dir.isValid(), "QTemporaryDir should be valid");
	const QString save_path = temp_dir.filePath(QStringLiteral("nested/test_cheat.json"));

	// Save new file (expected_data is empty)
	QString save_err = Cheats::SaveDocument(save_path, valid_doc, {});
	Check(save_err.isEmpty(), QStringLiteral("SaveDocument new file failed: %1").arg(save_err).toUtf8().constData());
	Check(QFile::exists(save_path), "SaveDocument did not create file on disk");

	// Read saved data
	QFile saved_file(save_path);
	Check(saved_file.open(QIODevice::ReadOnly), "Cannot open saved file for reading");
	const QByteArray saved_bytes = saved_file.readAll();
	saved_file.close();

	// Save updated file with correct expected_data
	save_err = Cheats::SaveDocument(save_path, valid_doc, saved_bytes);
	Check(save_err.isEmpty(), "SaveDocument with matching expected_data failed");

	// Concurrent modification check: save with mismatched expected_data must fail
	save_err = Cheats::SaveDocument(save_path, valid_doc, QByteArray("mismatched_data"));
	Check(!save_err.isEmpty(), "SaveDocument with wrong expected_data should reject concurrent write");

	// Save invalid document or empty path
	Check(!Cheats::SaveDocument(QString(), valid_doc, {}).isEmpty(), "empty path must fail");
	Check(!Cheats::SaveDocument(save_path, QJsonDocument(), {}).isEmpty(), "null document must fail");
}

void TestConvertTrainer() {
	// 1. Standard XML trainer conversion
	QByteArray xml = R"(<Trainer Game="Test Game" Ppsa="PPSA-12345" Version="1.00" Process="eboot.bin" Moder="Author1">
		<Cheat Text="Infinite Ammo">
			<Offset>0x100</Offset>
			<ValueOn>90-90</ValueOn>
			<ValueOff>00-00</ValueOff>
		</Cheat>
	</Trainer>)";
	auto err = Cheats::ConvertTrainer(&xml, false);
	Check(err.isEmpty(), "ConvertTrainer XML failed");
	auto doc = QJsonDocument::fromJson(xml);
	Check(doc.object().value(QStringLiteral("id")).toString() == QStringLiteral("PPSA12345"), "TitleId should be PPSA12345 without hyphen");
	Check(doc.object().value(QStringLiteral("credits")).toArray().at(0).toString() == QStringLiteral("Author1"), "Author should be Author1");

	// 2. Attribute Fallbacks: CUSA, TitleId, Id, Author, Credits (DEFECT-07)
	QByteArray cusa_xml = R"(<Trainer Game="PS4 Game" Cusa="CUSA-54321" Version="1.05" Process="eboot.bin" Author="AuthorX">
		<Cheat Name="God Mode">
			<Offset>0x200</Offset>
			<ValueOn>90 90</ValueOn>
			<ValueOff>00 00</ValueOff>
		</Cheat>
	</Trainer>)";
	err = Cheats::ConvertTrainer(&cusa_xml, false);
	Check(err.isEmpty(), "ConvertTrainer CUSA failed");
	doc = QJsonDocument::fromJson(cusa_xml);
	Check(doc.object().value(QStringLiteral("id")).toString() == QStringLiteral("CUSA54321"), "TitleId should be CUSA54321");
	Check(doc.object().value(QStringLiteral("credits")).toArray().at(0).toString() == QStringLiteral("AuthorX"), "Author should be AuthorX");

	QByteArray title_id_fallback_xml = R"(<Trainer TitleId="PPSA22222" Version="1.00" Process="eboot.bin" Credits="ContributorZ">
		<Cheat Text="Cheat 1"><Offset>0x10</Offset><ValueOn>90</ValueOn><ValueOff>00</ValueOff></Cheat>
	</Trainer>)";
	err = Cheats::ConvertTrainer(&title_id_fallback_xml, false);
	Check(err.isEmpty(), "ConvertTrainer TitleId fallback failed");
	doc = QJsonDocument::fromJson(title_id_fallback_xml);
	Check(doc.object().value(QStringLiteral("id")).toString() == QStringLiteral("PPSA22222"), "TitleId fallback failed");
	Check(doc.object().value(QStringLiteral("credits")).toArray().at(0).toString() == QStringLiteral("ContributorZ"), "Credits fallback failed");

	QByteArray id_fallback_xml = R"(<Trainer Id="CUSA33333" Version="1.00" Process="eboot.bin" Moder="ModderM">
		<Cheat Text="Cheat 2"><Offset>0x10</Offset><ValueOn>90</ValueOn><ValueOff>00</ValueOff></Cheat>
	</Trainer>)";
	err = Cheats::ConvertTrainer(&id_fallback_xml, false);
	Check(err.isEmpty(), "ConvertTrainer Id fallback failed");
	doc = QJsonDocument::fromJson(id_fallback_xml);
	Check(doc.object().value(QStringLiteral("id")).toString() == QStringLiteral("CUSA33333"), "Id fallback failed");

	// 3. Multi-Write Cheat & Code Caves
	QByteArray multi_write_xml = R"(<Trainer Ppsa="PPSA11111" Version="1.00" Process="eboot.bin" Moder="Hacker">
		<Cheat Text="Multi Patch">
			<Offset>0x1000</Offset>
			<ValueOn>11 22</ValueOn>
			<ValueOff>00 00</ValueOff>
			<Offset>0x2000</Offset>
			<ValueOn>33 44</ValueOn>
			<ValueOff>00 00</ValueOff>
		</Cheat>
	</Trainer>)";
	err = Cheats::ConvertTrainer(&multi_write_xml, false);
	Check(err.isEmpty(), "ConvertTrainer multi-write failed");
	doc = QJsonDocument::fromJson(multi_write_xml);
	const auto memory = doc.object().value(QStringLiteral("mods")).toArray()[0].toObject().value(QStringLiteral("memory")).toArray();
	Check(memory.size() == 2, "multi-write cheat should produce 2 memory writes");

	// 3b. Nested <Write> Tags (SEC4-LOW-03)
	QByteArray nested_write_xml = R"(<Trainer Ppsa="PPSA11111" Version="1.00" Process="eboot.bin" Moder="Hacker">
		<Cheat Text="Nested Write">
			<Write>
				<Offset>0x3000</Offset>
				<ValueOn>55 66</ValueOn>
				<ValueOff>00 00</ValueOff>
			</Write>
		</Cheat>
	</Trainer>)";
	err = Cheats::ConvertTrainer(&nested_write_xml, false);
	Check(err.isEmpty(), "ConvertTrainer nested write failed");
	doc = QJsonDocument::fromJson(nested_write_xml);
	const auto nested_memory = doc.object().value(QStringLiteral("mods")).toArray()[0].toObject().value(QStringLiteral("memory")).toArray();
	Check(nested_memory.size() == 1, "nested write cheat should produce 1 memory write");
	Check(nested_memory[0].toObject().value(QStringLiteral("offset")).toString() == QStringLiteral("0x3000"), "nested offset mismatch");
	Check(nested_memory[0].toObject().value(QStringLiteral("on")).toString() == QStringLiteral("5566"), "nested on value mismatch");

	// 4. Malformed XML handling
	QByteArray unclosed_xml = R"(<Trainer Ppsa="PPSA11111" Version="1.00"><Cheat Text="Test"></Trainer>)";
	Check(!Cheats::ConvertTrainer(&unclosed_xml, false).isEmpty(), "unclosed XML tag should report error");

	QByteArray non_xml = "This is not XML at all";
	Check(!Cheats::ConvertTrainer(&non_xml, false).isEmpty(), "non-XML data should report error");

	QByteArray missing_attr_xml = R"(<Trainer Version="1.00"><Cheat Text="A"><Offset>0x10</Offset><ValueOn>90</ValueOn><ValueOff>00</ValueOff></Cheat></Trainer>)";
	Check(!Cheats::ConvertTrainer(&missing_attr_xml, false).isEmpty(), "trainer without Title ID attribute should report error");

	// Incomplete write with preceding complete write (nested form)
	const QByteArray original_nested_incomplete = R"(<Trainer Ppsa="PPSA11111" Version="1.00" Process="eboot.bin">
		<Cheat Text="NestedPartial">
			<Write><Offset>0x1000</Offset><ValueOn>1122</ValueOn><ValueOff>0000</ValueOff></Write>
			<Write><Offset>0x2000</Offset><ValueOn>9090</ValueOn></Write>
		</Cheat>
	</Trainer>)";
	QByteArray test_nested_incomplete = original_nested_incomplete;
	Check(!Cheats::ConvertTrainer(&test_nested_incomplete, false).isEmpty(),
	      "nested incomplete trainer write should report error even with preceding complete write");
	Check(test_nested_incomplete == original_nested_incomplete,
	      "data should remain unchanged on nested conversion error");

	// Incomplete write with preceding complete write (flat form)
	const QByteArray original_flat_incomplete = R"(<Trainer Ppsa="PPSA11111" Version="1.00" Process="eboot.bin">
		<Cheat Text="FlatPartial">
			<Offset>0x1000</Offset><ValueOn>1122</ValueOn><ValueOff>0000</ValueOff>
			<Offset>0x2000</Offset><ValueOn>9090</ValueOn>
		</Cheat>
	</Trainer>)";
	QByteArray test_flat_incomplete = original_flat_incomplete;
	Check(!Cheats::ConvertTrainer(&test_flat_incomplete, false).isEmpty(),
	      "flat incomplete trainer write should report error even with preceding complete write");
	Check(test_flat_incomplete == original_flat_incomplete,
	      "data should remain unchanged on flat conversion error");

	// 5. Positive and Negative AES-256-CBC Encrypted MC4 Decryption (DEFECT-06)
	QByteArray corrupt_mc4 = "CorruptedBase64==";
	Check(!Cheats::ConvertTrainer(&corrupt_mc4, true).isEmpty(), "invalid encrypted MC4 should report error");

	const QByteArray valid_mc4_xml = R"(<Trainer Ppsa="PPSA77777" Version="1.00" Process="eboot.bin" Moder="MC4Expert">
		<Cheat Text="Encrypted Cheat"><Offset>0x500</Offset><ValueOn>9090</ValueOn><ValueOff>0000</ValueOff></Cheat>
	</Trainer>)";
	QByteArray encrypted_mc4 = EncryptMc4(valid_mc4_xml);
	Check(!encrypted_mc4.isEmpty(), "EncryptMc4 failed to generate ciphertext");
	err = Cheats::ConvertTrainer(&encrypted_mc4, true);
	Check(err.isEmpty(), QStringLiteral("ConvertTrainer positive MC4 decryption failed: %1").arg(err).toUtf8().constData());
	doc = QJsonDocument::fromJson(encrypted_mc4);
	Check(doc.object().value(QStringLiteral("id")).toString() == QStringLiteral("PPSA77777"), "decrypted MC4 Title ID mismatch");
	Check(doc.object().value(QStringLiteral("credits")).toArray().at(0).toString() == QStringLiteral("MC4Expert"), "decrypted MC4 author mismatch");
}

void TestCheatRepositoryClass() {
	// Test Cheats::CheatRepository class lifecycle, error signal dispatch, and cancel logic (DEFECT-03)
	Cheats::CheatRepository repo;

	// 1. Load with invalid/unsupported Title ID
	bool catalog_loaded_fired = false;
	QString catalog_error;
	QObject::connect(&repo, &Cheats::CheatRepository::CatalogLoaded,
	                 [&catalog_loaded_fired, &catalog_error](const QList<Cheats::RemoteFile>& files, const QString& err) {
		                 catalog_loaded_fired = true;
		                 catalog_error        = err;
		                 Check(files.isEmpty(), "files should be empty on error");
	                 });

	repo.Load(QStringLiteral("INVALID_ID"));
	QCoreApplication::processEvents();
	Check(catalog_loaded_fired, "CatalogLoaded signal should be emitted for invalid Title ID");
	Check(!catalog_error.isEmpty(), "CatalogLoaded should contain error message");

	// 2. Fetch with invalid / insecure URL
	bool file_loaded_fired = false;
	QString file_error;
	QObject::connect(&repo, &Cheats::CheatRepository::FileLoaded,
	                 [&file_loaded_fired, &file_error](const QByteArray& data, const QString& err) {
		                 file_loaded_fired = true;
		                 file_error        = err;
		                 Check(data.isEmpty(), "data should be empty on error");
	                 });

	Cheats::RemoteFile bad_file;
	bad_file.url = QUrl(QStringLiteral("http://insecure.example.com/cheat.json"));
	repo.Fetch(bad_file);
	QCoreApplication::processEvents();
	Check(file_loaded_fired, "FileLoaded signal should be emitted for insecure URL");
	Check(!file_error.isEmpty(), "FileLoaded should report error for insecure URL");

	// 3. CancelFetch generation tracking: calling CancelFetch before event delivery invalidates generation
	file_loaded_fired = false;
	repo.Fetch(bad_file);
	repo.CancelFetch();
	QCoreApplication::processEvents();
	Check(!file_loaded_fired, "Cancelled fetch should not deliver stale FileLoaded signal");
}

} // namespace

int main(int argc, char** argv) {
	try {
		QCoreApplication app(argc, argv);
		TestVersionsAndTitles();
		TestParseModulesCatalog();
		TestParseIndex();
		TestConfigPreset();
		TestMergeDocumentsAndPrepare();
		TestSuccessiveCrossVersionMerge();
		TestValidateImportAndSave();
		TestConvertTrainer();
		TestCheatRepositoryClass();
		std::printf("CheatRepositoryTests PASSED successfully!\n");
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "CheatRepositoryTests caught exception: %s\n", e.what());
		return 1;
	} catch (...) {
		std::fprintf(stderr, "CheatRepositoryTests caught unknown exception!\n");
		return 1;
	}
}
