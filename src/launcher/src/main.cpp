#include "mainDialog.h"
#include "launcherTheme.h"

#include <QApplication>
#include <QDir>
#include <QLocale>
#include <QIcon>
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
	const QStringList ui_languages = QLocale().uiLanguages();
	const bool english_first =
		!ui_languages.isEmpty() && QLocale(ui_languages.first()).language() == QLocale::English;
	if (!english_first && translator.load(QLocale(), "kytyps5", "_", TranslationDirectory())) {
		a.installTranslator(&translator);
	}
	// Title bars and taskbars get the bolder small-size artwork.
	const QIcon small_art(QStringLiteral(":/icons/kyty-small.svg"));
	const QIcon large_art(QStringLiteral(":/icons/kyty.svg"));
	QIcon       icon;
	for (int size: {16, 24, 32, 48, 64}) {
		icon.addPixmap(small_art.pixmap(QSize(size, size), 1.0));
	}
	for (int size: {128, 256}) {
		icon.addPixmap(large_art.pixmap(QSize(size, size), 1.0));
	}
	QApplication::setWindowIcon(icon);
	LauncherTheme::Initialize(a);

	MainDialog w;

	w.emit Start();

	w.show();

	return QApplication::exec();
}
