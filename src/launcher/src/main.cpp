#include "mainDialog.h"
#include "launcherTheme.h"

#include <QApplication>
#include <QDir>
#include <QLocale>
#include <QTranslator>

namespace {

QString TranslationDirectory() {
#	if defined(Q_OS_MACOS)
	return QDir(QCoreApplication::applicationDirPath()).filePath("../Resources/translations");
#	else
	return QDir(QCoreApplication::applicationDirPath()).filePath("translations");
#	endif
}

} // namespace

int main(int argc, char* argv[]) {
	QApplication a(argc, argv);
	QTranslator translator;
	if (translator.load(QLocale(), "kytyps5", "_", TranslationDirectory())) {
		a.installTranslator(&translator);
	}
	LauncherTheme::Initialize(a);

	MainDialog w;

	w.emit Start();

	w.show();

	return QApplication::exec();
}
