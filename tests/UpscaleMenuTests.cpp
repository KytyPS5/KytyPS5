#include "launcher/include/configuration.h"
#include "launcher/include/configurationEditDialog.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <cstdio>
#include <cstdlib>

static void Check(bool value, const char* message) {
	if (!value) { std::fprintf(stderr, "UpscaleMenuTests: %s\n", message); std::exit(1); }
}
int main(int argc, char** argv) {
	qputenv("SDL_AUDIODRIVER", "dummy");
	QApplication app(argc, argv);
	Configuration info;
	ConfigurationEditDialog dialog(info);
	dialog.SetGlobalSettings({});
	auto* output = dialog.findChild<QComboBox*>("comboBox_screen_resolution");
	auto* mode = dialog.findChild<QComboBox*>("comboBox_dlss");
	auto* scale = dialog.findChild<QSpinBox*>("spinBox_render_scale");
	auto* fg = dialog.findChild<QCheckBox*>("checkBox_dlss_frame_generation");
	auto* summary = dialog.findChild<QLabel*>("label_upscale_summary");
	Check(output && mode && scale && fg && summary, "upscale controls missing");
	output->setCurrentText("2560x1440");
	mode->setCurrentText("Quality");
	scale->setValue(50);
	fg->setChecked(true);
	Check(summary->text().contains("50%") && summary->text().contains("2560x1440") && summary->text().contains("Quality"),
	      "upscale summary does not follow selected resolution/scale/mode");
#if defined(KYTY_HAS_DLSS_FG)
	Check(fg->isEnabled(), "Frame Generation unavailable despite enabled build");
#else
	Check(!fg->isEnabled(), "Frame Generation enabled without a backend");
#endif
	dialog.show();
	app.processEvents();
	if (const auto capture = qEnvironmentVariable("KYTY_MENU_CAPTURE"); !capture.isEmpty()) {
		Check(dialog.grab().save(capture), "settings screenshot failed");
	}
	dialog.findChild<QPushButton*>("ok_button")->click();
	Check(dialog.result() == QDialog::Accepted, "settings dialog did not save");
	Check(info.screen_resolution == Configuration::Resolution::R2560X1440 &&
	      info.dlss_mode == Configuration::DlssMode::Quality && info.render_scale_percent == 50 && info.dlss_frame_generation,
	      "saved menu choices do not reach emulator configuration");
	std::puts("Upscale menu integration passed");
}
