#include "cheatFile.h"
#include "cheatRepository.h"

#include <QJsonArray>
#include <QJsonObject>

#include <gtest/gtest.h>

namespace {

QJsonDocument Plan(const char* version, QJsonArray mods, const char* process = "eboot.bin") {
	QJsonObject root {{"id", "PPSA00001"}, {"version", version}, {"mods", mods}};
	if (process != nullptr) {
		root.insert("process", process);
	}
	return QJsonDocument(root);
}

QJsonObject Mod(const char* name) {
	return {{"name", name},
	        {"enabled", true},
	        {"memory", QJsonArray {QJsonObject {{"offset", "0x10"}, {"on", "90"}, {"off", "c3"}}}}};
}

QStringList Names(const QJsonDocument& document) {
	QStringList names;
	for (const auto& mod: document.object().value("mods").toArray()) {
		names.append(mod.toObject().value("name").toString() +
		             (mod.toObject().value("enabled").toBool() ? "=on" : "=off"));
	}
	return names;
}

} // namespace

TEST(LauncherCheatTests, ParseModulesListsTitlePatchesAndCheatsInsideTheCatalog) {
	const auto catalog = R"({
		"patches": {
			"PPSA00001": [{"path": "patches/json/a.json", "title": "Unlock", "gameName": "Game",
			               "version": "01.000.000"}],
			"PPSA00002": [{"path": "patches/json/other.json", "title": "Other"}]
		},
		"cheats": {
			"PPSA00001": [{"path": "cheats/json/b.json", "title": "Lives", "version": "01.001.000"},
			              {"path": "../escape.json"}, {"path": "https://example.com/x.json"},
			              {"path": "//example.com/x.json"}, {}]
		},
		"configs": {"PPSA00001": [{"path": "configs/json/c.json", "title": "Preset"}]}
	})";
	const auto result  = Cheats::ParseModules(catalog, "ppsa00001");
	ASSERT_EQ(result.files.size(), 2);
	EXPECT_EQ(result.files[0].name, "Unlock");
	EXPECT_EQ(result.files[0].title, "Game");
	EXPECT_EQ(result.files[0].version, "01.000.000");
	EXPECT_EQ(result.files[0].format, "json");
	EXPECT_EQ(result.files[0].kind, "Patch");
	EXPECT_EQ(result.files[0].url.toString(),
	          "https://kytyps5.github.io/KytyPS5-Modules/patches/json/a.json");
	EXPECT_EQ(result.files[1].name, "Lives");
	EXPECT_EQ(result.files[1].kind, "Cheat");
	EXPECT_EQ(result.error, "Skipped 4 invalid module catalog entries.");

	EXPECT_EQ(Cheats::ParseModules("[]", "PPSA00001").error, "Invalid module catalog.");
}

TEST(LauncherCheatTests, ParseIndexPointsAtTheCollectionFile) {
	const auto result =
	    Cheats::ParseIndex("PPSA00001_01.000.000_0a1b2c3d.mc4=Game\n", "mc4", "PPSA00001");
	ASSERT_EQ(result.files.size(), 1);
	EXPECT_EQ(result.files[0].kind, "Cheat");
	EXPECT_EQ(result.files[0].url.toString(),
	          "https://raw.githubusercontent.com/TeeKay87/HEN-Cheats-Collection/master/cheats/mc4/"
	          "PPSA00001_01.000.000_0a1b2c3d.mc4");
}

TEST(LauncherCheatTests, ValidateImportChecksTheProcessOnlyWhenTheFileNamesOne) {
	const auto validate = [](const char* file_process, const char* process) {
		return Cheats::ValidateImport(Plan("01.000.000", {Mod("A")}, file_process), "PPSA00001",
		                              "01.000.000", process);
	};
	EXPECT_EQ(validate(nullptr, "eboot.bin"), "");
	EXPECT_EQ(validate("EBOOT.BIN", "eboot.bin"), "");
	EXPECT_NE(validate("other.elf", "eboot.bin"), "");
	EXPECT_NE(validate(nullptr, ""), "");
}

TEST(LauncherCheatTests, MergeKeepsTheOtherCheatsOfAValidLocalFile) {
	const auto local    = Plan("01.000.000", {Mod("A"), Mod("B"), Mod("C")});
	const auto imported = Plan("01.000.000", {Mod("B"), Mod("D")}, nullptr);

	const auto merged = Cheats::Merge(local, imported, "eboot.bin");
	EXPECT_EQ(Names(merged), QStringList({"A=on", "C=on", "B=off", "D=off"}));
	EXPECT_EQ(merged.object().value("process").toString(), "eboot.bin");

	const auto replaced = Cheats::Merge(Plan("00.900.000", {Mod("E")}), imported, "game.elf");
	EXPECT_EQ(Names(replaced), QStringList({"B=off", "D=off"}));
	EXPECT_EQ(replaced.object().value("version").toString(), "01.000.000");
	EXPECT_EQ(replaced.object().value("process").toString(), "game.elf");
	EXPECT_EQ(Cheats::Merge({}, Plan("01.000.000", {Mod("B")}, "EBOOT.BIN"), "eboot.bin")
	              .object()
	              .value("process")
	              .toString(),
	          "eboot.bin");
	EXPECT_EQ(Names(Cheats::Merge(Plan("01.000.000", {Mod("E")}, "other.elf"), imported, "eboot.bin")),
	          QStringList({"B=off", "D=off"}));
}

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
