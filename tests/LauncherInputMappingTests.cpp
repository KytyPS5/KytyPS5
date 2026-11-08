#include "inputMappingDialog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "LauncherInputMappingTests: %s\n", message);
		std::abort();
	}
}

QPushButton* Binding(InputMappingDialog& dialog, const QString& id) {
	auto* button = dialog.findChild<QPushButton*>(QStringLiteral("binding_") + id);
	Check(button != nullptr, "a swipe binding is missing from the dialog");
	return button;
}

void Assign(InputMappingDialog& dialog, const QString& id, Qt::Key key) {
	QTimer::singleShot(0, [&] {
		auto* capture = QApplication::activeModalWidget();
		Check(capture != nullptr, "the binding capture dialog did not open");
		QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
		QApplication::sendEvent(capture, &press);
	});
	Binding(dialog, id)->click();
}
} // namespace

int main(int argc, char** argv) {
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QApplication       application(argc, argv);
	const QStringList  ids {"TouchPadSwipeUp", "TouchPadSwipeDown", "TouchPadSwipeLeft",
	                        "TouchPadSwipeRight"};
	const Qt::Key      keys[] = {Qt::Key_U, Qt::Key_N, Qt::Key_B, Qt::Key_M};
	InputMappingDialog dialog({}, nullptr);
	auto*              tabs = dialog.findChild<QTabWidget*>(QStringLiteral("inputTabs"));
	Check(tabs != nullptr, "the mapping tabs are missing");
	tabs->setCurrentIndex(1);
	dialog.show();
	QApplication::processEvents();
	for (int i = 0; i < ids.size(); ++i) {
		Check(Binding(dialog, ids[i])->text() == QStringLiteral("None"),
		      "a swipe took over an existing default key");
		Check(dialog.findChild<QPushButton*>(QStringLiteral("padBinding_") + ids[i]) == nullptr,
		      "a keyboard swipe was exposed as a gamepad mapping");
		Assign(dialog, ids[i], keys[i]);
	}
	Check(dialog.Mapping().contains(QStringLiteral("Cross=J")),
	      "assigning a swipe lost the other keyboard mappings");
	for (int i = 0; i < ids.size(); ++i) {
		Check(dialog.Mapping().contains(ids[i] + QLatin1Char('=') +
		                                QChar(static_cast<char16_t>(keys[i]))),
		      "a captured swipe was not serialized");
	}
	// Go through an actual settings file, as the launcher does when saving controls.
	QTemporaryDir directory;
	Check(directory.isValid(), "cannot create temporary settings directory");
	const auto path = directory.filePath(QStringLiteral("Kyty.ini"));
	{
		QSettings settings(path, QSettings::IniFormat);
		settings.setValue(QStringLiteral("host_input_mapping"), dialog.Mapping());
		settings.sync();
		Check(settings.status() == QSettings::NoError, "cannot save swipe mappings");
	}
	QSettings          settings(path, QSettings::IniFormat);
	InputMappingDialog restored(settings.value(QStringLiteral("host_input_mapping")).toStringList(),
	                            nullptr);
	Check(restored.Mapping() == dialog.Mapping(), "saved swipe bindings did not round trip");
	for (int i = 0; i < ids.size(); ++i) {
		Check(Binding(restored, ids[i])->text() == QString(QChar(static_cast<char16_t>(keys[i]))),
		      "a reloaded swipe binding was not displayed");
	}
	Assign(dialog, ids[1], Qt::Key_U);
	Check(Binding(dialog, ids[0])->text() == QStringLiteral("None") &&
	          dialog.Mapping().contains(QStringLiteral("TouchPadSwipeDown=U")),
	      "assigning a conflicting key did not move the binding");
	Binding(dialog, ids[1])->customContextMenuRequested(QPoint());
	Check(!dialog.Mapping().contains(QStringLiteral("TouchPadSwipeDown=U")),
	      "clearing a swipe retained its saved binding");
	dialog.findChild<QPushButton*>(QStringLiteral("defaults"))->click();
	Check(dialog.Mapping().isEmpty(), "restoring keyboard defaults retained a swipe binding");
	for (const auto& id: ids) {
		Check(Binding(dialog, id)->text() == QStringLiteral("None"),
		      "restoring keyboard defaults retained a displayed swipe");
	}
	return 0;
}
