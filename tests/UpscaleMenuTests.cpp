#include "launcher/include/configuration.h"
#include "launcher/include/configurationEditDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>

#include <cstdio>
#include <cstdlib>

static void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "UpscaleMenuTests: %s\n", message);
		std::exit(1);
	}
}
int main(int argc, char** argv) {
	qputenv("SDL_AUDIODRIVER", "dummy");
	QApplication            app(argc, argv);
	Configuration           info;
	ConfigurationEditDialog dialog(info);
	dialog.SetGlobalSettings({});
	auto* output       = dialog.findChild<QComboBox*>("comboBox_screen_resolution");
	auto* mode         = dialog.findChild<QComboBox*>("comboBox_dlss");
	auto* scale        = dialog.findChild<QSpinBox*>("spinBox_render_scale");
	auto* fg           = dialog.findChild<QCheckBox*>("checkBox_dlss_frame_generation");
	auto* summary      = dialog.findChild<QLabel*>("label_upscale_summary");
	auto* experimental = dialog.findChild<QGroupBox*>("experimental_group");
	Check(output && mode && scale && fg && summary && experimental && experimental->isCheckable(),
	      "upscale controls missing");
	Check(!experimental->isChecked() && !mode->isEnabled() && !fg->isEnabled() &&
	          !scale->isEnabled(),
	      "experimental defaults must be disabled");
	experimental->setChecked(true);
#if !defined(KYTY_HAS_DLSS)
	Check(!mode->isEnabled(), "native DLSS enabled without its SDK");
#endif
	output->setCurrentText("2560x1440");
	mode->setCurrentText("Quality");
	scale->setValue(50);
	auto* backend   = dialog.findChild<QComboBox*>("comboBox_upscale_backend");
	auto* motion    = dialog.findChild<QComboBox*>("comboBox_upscale_motion");
	auto* runtime   = dialog.findChild<QLineEdit*>("lineEdit_optiscaler_path");
	auto* upscaler  = dialog.findChild<QComboBox*>("comboBox_optiscaler_upscaler");
	auto* generator = dialog.findChild<QComboBox*>("comboBox_optiscaler_frame_generation");
	Check(backend && motion && runtime && runtime->isHidden() && upscaler && generator &&
	          upscaler->isHidden() && generator->isHidden(),
	      "backend/motion/path controls missing or initially incorrect");
	backend->setCurrentText("OptiScaler");
	motion->setCurrentText("Geometry");
	runtime->setText("D:/OptiScaler/OptiScaler.dll");
	Check(!runtime->isHidden() && !upscaler->isHidden() && !generator->isHidden() &&
	          runtime->isEnabled() && summary->text().contains("OptiScaler"),
	      "OptiScaler path or summary did not follow backend");
	Check(upscaler->isEnabled() && generator->isEnabled(),
	      "OptiScaler algorithm choices stay disabled");
	upscaler->setCurrentText("XeSS");
	generator->setCurrentText("XeSS");
#if defined(_WIN32)
	Check(mode->isEnabled(), "direct XeSS upscaling is disabled without NVIDIA SDKs");
#endif
	Check(summary->text().contains("OptiScaler XeSS"), "summary does not name the direct upscaler");

	fg->setChecked(true);
	auto* frames = dialog.findChild<QSpinBox*>("spinBox_frame_generation_level");
	Check(frames && frames->isHidden() && !frames->isEnabled() && frames->minimum() == 1 &&
	          frames->maximum() == 4 && frames->value() == 1,
	      "Frame Generation level control missing or out of range");
	frames->setValue(3);
	Check(experimental && experimental->isAncestorOf(backend) && experimental->isAncestorOf(fg) &&
	          experimental->isAncestorOf(frames) && experimental->isAncestorOf(mode),
	      "upscaling and Frame Generation settings are not in the Experimental group");
	Check(summary->text().contains("50%") && summary->text().contains("2560x1440") &&
	          summary->text().contains("Quality"),
	      "upscale summary does not follow selected resolution/scale/mode");
#if defined(KYTY_HAS_DLSS_FG) || defined(_WIN32)
	Check(fg->isEnabled(), "Frame Generation unavailable despite enabled build");
#else
	Check(!fg->isEnabled(), "Frame Generation enabled without a backend");
#endif
	Check(fg->text().contains("OptiScaler") && fg->text().contains("XeSS"),
	      "Frame Generation does not identify selected backend and generator");
	backend->setCurrentText("Native");
	Check(runtime->isHidden() && upscaler->isHidden() && generator->isHidden() &&
	          !frames->isHidden() && fg->text() == "DLSS Frame Generation",
	      "native selection did not hide OptiScaler options or restore DLSS options");
#if defined(KYTY_HAS_DLSS_FG)
	Check(frames->isEnabled(), "native frame count disabled with FG selected");
#endif
	fg->setChecked(false);
	Check(!frames->isEnabled(), "frame count enabled with FG off");
	mode->setCurrentText("Off");
	Check(!motion->isEnabled(), "motion enabled without SR or FG");
	fg->setChecked(true);
	mode->setCurrentText("Quality");
	backend->setCurrentText("OptiScaler");
	experimental->setChecked(false);
	Check(!backend->isEnabled() && !mode->isEnabled() && !fg->isEnabled() &&
	          !runtime->isEnabled() && !motion->isEnabled() && !scale->isEnabled() &&
	          !upscaler->isEnabled() && !generator->isEnabled(),
	      "experimental Off left editable controls");
	Check(summary->text().contains("100%") && mode->currentText() == "Quality" && fg->isChecked(),
	      "experimental Off does not summarize effective settings or preserve selections");
	experimental->setChecked(true);
	dialog.show();
	app.processEvents();
	{
		// Save must stay reachable: the dialog fits the screen and its contents scroll.
		const QRect available = dialog.screen()->availableGeometry();
		auto*       save      = dialog.findChild<QPushButton*>("ok_button");
		const QRect button(save->mapToGlobal(QPoint(0, 0)), save->size());
		std::printf("screen %dx%d, dialog %dx%d, Save at y=%d\n", available.width(),
		            available.height(), dialog.frameGeometry().width(),
		            dialog.frameGeometry().height(), button.bottom());
		Check(dialog.height() <= available.height() && available.contains(button),
		      "settings dialog leaves the Save button off screen");
	}
	if (const auto capture = qEnvironmentVariable("KYTY_MENU_CAPTURE"); !capture.isEmpty()) {
		Check(dialog.grab().save(capture), "settings screenshot failed");
		backend->setCurrentText("Native");
		app.processEvents();
		Check(dialog.grab().save(capture + ".native.png"), "native settings screenshot failed");
		experimental->setChecked(false);
		app.processEvents();
		Check(dialog.grab().save(capture + ".disabled.png"), "disabled settings screenshot failed");
		experimental->setChecked(true);
		backend->setCurrentText("OptiScaler");
	}
	dialog.findChild<QPushButton*>("ok_button")->click();
	Check(dialog.result() == QDialog::Accepted, "settings dialog did not save");
	Check(info.screen_resolution == Configuration::Resolution::R2560X1440 &&
	          info.dlss_mode == Configuration::DlssMode::Quality &&
	          info.render_scale_percent == 50 && info.dlss_frame_generation &&
	          info.upscale_backend == Configuration::UpscaleBackend::OptiScaler &&
	          info.upscale_motion == Configuration::UpscaleMotion::Geometry &&
	          info.optiscaler_path == "D:/OptiScaler/OptiScaler.dll" &&
	          info.optiscaler_upscaler == Configuration::OptiScalerUpscaler::XeSS &&
	          info.optiscaler_frame_generation == Configuration::OptiScalerFrameGeneration::XeSS &&
	          info.frame_generation_frames == 1,
	      "saved menu choices do not reach emulator configuration");
	ConfigurationEditDialog saved(info);
	auto*                   saved_group = saved.findChild<QGroupBox*>("experimental_group");
	Check(saved_group->isChecked(), "active configuration did not reopen with Experimental on");
	saved_group->setChecked(false);
	saved.findChild<QPushButton*>("ok_button")->click();
	Check(info.dlss_mode == Configuration::DlssMode::Off && !info.dlss_frame_generation &&
	          info.render_scale_percent == 100,
	      "saving Experimental Off retained active features");
	ConfigurationEditDialog disabled(info);
	Check(!disabled.findChild<QGroupBox*>("experimental_group")->isChecked(),
	      "disabled configuration reopened with Experimental on");
	std::puts("Upscale menu integration passed");
}
