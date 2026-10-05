#include "launcher/include/configuration.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "DlssSettingsTests: %s\n", message);
		std::exit(1);
	}
}

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	QTemporaryDir directory;
	Check(directory.isValid(), "temporary directory");
	QSettings settings(directory.filePath("settings.ini"), QSettings::IniFormat);
	Configuration legacy;
	legacy.ReadSettings(&settings);
	Check(legacy.dlss_mode == Configuration::DlssMode::Off, "legacy config enables DLSS");
	Check(legacy.render_scale_percent == 100, "legacy config reduces render scale");
	Check(!legacy.dlss_frame_generation, "legacy config enables Frame Generation");
	for (const auto& text : EnumToList<Configuration::DlssMode>()) {
		Configuration original;
		original.dlss_mode = TextToEnum<Configuration::DlssMode>(text);
		original.render_scale_percent = 50;
		original.dlss_frame_generation = true;
		original.WriteSettings(&settings);
		settings.sync();
		QSettings saved(directory.filePath("settings.ini"), QSettings::IniFormat);
		Configuration loaded;
		loaded.ReadSettings(&saved);
		Check(loaded.dlss_mode == original.dlss_mode, "DLSS mode not persisted");
		Check(loaded.render_scale_percent == 50, "render scale not persisted");
		Check(loaded.dlss_frame_generation, "Frame Generation not persisted");
		Configuration game;
		game.CopyEmulatorSettingsFrom(loaded);
		Check(game.dlss_mode == loaded.dlss_mode, "global mode not inherited by game");
		Check(game.render_scale_percent == 50 && game.dlss_frame_generation, "upscale settings not inherited");
	}
	for (const auto& text : {QString("invalid"), QString("24"), QString("101"), QString("")}) {
		settings.setValue("render_scale_percent", text);
		Configuration loaded;
		loaded.ReadSettings(&settings);
		Check(loaded.render_scale_percent == 100, "corrupt scale not reset to native");
	}
	for (const auto& text : {QString("invalid"), QString("999"), QString("")}) {
		settings.setValue("dlss_mode", text);
		Configuration loaded;
		loaded.ReadSettings(&settings);
		Check(loaded.dlss_mode == Configuration::DlssMode::Off, "corrupt mode not disabled");
	}
	std::puts("DLSS settings tests passed");
}
