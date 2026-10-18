#include "configuration.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

static void Require(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "ExperimentalSettingsTests: %s\n", message);
		std::abort();
	}
}

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	QTemporaryDir    temporary;
	Require(temporary.isValid(), "temporary settings directory");
	QSettings     settings(temporary.filePath("settings.ini"), QSettings::IniFormat);
	Configuration original;
	Require(!original.defer_release_mem_interrupts_enabled, "experiment must default off");
	original.defer_release_mem_interrupts_enabled = true;
	original.WriteSettings(&settings);
	settings.sync();
	Require(settings.status() == QSettings::NoError, "settings write failed");
	QSettings     reloaded(temporary.filePath("settings.ini"), QSettings::IniFormat);
	Configuration restored;
	restored.ReadSettings(&reloaded);
	Require(restored.defer_release_mem_interrupts_enabled, "saved experiment was not restored");
	Configuration effective;
	effective.CopyEmulatorSettingsFrom(restored);
	Require(effective.defer_release_mem_interrupts_enabled,
	        "effective game configuration lost experiment");
	const auto key = QStringLiteral("defer_release_mem_interrupts_enabled");
	Require(restored.GameSettings().value(key).toString() == "true", "export omitted experiment");
	QString error;
	Require(restored.SetGameSettings(QJsonObject {{key, "false"}}, error),
	        "import rejected experiment");
	Require(!restored.defer_release_mem_interrupts_enabled, "import did not disable experiment");
	settings.remove(key);
	restored.defer_release_mem_interrupts_enabled = true;
	restored.ReadSettings(&settings);
	Require(!restored.defer_release_mem_interrupts_enabled,
	        "legacy settings unexpectedly enabled experiment");
	std::puts("Experimental settings tests passed");
}
